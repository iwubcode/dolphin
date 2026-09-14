// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModGraphicsHandler.h"

#include "Core/System.h"

#include "VideoCommon/GraphicsModSystem/Runtime/FBInfo.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModBackend.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"

#include "VideoCommon/TextureCacheBase.h"

GraphicsModGraphicsHandler::GraphicsModGraphicsHandler(FramebufferManager* frame_buffer_manager,
                                                       bool attempt_copy_efb_to_ram)
    : m_decorated_handler(frame_buffer_manager, attempt_copy_efb_to_ram)
{
}

RcTcacheEntry GraphicsModGraphicsHandler::OnFramebufferCopy(
    IFramebufferRenderer* renderer, PixelShaderManager& pixel_shader_manager,
    EmulatedGraphics::FramebufferCopyResolvedData resolved_data)
{
  auto& system = Core::System::GetInstance();
  auto& mod_manager = system.GetGraphicsModManager();

  FBInfo info;
  info.m_width = resolved_data.width;
  info.m_height = resolved_data.height;
  info.m_texture_format = TexDecoder_GetEFBCopyBaseFormat(resolved_data.efbcopy_format);

  if (!resolved_data.is_xfb)
  {
    bool skip = false;
    u32 scaled_width;
    u32 scaled_height;
    GraphicsModActionData::PreEFB efb{resolved_data.width, resolved_data.height, &skip,
                                      &scaled_width, &scaled_height};
    mod_manager.GetBackend().OnEFBStarted(info, &efb);
    if (skip == true)
    {
      renderer->UninitializeFramebufferMemory(resolved_data);
      return nullptr;
    }
  }
  else
  {
    mod_manager.GetBackend().OnXFBStarted(info);
  }

  auto render_target =
      m_decorated_handler.OnFramebufferCopy(renderer, pixel_shader_manager, resolved_data);

  if (!render_target)
    return nullptr;

  GraphicsModSystem::TextureRef texture;
  texture.entry = render_target;

  mod_manager.GetBackend().OnTextureCreate(texture);

  if (!resolved_data.is_xfb)
  {
    GraphicsModActionData::PostEFB efb;
    mod_manager.GetBackend().OnEFBFinished(info, &efb);
    if (efb.material)
    {
      g_texture_cache->ApplyMaterialToCacheEntry(*efb.material, render_target.get());
    }
  }

  return render_target;
}

void GraphicsModGraphicsHandler::FlushFramebufferCopy(IFramebufferRenderer* renderer,
                                                      const RcTcacheEntry& cache_entry)
{
  m_decorated_handler.FlushFramebufferCopy(renderer, cache_entry);
}

void GraphicsModGraphicsHandler::OnFramebufferClear(
    EmulatedGraphics::ClearCopyData clear_data,
    EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data, bool is_xfb)
{
  m_decorated_handler.OnFramebufferClear(clear_data, scissors_and_viewport_data, is_xfb);
}

void GraphicsModGraphicsHandler::SetScissorAndViewport(
    EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data)
{
  m_decorated_handler.SetScissorAndViewport(scissors_and_viewport_data);
}

void GraphicsModGraphicsHandler::PixelEngineOperation(
    EmulatedGraphics::PixelEngineData pixel_engine_data, bool notify_cpu, bool notify_gpu)
{
  m_decorated_handler.PixelEngineOperation(pixel_engine_data, notify_cpu, notify_gpu);
}

void GraphicsModGraphicsHandler::PixelFormatChange(
    EmulatedGraphics::PixelFormatChangeData pixel_format_change_data)
{
  m_decorated_handler.PixelFormatChange(pixel_format_change_data);
}

void GraphicsModGraphicsHandler::OnDraw(GraphicsModSystem::DrawDataView draw_data,
                                        IMeshRenderer* mesh_renderer)
{
  auto& system = Core::System::GetInstance();
  auto& mod_manager = system.GetGraphicsModManager();
  mod_manager.GetBackend().OnDraw(draw_data, mesh_renderer);
}

void GraphicsModGraphicsHandler::OnReinterpretTexture(
    EmulatedGraphics::ReinterpretTextureData reinterpret_texture_data)
{
  m_decorated_handler.OnReinterpretTexture(std::move(reinterpret_texture_data));
}

void GraphicsModGraphicsHandler::OnPaletteTexture(
    EmulatedGraphics::PaletteTextureData palette_texture_data)
{
  m_decorated_handler.OnPaletteTexture(std::move(palette_texture_data));
}

void GraphicsModGraphicsHandler::OnPartialTextureUpdate(
    PartialTextureUpdateContext partial_texture_update_context)
{
  m_decorated_handler.OnPartialTextureUpdate(std::move(partial_texture_update_context));
}
