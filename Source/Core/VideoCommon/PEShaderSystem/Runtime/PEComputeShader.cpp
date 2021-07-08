#include "VideoCommon/PEShaderSystem/Runtime/PEComputeShader.h"

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
bool ComputeShader::CreatePasses(const ShaderConfig& config)
{
  m_passes.clear();
  for (const auto& config_pass : config.m_passes)
  {
    bool should_skip_pass = false;
    if (config_pass.m_dependent_option != "")
    {
      for (const auto& option : config.m_options)
      {
        if (IsUnsatisfiedDependency(option, config_pass.m_dependent_option))
        {
          should_skip_pass = true;
          break;
        }
      }
    }

    if (should_skip_pass)
    {
      continue;
    }
    auto pass = std::make_unique<ComputeShaderPass>();

    pass->m_inputs.reserve(config_pass.m_inputs.size());
    for (const auto& config_input : config_pass.m_inputs)
    {
      auto input = ShaderInput::Create(config_input);
      if (!input)
        return false;

      pass->m_inputs.push_back(std::move(input));
    }

    pass->m_output_scale = config_pass.m_output_scale;
    pass->m_compute_shader = CompileShader(config, *pass, config_pass);
    if (!pass->m_compute_shader)
    {
      config.m_runtime_info->SetError(true);
      return false;
    }

    m_passes.push_back(std::move(pass));
  }

  if (m_passes.empty())
    return false;

  return true;
}

bool ComputeShader::CreateAllPassOutputTextures(u32 width, u32 height, u32 layers,
                                                AbstractTextureFormat format)
{
  std::size_t i = 0;
  for (auto& pass : m_passes)
  {
    auto* native_pass = static_cast<ComputeShaderPass*>(pass.get());
    u32 output_width;
    u32 output_height;
    if (native_pass->m_output_scale < 0.0f)
    {
      const float native_scale = -native_pass->m_output_scale;
      const int native_width = width * EFB_WIDTH / g_renderer->GetTargetWidth();
      const int native_height = height * EFB_HEIGHT / g_renderer->GetTargetHeight();
      output_width = std::max(
          static_cast<u32>(std::round((static_cast<float>(native_width) * native_scale))), 1u);
      output_height = std::max(
          static_cast<u32>(std::round((static_cast<float>(native_height) * native_scale))), 1u);
    }
    else
    {
      output_width = std::max(1u, static_cast<u32>(width * native_pass->m_output_scale));
      output_height = std::max(1u, static_cast<u32>(height * native_pass->m_output_scale));
    }

    native_pass->m_output_texture.reset();
    native_pass->m_output_texture =
        g_renderer->CreateTexture(TextureConfig(output_width, output_height, 1, layers, 1, format,
                                                AbstractTextureFlag_ComputeImage),
                                  fmt::format("Pass {}", i));
    if (!native_pass->m_output_texture)
    {
      PanicAlertFmt("Failed to create texture for PE shader pass");
      return false;
    }
    i++;
  }

  return true;
}

bool ComputeShader::RebuildPipeline(AbstractTextureFormat format, u32 layers)
{
  // Compute shaders don't have any pipelines
  return true;
}

void ComputeShader::Apply(bool skip_final_copy, const ShaderApplyOptions& options,
                          const AbstractTexture* prev_shader_texture, float prev_pass_output_scale)
{
  const auto parse_inputs = [&](const ComputeShaderPass& pass,
                                const AbstractTexture* prev_pass_texture) {
    for (auto&& input : pass.m_inputs)
    {
      switch (input->GetType())
      {
      case InputType::ColorBuffer:
        g_renderer->SetTexture(input->GetTextureUnit(), options.m_source_color_tex);
        break;

      case InputType::DepthBuffer:
        g_renderer->SetTexture(input->GetTextureUnit(), options.m_source_depth_tex);
        break;

      case InputType::ExternalImage:
      case InputType::PassOutput:
        g_renderer->SetTexture(input->GetTextureUnit(), input->GetTexture());
        break;
      case InputType::PreviousPassOutput:
        g_renderer->SetTexture(input->GetTextureUnit(), prev_pass_texture);
        break;
      case InputType::PreviousShaderOutput:
        g_renderer->SetTexture(input->GetTextureUnit(), prev_shader_texture);
        break;
      }

      g_renderer->SetSamplerState(input->GetTextureUnit(), input->GetSamplerState());
    }
  };

  const AbstractTexture* last_pass_texture = prev_shader_texture;
  float last_pass_output_scale = prev_pass_output_scale;
  for (const auto& pass : m_passes)
  {
    auto* native_pass = static_cast<ComputeShaderPass*>(pass.get());
    parse_inputs(*native_pass, last_pass_texture);

    m_builtin_uniforms.Update(options, last_pass_texture, last_pass_output_scale);
    UploadUniformBuffer();

    g_renderer->SetComputeImageTexture(native_pass->m_output_texture.get(), false, true);

    // Should these be exposed?
    const u32 working_group_size = 8;
    g_renderer->DispatchComputeShader(
        native_pass->m_compute_shader.get(),
        native_pass->m_output_texture->GetWidth() / working_group_size,
        native_pass->m_output_texture->GetHeight() / working_group_size, 1);

    last_pass_texture = native_pass->m_output_texture.get();
    last_pass_output_scale = native_pass->m_output_scale;
  }
}

