#include "VideoCommon/PEShaderSystem/Runtime/PEShader.h"

#include "VideoCommon/PEShaderSystem/Runtime/PEShaderOption.h"

namespace VideoCommon::PE
{
bool Shader::CreateOptions(const ShaderConfig& config)
{
  m_options.clear();
  std::size_t buffer_size;
  for (const auto& config_option : config.m_options)
  {
	  auto option = ShaderOption::Create(config_option);
	  buffer_size += option->Size();
	  m_options.push_back(std::move(option));
  }
  
  m_uniform_staging_buffer.resize(buffer_size);
}

bool Shader::CreatePasses(const ShaderConfig& config)
{
  if (g_ActiveConfig.backend_info.bSupportsGeometryShaders && !m_geometry_shader)
  {
    m_geometry_shader = CompileGeometryShader();
    if (!m_geometry_shader)
      return false;
  }

  std::shared_ptr<AbstractShader> vertex_shader = CompileVertexShader(config);
  if (!vertex_shader)
  {
    config.m_runtime_info->SetError(true);
    return false;
  }

  m_passes.clear();
  for (const auto& config_pass : config.m_passes)
  {
    if (!config_pass.dependent_option.empty())
    {
      // TODO:
    }

    ShaderPass pass;
	pass.m_output_scale = config_pass.output_scale;
	pass.vertex_shader = vertex_shader;
	pass.pixel_shader = CompilePixelShader(...);
    if (!pass.pixel_shader)
    {
      config.m_runtime_info->SetError(true);
      return false;
    }

    pass.inputs.reserve(pass.inputs.size());
    for (const auto& input : pass.inputs)
    {
      InputBinding binding;
      if (!CreateInputBinding(shader_index, input, &binding))
        return false;

      pass.inputs.push_back(std::move(binding));
    }

    m_passes.push_back(std::move(pass));
  }
  return true;
}

bool Shader::PrepareOutputTextures(u32 width, u32 height, u32 layers, AbstractTextureFormat format)
{
  for (ShaderPass& pass : m_passes)
  {
    u32 output_width;
    u32 output_height;
    if (pass.output_scale < 0.0f)
    {
      const float native_scale = -pass.output_scale;
      const int native_width = width * EFB_WIDTH / g_renderer->GetTargetWidth();
      const int native_height = height * EFB_HEIGHT / g_renderer->GetTargetHeight();
      output_width =
          std::max(static_cast<u32>(std::round((static_cast<float>(native_width) * native_scale))), 1u);
      output_height =
          std::max(static_cast<u32>(std::round((static_cast<float>(native_height) * native_scale))), 1u);
    }
    else
    {
      output_width = std::max(1u, static_cast<u32>(width * pass.output_scale));
      output_height = std::max(1u, static_cast<u32>(height * pass.output_scale));
    }

    pass.output_framebuffer.reset();
    pass.output_texture.reset();
    pass.output_texture = g_renderer->CreateTexture(TextureConfig(
        output_width, output_height, 1, layers, 1, format, AbstractTextureFlag_RenderTarget));
    if (!pass.output_texture)
      return false;
    pass.output_framebuffer = g_renderer->CreateFramebuffer(pass.output_texture.get(), nullptr);
    if (!pass.output_framebuffer)
      return false;
  }
  
  return true;
}

void Shader::Apply(bool skip_final_copy, const ShaderApplyOptions& options)
{
  for (std::size_t i = 0; i < m_passes.size() - 1; i++)
  {
    const auto& pass = m_passes[i];
	const auto output_fb = pass.output_framebuffer.get();
	const auto output_rect = pass.output_framebuffer->GetRect();
	g_renderer->SetAndDiscardFramebuffer(output_fb);

	g_renderer->SetPipeline(pass.pipeline.get());
	g_renderer->SetViewportAndScissor(
	g_renderer->ConvertFramebufferRectangle(output_rect, output_fb));
	
	// TODO: Pass inputs
	
	UploadUniformBuffer();
	
	g_renderer->Draw(0, 3);
	output_fb->GetColorAttachment()->FinishedRendering();
  }
  
  const auto& pass = m_passes[m_passes.size() - 1];
	if (skip_final_copy)
	{
		g_renderer->SetFramebuffer(options.dest_fb);
		g_renderer->SetPipeline(pass.pipeline.get());
		g_renderer->SetViewportAndScissor(
			g_renderer->ConvertFramebufferRectangle(options.dest_rect, options.dest_fb));
		
		// TODO: Pass inputs
		
		UploadUniformBuffer();
	
		g_renderer->Draw(0, 3);
	}
	else
	{
		g_renderer->SetPipeline(pass.pipeline.get());
		g_renderer->SetViewportAndScissor(
		g_renderer->ConvertFramebufferRectangle(pass.output_framebuffer->GetRect(), pass.output_framebuffer));
		
		// TODO: Pass inputs
		
		UploadUniformBuffer();
	
		g_renderer->Draw(0, 3);
		pass.output_framebuffer->GetColorAttachment()->FinishedRendering();
		
		g_renderer->ScaleTexture(options.dest_fb, options.dest_rect, pass.output_texture.get(), pass.output_framebuffer->GetRect());
	}
}

std::unique_ptr<AbstractShader> Shader::CompileGeometryShader() const
{
  std::string source = FramebufferShaderGen::GeneratePassthroughGeometryShader(2, 0);
  return g_renderer->CreateShaderFromSource(ShaderStage::Geometry, std::move(source));
}

void Shader::PrepareUniformHeader(fmt::memory_buffer& shader_source) const
{
  if (g_ActiveConfig.backend_info.api_type == APIType::D3D)
    fmt::format_to(shader_source, "cbuffer PSBlock : register(b0) {\n");
  else
    fmt::format_to(shader_source, "UBO_BINDING(std140, 1) uniform PSBlock {\n");

  for (const auto& option : m_options)
  {
	  option.WriteToShaderSource(shader_source);
  }
  fmt::format_to(shader_source, "}\n");
}

std::shared_ptr<AbstractShader> Shader::CompileVertexShader(const ShaderConfig& config) const
{
  fmt::memory_buffer shader_source;
  PrepareUniformHeader(shader_source);
  
  std::unique_ptr<AbstractShader> vs =
      g_renderer->CreateShaderFromSource(ShaderStage::Vertex, fmt::to_string(shader_source));
  if (!vs)
  {
    return nullptr;
  }
  
  // convert from unique_ptr to shared_ptr, as many passes share one VS
  return std::shared_ptr<AbstractShader>(vs.release());
}