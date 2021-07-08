#include "VideoCommon/PEShaderSystem/Runtime/PEBaseShader.h"

#include <algorithm>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "VideoCommon/PEShaderSystem/Runtime/PEShaderOption.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/FramebufferShaderGen.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace VideoCommon::PE
{
bool BaseShader::UpdateConfig(const ShaderConfig& config)
{
  const bool update_static_options = config.m_changes != m_last_static_option_change_count;
  const bool update_compile_options =
      config.m_compiletime_changes != m_last_compile_option_change_count;
  if (!update_static_options && !update_compile_options)
    return true;

  m_last_static_option_change_count = config.m_changes;
  m_last_compile_option_change_count = config.m_compiletime_changes;

  if (update_compile_options)
  {
    if (!RecompilePasses(config))
    {
      return false;
    }
  }

  u32 i = 0;
  for (const auto& config_option : config.m_options)
  {
    auto&& option = m_options[i];
    option->Update(config_option);
    i++;
  }

  return true;
}

void BaseShader::CreateOptions(const ShaderConfig& config)
{
  m_last_static_option_change_count = config.m_changes;
  m_last_compile_option_change_count = config.m_compiletime_changes;
  m_options.clear();
  std::size_t buffer_size = m_builtin_uniforms.Size();
  for (const auto& config_option : config.m_options)
  {
    auto option = ShaderOption::Create(config_option);
    buffer_size += option->Size();
    m_options.push_back(std::move(option));
  }
  const auto remainder = buffer_size % 16;

  m_uniform_staging_buffer.resize(buffer_size + remainder * sizeof(float));
}

void BaseShader::UpdatePassInputs(const AbstractTexture* previous_texture)
{
  const auto update_inputs = [this](const BaseShaderPass& pass,
                                    const AbstractTexture* previous_pass_texture) {
    for (auto&& input : pass.m_inputs)
    {
      input->Update(previous_pass_texture, GetPasses());
    }
  };

  if (m_passes.size() == 1)
  {
    update_inputs(*m_passes[0], previous_texture);
  }

  for (std::size_t i = 1; i < m_passes.size(); i++)
  {
    update_inputs(*m_passes[i], m_passes[i - 1]->m_output_texture.get());
  }
}

std::vector<BaseShaderPass*> BaseShader::GetPasses() const
{
  std::vector<BaseShaderPass*> passes;
  passes.reserve(m_passes.size());
  for (const auto& pass : m_passes)
  {
    passes.push_back(pass.get());
  }
  return passes;
}

void BaseShader::UploadUniformBuffer()
{
  u8* buffer = m_uniform_staging_buffer.data();
  std::memset(buffer, 0, m_uniform_staging_buffer.size());
  m_builtin_uniforms.WriteToMemory(buffer);
  for (const auto& option : m_options)
  {
    option->WriteToMemory(buffer);
  }

  g_vertex_manager->UploadUtilityUniforms(
      m_uniform_staging_buffer.data(), static_cast<u32>(buffer - m_uniform_staging_buffer.data()));
}

void BaseShader::PrepareUniformHeader(ShaderCode& shader_source) const
{
  if (g_ActiveConfig.backend_info.api_type == APIType::D3D)
    shader_source.Write("cbuffer PSBlock : register(b0) {{\n");
  else
    shader_source.Write("UBO_BINDING(std140, 1) uniform PSBlock {{\n");

  m_builtin_uniforms.WriteShaderUniforms(shader_source);

  for (const auto& option : m_options)
  {
    option->WriteShaderUniforms(shader_source);
  }
  shader_source.Write("}};\n\n");
}
}  // namespace VideoCommon::PE
