#include "VideoCommon/EmulatedGraphicsManager.h"

#include "Core/System.h"

#include "VideoCommon/EFBInterface.h"
#include "VideoCommon/EmulatedGraphicsHandler.h"
#include "VideoCommon/GraphicsModSystem/Runtime/DeferredGraphicsHandler.h"
#include "VideoCommon/GraphicsModSystem/Runtime/DeferredPerfTelemetry.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModGraphicsHandler.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/IGraphicsHandler.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"

namespace
{
/*class TestGraphicsHandler final : public IGraphicsHandler
{
public:
  TestGraphicsHandler(XFBFrame* frame)
      : m_xfb_frame(frame), m_emulated_graphics_handler(g_framebuffer_manager.get(), true)
  {
  }

  void
  OnFrameBufferCopy(RcTcacheEntry render_target, PixelShaderManager& pixel_shader_manager,
                    EmulatedGraphics::FrameBufferCopyData copy_data,
                    EmulatedGraphics::ClearCopyData clear_data,
                    EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data) override
  {
    m_emulated_graphics_handler.OnFrameBufferCopy(render_target, pixel_shader_manager, copy_data,
                                                  clear_data, scissors_and_viewport_data);

    if (copy_data.pe_copy.copy_to_xfb != 0)
    {
      m_xfb_frame->Reset();
    }
    else
    {
      m_xfb_frame->AddRenderTargetData(XFBFrame::RenderTargetData{
          .copy_data = std::move(copy_data),
          .clear_data = std::move(clear_data),
          .scissors_and_viewport_data = std::move(scissors_and_viewport_data),
          .cache_entry = std::move(render_target)});
    }
  }

  void SetScissorAndViewport(
      EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data) override
  {
    m_emulated_graphics_handler.SetScissorAndViewport(scissors_and_viewport_data);
    m_xfb_frame->AddScissorsAndViewportData(std::move(scissors_and_viewport_data));
  }

  void PixelEngineOperation(EmulatedGraphics::PixelEngineData pixel_engine_data, bool notify_cpu,
                            bool notify_gpu) override
  {
    m_emulated_graphics_handler.PixelEngineOperation(pixel_engine_data, notify_cpu, notify_gpu);
    m_xfb_frame->AddPixelEngineOperationData(std::move(pixel_engine_data));
  }
  void PixelFormatChange(EmulatedGraphics::PixelFormatChangeData pixel_format_change_data) override
  {
    m_emulated_graphics_handler.PixelFormatChange(pixel_format_change_data);
    m_xfb_frame->AddPixelFormatChangeData(std::move(pixel_format_change_data));
  }

  void OnDraw(EmulatedGraphics::DrawCallData draw_call_data) override
  {
    const bool geometry_shader_constants_dirty = draw_call_data.geometry_manager->dirty;
    const bool pixel_shader_constants_dirty = draw_call_data.pixel_manager->dirty;
    const bool vertex_shader_constants_dirty = draw_call_data.vertex_manager->dirty;

    m_emulated_graphics_handler.OnDraw(draw_call_data);

    // We only have one set of managers...set their dirty state back so the frame knows
    // if we had a change...
    draw_call_data.geometry_manager->dirty = geometry_shader_constants_dirty;
    draw_call_data.pixel_manager->dirty = pixel_shader_constants_dirty;
    draw_call_data.vertex_manager->dirty = vertex_shader_constants_dirty;
    m_xfb_frame->AddDrawData(std::move(draw_call_data));
  }

  void
  OnReinterpretTexture(EmulatedGraphics::ReinterpretTextureData reinterpret_texture_data) override
  {
    m_emulated_graphics_handler.OnReinterpretTexture(reinterpret_texture_data);
    m_xfb_frame->AddReinterpretTextureData(std::move(reinterpret_texture_data));
  }
  void OnPaletteTexture(EmulatedGraphics::PaletteTextureData palette_texture_data) override
  {
    m_emulated_graphics_handler.OnPaletteTexture(palette_texture_data);
    m_xfb_frame->AddPaletteTextureData(std::move(palette_texture_data));
  }
  void OnScaleTexture(EmulatedGraphics::ScaleTextureData scale_texture_data) override
  {
    m_emulated_graphics_handler.OnScaleTexture(scale_texture_data);
    m_xfb_frame->AddScaleTextureData(std::move(scale_texture_data));
  }

private:
  EmulatedGraphicsHandler m_emulated_graphics_handler;
  XFBFrame* m_xfb_frame;
};*/
}  // namespace

