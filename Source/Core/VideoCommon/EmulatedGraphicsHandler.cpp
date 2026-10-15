// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/EmulatedGraphicsHandler.h"

#include "Common/EnumMap.h"
#include "Common/Logging/Log.h"

#include "Core/CoreTiming.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/HW/VideoInterface.h"
#include "Core/System.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GraphicsPathTracer.h"
#include "VideoCommon/IFramebufferRenderer.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"

static constexpr Common::EnumMap<float, GammaCorrection::Invalid2_2> s_gammaLUT = {1.0f, 1.7f, 2.2f,
                                                                                   2.2f};

EmulatedGraphicsHandler::EmulatedGraphicsHandler(FramebufferManager* frame_buffer_manager,
                                                 bool attempt_copy_efb_to_ram)
    : m_frame_buffer_manager(frame_buffer_manager),
      m_attempt_copy_efb_to_ram(attempt_copy_efb_to_ram)
{
}

RcTcacheEntry EmulatedGraphicsHandler::OnFramebufferCopy(
    IFramebufferRenderer* renderer, PixelShaderManager& pixel_shader_manager,
    EmulatedGraphics::FramebufferCopyResolvedData resolved_data)
{
  RcTcacheEntry entry;
  if (resolved_data.dest_stride != 0)
  {
    // Check if we are to copy from the EFB or draw to the XFB
    if (!resolved_data.is_xfb)
    {
      entry = renderer->RenderFramebufferCopy(pixel_shader_manager, resolved_data);
    }
    else
    {
      // We should be able to get away with deactivating the current bbox tracking
      // here. Not sure if there's a better spot to put this.
      // the number of lines copied is determined by the y scale * source efb height
      g_bounding_box->Disable(pixel_shader_manager);

      entry = renderer->RenderFramebufferCopy(pixel_shader_manager, resolved_data);
    }
  }

  return entry;
}

void EmulatedGraphicsHandler::FlushFramebufferCopy(IFramebufferRenderer* renderer,
                                                   const RcTcacheEntry& cache_entry)
{
  renderer->FlushFramebufferCopy(cache_entry);
}

void EmulatedGraphicsHandler::OnFramebufferClear(
    EmulatedGraphics::ClearCopyData clear_data,
    EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data, bool is_xfb)
{
  // [FB-DBG] Log the framebuffer that will receive this clear.
  if (is_xfb)
  {
    const auto* cur_fb = g_gfx->GetCurrentFramebuffer();
    ERROR_LOG_FMT(VIDEO,
                  "[FB-DBG] EmulatedGraphicsHandler::OnFramebufferClear is_xfb=true "
                  "rect=({},{},{}x{}) C={} A={} Z={} bound_fb={:#x} fb_dims={}x{}",
                  clear_data.src_rect.left, clear_data.src_rect.top, clear_data.src_rect.GetWidth(),
                  clear_data.src_rect.GetHeight(), clear_data.color_enable, clear_data.alpha_enable,
                  clear_data.z_enable, reinterpret_cast<u64>(cur_fb),
                  cur_fb ? cur_fb->GetWidth() : 0, cur_fb ? cur_fb->GetHeight() : 0);
  }

  BPFunctions::ClearScreen(m_frame_buffer_manager, clear_data.src_rect, clear_data.color_enable,
                           clear_data.alpha_enable, clear_data.z_enable, clear_data.pixel_format,
                           clear_data.color_ar, clear_data.color_gb, clear_data.z_value);
  BPFunctions::SetScissorAndViewport(
      m_frame_buffer_manager, scissors_and_viewport_data.scissor_top_left,
      scissors_and_viewport_data.scissor_bottom_right, scissors_and_viewport_data.scissor_offset,
      scissors_and_viewport_data.viewport);
}

void EmulatedGraphicsHandler::SetScissorAndViewport(
    EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data)
{
  BPFunctions::SetScissorAndViewport(
      m_frame_buffer_manager, scissors_and_viewport_data.scissor_top_left,
      scissors_and_viewport_data.scissor_bottom_right, scissors_and_viewport_data.scissor_offset,
      scissors_and_viewport_data.viewport);
}

