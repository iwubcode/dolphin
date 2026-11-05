// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/OGL/OGLVertexManager.h"

#include <memory>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "Common/GL/GLUtil.h"

#include "VideoBackends/OGL/OGLGfx.h"
#include "VideoBackends/OGL/OGLStreamBuffer.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"

#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
// Utility arena sizes. The utility arena may only ever recycle itself, so
// these are sized for the largest transient consumer (editor ImGui command
// lists), not for game geometry.
static constexpr u32 UTILITY_VERTEX_BUFFER_SIZE = 4 * 1024 * 1024;
static constexpr u32 UTILITY_INDEX_BUFFER_SIZE = 1 * 1024 * 1024;

static void CheckBufferBinding()
{
  // Always request the current game format. A utility VAO may be bound
  // (IsValidVertexFormatBound would call it "valid"), and the game path must
  // never map or draw through a utility VAO's buffer bindings. The s_last_VAO
  // dedup inside BindVertexFormat makes the common case a no-op.
  ProgramShaderCache::BindVertexFormat(
      static_cast<GLVertexFormat*>(VertexLoaderManager::GetCurrentVertexFormat()));

  // The VAO binding was just mutated outside SetPipeline; force the next
  // SetPipeline to re-apply its pipeline's VAO even if the pipeline object
  // is unchanged.
  GetOGLGfx()->InvalidatePipelineBinding();
}

VertexManager::VertexManager() = default;

VertexManager::~VertexManager()
{
  if (g_backend_info.bSupportsPaletteConversion)
  {
    glDeleteTextures(static_cast<GLsizei>(m_texel_buffer_views.size()),
                     m_texel_buffer_views.data());
  }

  // VAO must be found when destroying the index buffer.
  CheckBufferBinding();
  m_texel_buffer.reset();
  m_utility_index_buffer.reset();
  m_utility_vertex_buffer.reset();
  m_index_buffer.reset();
  m_vertex_buffer.reset();
  if (m_scratch_vao)
    glDeleteVertexArrays(1, &m_scratch_vao);
}

bool VertexManager::Initialize()
{
  if (!VertexManagerBase::Initialize())
    return false;

  m_vertex_buffer = StreamBuffer::Create(GL_ARRAY_BUFFER, VERTEX_STREAM_BUFFER_SIZE);
  m_index_buffer = StreamBuffer::Create(GL_ELEMENT_ARRAY_BUFFER, INDEX_STREAM_BUFFER_SIZE);
  if (g_ActiveConfig.UseVSForLinePointExpand() || g_backend_info.bSupportsDynamicVertexLoader)
  {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_vertex_buffer->GetGLBufferId());
  }

  // Utility vertex arena: dedicated buffers for transient draws (ImGui, EFB
  // poke) that must never touch the game ring. The scratch VAO is a binding
  // container only (GLES3 forbids binding VAO 0); it is never drawn with.
  m_utility_vertex_buffer = StreamBuffer::Create(GL_ARRAY_BUFFER, UTILITY_VERTEX_BUFFER_SIZE);
  m_utility_index_buffer = StreamBuffer::Create(GL_ELEMENT_ARRAY_BUFFER, UTILITY_INDEX_BUFFER_SIZE);
  glGenVertexArrays(1, &m_scratch_vao);

  if (g_backend_info.bSupportsPaletteConversion)
  {
    // The minimum MAX_TEXTURE_BUFFER_SIZE that the spec mandates is 65KB, we are asking for a 1MB
    // buffer here. This buffer is also used as storage for undecoded textures when compute shader
    // texture decoding is enabled, in which case the requested size is 32MB.
    GLint max_buffer_size;
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &max_buffer_size);
    m_texel_buffer = StreamBuffer::Create(
        GL_TEXTURE_BUFFER, std::min(max_buffer_size, static_cast<GLint>(TEXEL_STREAM_BUFFER_SIZE)));

    // Allocate texture views backed by buffer.
    static constexpr std::array<std::pair<TexelBufferFormat, GLenum>, NUM_TEXEL_BUFFER_FORMATS>
        format_mapping = {{
            {TEXEL_BUFFER_FORMAT_R8_UINT, GL_R8UI},
            {TEXEL_BUFFER_FORMAT_R16_UINT, GL_R16UI},
            {TEXEL_BUFFER_FORMAT_RGBA8_UINT, GL_RGBA8UI},
            {TEXEL_BUFFER_FORMAT_R32G32_UINT, GL_RG32UI},
        }};
    glGenTextures(static_cast<GLsizei>(m_texel_buffer_views.size()), m_texel_buffer_views.data());
    glActiveTexture(GL_MUTABLE_TEXTURE_INDEX);
    for (const auto& it : format_mapping)
    {
      glBindTexture(GL_TEXTURE_BUFFER, m_texel_buffer_views[it.first]);
      glTexBuffer(GL_TEXTURE_BUFFER, it.second, m_texel_buffer->GetGLBufferId());
    }
  }

  return true;
}

