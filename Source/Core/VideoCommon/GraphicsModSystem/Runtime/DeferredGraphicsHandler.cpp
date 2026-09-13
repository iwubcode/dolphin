// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/DeferredGraphicsHandler.h"

#include <iterator>
#include <string>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "Core/Core.h"
#include "Core/System.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/EmulatedGraphicsHandler.h"
#include "VideoCommon/GraphicsModSystem/Runtime/DeferredPerfTelemetry.h"
#include "VideoCommon/GraphicsModSystem/Runtime/ReplayExecutor.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
// File-local range builders (D12). BuildScrubRange is the ONLY FindOperation call
// site; the replay loop never sees global indices.
GraphicsModSystem::Runtime::PresentedFrameCopyRange
BuildFullRange(const GraphicsModSystem::Runtime::PresentedFrame& frame)
{
  GraphicsModSystem::Runtime::PresentedFrameCopyRange range;
  range.copies.reserve(frame.copies.size());
  for (std::size_t copy_index = 0; copy_index < frame.copies.size(); ++copy_index)
    range.copies.push_back({copy_index, frame.copies[copy_index].GetView().operations.size()});
  return range;
}

GraphicsModSystem::Runtime::PresentedFrameCopyRange
BuildScrubRange(const GraphicsModSystem::Runtime::PresentedFrame& frame, std::size_t global_op)
{
  const auto op = frame.FindOperation(global_op);
  GraphicsModSystem::Runtime::PresentedFrameCopyRange range;
  range.copies.reserve(op.copy_index + 1);
  for (std::size_t copy_index = 0; copy_index < op.copy_index; ++copy_index)
    range.copies.push_back({copy_index, frame.copies[copy_index].GetView().operations.size()});
  range.copies.push_back({op.copy_index, op.local_op_count});
  return range;
}
}  // namespace

DeferredGraphicsHandler::DeferredGraphicsHandler(XFBCopyData* frame,
                                                 FramebufferManager* native_efb_framebuffer_manager)
    : m_xfb_copy_data(frame),
      m_native_sync(native_efb_framebuffer_manager ?
                        std::make_unique<NativeEFBSync>(native_efb_framebuffer_manager) :
                        nullptr),
      m_graphics_mod_handler(g_framebuffer_manager.get(), true)
{
}