bool ComputeShader::RecompilePasses(const ShaderConfig& config)
{
  for (std::size_t i = 0; i < m_passes.size(); i++)
  {
    auto& pass = m_passes[i];
    auto* native_pass = static_cast<ComputeShaderPass*>(pass.get());
    auto& config_pass = config.m_passes[i];

    native_pass->m_compute_shader = CompileShader(config, *native_pass, config_pass);
    if (!native_pass->m_compute_shader)
    {
      config.m_runtime_info->SetError(true);
      return false;
    }
  }

  return true;
}

std::unique_ptr<AbstractShader>
ComputeShader::CompileShader(const ShaderConfig& config, const ComputeShaderPass& pass,
                             const ShaderConfigPass& config_pass) const
{
  ShaderCode shader_source;
  PrepareUniformHeader(shader_source);
  ShaderHeader(shader_source, pass);

  std::string shader_body;
  File::ReadFileToString(config.m_shader_path, shader_body);
  shader_body = ReplaceAll(shader_body, "\r\n", "\n");
  shader_body = ReplaceAll(shader_body, "{", "{{");
  shader_body = ReplaceAll(shader_body, "}", "}}");

  shader_source.Write(shader_body);
  shader_source.Write("\n");
  ShaderFooter(shader_source, config_pass);
  return g_renderer->CreateShaderFromSource(ShaderStage::Compute, shader_source.GetBuffer(),
                                            fmt::format("{} compute", config.m_name));
}