void VertexManager::UploadUtilityUniforms(const void* uniforms, u32 uniforms_size)
{
  InvalidateConstants();
  ProgramShaderCache::UploadConstants(uniforms, uniforms_size);
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset)
{
  if (data_size > m_texel_buffer->GetSize())
    return false;

  const u32 elem_size = GetTexelBufferElementSize(format);
  const auto dst = m_texel_buffer->Map(data_size, elem_size);
  std::memcpy(dst.first, data, data_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, data_size);
  *out_offset = dst.second / elem_size;
  m_texel_buffer->Unmap(data_size);

  // Bind the correct view to the texel buffer slot.
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_BUFFER, m_texel_buffer_views[static_cast<u32>(format)]);
  GetOGLGfx()->InvalidateTextureBinding(0);
  return true;
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset, const void* palette_data, u32 palette_size,
                                      TexelBufferFormat palette_format, u32* out_palette_offset)
{
  const u32 elem_size = GetTexelBufferElementSize(format);
  const u32 palette_elem_size = GetTexelBufferElementSize(palette_format);
  const u32 reserve_size = data_size + palette_size + palette_elem_size;
  if (reserve_size > m_texel_buffer->GetSize())
    return false;

  const auto dst = m_texel_buffer->Map(reserve_size, elem_size);
  const u32 palette_byte_offset = Common::AlignUp(data_size, palette_elem_size);
  std::memcpy(dst.first, data, data_size);
  std::memcpy(dst.first + palette_byte_offset, palette_data, palette_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, palette_byte_offset + palette_size);
  *out_offset = dst.second / elem_size;
  *out_palette_offset = (dst.second + palette_byte_offset) / palette_elem_size;
  m_texel_buffer->Unmap(palette_byte_offset + palette_size);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_BUFFER, m_texel_buffer_views[static_cast<u32>(format)]);
  GetOGLGfx()->InvalidateTextureBinding(0);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_BUFFER, m_texel_buffer_views[static_cast<u32>(palette_format)]);
  GetOGLGfx()->InvalidateTextureBinding(1);

  return true;
}

GLuint VertexManager::GetVertexBufferHandle() const
{
  return m_vertex_buffer->m_buffer;
}

GLuint VertexManager::GetIndexBufferHandle() const
{
  return m_index_buffer->m_buffer;
}

GLuint VertexManager::GetUtilityVertexBufferHandle() const
{
  return m_utility_vertex_buffer->m_buffer;
}

GLuint VertexManager::GetUtilityIndexBufferHandle() const
{
  return m_utility_index_buffer->m_buffer;
}

