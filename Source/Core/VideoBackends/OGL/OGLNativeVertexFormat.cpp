// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/OGL/OGLGfx.h"
#include "VideoBackends/OGL/OGLVertexManager.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"

#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VertexShaderGen.h"

// Here's some global state. We only use this to keep track of what we've sent to the OpenGL state
// machine.

namespace OGL
{
std::unique_ptr<NativeVertexFormat>
OGLGfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return std::make_unique<GLVertexFormat>(vtx_decl);
}

static inline GLuint VarToGL(ComponentFormat t)
{
  static constexpr Common::EnumMap<GLuint, ComponentFormat::InvalidFloat7> lookup = {
      GL_UNSIGNED_BYTE, GL_BYTE,  GL_UNSIGNED_SHORT, GL_SHORT,
      GL_FLOAT,         GL_FLOAT, GL_FLOAT,          GL_FLOAT,
  };
  return lookup[t];
}

static void SetPointer(ShaderAttrib attrib, u32 stride, const AttributeFormat& format)
{
  if (!format.enable)
    return;

  glEnableVertexAttribArray(static_cast<GLuint>(attrib));
  if (format.integer)
    glVertexAttribIPointer(static_cast<GLuint>(attrib), format.components, VarToGL(format.type),
                           stride, (u8*)nullptr + format.offset);
  else
    glVertexAttribPointer(static_cast<GLuint>(attrib), format.components, VarToGL(format.type),
                          true, stride, (u8*)nullptr + format.offset);
}

GLVertexFormat::GLVertexFormat(const PortableVertexDeclaration& vtx_decl)
    : NativeVertexFormat(vtx_decl)
{
  const u32 vertex_stride = vtx_decl.stride;

  // We will not allow vertex components causing uneven strides.
  if (vertex_stride & 3)
    PanicAlertFmt("Uneven vertex stride: {}", vertex_stride);

  VertexManager* const vm = static_cast<VertexManager*>(g_vertex_manager.get());

  glGenVertexArrays(1, &VAO);
  glBindVertexArray(VAO);

  // the element buffer is bound directly to the vao, so we must it set for every vao
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vm->GetIndexBufferHandle());
  glBindBuffer(GL_ARRAY_BUFFER, vm->GetVertexBufferHandle());

  SetupAttributePointers(vtx_decl, vertex_stride);

  // Other code shouldn't have to worry about its vertex formats being randomly unbound
  ProgramShaderCache::ReBindVertexFormat();
}

void GLVertexFormat::SetupAttributePointers(const PortableVertexDeclaration& vtx_decl,
                                            u32 vertex_stride) const
{
  SetPointer(ShaderAttrib::Position, vertex_stride, vtx_decl.position);

  for (u32 i = 0; i < 3; i++)
    SetPointer(ShaderAttrib::Normal + i, vertex_stride, vtx_decl.normals[i]);

  for (u32 i = 0; i < 2; i++)
    SetPointer(ShaderAttrib::Color0 + i, vertex_stride, vtx_decl.colors[i]);

  for (u32 i = 0; i < 8; i++)
    SetPointer(ShaderAttrib::TexCoord0 + i, vertex_stride, vtx_decl.texcoords[i]);

  SetPointer(ShaderAttrib::PositionMatrix, vertex_stride, vtx_decl.posmtx);
}

GLuint GLVertexFormat::GetUtilityVAO() const
{
  if (m_utility_vao)
    return m_utility_vao;

  VertexManager* const vm = static_cast<VertexManager*>(g_vertex_manager.get());
  const GLuint utility_vbo = vm->GetUtilityVertexBufferHandle();
  const GLuint utility_ebo = vm->GetUtilityIndexBufferHandle();
  ASSERT(utility_vbo != 0 && utility_ebo != 0);

  glGenVertexArrays(1, &m_utility_vao);
  glBindVertexArray(m_utility_vao);

  // The utility VAO captures the utility buffer handles. The utility
  // StreamBuffers keep their GL buffer object for their whole lifetime, so
  // these handles can never go stale.
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, utility_ebo);
  glBindBuffer(GL_ARRAY_BUFFER, utility_vbo);

  SetupAttributePointers(m_decl, static_cast<u32>(m_decl.stride));

  // Other code shouldn't have to worry about its vertex formats being randomly unbound
  ProgramShaderCache::ReBindVertexFormat();
  return m_utility_vao;
}

GLVertexFormat::~GLVertexFormat()
{
  ProgramShaderCache::InvalidateVertexFormatIfBound(VAO);
  glDeleteVertexArrays(1, &VAO);
  if (m_utility_vao)
  {
    ProgramShaderCache::InvalidateVertexFormatIfBound(m_utility_vao);
    glDeleteVertexArrays(1, &m_utility_vao);
  }
}
}  // namespace OGL
