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

RcTcacheEntry
GraphicsModGraphicsHandler::OnFrameBufferCopy(IFramebufferRenderer* renderer,
                                              PixelShaderManager& pixel_shader_manager,
                                              EmulatedGraphics::FrameBufferCopyData copy_data)
{
  auto& system = Core::System::GetInstance();
  auto& mod_manager = system.GetGraphicsModManager();

  FBInfo info;
  info.m_width = copy_data.copy_width;
  info.m_height = copy_data.copy_height;
  info.m_texture_format = TexDecoder_GetEFBCopyBaseFormat(copy_data.efbcopy_format);

  if (copy_data.pe_copy.copy_to_xfb == 0)
  {
    bool skip = false;
    u32 scaled_width;
    u32 scaled_height;
    GraphicsModActionData::PreEFB efb{copy_data.copy_width, copy_data.copy_height, &skip,
                                      &scaled_width, &scaled_height};
    mod_manager.GetBackend().OnEFBStarted(info, &efb);
    if (skip == true)
    {
      renderer->UninitializeFramebufferMemory(copy_data);
      return nullptr;
    }
  }
  else
  {
    mod_manager.GetBackend().OnXFBStarted(info);
  }

  auto render_target =
      m_decorated_handler.OnFrameBufferCopy(renderer, pixel_shader_manager, copy_data);

  if (!render_target)
    return nullptr;

  GraphicsModSystem::TextureView texture;
  texture.hash_name = render_target->texture_info_name;
  texture.texture_data = render_target->texture.get();
  if (copy_data.pe_copy.copy_to_xfb == 0)
  {
    texture.texture_type = GraphicsModSystem::TextureType::EFB;
  }
  else
  {
    texture.texture_type = GraphicsModSystem::TextureType::XFB;
  }

  mod_manager.GetBackend().OnTextureCreate(texture);

  if (copy_data.pe_copy.copy_to_xfb == 0)
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

void GraphicsModGraphicsHandler::OnFrameBufferClear(
    EmulatedGraphics::ClearCopyData clear_data,
    EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data, bool is_xfb)
{
  m_decorated_handler.OnFrameBufferClear(clear_data, scissors_and_viewport_data, is_xfb);
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

void GraphicsModGraphicsHandler::OnScaleTexture(
    EmulatedGraphics::ScaleTextureData scale_texture_data)
{
  m_decorated_handler.OnScaleTexture(std::move(scale_texture_data));
}
