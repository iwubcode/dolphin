#include "VideoCommon/PEShaderSystem/Runtime/PETriggerPointManager.h"

#include <algorithm>

#include "Common/VariantUtil.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GraphicsTriggerManager.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/TextureInfo.h"
#include "VideoCommon/VideoConfig.h"

namespace VideoCommon::PE
{
void TriggerPointManager::UpdateConfig(const TriggerConfig& config,
                                       const GraphicsTriggerManager& manager)
{
  const bool reset = config.m_changes != m_trigger_changes;
  if (reset || m_post_group == nullptr)
  {
    m_trigger_name_to_group.clear();
    m_efb_shadergroups.clear();
    m_texture_shadergroups.clear();
    m_draw_call_2d_shadergroups.clear();
    m_draw_call_3d_shadergroups.clear();

    m_post_group = &m_trigger_name_to_group[Constants::default_trigger];

    m_trigger_changes = config.m_changes;

    for (const auto& [trigger_name, shader_group_config] : config.m_trigger_name_to_shader_groups)
    {
      auto& shader_group = m_trigger_name_to_group[trigger_name];
      const GraphicsTrigger* trigger = manager.GetTrigger(trigger_name);
      if (!trigger)
        continue;

      std::visit(overloaded{[&](const EFBGraphicsTrigger& t) {
                              m_efb_shadergroups.push_back({false, t, &shader_group});
                            },
                            [&](const TextureLoadGraphicsTrigger& t) {
                              m_texture_shadergroups.push_back({false, t, &shader_group});
                            },
                            [&](const DrawCall2DGraphicsTrigger& t) {
                              m_draw_call_2d_shadergroups.push_back({false, t, &shader_group});
                            },
                            [&](const DrawCall3DGraphicsTrigger& t) {
                              m_draw_call_3d_shadergroups.push_back({false, t, &shader_group});
                            },
                            [&](const PostGraphicsTrigger&) {}},
                 *trigger);
    }
  }

  for (const auto& [trigger_name, shader_group_config] : config.m_trigger_name_to_shader_groups)
  {
    auto& shader_group = m_trigger_name_to_group[trigger_name];
    shader_group.UpdateConfig(shader_group_config);
  }
}

void TriggerPointManager::OnEFB(const MathUtil::Rectangle<int>& srcRect, TextureFormat format)
{
  const u32 width = srcRect.GetWidth();
  const u32 height = srcRect.GetHeight();

  VideoCommon::PE::TriggerParameters parameters;
  const auto scaled_src_rect = g_renderer->ConvertEFBRectangle(srcRect);
  const auto framebuffer_rect = g_renderer->ConvertFramebufferRectangle(
      scaled_src_rect, g_framebuffer_manager->GetEFBFramebuffer());
  parameters.m_dest_fb = g_framebuffer_manager->GetEFBFramebuffer();
  parameters.m_dest_rect = g_framebuffer_manager->GetEFBFramebuffer()->GetRect();
  parameters.m_source_color_tex = g_framebuffer_manager->ResolveEFBColorTexture(framebuffer_rect);
  parameters.m_source_depth_tex = g_framebuffer_manager->ResolveEFBDepthTexture(framebuffer_rect);
  parameters.m_source_layer = 0;
  parameters.m_source_rect = srcRect;
  const auto options = OptionsFromParameters(parameters);
  for (auto& [seen, trigger, shader_group] : m_efb_shadergroups)
  {
    if (seen)
    {
      continue;
    }

    const bool format_matches = (trigger.format_operation == MultiGenericOperation::Exact &&
                                 !trigger.formats.empty() && trigger.formats[0] == format) ||
                                (trigger.format_operation == MultiGenericOperation::OneOf &&
                                 std::any_of(trigger.formats.begin(), trigger.formats.end(),
                                             [&](TextureFormat f) { return f == format; })) ||
                                trigger.format_operation == MultiGenericOperation::Any;
    const bool width_matches =
        (trigger.width_operation == NumericOperation::Exact && trigger.width == width) ||
        (trigger.width_operation == NumericOperation::Greater && width > trigger.width) ||
        (trigger.width_operation == NumericOperation::Greater_Equal && width >= trigger.width) ||
        (trigger.width_operation == NumericOperation::Less && width < trigger.width) ||
        (trigger.width_operation == NumericOperation::Less_Equal && width <= trigger.width) ||
        trigger.width_operation == NumericOperation::Any;
    const bool height_matches =
        (trigger.height_operation == NumericOperation::Exact && trigger.height == height) ||
        (trigger.height_operation == NumericOperation::Greater && height > trigger.height) ||
        (trigger.height_operation == NumericOperation::Greater_Equal && height >= trigger.height) ||
        (trigger.height_operation == NumericOperation::Less && height < trigger.height) ||
        (trigger.height_operation == NumericOperation::Less_Equal && height <= trigger.height) ||
        trigger.height_operation == NumericOperation::Any;

    if (format_matches && width_matches && height_matches)
    {
      seen = true;
      shader_group->Apply(options);
    }
  }
}

void TriggerPointManager::OnTextureLoad(const TriggerParameters&)
{
}

void TriggerPointManager::OnDraw(ProjectionType type, const std::vector<TextureInfo>& textures)
{
  int scissor_x_off = bpmem.scissorOffset.x * 2;
  int scissor_y_off = bpmem.scissorOffset.y * 2;
  float x = g_renderer->EFBToScaledXf(xfmem.viewport.xOrig - xfmem.viewport.wd - scissor_x_off);
  float y = g_renderer->EFBToScaledYf(xfmem.viewport.yOrig + xfmem.viewport.ht - scissor_y_off);

  float width = g_renderer->EFBToScaledXf(2.0f * xfmem.viewport.wd);
  float height = g_renderer->EFBToScaledYf(-2.0f * xfmem.viewport.ht);
  if (width < 0.f)
  {
    x += width;
    width *= -1;
  }
  if (height < 0.f)
  {
    y += height;
    height *= -1;
  }

  // Clamp to size if oversized not supported. Required for D3D.
  if (!g_ActiveConfig.backend_info.bSupportsOversizedViewports)
  {
    const float max_width = static_cast<float>(g_renderer->GetCurrentFramebuffer()->GetWidth());
    const float max_height = static_cast<float>(g_renderer->GetCurrentFramebuffer()->GetHeight());
    x = std::clamp(x, 0.0f, max_width - 1.0f);
    y = std::clamp(y, 0.0f, max_height - 1.0f);
    width = std::clamp(width, 1.0f, max_width - x);
    height = std::clamp(height, 1.0f, max_height - y);
  }

  // Lower-left flip.
  if (g_ActiveConfig.backend_info.bUsesLowerLeftOrigin)
    y = static_cast<float>(g_renderer->GetCurrentFramebuffer()->GetHeight()) - y - height;

  MathUtil::Rectangle<int> srcRect;
  srcRect.left = static_cast<int>(x);
  srcRect.top = static_cast<int>(y);
  srcRect.right = srcRect.left + static_cast<int>(width);
  srcRect.bottom = srcRect.top + static_cast<int>(height);

  VideoCommon::PE::TriggerParameters parameters;
  parameters.m_dest_fb = g_framebuffer_manager->GetEFBFramebuffer();
  parameters.m_dest_rect = g_framebuffer_manager->GetEFBFramebuffer()->GetRect();
  parameters.m_source_color_tex = g_framebuffer_manager->GetEFBColorTexture();
  parameters.m_source_depth_tex = g_framebuffer_manager->GetEFBDepthTexture();
  parameters.m_source_layer = 0;
  parameters.m_source_rect = srcRect;
  const auto options = OptionsFromParameters(parameters);
  if (type == ProjectionType::Orthographic)
  {
    for (auto& [seen, trigger, shader_group] : m_draw_call_2d_shadergroups)
    {
      if (seen)
      {
        continue;
      }

      for (const auto& texture : textures)
      {
        const bool format_matches =
            (trigger.format_operation == MultiGenericOperation::Exact && !trigger.formats.empty() &&
             trigger.formats[0] == texture.GetTextureFormat()) ||
            (trigger.format_operation == MultiGenericOperation::OneOf &&
             std::any_of(trigger.formats.begin(), trigger.formats.end(),
                         [&](TextureFormat f) { return f == texture.GetTextureFormat(); })) ||
            trigger.format_operation == MultiGenericOperation::Any;
        const bool width_matches = (trigger.width_operation == NumericOperation::Exact &&
                                    trigger.width == texture.GetRawWidth()) ||
                                   (trigger.width_operation == NumericOperation::Greater &&
                                    texture.GetRawWidth() > trigger.width) ||
                                   (trigger.width_operation == NumericOperation::Greater_Equal &&
                                    texture.GetRawWidth() >= trigger.width) ||
                                   (trigger.width_operation == NumericOperation::Less &&
                                    texture.GetRawWidth() < trigger.width) ||
                                   (trigger.width_operation == NumericOperation::Less_Equal &&
                                    texture.GetRawWidth() <= trigger.width) ||
                                   trigger.width_operation == NumericOperation::Any;
        const bool height_matches = (trigger.height_operation == NumericOperation::Exact &&
                                     trigger.height == texture.GetRawHeight()) ||
                                    (trigger.height_operation == NumericOperation::Greater &&
                                     texture.GetRawHeight() > trigger.height) ||
                                    (trigger.height_operation == NumericOperation::Greater_Equal &&
                                     texture.GetRawHeight() >= trigger.height) ||
                                    (trigger.height_operation == NumericOperation::Less &&
                                     texture.GetRawHeight() < trigger.height) ||
                                    (trigger.height_operation == NumericOperation::Less_Equal &&
                                     texture.GetRawHeight() <= trigger.height) ||
                                    trigger.height_operation == NumericOperation::Any;

        if (format_matches && width_matches && height_matches)
        {
          seen = true;
          shader_group->Apply(options);
          break;
        }
      }
    }
  }
  else
  {
    for (auto& [seen, trigger, shader_group] : m_draw_call_3d_shadergroups)
    {
      if (seen)
      {
        continue;
      }
    }
  }
}

void TriggerPointManager::OnPost(const TriggerParameters& trigger_parameters)
{
  const auto options = OptionsFromParameters(trigger_parameters);
  m_post_group->Apply(options);
}

void TriggerPointManager::SetDepthNearFar(float depth_near, float depth_far)
{
  m_depth_near = depth_near;
  m_depth_far = depth_far;
}

void TriggerPointManager::Start()
{
  m_timer.Start();
  ShaderGroup::Initialize();
}

void TriggerPointManager::Stop()
{
  m_timer.Stop();
  ShaderGroup::Shutdown();
}

void TriggerPointManager::ResetFrame()
{
  for (auto& [seen, trigger, shader_group] : m_efb_shadergroups)
  {
    seen = false;
  }

  for (auto& [seen, trigger, shader_group] : m_draw_call_2d_shadergroups)
  {
    seen = false;
  }

  for (auto& [seen, trigger, shader_group] : m_draw_call_3d_shadergroups)
  {
    seen = false;
  }
}

ShaderApplyOptions
TriggerPointManager::OptionsFromParameters(const TriggerParameters& trigger_parameters)
{
  ShaderApplyOptions options;
  options.m_dest_fb = trigger_parameters.m_dest_fb;
  options.m_dest_rect = trigger_parameters.m_dest_rect;
  options.m_source_color_tex = trigger_parameters.m_source_color_tex;
  options.m_source_depth_tex = trigger_parameters.m_source_depth_tex;

  options.m_source_rect = trigger_parameters.m_source_rect;
  options.m_source_layer = trigger_parameters.m_source_layer;
  options.m_time_elapsed = m_timer.GetTimeElapsed();
  options.m_depth_near = m_depth_near;
  options.m_depth_far = m_depth_far;
  return options;
}
}  // namespace VideoCommon::PE
