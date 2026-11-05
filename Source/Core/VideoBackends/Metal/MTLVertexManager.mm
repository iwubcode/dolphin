// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/Metal/MTLVertexManager.h"

#include "Common/MsgHandler.h"

#include "Core/System.h"

#include "VideoBackends/Metal/MTLObjectCache.h"
#include "VideoBackends/Metal/MTLStateTracker.h"

#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexShaderManager.h"

Metal::VertexManager::VertexManager()
{
}

Metal::VertexManager::~VertexManager() = default;

bool Metal::VertexManager::Initialize()
{
  if (!VertexManagerBase::Initialize())
    return false;

  // Utility vertex arena: dedicated persistent CPU-shared MTLBuffers OUTSIDE the
  // StateTracker upload ring. Overwritten in place; offsets always 0.
  const MTLResourceOptions options =
      MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
  m_utility_vertex_buffer =
      MRCTransfer([g_device newBufferWithLength:UTILITY_VERTEX_BUFFER_SIZE options:options]);
  [m_utility_vertex_buffer setLabel:@"Utility Vertex Buffer"];
  m_utility_index_buffer =
      MRCTransfer([g_device newBufferWithLength:UTILITY_INDEX_BUFFER_SIZE options:options]);
  [m_utility_index_buffer setLabel:@"Utility Index Buffer"];
  if (!m_utility_vertex_buffer || !m_utility_index_buffer)
  {
    PanicAlertFmt("Failed to allocate utility vertex buffers");
    return false;
  }

  return true;
}

void Metal::VertexManager::UploadUtilityUniforms(const void* uniforms, u32 uniforms_size)
{
  g_state_tracker->SetUtilityUniform(uniforms, uniforms_size);
}

bool Metal::VertexManager::UploadTexelBuffer(const void* data, u32 data_size,
                                             TexelBufferFormat format, u32* out_offset)
{
  *out_offset = 0;
  StateTracker::Map map = g_state_tracker->Allocate(StateTracker::UploadBuffer::Texels, data_size,
                                                    StateTracker::AlignMask::Other);
  memcpy(map.cpu_buffer, data, data_size);
  g_state_tracker->SetTexelBuffer(map.gpu_buffer, map.gpu_offset, 0);
  return true;
}

bool Metal::VertexManager::UploadTexelBuffer(const void* data, u32 data_size,
                                             TexelBufferFormat format, u32* out_offset,
                                             const void* palette_data, u32 palette_size,
                                             TexelBufferFormat palette_format,
                                             u32* out_palette_offset)
{
  *out_offset = 0;
  *out_palette_offset = 0;

  const u32 aligned_data_size = g_state_tracker->Align(data_size, StateTracker::AlignMask::Other);
  const u32 total_size = aligned_data_size + palette_size;
  StateTracker::Map map = g_state_tracker->Allocate(StateTracker::UploadBuffer::Texels, total_size,
                                                    StateTracker::AlignMask::Other);
  memcpy(map.cpu_buffer, data, data_size);
  memcpy(static_cast<char*>(map.cpu_buffer) + aligned_data_size, palette_data, palette_size);
  g_state_tracker->SetTexelBuffer(map.gpu_buffer, map.gpu_offset,
                                  map.gpu_offset + aligned_data_size);
  return true;
}

void Metal::VertexManager::ResetBuffer(u32 vertex_stride)
{
  const u32 max_vertex_size = 65535 * vertex_stride;
  const u32 vertex_alloc = max_vertex_size + vertex_stride - 1;  // for alignment
  auto vertex = g_state_tracker->Preallocate(StateTracker::UploadBuffer::Vertex, vertex_alloc);
  auto index =
      g_state_tracker->Preallocate(StateTracker::UploadBuffer::Index, MAXIBUFFERSIZE * sizeof(u16));

  // Align the base vertex
  m_base_vertex = (vertex.second + vertex_stride - 1) / vertex_stride;
  m_vertex_offset = m_base_vertex * vertex_stride - vertex.second;
  m_cur_buffer_pointer = m_base_buffer_pointer = static_cast<u8*>(vertex.first) + m_vertex_offset;
  m_end_buffer_pointer = m_base_buffer_pointer + max_vertex_size;
  m_last_reset_pointer = m_cur_buffer_pointer;
  m_index_generator.Start(static_cast<u16*>(index.first));
}

