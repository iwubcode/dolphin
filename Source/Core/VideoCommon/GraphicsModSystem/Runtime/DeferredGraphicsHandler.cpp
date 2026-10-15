// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/DeferredGraphicsHandler.h"

#include <string>
#include <unordered_set>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "Core/System.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/GraphicsModSystem/Runtime/DeferredPerfTelemetry.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
/// RAII guard that makes a replay pass a self-contained transaction on GPU state.
///
/// On construction:
///   - Saves g_gfx's stored viewport/scissor bookmark (the value EndUtilityDrawing
///     restores to), the currently bound framebuffer, and the live viewport/scissor,
///     so none of it can be corrupted by the replay.
///   - Applies `initial` (this target's last-known viewport/scissor) to the GPU,
///     establishing a correct starting point even if utility drawing reset the GPU
///     state since the previous replay. This call does mutate the global store and
///     live state, but both are undone when the guard is destroyed.
///
/// On destruction:
///   - Restores the saved bookmark, framebuffer binding, and viewport/scissor, so any
///     subsequent GPU work (main-pass draws, texture uploads, utility drawing) sees
///     exactly the state that existed before the replay began.
class ScopedRenderState
{
public:
  ScopedRenderState(FramebufferManager* fbm,
                    const std::optional<EmulatedGraphics::ScissorsAndViewportData>& initial)
      : m_saved(g_gfx->GetStoredViewportAndScissor()),
        m_saved_framebuffer(g_gfx->GetCurrentFramebuffer())
  {
    if (initial.has_value())
    {
      BPFunctions::SetScissorAndViewport(fbm, initial->scissor_top_left,
                                         initial->scissor_bottom_right, initial->scissor_offset,
                                         initial->viewport);
    }
  }

  ~ScopedRenderState()
  {
    // Restore the stored bookmark so EndUtilityDrawing restores a correct value.
    g_gfx->StoreViewportAndScissor(m_saved);

    // Restore the live GPU state (framebuffer binding, viewport, scissor) so that no
    // target-specific values leak into subsequent work outside this replay scope.
    if (m_saved_framebuffer)
      g_gfx->SetFramebuffer(m_saved_framebuffer);
    g_gfx->SetViewport(m_saved.viewport_x, m_saved.viewport_y, m_saved.viewport_width,
                       m_saved.viewport_height, m_saved.viewport_near_depth,
                       m_saved.viewport_far_depth);
    g_gfx->SetScissorRect(m_saved.scissor_rect);
  }

  ScopedRenderState(const ScopedRenderState&) = delete;
  ScopedRenderState& operator=(const ScopedRenderState&) = delete;

private:
  AbstractGfx::ViewportAndScissor m_saved;
  AbstractFramebuffer* m_saved_framebuffer;
};

