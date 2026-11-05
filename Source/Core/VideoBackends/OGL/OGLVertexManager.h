// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>

#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VertexManagerBase.h"

namespace OGL
{
class StreamBuffer;
class GLVertexFormat : public NativeVertexFormat
{
public:
  GLVertexFormat(const PortableVertexDeclaration& vtx_decl);
  ~GLVertexFormat() override;

  GLuint VAO;

  // Lazily created VAO bound to the utility vertex/index buffers. Used by
  // pipelines whose usage is AbstractPipelineUsage::Utility (ImGui, EFB poke).
  // The main VAO keeps the game ring buffer handles; the two are never mixed.
  GLuint GetUtilityVAO() const;

private:
  void SetupAttributePointers(const PortableVertexDeclaration& vtx_decl, u32 vertex_stride) const;

  // mutable: lazily created on first use (a cache, not observable state).
  mutable GLuint m_utility_vao = 0;
};

// Handles the OpenGL details of drawing lots of vertices quickly.
// Other functionality is moving out.
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

  GLuint GetVertexBufferHandle() const;
  GLuint GetIndexBufferHandle() const;
  GLuint GetUtilityVertexBufferHandle() const;
  GLuint GetUtilityIndexBufferHandle() const;
  GLuint GetScratchVAO() const { return m_scratch_vao; }

protected:
  void ResetBuffer(u32 vertex_stride) override;
  void CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices, u32* out_base_vertex,
                    u32* out_base_index) override;
  void SetBuffer(u32 vertex_stride) override;
  std::optional<UtilityRegion> LockUtilityBuffers(u32 vertex_bytes, u32 index_bytes,
                                                  u32 vertex_stride) override;
  void UnlockUtilityBuffers(u32 vertex_bytes, u32 index_bytes) override;
  void UploadUniforms() override;

private:
  std::unique_ptr<StreamBuffer> m_vertex_buffer;
  std::unique_ptr<StreamBuffer> m_index_buffer;
  std::unique_ptr<StreamBuffer> m_utility_vertex_buffer;
  std::unique_ptr<StreamBuffer> m_utility_index_buffer;
  std::unique_ptr<StreamBuffer> m_texel_buffer;
  std::array<GLuint, NUM_TEXEL_BUFFER_FORMATS> m_texel_buffer_views{};
  // Throwaway VAO used only as a binding container while mapping the utility
  // buffers (array/element bindings are VAO state; GLES3 forbids VAO 0).
  // Never drawn with; its bindings are meaningless by design.
  GLuint m_scratch_vao = 0;
};
}  // namespace OGL