RcTcacheEntry DeferredGraphicsHandler::OnFramebufferCopy(
    IFramebufferRenderer* renderer, PixelShaderManager& pixel_shader_manager,
    EmulatedGraphics::FramebufferCopyResolvedData resolved_data)
{
  if (resolved_data.is_xfb)
  {
    ExecuteXFBOperations();

    m_last_clear_copy_data.reset();

    // Note C2: copy resolved_data before the move; attach after the mod handler
    // returns so the entry is the one the mod handler produced.
    const auto copy_data = resolved_data;
    const auto entry = m_graphics_mod_handler.OnFramebufferCopy(renderer, pixel_shader_manager,
                                                                std::move(resolved_data));
    m_director.AttachXfbCopy(std::move(copy_data), entry);
    return entry;
  }

  const u32 stride = resolved_data.dest_stride;
  const auto entry = m_graphics_mod_handler.OnFramebufferCopy(this, pixel_shader_manager,
                                                              std::move(resolved_data));
  if (m_native_sync && stride != 0)
  {
    if (g_ActiveConfig.bDeferEFBCopies && !g_ActiveConfig.bSkipEFBCopyToRam)
    {
      // Store this draw off, we will render later in a batch
      m_native_sync->DeferCopy(entry.get(), m_xfb_copy_data->GetTotalOperations());
    }
    else
    {
      const auto& uniform_state = m_xfb_copy_data->GetUniformStateChanges();
      const XFBCopyData::View view = m_xfb_copy_data->GetView();
      m_native_sync->SyncTo(view, uniform_state, m_xfb_copy_data->GetTotalOperations());
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

  if (m_native_sync)
  {
    m_native_sync->OnClear(clear_data, scissors_and_viewport_data, is_xfb);
  }

  if (is_xfb)
  {
    m_graphics_mod_handler.OnFramebufferClear(clear_data, scissors_and_viewport_data, is_xfb);
    m_last_clear_copy_data.emplace(clear_data);
    return;
  }

  m_xfb_copy_data->AddClearData(std::move(clear_data));
  m_xfb_copy_data->AddScissorsAndViewportData(std::move(scissors_and_viewport_data));
}

void DeferredGraphicsHandler::SetScissorAndViewport(
    EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data)
{
  m_xfb_copy_data->AddScissorsAndViewportData(std::move(scissors_and_viewport_data));
}

void DeferredGraphicsHandler::PixelEngineOperation(
    EmulatedGraphics::PixelEngineData pixel_engine_data, bool notify_cpu, bool notify_gpu)
{
  const bool force_notify_cpu = true;

  // By default if deferring, we wait to tell the GPU
  bool force_notify_gpu = false;

  // However, if we have a native operation we need to
  // ensure the gpu runs now if requested
  if (m_native_sync)
  {
    force_notify_gpu = notify_gpu;
  }
  m_graphics_mod_handler.PixelEngineOperation(pixel_engine_data, force_notify_cpu,
                                              force_notify_gpu);

  if (!force_notify_gpu)
    m_xfb_copy_data->AddPixelEngineOperationData(std::move(pixel_engine_data));
}

void DeferredGraphicsHandler::PixelFormatChange(
    EmulatedGraphics::PixelFormatChangeData pixel_format_change_data)
{
  m_xfb_copy_data->AddPixelFormatChangeData(std::move(pixel_format_change_data));
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
    m_xfb_copy_data->AddReinterpretTextureData(std::move(reinterpret_texture_data));
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
    m_xfb_copy_data->AddPaletteTextureData(std::move(palette_texture_data));
    return;
  }

  m_graphics_mod_handler.OnPaletteTexture(std::move(palette_texture_data));
}

void DeferredGraphicsHandler::OnPartialTextureUpdate(
    PartialTextureUpdateContext partial_texture_update_context)
{
  m_xfb_copy_data->AddPartialTextureUpdateData(std::move(partial_texture_update_context));
}

std::shared_ptr<TCacheEntry> DeferredGraphicsHandler::RenderFramebufferCopy(
    PixelShaderManager& pixel_shader_manager,
    const EmulatedGraphics::FramebufferCopyResolvedData& resolved_data)
{
  FramebufferManager* chosen_fbm =
      m_native_sync ? m_native_sync->GetFbm() : g_framebuffer_manager.get();

  if (DeferredPerf::kTelemetryEnabled && DeferredPerf::kCopyStreamEnabled)
    DeferredPerf::Telemetry::Get().LogRecordCopy(
        chosen_fbm, chosen_fbm != g_framebuffer_manager.get(), resolved_data);

  RcTcacheEntry deferred_entry = g_texture_cache->DeferFramebufferCopy(chosen_fbm, resolved_data);

  m_xfb_copy_data->AddRenderTargetData(XFBCopyData::RenderTargetData{
      .resolved_copy_data = resolved_data, .cache_entry = deferred_entry});

  return deferred_entry;
}

void DeferredGraphicsHandler::FlushFramebufferCopy(const std::shared_ptr<TCacheEntry>& cache_entry)
{
  if (!m_native_sync)
    return;

  const auto& uniform_state = m_xfb_copy_data->GetUniformStateChanges();
  const XFBCopyData::View view = m_xfb_copy_data->GetView();
  m_native_sync->FlushCopy(view, uniform_state, cache_entry);
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
    m_xfb_copy_data->AddDrawData(std::move(dcd));
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
    m_xfb_copy_data->AddDrawData(std::move(dcd));
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
    m_xfb_copy_data->AddDrawData(std::move(dcd));
  }
}