void ReplayOperations(EmulatedGraphicsHandler& emulated_graphics_handler,
                      FramebufferManager* frame_buffer_manager,
                      const XFBFrame::FrameView& frame_view,
                      std::span<const XFBFrame::UniformState> uniform_state, auto indices,
                      bool replaying_xfb, const char* op_dump_pass_name,
                      const std::optional<EmulatedGraphics::ScissorsAndViewportData>& op_dump_seed)
{
  auto& system = Core::System::GetInstance();
  auto& geometry_shader_manager = system.GetGeometryShaderManager();
  auto& pixel_shader_manager = system.GetPixelShaderManager();
  auto& vertex_shader_manager = system.GetVertexShaderManager();

  const bool telemetry = DeferredPerf::kTelemetryEnabled;
  auto& telemetry_instance = DeferredPerf::Telemetry::Get();
  u64 replay_start = 0;
  if (telemetry)
  {
    telemetry_instance.BeginReplay();
    telemetry_instance.BeginOpDump(op_dump_pass_name, op_dump_seed);
    replay_start = Common::Timer::NowUs();
  }

  // [FB-DBG] Track draws-before-RT to detect EFB-copy-before-draw ordering bugs.
  std::size_t draws_since_last_rt_or_clear = 0;
  std::size_t total_draws_replayed = 0;

  // [FB-DBG] Set of GC addresses whose EFB->VRAM upload has already run during this replay.
  // A draw that samples an EFB/XFB copy NOT in this set is reading a texture whose content
  // has not been uploaded yet (null/stale GPU texture) -> black. This proves the ordering bug.
  std::unordered_set<u32> efb_copies_uploaded;

  for (const std::size_t index : indices)
  {
    // Utility passes fired by ops in this loop are transactions: Begin/EndUtilityDrawing save
    // and restore their own binding, so the replay target must still be bound here. This assert
    // is a transition-period safety net; delete it once clean across test games.
    ASSERT(g_gfx->GetCurrentFramebuffer() == frame_buffer_manager->GetEFBFramebuffer());

    const XFBFrame::OperationReference& operation_reference = frame_view.operations[index];
    if (telemetry)
      telemetry_instance.AddOpReplayed(operation_reference.type);
    switch (operation_reference.type)
    {
    case XFBFrame::OperationType::Draw:
    {
      draws_since_last_rt_or_clear++;
      total_draws_replayed++;
      const XFBFrame::DrawData& draw_call_data = frame_view.draws[operation_reference.index];
      if (draw_call_data.geometry_constant_index)
      {
        geometry_shader_manager.constants = std::get<GeometryShaderConstants>(
            uniform_state[*draw_call_data.geometry_constant_index]);
        geometry_shader_manager.dirty = true;
      }
      if (draw_call_data.pixel_constant_index)
      {
        pixel_shader_manager.constants =
            std::get<PixelShaderConstants>(uniform_state[*draw_call_data.pixel_constant_index]);
        pixel_shader_manager.dirty = true;
      }
      if (draw_call_data.vertex_constant_index)
      {
        vertex_shader_manager.constants =
            std::get<VertexShaderConstants>(uniform_state[*draw_call_data.vertex_constant_index]);
        vertex_shader_manager.dirty = true;
      }
      if (telemetry)
      {
        telemetry_instance.AddDrawReplayed();
        telemetry_instance.LogDrawOp(draw_call_data.draw_call_data,
                                     draw_call_data.geometry_constant_index.has_value(),
                                     draw_call_data.pixel_constant_index.has_value(),
                                     draw_call_data.vertex_constant_index.has_value());
      }
      // Ensure any EFB/XFB copy textures sampled by this draw have been uploaded to VRAM.
      // In the deferred path, RT ops may appear before draws in the replay sequence, so a
      // draw can reference an EFB copy whose CompleteVRAMFramebufferCopy hasn't run yet.
      // We trigger the upload on-demand here so the texture has valid content at sample time.
      if (replaying_xfb)
      {
        for (const auto& tex : draw_call_data.draw_call_data.textures)
        {
          const TCacheEntry* e = tex.entry.get();
          if (!e || (!e->is_efb_copy && !e->is_xfb_copy))
            continue;
          if (efb_copies_uploaded.find(e->addr) != efb_copies_uploaded.end())
            continue;  // Already uploaded

          // Find the matching RT op and complete it now.
          bool found = false;
          for (const auto& rt : frame_view.render_targets)
          {
            if (!rt.cache_entry || rt.cache_entry->addr != e->addr)
              continue;
            if (rt.resolved_copy_data.dest_stride == 0 || !rt.resolved_copy_data.copy_to_vram)
              continue;
            g_texture_cache->CompleteVRAMFramebufferCopy(rt.cache_entry,
                                                         rt.resolved_copy_data);
            found = true;
            break;
          }
          efb_copies_uploaded.insert(e->addr);
          if (found)
          {
            ERROR_LOG_FMT(VIDEO,
                          "[FB-DBG]   On-demand EFB copy upload: addr={:#010x} ({}x{}) "
                          "triggered by draw (was not yet uploaded in sequence order)",
                          e->addr, e->native_width, e->native_height);
          }
        }
      }
      g_vertex_manager->DrawGraphicsData(frame_buffer_manager, draw_call_data.draw_call_data);
      break;
    }
    case XFBFrame::OperationType::RenderTarget:
    {
      const XFBFrame::RenderTargetData& render_target_data =
          frame_view.render_targets[operation_reference.index];
      if (telemetry)
        telemetry_instance.LogRenderTargetOp(render_target_data.resolved_copy_data);
      // [FB-DBG] Log render target ops during replay to track where copies complete.
      const auto& rcd = render_target_data.resolved_copy_data;
      ERROR_LOG_FMT(
          VIDEO,
          "[FB-DBG] Replay RT op_idx={} cache_entry={:#x} gc_dest_addr={:#010x} "
          "src_rect=({},{},{}x{}) dest_stride={} replaying_xfb={} w={} h={} copy_to_vram={} "
          "is_xfb={} draws_before_rt={} total_draws_so_far={}",
          operation_reference.index, reinterpret_cast<u64>(render_target_data.cache_entry.get()),
          rcd.dest_address, rcd.src_rect.left, rcd.src_rect.top, rcd.src_rect.GetWidth(),
          rcd.src_rect.GetHeight(), rcd.dest_stride, replaying_xfb, rcd.width, rcd.height,
          rcd.copy_to_vram, rcd.is_xfb, draws_since_last_rt_or_clear, total_draws_replayed);
      if (draws_since_last_rt_or_clear == 0 && replaying_xfb)
        ERROR_LOG_FMT(VIDEO, "[FB-DBG]   !! WARNING: RT copy with ZERO draws since last clear/rt "
                             "- EFB may be empty/black at copy time");
      draws_since_last_rt_or_clear = 0;
      if (render_target_data.cache_entry &&
          render_target_data.resolved_copy_data.dest_stride != 0 && replaying_xfb)
      {
        ERROR_LOG_FMT(VIDEO, "[FB-DBG]   -> CompleteVRAMFramebufferCopy entry={:#x}",
                      reinterpret_cast<u64>(render_target_data.cache_entry.get()));
        g_texture_cache->CompleteVRAMFramebufferCopy(render_target_data.cache_entry,
                                                     render_target_data.resolved_copy_data);
        // [FB-DBG] Mark this EFB/XFB copy as uploaded so later draws can be checked against it.
        if (render_target_data.cache_entry)
          efb_copies_uploaded.insert(render_target_data.cache_entry->addr);
      }
      break;
    }
    case XFBFrame::OperationType::ScissorViewport:
      if (telemetry)
        telemetry_instance.LogScissorViewportOp(
            frame_view.scissors_viewports[operation_reference.index]);
      emulated_graphics_handler.SetScissorAndViewport(
          frame_view.scissors_viewports[operation_reference.index]);
      break;
    case XFBFrame::OperationType::Clear:
    {
      const EmulatedGraphics::ClearCopyData& clear_data =
          frame_view.clears[operation_reference.index];
      if (telemetry)
        telemetry_instance.LogClearOp(clear_data);
      // [FB-DBG] Log clear ops to confirm z_enable state during replay.
      const auto clr_vs = g_gfx->GetStoredViewportAndScissor();
      ERROR_LOG_FMT(VIDEO,
                    "[FB-DBG] Replay Clear op_idx={} C={} A={} Z={} fmt={:x} "
                    "rect=({},{},{}x{}) colorAR={:#08x} colorGB={:#08x} "
                    "gpu_scissor=({},{},{}x{})",
                    operation_reference.index, clear_data.color_enable, clear_data.alpha_enable,
                    clear_data.z_enable, static_cast<u32>(clear_data.pixel_format),
                    clear_data.src_rect.left, clear_data.src_rect.top,
                    clear_data.src_rect.GetWidth(), clear_data.src_rect.GetHeight(),
                    clear_data.color_ar, clear_data.color_gb, clr_vs.scissor_rect.left,
                    clr_vs.scissor_rect.top, clr_vs.scissor_rect.GetWidth(),
                    clr_vs.scissor_rect.GetHeight());
      BPFunctions::ClearScreen(frame_buffer_manager, clear_data.src_rect, clear_data.color_enable,
                               clear_data.alpha_enable, clear_data.z_enable,
                               clear_data.pixel_format, clear_data.color_ar, clear_data.color_gb,
                               clear_data.z_value);
      draws_since_last_rt_or_clear = 0;
      break;
    }
    case XFBFrame::OperationType::PixelEngine:
    {
      const auto& pe = frame_view.pixel_engine[operation_reference.index];
      if (telemetry)
        telemetry_instance.LogPixelEngineOp(pe);
      // [FB-DBG] Log PE ops during replay to see what fires before the floor copy.
      ERROR_LOG_FMT(VIDEO,
                    "[FB-DBG] Replay PE op_idx={} type={} reset={} new_value={:#06x} "
                    "cycles_into_future={} (notify_gpu triggers FlushEFBCopies if reset)",
                    operation_reference.index, static_cast<int>(pe.type), pe.reset ? 1 : 0,
                    static_cast<u32>(pe.new_value) & 0xFFFF, pe.cycles_into_the_future);
      if (!replaying_xfb)
        continue;
      emulated_graphics_handler.PixelEngineOperation(pe, false, true);
      break;
    }
    case XFBFrame::OperationType::PixelFormatChange:
      if (telemetry)
        telemetry_instance.LogPixelFormatChangeOp(
            frame_view.pixel_format_changes[operation_reference.index]);
      emulated_graphics_handler.PixelFormatChange(
          frame_view.pixel_format_changes[operation_reference.index]);
      break;
    case XFBFrame::OperationType::ReinterpretTexture:
      if (telemetry)
        telemetry_instance.LogReinterpretTextureOp(
            frame_view.reinterpret_textures[operation_reference.index]);
      emulated_graphics_handler.OnReinterpretTexture(
          frame_view.reinterpret_textures[operation_reference.index]);
      break;
    case XFBFrame::OperationType::PaletteTexture:
      if (telemetry)
        telemetry_instance.LogPaletteTextureOp(
            frame_view.palette_textures[operation_reference.index]);
      emulated_graphics_handler.OnPaletteTexture(
          frame_view.palette_textures[operation_reference.index]);
      break;
    case XFBFrame::OperationType::PartialTextureUpdate:
    {
      if (telemetry)
        telemetry_instance.LogPartialTextureUpdateOp(
            frame_view.partial_texture_updates[operation_reference.index]);
      emulated_graphics_handler.OnPartialTextureUpdate(
          frame_view.partial_texture_updates[operation_reference.index]);
      break;
    }
    case XFBFrame::OperationType::Max:
      break;  // Unreachable
    }
  }

  if (telemetry)
  {
    telemetry_instance.AddReplayUs(Common::Timer::NowUs() - replay_start);
    telemetry_instance.EndOpDump();
    telemetry_instance.EndReplay();
  }
}
}  // namespace

