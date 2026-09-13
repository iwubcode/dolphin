// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/DeferredGraphicsHandler.h"

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

  for (const std::size_t index : indices)
  {
    // Re-establish the target framebuffer. Ops that trigger utility passes (Clear, texture
    // reinterpret, palette decode, etc.) call EndUtilityDrawing which rebinds to the main EFB;
    // this ensures every op starts on the intended replay target. SetFramebuffer is a no-op when
    // already bound, so this has zero cost for ops that don't change the binding.
    frame_buffer_manager->BindEFBFramebuffer();

    const XFBFrame::OperationReference& operation_reference = frame_view.operations[index];
    if (telemetry)
      telemetry_instance.AddOpReplayed(operation_reference.type);
    switch (operation_reference.type)
    {
    case XFBFrame::OperationType::Draw:
    {
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
      g_vertex_manager->DrawGraphicsData(frame_buffer_manager, draw_call_data.draw_call_data);
      break;
    }
    case XFBFrame::OperationType::RenderTarget:
    {
      const XFBFrame::RenderTargetData& render_target_data =
          frame_view.render_targets[operation_reference.index];
      if (telemetry)
        telemetry_instance.LogRenderTargetOp(render_target_data.resolved_copy_data);
      if (render_target_data.cache_entry &&
          render_target_data.resolved_copy_data.dest_stride != 0 && replaying_xfb)
      {
        g_texture_cache->CompleteVRAMFramebufferCopy(render_target_data.cache_entry,
                                                     render_target_data.resolved_copy_data);
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
      BPFunctions::ClearScreen(frame_buffer_manager, clear_data.src_rect, clear_data.color_enable,
                               clear_data.alpha_enable, clear_data.z_enable,
                               clear_data.pixel_format, clear_data.color_ar, clear_data.color_gb,
                               clear_data.z_value);
      break;
    }
    case XFBFrame::OperationType::PixelEngine:
      if (telemetry)
        telemetry_instance.LogPixelEngineOp(frame_view.pixel_engine[operation_reference.index]);
      if (!replaying_xfb)
        continue;
      emulated_graphics_handler.PixelEngineOperation(
          frame_view.pixel_engine[operation_reference.index], false, true);
      break;
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
    if (resolved_data.dest_stride != 0)
    {
      DrawXFBFrame();
    }

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