void EmulatedGraphicsHandler::PixelEngineOperation(
    EmulatedGraphics::PixelEngineData pixel_engine_data, bool notify_cpu, bool notify_gpu)
{
  auto& system = Core::System::GetInstance();

  switch (pixel_engine_data.type)
  {
  case EmulatedGraphics::PixelEngineData::Type::SetToken:
  case EmulatedGraphics::PixelEngineData::Type::SetInterrupt:
  {
    const bool is_interrupt =
        pixel_engine_data.type == EmulatedGraphics::PixelEngineData::Type::SetInterrupt;
    if (pixel_engine_data.reset)
    {
      INCSTAT(g_stats.this_frame.num_token);
      if (notify_gpu)
      {
        g_texture_cache->FlushEFBCopies();
        g_texture_cache->FlushStaleBinds();
        m_frame_buffer_manager->InvalidatePeekCache(false);
        m_frame_buffer_manager->RefreshPeekCache();
      }
      if (notify_cpu && !system.GetFifo().UseDeterministicGPUThread())
      {
        system.GetPixelEngine().SetToken(static_cast<u16>(pixel_engine_data.new_value & 0xFFFF),
                                         is_interrupt, pixel_engine_data.cycles_into_the_future);
      }
      DEBUG_LOG_FMT(VIDEO, "SetPEToken {:#06X}", pixel_engine_data.new_value & 0xFFFF);
    }
    else
    {
      if (!notify_cpu)
      {
        return;
      }
      system.GetPixelEngine().SetToken(static_cast<u16>(pixel_engine_data.new_value & 0xFFFF),
                                       is_interrupt, pixel_engine_data.cycles_into_the_future);
    }
  }
  break;

  case EmulatedGraphics::PixelEngineData::Type::Finish:
  {
    if (pixel_engine_data.reset)
    {
      INCSTAT(g_stats.this_frame.num_draw_done);
      if (notify_gpu)
      {
        g_texture_cache->FlushEFBCopies();
        g_texture_cache->FlushStaleBinds();
        m_frame_buffer_manager->InvalidatePeekCache(false);
        m_frame_buffer_manager->RefreshPeekCache();
      }
      if (notify_cpu && !system.GetFifo().UseDeterministicGPUThread())
      {
        system.GetPixelEngine().SetFinish(
            pixel_engine_data.cycles_into_the_future);  // may generate interrupt
      }
      DEBUG_LOG_FMT(VIDEO, "GXSetDrawDone SetPEFinish (value: {:#04X})",
                    pixel_engine_data.new_value & 0xFFFF);
    }
    else
    {
      if (!notify_cpu)
      {
        return;
      }
      system.GetPixelEngine().SetFinish(
          pixel_engine_data.cycles_into_the_future);  // may generate interrupt
    }
  }
  };
}

void EmulatedGraphicsHandler::PixelFormatChange(
    EmulatedGraphics::PixelFormatChangeData pixel_format_change_data)
{
  BPFunctions::OnPixelFormatChange(m_frame_buffer_manager, pixel_format_change_data.pixel_format,
                                   pixel_format_change_data.z_format);
}

void EmulatedGraphicsHandler::OnDraw(GraphicsModSystem::DrawDataView draw_data,
                                     IMeshRenderer* mesh_renderer)
{
  // m_frame_buffer_manager->BindEFBFramebuffer();
  mesh_renderer->DrawEmulatedMesh(draw_data);
}

void EmulatedGraphicsHandler::OnReinterpretTexture(
    EmulatedGraphics::ReinterpretTextureData reinterpret_texture_data)
{
  TextureCacheBase::RenderReinterpretEntry(
      reinterpret_texture_data.reinterpreted_texture_entry,
      reinterpret_texture_data.existing_texture_entry->texture.get(),
      reinterpret_texture_data.existing_texture_entry->format.texfmt,
      reinterpret_texture_data.reinterpreted_texture_entry->format.texfmt);
}

void EmulatedGraphicsHandler::OnPaletteTexture(
    EmulatedGraphics::PaletteTextureData palette_texture_data)
{
  TextureCacheBase::RenderPaletteEntry(
      palette_texture_data.texel_buffer_offset, palette_texture_data.decoded_texture_entry,
      palette_texture_data.existing_texture_entry->texture.get(), palette_texture_data.tlutfmt);
}

void EmulatedGraphicsHandler::OnPartialTextureUpdate(
    PartialTextureUpdateContext partial_texture_update_context)
{
  g_texture_cache->ExecutePartialTextureUpdate(std::move(partial_texture_update_context));
}