void ComputeShader::ShaderHeader(ShaderCode& shader_source, const ComputeShaderPass& pass) const
{
  if (g_ActiveConfig.backend_info.api_type == APIType::D3D)
  {
    // Rename main, since we need to set up globals
    shader_source.Write(R"(
#define HLSL 1
#define main real_main

Texture2DArray samp[{0}] : register(t0);
SamplerState samp_ss[{0}] : register(s0);
RWTexture2DArray<float4> output_image : register(u0);

static float3 v_tex0;
static float3 v_tex1;

// Type aliases.
#define mat2 float2x2
#define mat3 float3x3
#define mat4 float4x4
#define mat4x3 float4x3

// function aliases. (see https://anteru.net/blog/2016/mapping-between-HLSL-and-GLSL/)
#define dFdx ddx
#define dFdxCoarse ddx_coarse
#define dFdxFine ddx_fine
#define dFdy ddy
#define dFdyCoarse ddy_coarse
#define dFdyFine ddy_fine
#define interpolateAtCentroid EvaluateAttributeAtCentroid
#define interpolateAtSample EvaluateAttributeAtSample
#define interpolateAtOffset EvaluateAttributeSnapped
#define fract frac
#define mix lerp
#define fma mad

#define GROUP_MEMORY_BARRIER_WITH_SYNC GroupMemoryBarrierWithGroupSync();
#define GROUP_SHARED groupshared

// Wrappers for sampling functions.
float4 SampleInputLocation(int value, float2 location) {{ return samp[value].SampleLevel(samp_ss[value], float3(location, float(u_layer)), 0); }}
void SetOutput(float4 color, int3 pixel_coord) {{ output_image[pixel_coord] = color; }}

int2 SampleInputSize(int value, int lod)
{{
  uint width;
  uint height;
  uint elements;
  uint miplevels;
  samp[value].GetDimensions(lod, width, height, elements, miplevels);
  return int2(width, height);
}}

)",
                        pass.m_inputs.size());
  }
  else
  {
    shader_source.Write(R"(
#define GLSL 1
#define main real_main

// Type aliases.
#define float2x2 mat2
#define float3x3 mat3
#define float4x4 mat4
#define float4x3 mat4x3

// Utility functions.
float saturate(float x) {{ return clamp(x, 0.0f, 1.0f); }}
float2 saturate(float2 x) {{ return clamp(x, float2(0.0f, 0.0f), float2(1.0f, 1.0f)); }}
float3 saturate(float3 x) {{ return clamp(x, float3(0.0f, 0.0f, 0.0f), float3(1.0f, 1.0f, 1.0f)); }}
float4 saturate(float4 x) {{ return clamp(x, float4(0.0f, 0.0f, 0.0f, 0.0f), float4(1.0f, 1.0f, 1.0f, 1.0f)); }}

// Flipped multiplication order because GLSL matrices use column vectors.
float2 mul(float2x2 m, float2 v) {{ return (v * m); }}
float3 mul(float3x3 m, float3 v) {{ return (v * m); }}
float4 mul(float4x3 m, float3 v) {{ return (v * m); }}
float4 mul(float4x4 m, float4 v) {{ return (v * m); }}
float2 mul(float2 v, float2x2 m) {{ return (m * v); }}
float3 mul(float3 v, float3x3 m) {{ return (m * v); }}
float3 mul(float4 v, float4x3 m) {{ return (m * v); }}
float4 mul(float4 v, float4x4 m) {{ return (m * v); }}

float4x3 mul(float4x3 m, float3x3 m2) {{ return (m2 * m); }}

SAMPLER_BINDING(0) uniform sampler2DArray samp[{0}];
IMAGE_BINDING(rgba8, 0) uniform writeonly image2DArray output_image;

#define GROUP_MEMORY_BARRIER_WITH_SYNC memoryBarrierShared(); barrier();
#define GROUP_SHARED shared

// Wrappers for sampling functions.
float4 SampleInputLocation(int value, float2 location) {{ return texture(samp[value], float3(location, float(u_layer))); }}
void SetOutput(float4 color, int3 pixel_coord) {{ imageStore(output_image, pixel_coord, color); }}

ivec3 SampleInputSize(int value, int lod) {{ return textureSize(samp[value], lod); }}
)",
                        pass.m_inputs.size());
  }

  pass.WriteShaderIndices(shader_source);

  for (const auto& option : m_options)
  {
    option->WriteShaderConstants(shader_source);
  }

  shader_source.Write(R"(

float2 GetResolution() {{ return prev_resolution.xy; }}
float2 GetInvResolution() {{ return prev_resolution.zw; }}
float2 GetWindowResolution() {{  return window_resolution.xy; }}
float2 GetInvWindowResolution() {{ return window_resolution.zw; }}

float2 GetPrevResolution() {{ return prev_resolution.xy; }}
float2 GetInvPrevResolution() {{ return prev_resolution.zw; }}
float2 GetPrevRectOrigin() {{ return prev_rect.xy; }}
float2 GetPrevRectSize() {{ return prev_rect.zw; }}

float2 GetSrcResolution() {{ return src_resolution.xy; }}
float2 GetInvSrcResolution() {{ return src_resolution.zw; }}
float2 GetSrcRectOrigin() {{ return src_rect.xy; }}
float2 GetSrcRectSize() {{ return src_rect.zw; }}

float GetLayer() {{ return u_layer; }}
float GetTime() {{ return u_time; }}

#define GetOption(x) (x)
#define OptionEnabled(x) (x)

)");
}

void ComputeShader::ShaderFooter(ShaderCode& shader_source,
                                 const ShaderConfigPass& config_pass) const
{
  if (g_ActiveConfig.backend_info.api_type == APIType::D3D)
  {
    shader_source.Write("#undef main\n");
    shader_source.Write("[numthreads(8, 8, 1)]\n");
    shader_source.Write("void main(uint3 workGroupID : SV_GroupId,\n");
    shader_source.Write("\tuint3 localInvocationID : SV_GroupThreadID,\n");
    shader_source.Write("\tuint3 globalInvocationID : SV_DispatchThreadID)\n");
    shader_source.Write("{{\n");

    if (config_pass.m_entry_point == "main")
    {
      shader_source.Write("\treal_main(workGroupID, localInvocationID, globalInvocationID);\n");
    }
    else if (!config_pass.m_entry_point.empty())
    {
      shader_source.Write("\t{}(workGroupID, localInvocationID, globalInvocationID);\n",
                          config_pass.m_entry_point);
    }
    shader_source.Write("}}\n");
  }
  else
  {
    shader_source.Write("#undef main\n");
    shader_source.Write("layout(local_size_x = 8, local_size_y = 8) in;\n");

    shader_source.Write("void main()\n");
    shader_source.Write("{{\n");

    if (config_pass.m_entry_point == "main")
    {
      shader_source.Write(
          "\treal_main(gl_WorkGroupID, gl_LocalInvocationID, gl_GlobalInvocationID);\n");
    }
    else if (!config_pass.m_entry_point.empty())
    {
      shader_source.Write("\t{}(gl_WorkGroupID, gl_LocalInvocationID, gl_GlobalInvocationID);\n",
                          config_pass.m_entry_point);
    }

    shader_source.Write("}}\n");
  }
}
}  // namespace VideoCommon::PE