DeferredGraphicsHandler::DeferredGraphicsHandler(XFBFrame* frame,
                                                 FramebufferManager* native_efb_framebuffer_manager)
    : m_xfb_frame(frame), m_native_efb_framebuffer_manager(native_efb_framebuffer_manager),
      m_graphics_mod_handler(g_framebuffer_manager.get(), true)
{
}

RcTcacheEntry DeferredGraphicsHandler::OnFramebufferCopy(
    IFramebufferRenderer* renderer, PixelShaderManager& pixel_shader_manager,
    EmulatedGraphics::FramebufferCopyResolvedData resolved_data)
{
  if (resolved_data.is_xfb)
  {
    DrawXFBFrame();

    return m_graphics_mod_handler.OnFramebufferCopy(renderer, pixel_shader_manager,
                                                    std::move(resolved_data));
  }

  const u32 stride = resolved_data.dest_stride;
  const auto entry = m_graphics_mod_handler.OnFramebufferCopy(this, pixel_shader_manager,
                                                              std::move(resolved_data));
  if (m_native_efb_framebuffer_manager && stride != 0)
  {
    if (g_ActiveConfig.bDeferEFBCopies && !g_ActiveConfig.bSkipEFBCopyToRam)
    {
      // Store this draw off, we will render later in a batch
      m_deferred_native_efb_to_last_operation_index[entry.get()] =
          m_xfb_frame->GetTotalOperations();
    }
    else
    {
      DrawNativeEFBCopy(m_last_native_efb_operation_index, m_xfb_frame->GetTotalOperations());
      m_last_native_efb_operation_index = m_xfb_frame->GetTotalOperations();
    }
  }
  return entry;
}

