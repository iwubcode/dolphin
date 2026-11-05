// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "VideoBackends/Metal/MTLUtil.h"
#include "VideoCommon/VertexManagerBase.h"

namespace Metal
{
class VertexManager final : public VertexManagerBase
{
public:
  VertexManager();
  ~VertexManager() override;

  bool Initialize() override;

  void UploadUtilityUniforms(const void* uniforms, u32 uniforms_size) override;
  bool UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                         u32* out_offset) override;
  bool UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format, u32* out_offset,
                         const void* palette_data, u32 palette_size,
                         TexelBufferFormat palette_format, u32* out_palette_offset) override;

protected:
  void ResetBuffer(u32 vertex_stride) override;
  void CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices, u32* out_base_vertex,
                    u32* out_base_index) override;
  void SetBuffer(u32 vertex_stride) override;
  void UploadUniforms() override;

  std::optional<UtilityRegion> LockUtilityBuffers(u32 vertex_bytes, u32 index_bytes,
                                                  u32 vertex_stride) override;
  void UnlockUtilityBuffers(u32 vertex_bytes, u32 index_bytes) override;

private:
  u32 m_vertex_offset;
  u32 m_base_vertex;

  // Cached committed game vertex/index buffer pair, re-applied by SetBuffer.
  // A single cached pair is correct on Metal because deferred rendering is gated
  // OFF (bSupportsDeferredGraphics = false): no snapshot offsets ever outlive a
  // frame, so "the buffers the draw's offsets reference" is always "the last
  // committed pair."
  id<MTLBuffer> m_committed_vertex_buffer;
  id<MTLBuffer> m_committed_index_buffer;

  // Utility vertex arena: dedicated persistent CPU-shared MTLBuffers OUTSIDE the
  // StateTracker upload ring. Overwritten in place; offsets always 0. Sizing:
  // vertex 4 MB, index 1 MB.
  static constexpr u32 UTILITY_VERTEX_BUFFER_SIZE = 4 * 1024 * 1024;
  static constexpr u32 UTILITY_INDEX_BUFFER_SIZE = 1 * 1024 * 1024;
  id<MTLBuffer> m_utility_vertex_buffer;
  id<MTLBuffer> m_utility_index_buffer;
};
}  // namespace Metal
