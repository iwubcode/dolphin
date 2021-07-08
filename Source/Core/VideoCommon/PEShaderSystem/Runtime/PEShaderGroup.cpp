// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/PEShaderSystem/Runtime/PEShaderGroup.h"

namespace VideoCommon::PE
{
  void ShaderGroup::Apply(const PEShaderConfigGroup& group, const ShaderApplyOptions& options)
  {
    const bool needs_compile = group.m_changes != m_last_change_count;
    m_last_change_count = group.m_changes;
    if (needs_compile)
    {
      if (!CreateShaders(group))
        return;
    }
	
	if (m_shaders.empty())
		return;

  const u32 dest_rect_width = static_cast<u32>(dest_rect.GetWidth());
  const u32 dest_rect_height = static_cast<u32>(dest_rect.GetHeight());
  if (m_target_width != dest_rect_width || m_target_height != dest_rect_height ||
      m_target_layers != dest_fb->GetLayers() || m_target_format != dest_fb->GetColorFormat())
  {
    const bool rebuild_pipelines = m_target_format != dest_fb->GetColorFormat() || m_target_layers != dest_fb->GetLayers();
    m_target_width = dest_rect_width;
    m_target_height = dest_rect_height;
    m_target_layers = dest_fb->GetLayers();
    m_target_format = dest_fb->GetColorFormat();
    for (const auto& shader : m_shaders)
    {
      if (!shader.PrepareOutputTextures(dest_rect_width, dest_rect_height, dest_fb->GetLayers(), dest_fb->GetColorFormat()))
        return;
      shader.LinkPasses();
      if (rebuild_pipelines)
        shader.RebuildPipeline();
    }
  }
	
	for (std::size_t i = 0; i < m_shaders.size() - 1; i++)
	{
		const bool skip_final_copy = false;
		m_shaders[i]->Apply(skip_final_copy, options);
	}
	
	const auto& last_pass = group.m_shaders[group.m_shader_order.back()].m_passes.back();
	const bool last_pass_scaled = last_pass.output_scale != 1.0;
	const bool last_pass_uses_color_buffer = last_pass??;
	
	// Determine whether we can skip the final copy by writing directly to the output texture, if the
  // last pass is not scaled, and the target isn't multisampled.
  const bool skip_final_copy = !last_pass_scaled && !last_pass_uses_color_buffer &&
                               options.dest_fb->GetColorAttachment() != options.source_color_tex &&
                               options.dest_fb->GetSamples() == 1;
	
	const auto last_index = m_shaders.size() - 1;
	m_shaders[last_index]->Apply(skip_final_copy, options);
  }
  
  void ShaderGroup::CreateShaders(const PEShaderConfigGroup& group)
  {
	  m_shaders.clear();
	  for (const u32& shader_index : group.m_shader_order)
	  {
		  const auto& config_shader = group.m_shaders[shader_index];
		  if (!config_shader.m_enabled)
			  continue;
		  Shader shader(config_shader);
		  if (!shader.CreatePasses())
		  {
			  m_shaders.clear();
			  return false;
		  }
		  shader.PrepareUniformBuffer();
		  m_shaders.push_back(std::move(shader));
	  }
	  
	  return true;
  }
}