void DeferredGraphicsHandler::ExecuteXFBOperations()
{
  static std::size_t s_xfb_batch_counter = 0;
  ++s_xfb_batch_counter;

  const bool telemetry = DeferredPerf::kTelemetryEnabled;
  auto& telemetry_instance = DeferredPerf::Telemetry::Get();
  u64 drawxfb_start = 0;
  if (telemetry)
    drawxfb_start = Common::Timer::NowUs();

  // Begin viewport-write capture BEFORE Execute so that the seed application done by
  // the executor's scope-entry guard is captured under this pass name.
  if (telemetry && DeferredPerf::kViewportWriteCapture)
    telemetry_instance.BeginViewportWriteCapture("main");

  // [FB-DBG] Log the seed scissor/viewport applied at replay start.
  if (telemetry)
  {
    if (m_main_scissor_viewport.has_value())
    {
      const auto& sv = *m_main_scissor_viewport;
      ERROR_LOG_FMT(VIDEO,
                    "[FB-DBG] ExecuteXFBOperations seed: scissor_tl=({},{}) scissor_br=({},{}) "
                    "scissor_off=({},{}) vp=(xOrig={},yOrig={},wd={},ht={},zRange={},farZ={})",
                    sv.scissor_top_left.x, sv.scissor_top_left.y, sv.scissor_bottom_right.x,
                    sv.scissor_bottom_right.y, sv.scissor_offset.x, sv.scissor_offset.y,
                    sv.viewport.xOrig, sv.viewport.yOrig, sv.viewport.wd, sv.viewport.ht,
                    sv.viewport.zRange, sv.viewport.farZ);
    }
    else
    {
      ERROR_LOG_FMT(VIDEO,
                    "[FB-DBG] ExecuteXFBOperations seed: NONE (first frame or no prior SV op)");
    }
  }

  // If necessary, we already did our copy to ram, no need to attempt it again
  const bool attempt_copy_efb_to_ram = false;
  EmulatedGraphicsHandler emulated_graphics_handler(g_framebuffer_manager.get(),
                                                    attempt_copy_efb_to_ram);

  const auto& uniform_state = m_xfb_copy_data->GetUniformStateChanges();
  const XFBCopyData::View view = m_xfb_copy_data->GetView();

  if (telemetry)
  {
    telemetry_instance.SetOpsSizeCapacity(static_cast<u32>(view.operations.size()),
                                          static_cast<u32>(view.operations.size()));
    telemetry_instance.SetUniformStateSize(static_cast<u32>(uniform_state.size()));
  }

  // [FB-DBG] Compact op-sequence trace + detailed dump for batches containing the floor.
  if (telemetry)
  {
    std::string seq;
    seq.reserve(view.operations.size());
    bool has_floor = false;
    for (const auto& op : view.operations)
    {
      switch (op.type)
      {
      case XFBCopyData::OperationType::Draw:
        seq += 'D';
        break;
      case XFBCopyData::OperationType::RenderTarget:
        seq += 'R';
        if (view.render_targets[op.index].resolved_copy_data.dest_address == 0x117cf180)
          has_floor = true;
        break;
      case XFBCopyData::OperationType::Clear:
        seq += 'C';
        break;
      case XFBCopyData::OperationType::ScissorViewport:
        seq += 'S';
        break;
      case XFBCopyData::OperationType::PixelEngine:
        seq += 'P';
        break;
      case XFBCopyData::OperationType::PixelFormatChange:
        seq += 'F';
        break;
      case XFBCopyData::OperationType::ReinterpretTexture:
        seq += 'I';
        break;
      case XFBCopyData::OperationType::PaletteTexture:
        seq += 'L';
        break;
      case XFBCopyData::OperationType::PartialTextureUpdate:
        seq += 'U';
        break;
      default:
        seq += '?';
        break;
      }
    }
    ERROR_LOG_FMT(VIDEO, "[FB-DBG] ExecuteXFBOperations batch={} op_seq ({} ops): {}",
                  s_xfb_batch_counter, view.operations.size(), seq);

    // [FB-DBG] Detailed per-op dump for the batch containing the floor copy.
    if (has_floor)
    {
      const int max_dump = 20;  // Only first N ops to keep log manageable
      for (std::size_t i = 0; i < view.operations.size() && static_cast<int>(i) < max_dump; ++i)
      {
        const auto& op = view.operations[i];
        switch (op.type)
        {
        case XFBCopyData::OperationType::Draw:
        {
          const auto& d = view.draws[op.index].draw_call_data;
          ERROR_LOG_FMT(VIDEO, "[FB-DBG]   op[{}] DRAW fb={:#x} idx_count={} base_idx={}", i,
                        reinterpret_cast<u64>(d.frame_buffer), d.textures.size(), d.base_index);
          break;
        }
        case XFBCopyData::OperationType::RenderTarget:
        {
          const auto& r = view.render_targets[op.index].resolved_copy_data;
          ERROR_LOG_FMT(VIDEO,
                        "[FB-DBG]   op[{}] RT dest={:#010x} src=({},{},{}x{}) stride={} xfb={}", i,
                        r.dest_address, r.src_rect.left, r.src_rect.top, r.src_rect.GetWidth(),
                        r.src_rect.GetHeight(), r.dest_stride, r.is_xfb ? 1 : 0);
          break;
        }
        case XFBCopyData::OperationType::Clear:
        {
          const auto& c = view.clears[op.index];
          ERROR_LOG_FMT(VIDEO, "[FB-DBG]   op[{}] CLEAR rect=({},{},{}x{}) C={} A={} Z={}", i,
                        c.src_rect.left, c.src_rect.top, c.src_rect.GetWidth(),
                        c.src_rect.GetHeight(), c.color_enable ? 1 : 0, c.alpha_enable ? 1 : 0,
                        c.z_enable ? 1 : 0);
          break;
        }
        case XFBCopyData::OperationType::ScissorViewport:
        {
          const auto& s = view.scissors_viewports[op.index];
          ERROR_LOG_FMT(VIDEO, "[FB-DBG]   op[{}] SCISSOR tl=({},{}) br=({},{}) off=({},{})", i,
                        s.scissor_top_left.x, s.scissor_top_left.y, s.scissor_bottom_right.x,
                        s.scissor_bottom_right.y, s.scissor_offset.x, s.scissor_offset.y);
          break;
        }
        case XFBCopyData::OperationType::PixelEngine:
        {
          const auto& p = view.pixel_engine[op.index];
          ERROR_LOG_FMT(VIDEO, "[FB-DBG]   op[{}] PE type={} reset={}", i, static_cast<int>(p.type),
                        p.reset ? 1 : 0);
          break;
        }
        case XFBCopyData::OperationType::PixelFormatChange:
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

  // Build the plan via the director (D7: the live path routes through BuildDrawPlan).
  const ReplayPassPlan plan =
      m_director.BuildDrawPlan(view, view.operations.size(), m_main_scissor_viewport);

  ReplayExecutor executor;
  executor.Execute(emulated_graphics_handler, plan, view, uniform_state, "main");

  // Caller-side postcondition: re-bind the main EFB (free via backend pointer-compare
  // early-return). The executor's uniform postcondition restores the caller's state.
  g_framebuffer_manager->BindEFBFramebuffer();

  if (telemetry && DeferredPerf::kViewportWriteCapture)
    telemetry_instance.EndViewportWriteCapture();

  ReplayExecutor::UpdateTrackedScissorViewport(view, 0, view.operations.size(),
                                               &m_main_scissor_viewport);

  // Capture the flushed ops as a pending copy (move-out via AddData). The copy
  // reaches the ring only at present time (finalize).
  m_director.StoreXFBCopyOperations(*m_xfb_copy_data);

  if (telemetry)
    telemetry_instance.AddDrawXfbUs(Common::Timer::NowUs() - drawxfb_start);

  if (m_native_sync)
  {
    m_native_sync->Reset();
  }
}

void DeferredGraphicsHandler::RequestScrubPrefix(std::optional<std::size_t> global_op)
{
  // P3-c: scrubbing is paused-only. nullopt (un-scrub) clears unconditionally.
  if (global_op.has_value())
    ASSERT(Core::GetState(Core::System::GetInstance()) == Core::State::Paused);

  // Clamp at request time (D12, FifoPlayer SetFrameRange* precedent): the replay
  // loop may assume a stored value is always < TotalOperations() of the current
  // frame. Safe: while paused the ring slot is static, and a new frame clears the
  // position anyway (session reset in ReplayPresentedFrame).
  if (global_op.has_value())
  {
    const GraphicsModSystem::Runtime::PresentedFrame* frame = m_director.GetRing().Latest();
    if (!frame || *global_op >= frame->TotalOperations())
      global_op.reset();
  }

  // Explicit un-scrub: reset the executed-set session so a full replay re-runs
  // every Class-B op (contract §4).
  if (!global_op && m_scrub_global_op.has_value())
  {
    m_scrub_executed_texture_ops.clear();
    if (const GraphicsModSystem::Runtime::PresentedFrame* frame = m_director.GetRing().Latest())
    {
      ASSERT(frame->copies.size() <= m_scrub_executed_texture_ops.capacity());
      m_scrub_executed_texture_ops.resize(frame->copies.size());
    }
  }

  m_scrub_global_op = global_op;
}

void DeferredGraphicsHandler::ResetScrubSession()
{
  m_scrub_global_op.reset();
  m_scrub_session_frame = 0;
  m_scrub_executed_texture_ops.clear();
}

void DeferredGraphicsHandler::ReplayPresentedFrame()
{
  const GraphicsModSystem::Runtime::PresentedFrame* frame = m_director.GetRing().Latest();
  if (!frame || frame->copies.empty())
    return;  // ring empty — nothing finalized yet

  // Session reset on new presented frame (frame_number changed) — executed sets
  // are per session (contract §3).
  if (frame->frame_number != m_scrub_session_frame)
  {
    m_scrub_session_frame = frame->frame_number;
    m_scrub_executed_texture_ops.clear();
    ASSERT(frame->copies.size() <= m_scrub_executed_texture_ops.capacity());
    m_scrub_executed_texture_ops.resize(frame->copies.size());  // size == copies.size() from here on
    m_scrub_global_op.reset();                          // new frame clears a stale scrub position
  }

  // Range normalization (D12): resolve the scrub position ONCE into a per-copy
  // prefix list. A stored m_scrub_global_op is guaranteed < TotalOperations() by
  // the clamp in RequestScrubPrefix, so no validity checks survive in the loop.
  // The range is a local, rebuilt every tick — never stored, never stale.
  const bool full = !m_scrub_global_op;
  const GraphicsModSystem::Runtime::PresentedFrameCopyRange range =
      full ? BuildFullRange(*frame) : BuildScrubRange(*frame, *m_scrub_global_op);

  const bool attempt_copy_efb_to_ram = false;
  EmulatedGraphicsHandler emulated_graphics_handler(g_framebuffer_manager.get(),
                                                    attempt_copy_efb_to_ram);

  // Replay the range against the persistent main EFB — exactly how the live path
  // composes a multi-copy frame, with a possibly truncated last copy. Then
  // complete each covered copy's XFB copy so the presenter's stitch-by-overlap
  // picks up the rebuilt content, including the presented container.
  ReplayExecutor executor;
  std::optional<EmulatedGraphics::ScissorsAndViewportData> seed;  // copy 0: no seed
  for (const GraphicsModSystem::Runtime::XFBCopyRange& entry : range.copies)
  {
    const GraphicsModSystem::Runtime::XFBCopy& copy = frame->copies[entry.copy_index];
    const ReplayPassPlan plan = m_director.BuildCopyPlan(copy, seed, entry.op_count);
    executor.Execute(emulated_graphics_handler, plan, copy.GetView(),
                     copy.m_data.GetUniformStateChanges(), full ? "paused_replay" : "scrub_replay",
                     frame->vertex_lifetime_token,
                     &m_scrub_executed_texture_ops[entry.copy_index]);

    // Chain the seed like the live path's m_main_scissor_viewport carry-over.
    // For the PARTIAL last copy, chain the prefix's own end SV instead of the
    // full copy's end_sv (which the prefix never reached).
    // optional<ScissorsAndViewportData> is non-assignable (ScissorPos union has
    // deleted copy-assignment): reset + emplace.
    seed.reset();
    if (entry.op_count == copy.GetView().operations.size())
    {
      if (copy.end_sv.has_value())
        seed.emplace(*copy.end_sv);
    }
    else
    {
      ReplayExecutor::UpdateTrackedScissorViewport(copy.GetView(), 0, entry.op_count, &seed);
    }

    // XFB copy re-run (decision D6): a copy's copy re-runs only when the copy was
    // replayed in full, EXCEPT the presented frame's last copy, whose copy always
    // re-runs so the presented image updates at any scrub position.
    const bool copy_full = entry.op_count == copy.GetView().operations.size();
    if ((copy_full || entry.copy_index == frame->copies.size() - 1) && copy.copy_entry &&
        copy.resolved_copy)
    {
      g_texture_cache->CompleteVRAMFramebufferCopy(copy.copy_entry, *copy.resolved_copy);
    }
  }

  if (m_last_clear_copy_data)
  {
    BPFunctions::ClearScreen(g_framebuffer_manager.get(), m_last_clear_copy_data->src_rect,
                             m_last_clear_copy_data->color_enable,
                             m_last_clear_copy_data->alpha_enable, m_last_clear_copy_data->z_enable,
                             m_last_clear_copy_data->pixel_format, m_last_clear_copy_data->color_ar,
                             m_last_clear_copy_data->color_gb, m_last_clear_copy_data->z_value);
  }
}