void DeferredGraphicsHandler::FlushFramebufferCopy(IFramebufferRenderer* renderer,
                                                   const RcTcacheEntry& cache_entry)
{
  m_graphics_mod_handler.FlushFramebufferCopy(this, cache_entry);
}

void DeferredGraphicsHandler::OnFramebufferClear(
    EmulatedGraphics::ClearCopyData clear_data,
    EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data, bool is_xfb)
{
  // [FB-DBG] Log XFB clears to track the missing-clear divergence.
  if (is_xfb)
  {
    const auto* cur_fb = g_gfx->GetCurrentFramebuffer();
    const auto vs = g_gfx->GetStoredViewportAndScissor();
    ERROR_LOG_FMT(VIDEO,
                  "[FB-DBG] Deferred OnFramebufferClear is_xfb=true rect=({},{},{}x{}) "
                  "C={} A={} Z={} colorAR={:#08x} colorGB={:#08x} fmt={:x} "
                  "bound_fb={:#x} fb_dims={}x{} gpu_scissor=({},{},{}x{})",
                  clear_data.src_rect.left, clear_data.src_rect.top, clear_data.src_rect.GetWidth(),
                  clear_data.src_rect.GetHeight(), clear_data.color_enable, clear_data.alpha_enable,
                  clear_data.z_enable, clear_data.color_ar, clear_data.color_gb,
                  static_cast<u32>(clear_data.pixel_format), reinterpret_cast<u64>(cur_fb),
                  cur_fb ? cur_fb->GetWidth() : 0, cur_fb ? cur_fb->GetHeight() : 0,
                  vs.scissor_rect.left, vs.scissor_rect.top, vs.scissor_rect.GetWidth(),
                  vs.scissor_rect.GetHeight());
  }

  if (m_native_efb_framebuffer_manager)
  {
    ScopedRenderState guard(m_native_efb_framebuffer_manager, m_native_scissor_viewport);

    const bool attempt_copy_efb_to_ram = false;
    EmulatedGraphicsHandler emulated_graphics_handler(m_native_efb_framebuffer_manager,
                                                      attempt_copy_efb_to_ram);
    emulated_graphics_handler.OnFramebufferClear(clear_data, scissors_and_viewport_data, is_xfb);

    // The native target's GPU state is now at this scissor/viewport. Copy (not move)
    // because the non-XFB path below moves it into the frame record.
    m_native_scissor_viewport.reset();
    m_native_scissor_viewport.emplace(scissors_and_viewport_data);
  }

  if (is_xfb)
  {
    m_graphics_mod_handler.OnFramebufferClear(clear_data, scissors_and_viewport_data, is_xfb);
    return;
  }

  m_xfb_frame->AddClearData(std::move(clear_data));
  m_xfb_frame->AddScissorsAndViewportData(std::move(scissors_and_viewport_data));
}