void Metal::VertexManager::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                        u32* out_base_vertex, u32* out_base_index)
{
  const u32 vsize = num_vertices * vertex_stride + m_vertex_offset;
  const u32 isize = num_indices * sizeof(u16);
  StateTracker::Map vmap = g_state_tracker->CommitPreallocation(
      StateTracker::UploadBuffer::Vertex, vsize, StateTracker::AlignMask::None);
  StateTracker::Map imap = g_state_tracker->CommitPreallocation(
      StateTracker::UploadBuffer::Index, isize, StateTracker::AlignMask::None);

  ADDSTAT(g_stats.this_frame.bytes_vertex_streamed, vsize);
  ADDSTAT(g_stats.this_frame.bytes_index_streamed, isize);

  DEBUG_ASSERT(vmap.gpu_offset + m_vertex_offset == m_base_vertex * vertex_stride);
  // Cache the committed pair; the bind moved to SetBuffer (the single game-draw
  // bind site). CommitBuffer no longer binds.
  m_committed_vertex_buffer = vmap.gpu_buffer;
  m_committed_index_buffer = imap.gpu_buffer;
  *out_base_vertex = m_base_vertex;
  *out_base_index = imap.gpu_offset / sizeof(u16);
}

void Metal::VertexManager::SetBuffer(u32)
{
  // The single game-draw bind site. Re-applies the cached committed pair for
  // every draw, which is what makes paused replay correct: after a utility
  // upload binds the utility buffers, the next game draw rebinds the game ones.
  //
  // A single cached pair is correct on Metal because deferred rendering is gated
  // OFF (bSupportsDeferredGraphics = false): no snapshot offsets ever outlive a
  // frame, so "the buffers the draw's offsets reference" is always "the last
  // committed pair."
  if (m_committed_vertex_buffer)
    g_state_tracker->SetVerticesAndIndices(m_committed_vertex_buffer, m_committed_index_buffer);
}

std::optional<UtilityRegion> Metal::VertexManager::LockUtilityBuffers(u32 vertex_bytes,
                                                                      u32 index_bytes, u32)
{
  // Bounds-check against the dedicated persistent utility buffers. Offsets are
  // always 0 (the buffers are overwritten in place). Returns nullopt on
  // overflow (skip the draw, never panic).
  if (vertex_bytes > UTILITY_VERTEX_BUFFER_SIZE)
    return std::nullopt;
  if (index_bytes > UTILITY_INDEX_BUFFER_SIZE)
    return std::nullopt;

  return UtilityRegion{
      .vertex_pointer = static_cast<u8*>([m_utility_vertex_buffer contents]),
      .index_pointer = static_cast<u16*>([m_utility_index_buffer contents]),
      .vertex_offset = 0,
      .index_offset = 0,
  };
}

void Metal::VertexManager::UnlockUtilityBuffers(u32 vertex_bytes, u32 index_bytes)
{
  ADDSTAT(g_stats.this_frame.bytes_vertex_streamed, vertex_bytes);
  ADDSTAT(g_stats.this_frame.bytes_index_streamed, index_bytes);

  // Commit AND bind the utility buffers for the caller's immediate draws.
  g_state_tracker->SetVerticesAndIndices(m_utility_vertex_buffer, m_utility_index_buffer);
}

void Metal::VertexManager::UploadUniforms()
{
  auto& system = Core::System::GetInstance();
  auto& vertex_shader_manager = system.GetVertexShaderManager();
  auto& geometry_shader_manager = system.GetGeometryShaderManager();
  auto& pixel_shader_manager = system.GetPixelShaderManager();
  g_state_tracker->InvalidateUniforms(vertex_shader_manager.dirty, geometry_shader_manager.dirty,
                                      pixel_shader_manager.dirty);
  vertex_shader_manager.dirty = false;
  geometry_shader_manager.dirty = false;
  pixel_shader_manager.dirty = false;
}