void VertexManager::ResetBuffer(u32 vertex_stride)
{
  CheckBufferBinding();

  auto buffer = m_vertex_buffer->Map(MAXVBUFFERSIZE, vertex_stride);
  m_cur_buffer_pointer = m_base_buffer_pointer = buffer.first;
  m_end_buffer_pointer = buffer.first + MAXVBUFFERSIZE;
  m_last_reset_pointer = m_cur_buffer_pointer;

  buffer = m_index_buffer->Map(MAXIBUFFERSIZE * sizeof(u16));
  m_index_generator.Start(reinterpret_cast<u16*>(buffer.first));
}

void VertexManager::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                 u32* out_base_vertex, u32* out_base_index)
{
  u32 vertex_data_size = num_vertices * vertex_stride;
  u32 index_data_size = num_indices * sizeof(u16);

  *out_base_vertex = vertex_stride > 0 ? (m_vertex_buffer->GetCurrentOffset() / vertex_stride) : 0;
  *out_base_index = m_index_buffer->GetCurrentOffset() / sizeof(u16);

  // NOTE: the vertex/index buffer bind moved to SetBuffer (the single
  // game-draw bind site). CommitBuffer no longer binds.
  m_vertex_buffer->Unmap(vertex_data_size);
  m_index_buffer->Unmap(index_data_size);

  ADDSTAT(g_stats.this_frame.bytes_vertex_streamed, vertex_data_size);
  ADDSTAT(g_stats.this_frame.bytes_index_streamed, index_data_size);
}

void VertexManager::SetBuffer(u32)
{
  // The single game-draw bind site. Re-establishes the game VAO for every
  // draw, which is what makes paused replay correct: after a utility upload
  // binds a utility VAO, the next game draw rebinds the game one.
  CheckBufferBinding();
}

std::optional<UtilityRegion> VertexManager::LockUtilityBuffers(u32 vertex_bytes, u32 index_bytes,
                                                               u32 vertex_stride)
{
  // Reserve EXACT bytes (never MAXVBUFFERSIZE) — the utility arena can only
  // ever recycle itself, never sweep a game region.
  if (vertex_bytes > m_utility_vertex_buffer->GetSize())
    return std::nullopt;
  if (index_bytes > m_utility_index_buffer->GetSize())
    return std::nullopt;

  // glMapBufferRange acts on the buffer bound to the target, and the
  // ARRAY/ELEMENT bindings are VAO state. All binds/maps happen under the
  // scratch VAO so no real VAO's bindings are ever mutated.
  glBindVertexArray(m_scratch_vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_utility_vertex_buffer->GetGLBufferId());
  const auto [vertex_pointer, vertex_offset] =
      m_utility_vertex_buffer->Map(vertex_bytes, vertex_stride > 0 ? vertex_stride : 1u);

  u16* index_pointer = nullptr;
  u32 index_offset = 0;
  if (index_bytes > 0)
  {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_utility_index_buffer->GetGLBufferId());
    const auto [iptr, ioff] = m_utility_index_buffer->Map(index_bytes);
    index_pointer = reinterpret_cast<u16*>(iptr);
    index_offset = ioff;
  }

  return UtilityRegion{
      .vertex_pointer = vertex_pointer,
      .index_pointer = index_pointer,
      .vertex_offset = vertex_offset,
      .index_offset = index_offset,
  };
}

void VertexManager::UnlockUtilityBuffers(u32 vertex_bytes, u32 index_bytes)
{
  // Commit the utility buffers for the caller's immediate draws. The VAO
  // select happens in SetPipeline (usage == Utility); here we only restore
  // the pre-lock VAO so the scratch binding doesn't leak.
  m_utility_vertex_buffer->Unmap(vertex_bytes);
  if (index_bytes > 0)
    m_utility_index_buffer->Unmap(index_bytes);

  ADDSTAT(g_stats.this_frame.bytes_vertex_streamed, static_cast<int>(vertex_bytes));
  ADDSTAT(g_stats.this_frame.bytes_index_streamed, static_cast<int>(index_bytes));

  ProgramShaderCache::ReBindVertexFormat();
}

void VertexManager::UploadUniforms()
{
  ProgramShaderCache::UploadConstants();
}
}  // namespace OGL