void DeferredGraphicsHandler::SetScissorAndViewport(
    EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data)
{
  m_xfb_frame->AddScissorsAndViewportData(std::move(scissors_and_viewport_data));
}

void DeferredGraphicsHandler::PixelEngineOperation(
    EmulatedGraphics::PixelEngineData pixel_engine_data, bool notify_cpu, bool notify_gpu)
{
  const bool force_notify_cpu = true;

  // By default if deferring, we wait to tell the GPU
  bool force_notify_gpu = false;

  // However, if we have a native operation we need to
  // ensure the gpu runs now if requested
  if (m_native_efb_framebuffer_manager)
  {
    force_notify_gpu = notify_gpu;
  }
  m_graphics_mod_handler.PixelEngineOperation(pixel_engine_data, force_notify_cpu,
                                              force_notify_gpu);

  if (!force_notify_gpu)
    m_xfb_frame->AddPixelEngineOperationData(std::move(pixel_engine_data));
}

void DeferredGraphicsHandler::PixelFormatChange(
    EmulatedGraphics::PixelFormatChangeData pixel_format_change_data)
{
  m_xfb_frame->AddPixelFormatChangeData(std::move(pixel_format_change_data));
}

void DeferredGraphicsHandler::OnDraw(GraphicsModSystem::DrawDataView draw_data,
                                     IMeshRenderer* renderer)
{
  m_graphics_mod_handler.OnDraw(draw_data, this);
}

void DeferredGraphicsHandler::OnReinterpretTexture(
    EmulatedGraphics::ReinterpretTextureData reinterpret_texture_data)
{
  if (reinterpret_texture_data.existing_texture_entry->IsEfbCopy())
  {
    // Need to wait...not sure what this means for our native EFB buffer?
    // We only have one texture cache
    m_xfb_frame->AddReinterpretTextureData(std::move(reinterpret_texture_data));
    return;
  }

  m_graphics_mod_handler.OnReinterpretTexture(std::move(reinterpret_texture_data));
}

void DeferredGraphicsHandler::OnPaletteTexture(
    EmulatedGraphics::PaletteTextureData palette_texture_data)
{
  if (palette_texture_data.existing_texture_entry->IsEfbCopy())
  {
    // Need to wait...not sure what this means for our native EFB buffer?
    // We only have one texture cache
    m_xfb_frame->AddPaletteTextureData(std::move(palette_texture_data));
    return;
  }

  m_graphics_mod_handler.OnPaletteTexture(std::move(palette_texture_data));
}

void DeferredGraphicsHandler::OnPartialTextureUpdate(
    PartialTextureUpdateContext partial_texture_update_context)
{
  m_xfb_frame->AddPartialTextureUpdateData(std::move(partial_texture_update_context));
}

std::shared_ptr<TCacheEntry> DeferredGraphicsHandler::RenderFramebufferCopy(
    PixelShaderManager& pixel_shader_manager,
    const EmulatedGraphics::FramebufferCopyResolvedData& resolved_data)
{
  FramebufferManager* chosen_fbm = m_native_efb_framebuffer_manager != nullptr ?
                                       m_native_efb_framebuffer_manager :
                                       g_framebuffer_manager.get();

  if (DeferredPerf::kTelemetryEnabled && DeferredPerf::kCopyStreamEnabled)
    DeferredPerf::Telemetry::Get().LogRecordCopy(
        chosen_fbm, chosen_fbm == m_native_efb_framebuffer_manager, resolved_data);

  RcTcacheEntry deferred_entry = g_texture_cache->DeferFramebufferCopy(chosen_fbm, resolved_data);

  m_xfb_frame->AddRenderTargetData(XFBFrame::RenderTargetData{.resolved_copy_data = resolved_data,
                                                              .cache_entry = deferred_entry});

  return deferred_entry;
}

