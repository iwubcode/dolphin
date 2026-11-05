// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>

#include "VideoBackends/D3D12/D3D12StreamBuffer.h"
#include "VideoBackends/D3D12/DescriptorHeapManager.h"
#include "VideoCommon/VertexManagerBase.h"

namespace DX12
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

  void UpdateVertexShaderConstants();
  void UpdateGeometryShaderConstants();
  void UpdatePixelShaderConstants();
  void UpdateCustomShaderConstants();

  // Allocates storage in the uniform buffer of the specified size. If this storage cannot be
  // allocated immediately, the current command buffer will be submitted and all stage's
  // constants will be re-uploaded. false will be returned in this case, otherwise true.
  bool ReserveConstantStorage();
  void UploadAllConstants();

  StreamBuffer m_vertex_stream_buffer;
  StreamBuffer m_index_stream_buffer;
  StreamBuffer m_uniform_stream_buffer;
  StreamBuffer m_texel_stream_buffer;
  std::array<DescriptorHandle, NUM_TEXEL_BUFFER_FORMATS> m_texel_buffer_views = {};
  DescriptorHandle m_vertex_srv = {};

  // Utility vertex arena: small dedicated stream buffers for transient draws
  // (ImGui, EFB poke). Never recorded, never replayed. Sizing: vertex 4 MB,
  // index 1 MB.
  static constexpr u32 UTILITY_VERTEX_BUFFER_SIZE = 4 * 1024 * 1024;
  static constexpr u32 UTILITY_INDEX_BUFFER_SIZE = 1 * 1024 * 1024;
  StreamBuffer m_utility_vertex_stream_buffer;
  StreamBuffer m_utility_index_stream_buffer;
  DescriptorHandle m_utility_vertex_srv = {};

  // Stride of the vertices in the current utility lock (set in LockUtilityBuffers,
  // consumed by UnlockUtilityBuffers when binding the vertex buffer).
  u32 m_utility_vertex_stride = 0;
};

}  // namespace DX12