EmulatedGraphicsManager::EmulatedGraphicsManager() = default;
EmulatedGraphicsManager::~EmulatedGraphicsManager() = default;

void EmulatedGraphicsManager::Initialize()
{
  m_native_framebuffer_manager = std::make_unique<FramebufferManager>();
  m_native_framebuffer_manager->Initialize(1);

  auto& system = Core::System::GetInstance();
  auto& mod_manager = system.GetGraphicsModManager();
  XFBFrame* frame_data = mod_manager.GetBackend().GetFrameData();

  m_deferred_native_graphics_handler =
      std::make_unique<DeferredGraphicsHandler>(frame_data, m_native_framebuffer_manager.get());
  m_deferred_graphics_handler = std::make_unique<DeferredGraphicsHandler>(frame_data, nullptr);

  // Whether we actually place our EFB copy in RAM is still determined by config at that time
  // This just says whether to actually try and trigger one during the EFB copy step
  const bool attempt_copy_efb_to_ram = true;
  m_normal_graphics_handler = std::make_unique<EmulatedGraphicsHandler>(g_framebuffer_manager.get(),
                                                                        attempt_copy_efb_to_ram);
  m_normal_graphicsmod_graphics_handler = std::make_unique<GraphicsModGraphicsHandler>(
      g_framebuffer_manager.get(), attempt_copy_efb_to_ram);
  // m_normal_graphics_handler = std::make_unique<TestGraphicsHandler>(frame_data);

  m_config_changed =
      GetVideoEvents().config_changed_event.Register([this](u32) { SettingsChanged(); });

  SettingsChanged();

  // Start with binding the main framebuffer
  // this is because initializing the native framebuffer, actually sets
  // the Begin/End utility drawing global...
  // TODO: This probably shouldn't be here, need to find a cleaner fix
  g_framebuffer_manager->BindEFBFramebuffer();
}

void EmulatedGraphicsManager::Shutdown()
{
  m_native_framebuffer_manager.reset();
  m_deferred_native_graphics_handler.reset();
  m_deferred_graphics_handler.reset();
  m_normal_graphics_handler.reset();
  m_normal_graphicsmod_graphics_handler.reset();
}

FramebufferManager* EmulatedGraphicsManager::GetPeekPokeFramebuffer() const
{
  return m_peek_poke_framebuffer_manager;
}

IGraphicsHandler* EmulatedGraphicsManager::GetGraphicsHandler() const
{
  return m_graphics_handler;
}

void EmulatedGraphicsManager::SettingsChanged()
{
  m_peek_poke_framebuffer_manager = g_framebuffer_manager.get();

  const bool should_defer = true;

  if (g_ActiveConfig.bGraphicMods)
  {
    if (!should_defer)
    {
      m_graphics_handler = m_normal_graphicsmod_graphics_handler.get();
      DeferredPerf::Telemetry::Get().SetMode(DeferredPerf::Mode::Normal);
    }
    else
    {
      if (!g_ActiveConfig.bSkipEFBCopyToRam || g_ActiveConfig.bEFBAccessEnable)
      {
        // Need to force a native resolution version as well
        m_graphics_handler = m_deferred_native_graphics_handler.get();
        DeferredPerf::Telemetry::Get().SetMode(DeferredPerf::Mode::DeferredNativeEfb);
        m_peek_poke_framebuffer_manager = m_native_framebuffer_manager.get();
      }
      else
      {
        m_graphics_handler = m_deferred_graphics_handler.get();
        DeferredPerf::Telemetry::Get().SetMode(DeferredPerf::Mode::Deferred);
      }
    }
  }
  else
  {
    m_graphics_handler = m_normal_graphics_handler.get();
    DeferredPerf::Telemetry::Get().SetMode(DeferredPerf::Mode::Normal);
  }
}