void DeferredGraphicsHandler::FlushFramebufferCopy(const std::shared_ptr<TCacheEntry>& cache_entry)
{
  auto it = m_deferred_native_efb_to_last_operation_index.find(cache_entry.get());
  if (it == m_deferred_native_efb_to_last_operation_index.end())
    return;
  const std::size_t last_operation_index = it->second;
  m_deferred_native_efb_to_last_operation_index.erase(it);
  if (m_last_native_efb_operation_index > last_operation_index)
    return;

  DrawNativeEFBCopy(m_last_native_efb_operation_index, last_operation_index);
  m_last_native_efb_operation_index = last_operation_index;

  const auto& operation_reference = m_xfb_frame->GetView().operations[last_operation_index - 1];
  const XFBFrame::RenderTargetData& render_target_data =
      m_xfb_frame->GetView().render_targets[operation_reference.index];
  g_texture_cache->CompleteRAMFramebufferCopy(m_native_efb_framebuffer_manager,
                                              render_target_data.resolved_copy_data, cache_entry);

  static_cast<IFramebufferRenderer*>(g_texture_cache.get())->FlushFramebufferCopy(cache_entry);
}

void DeferredGraphicsHandler::UninitializeFramebufferMemory(
    const EmulatedGraphics::FramebufferCopyResolvedData& resolved_data)
{
  g_texture_cache->UninitializeFramebufferMemory(resolved_data);
}

void DeferredGraphicsHandler::DrawEmulatedMesh(const GraphicsModSystem::DrawDataView& draw_data,
                                               const Common::Matrix44& custom_transform)
{
  // Defer: upload vertices + capture shader state for replay at frame boundary
  std::vector<EmulatedGraphics::DrawCallData> deferred_data;
  g_vertex_manager->DeferEmulatedMesh(draw_data, custom_transform, &deferred_data);
  for (auto& dcd : deferred_data)
  {
    m_xfb_frame->AddDrawData(std::move(dcd));
  }
}

void DeferredGraphicsHandler::DrawEmulatedMesh(
    const GraphicsModSystem::DrawDataView& draw_data,
    const VideoCommon::MaterialResource::Data& material_data,
    const Common::Matrix44& custom_transform)
{
  // Defer: upload vertices + capture shader state for replay at frame boundary
  std::vector<EmulatedGraphics::DrawCallData> deferred_data;
  g_vertex_manager->DeferEmulatedMesh(draw_data, material_data, custom_transform, &deferred_data);
  for (auto& dcd : deferred_data)
  {
    m_xfb_frame->AddDrawData(std::move(dcd));
  }
}

void DeferredGraphicsHandler::DrawCustomMesh(const GraphicsModSystem::DrawDataView& draw_data,
                                             GraphicsModSystem::DrawCallID draw_call,
                                             const VideoCommon::MeshResource::Data& mesh_data,
                                             const Common::Matrix44& custom_transform,
                                             bool ignore_mesh_transform,
                                             VideoCommon::SkeletonInstance* skeleton_instance)
{
  // Defer: upload custom mesh vertices + capture shader state for replay at frame boundary
  std::vector<EmulatedGraphics::DrawCallData> deferred_data;
  g_vertex_manager->DeferCustomMesh(draw_data, draw_call, mesh_data, custom_transform,
                                    ignore_mesh_transform, skeleton_instance, &deferred_data);
  for (auto& dcd : deferred_data)
  {
    m_xfb_frame->AddDrawData(std::move(dcd));
  }
}

void DeferredGraphicsHandler::UpdateTrackedScissorViewport(
    const XFBFrame::FrameView& view, std::size_t first, std::size_t last,
    std::optional<EmulatedGraphics::ScissorsAndViewportData>* out)
{
  // Walk backward from the end of the range to find the most recent ScissorViewport.
  // Draws do not modify viewport/scissor state, so this is the value the target was
  // left in after replaying [first, last).
  for (std::size_t i = last; i-- > first;)
  {
    const auto& op = view.operations[i];
    if (op.type == XFBFrame::OperationType::ScissorViewport)
    {
      out->reset();
      out->emplace(view.scissors_viewports[op.index]);
      return;
    }
  }
}

void DeferredGraphicsHandler::DrawXFBFrame()
{
  static std::size_t s_xfb_batch_counter = 0;
  ++s_xfb_batch_counter;

  const bool telemetry = DeferredPerf::kTelemetryEnabled;
  auto& telemetry_instance = DeferredPerf::Telemetry::Get();
  u64 drawxfb_start = 0;
  if (telemetry)
    drawxfb_start = Common::Timer::NowUs();

  // Begin viewport-write capture BEFORE the guard so that the seed application done by
  // ScopedRenderState's constructor is captured under this pass name.
  if (telemetry && DeferredPerf::kViewportWriteCapture)
    telemetry_instance.BeginViewportWriteCapture("main");

  // Bind our normal (user IR) frame buffer before constructing the guard so that the
  // saved framebuffer binding is the main EFB framebuffer. The guard restores this
  // binding on destruction, leaving the GPU where XFB replay expects it to be.
  g_framebuffer_manager->BindEFBFramebuffer();

  // [FB-DBG] Log the seed scissor/viewport applied at replay start.
  if (m_main_scissor_viewport.has_value())
  {
    const auto& sv = *m_main_scissor_viewport;
    ERROR_LOG_FMT(VIDEO,
                  "[FB-DBG] DrawXFBFrame seed: scissor_tl=({},{}) scissor_br=({},{}) "
                  "scissor_off=({},{}) vp=(xOrig={},yOrig={},wd={},ht={},zRange={},farZ={})",
                  sv.scissor_top_left.x, sv.scissor_top_left.y,
                  sv.scissor_bottom_right.x, sv.scissor_bottom_right.y,
                  sv.scissor_offset.x, sv.scissor_offset.y,
                  sv.viewport.xOrig, sv.viewport.yOrig, sv.viewport.wd, sv.viewport.ht,
                  sv.viewport.zRange, sv.viewport.farZ);
  }
  else
  {
    ERROR_LOG_FMT(VIDEO, "[FB-DBG] DrawXFBFrame seed: NONE (first frame or no prior SV op)");
  }

  ScopedRenderState guard(g_framebuffer_manager.get(), m_main_scissor_viewport);

  // If necessary, we already did our copy to ram, no need to attempt it again
  const bool attempt_copy_efb_to_ram = false;
  EmulatedGraphicsHandler emulated_graphics_handler(g_framebuffer_manager.get(),
                                                    attempt_copy_efb_to_ram);

  const auto& uniform_state = m_xfb_frame->GetUniformStateChanges();
  const XFBFrame::FrameView view = m_xfb_frame->GetView();

  if (telemetry)
  {
    telemetry_instance.SetOpsSizeCapacity(static_cast<u32>(view.operations.size()),
                                          static_cast<u32>(view.operations.size()));
    telemetry_instance.SetUniformStateSize(static_cast<u32>(uniform_state.size()));
  }

  // [FB-DBG] Compact op-sequence trace + detailed dump for batches containing the floor.
  {
    std::string seq;
    seq.reserve(view.operations.size());
    bool has_floor = false;
    for (const auto& op : view.operations)
    {
      switch (op.type)
      {
        case XFBFrame::OperationType::Draw: seq += 'D'; break;
        case XFBFrame::OperationType::RenderTarget:
          seq += 'R';
          if (view.render_targets[op.index].resolved_copy_data.dest_address == 0x117cf180)
            has_floor = true;
          break;
        case XFBFrame::OperationType::Clear: seq += 'C'; break;
        case XFBFrame::OperationType::ScissorViewport: seq += 'S'; break;
        case XFBFrame::OperationType::PixelEngine: seq += 'P'; break;
        case XFBFrame::OperationType::PixelFormatChange: seq += 'F'; break;
        case XFBFrame::OperationType::ReinterpretTexture: seq += 'I'; break;
        case XFBFrame::OperationType::PaletteTexture: seq += 'L'; break;
        case XFBFrame::OperationType::PartialTextureUpdate: seq += 'U'; break;
        default: seq += '?'; break;
      }
    }
    ERROR_LOG_FMT(VIDEO, "[FB-DBG] DrawXFBFrame batch={} op_seq ({} ops): {}", s_xfb_batch_counter,
                  view.operations.size(), seq);

    // [FB-DBG] Detailed per-op dump for the batch containing the floor copy.
    if (has_floor)
    {
      const int max_dump = 20;  // Only first N ops to keep log manageable
      for (std::size_t i = 0; i < view.operations.size() && static_cast<int>(i) < max_dump; ++i)
      {
        const auto& op = view.operations[i];
        switch (op.type)
        {
          case XFBFrame::OperationType::Draw:
          {
            const auto& d = view.draws[op.index].draw_call_data;
            ERROR_LOG_FMT(VIDEO, "[FB-DBG]   op[{}] DRAW fb={:#x} idx_count={} base_idx={}",
                          i, reinterpret_cast<u64>(d.frame_buffer), d.textures.size(),
                          d.base_index);
            break;
          }
          case XFBFrame::OperationType::RenderTarget:
          {
            const auto& r = view.render_targets[op.index].resolved_copy_data;
            ERROR_LOG_FMT(VIDEO, "[FB-DBG]   op[{}] RT dest={:#010x} src=({},{},{}x{}) stride={} xfb={}",
                          i, r.dest_address, r.src_rect.left, r.src_rect.top,
                          r.src_rect.GetWidth(), r.src_rect.GetHeight(), r.dest_stride,
                          r.is_xfb ? 1 : 0);
            break;
          }
          case XFBFrame::OperationType::Clear:
          {
            const auto& c = view.clears[op.index];
            ERROR_LOG_FMT(VIDEO, "[FB-DBG]   op[{}] CLEAR rect=({},{},{}x{}) C={} A={} Z={}",
                          i, c.src_rect.left, c.src_rect.top, c.src_rect.GetWidth(),
                          c.src_rect.GetHeight(), c.color_enable ? 1 : 0, c.alpha_enable ? 1 : 0,
                          c.z_enable ? 1 : 0);
            break;
          }
          case XFBFrame::OperationType::ScissorViewport:
          {
            const auto& s = view.scissors_viewports[op.index];
            ERROR_LOG_FMT(VIDEO, "[FB-DBG]   op[{}] SCISSOR tl=({},{}) br=({},{}) off=({},{})",
                          i, s.scissor_top_left.x, s.scissor_top_left.y,
                          s.scissor_bottom_right.x, s.scissor_bottom_right.y,
                          s.scissor_offset.x, s.scissor_offset.y);
            break;
          }
          case XFBFrame::OperationType::PixelEngine:
          {
            const auto& p = view.pixel_engine[op.index];
            ERROR_LOG_FMT(VIDEO, "[FB-DBG]   op[{}] PE type={} reset={}", i, static_cast<int>(p.type),
                          p.reset ? 1 : 0);
            break;
          }
          case XFBFrame::OperationType::PixelFormatChange:
          {
            const auto& f = view.pixel_format_changes[op.index];
            ERROR_LOG_FMT(VIDEO, "[FB-DBG]   op[{}] PIXFMT pixel={} z={}", i,
                          static_cast<int>(f.pixel_format), static_cast<int>(f.z_format));
            break;
          }
          default:
            ERROR_LOG_FMT(VIDEO, "[FB-DBG]   op[{}] OTHER type={}", i, static_cast<int>(op.type));
            break;
        }
      }
    }
  }

  auto all_indices = std::views::iota(0ULL, view.operations.size());
  ReplayOperations(emulated_graphics_handler, g_framebuffer_manager.get(), view, uniform_state,
                   all_indices, true, "main", m_main_scissor_viewport);

  if (telemetry && DeferredPerf::kViewportWriteCapture)
    telemetry_instance.EndViewportWriteCapture();

  UpdateTrackedScissorViewport(view, 0, view.operations.size(), &m_main_scissor_viewport);

  m_xfb_frame->Reset();

  if (telemetry)
    telemetry_instance.AddDrawXfbUs(Common::Timer::NowUs() - drawxfb_start);

  if (m_native_efb_framebuffer_manager)
  {
    // Clear native operation tracking
    m_deferred_native_efb_to_last_operation_index.clear();
    m_last_native_efb_operation_index = 0;
  }
}

void DeferredGraphicsHandler::DrawNativeEFBCopy(std::size_t first_operation_index,
                                                std::size_t last_operation_index)
{
  // Begin viewport-write capture BEFORE the guard so that the seed application done by
  // ScopedRenderState's constructor is captured under this pass name.
  if (DeferredPerf::kTelemetryEnabled && DeferredPerf::kViewportWriteCapture)
    DeferredPerf::Telemetry::Get().BeginViewportWriteCapture("native_efb");

  ScopedRenderState guard(m_native_efb_framebuffer_manager, m_native_scissor_viewport);

  // Bind our native frame buffer
  m_native_efb_framebuffer_manager->BindEFBFramebuffer();

  const bool attempt_copy_efb_to_ram = false;
  EmulatedGraphicsHandler emulated_graphics_handler(m_native_efb_framebuffer_manager,
                                                    attempt_copy_efb_to_ram);

  const auto& uniform_state = m_xfb_frame->GetUniformStateChanges();
  const XFBFrame::FrameView view = m_xfb_frame->GetView();
  auto all_indices = std::views::iota(first_operation_index, last_operation_index);
  ReplayOperations(emulated_graphics_handler, m_native_efb_framebuffer_manager, view, uniform_state,
                   all_indices, false, "native_efb", m_native_scissor_viewport);

  if (DeferredPerf::kTelemetryEnabled && DeferredPerf::kViewportWriteCapture)
    DeferredPerf::Telemetry::Get().EndViewportWriteCapture();

  // Persist the viewport/scissor this target was left in, for the next replay.
  UpdateTrackedScissorViewport(view, first_operation_index, last_operation_index,
                               &m_native_scissor_viewport);
}
