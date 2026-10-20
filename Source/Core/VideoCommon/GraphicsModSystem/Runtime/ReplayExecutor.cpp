// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/ReplayExecutor.h"

#include <string>
#include <unordered_set>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "Core/System.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/EmulatedGraphicsHandler.h"
#include "VideoCommon/GraphicsModSystem/Runtime/CameraModifierService.h"
#include "VideoCommon/GraphicsModSystem/Runtime/DeferredPerfTelemetry.h"
#include "VideoCommon/GraphicsModSystem/Runtime/PresentedFrame.h"
#include "VideoCommon/GraphicsModSystem/Runtime/ScopedRenderState.h"
#include "VideoCommon/GraphicsModSystem/Runtime/XFBCopyData.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoConfig.h"

using GraphicsModSystem::Runtime::g_camera_modifier_service;

void ReplayExecutor::Execute(EmulatedGraphicsHandler& emulated_graphics_handler,
                             const ReplayPassPlan& plan, const XFBCopyData::View& frame_view,
                             std::span<const XFBCopyData::UniformState> uniform_state,
                             const char* pass_name, std::optional<u64> vertex_lifetime_token,
                             ExecutedTextureOperations* executed_texture_ops)
{
  // P1-d: No lerped plan is authoritative (I1 seed)
  ASSERT(!plan.interpolation_phase || !plan.impacts_emulation_state);

  // I6 (relaxed): Vertex-lifetime token — a presented frame is vertex-valid only while no
  // NEWER capture has evicted its vertices. With present-time retention the
  // presented frame can trail the newest capture by a frame (late-flip double buffering),
  // so the check is token <= counter, not equality: the counter only advances on
  // capture, and while paused no captures run, so retained vertices stay resident.
  // Debug-only.
  ASSERT(!vertex_lifetime_token ||
         *vertex_lifetime_token <= GraphicsModSystem::Runtime::GetVertexLifetimeCounter());

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
    telemetry_instance.BeginOpDump(pass_name, plan.sv_seed);
    replay_start = Common::Timer::NowUs();
  }

  // [FB-DBG] Track draws-before-RT to detect EFB-copy-before-draw ordering bugs.
  std::size_t draws_since_last_rt_or_clear = 0;
  std::size_t total_draws_replayed = 0;

  // [FB-DBG] Set of GC addresses whose EFB->VRAM upload has already run during this replay.
  // A draw that samples an EFB/XFB copy NOT in this set is reading a texture whose content
  // has not been uploaded yet (null/stale GPU texture) -> black. This proves the ordering bug.
  std::unordered_set<u32> efb_copies_uploaded;

  // P1-c: Count PE ops executed in this pass (assert zero when impacts == false)
  std::size_t pe_ops_executed = 0;

  // Scope entry: save caller state, apply seed, bind target (contract §2)
  ScopedRenderState guard(plan.target, plan.sv_seed);
  plan.target->BindEFBFramebuffer();

  for (std::size_t op_index : plan.op_indices)
  {
    // Re-establish target binding (permanent contract, D4)
    plan.target->BindEFBFramebuffer();
    // P1-a: Target bound at every op dispatch
    ASSERT(g_gfx->GetCurrentFramebuffer() == plan.target->GetEFBFramebuffer());

    const XFBCopyData::OperationReference& operation_reference = frame_view.operations[op_index];
    if (telemetry)
      telemetry_instance.AddOpReplayed(operation_reference.type);
    switch (operation_reference.type)
    {
    case XFBCopyData::OperationType::Draw:
    {
      draws_since_last_rt_or_clear++;
      total_draws_replayed++;
      const XFBCopyData::DrawData& draw_call_data = frame_view.draws[operation_reference.index];
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
        // Camera truth contract §2.4: the restored field holds the raw base;
        // notify the service so the draw-time hook recomposes from it.
        g_camera_modifier_service.OnProjectionRestored(vertex_shader_manager.constants.projection);
      }
      if (telemetry)
      {
        telemetry_instance.AddDrawReplayed();
        telemetry_instance.LogDrawOp(draw_call_data.draw_call_data,
                                     draw_call_data.geometry_constant_index.has_value(),
                                     draw_call_data.pixel_constant_index.has_value(),
                                     draw_call_data.vertex_constant_index.has_value());
      }
      // TODO Phase 5: interpolation_phase lerp hook
      if (plan.interpolation_phase.has_value())
      {
        // no-op in P1
      }
      // Camera recompose happens in VertexManagerBase::DrawGraphicsData (camera
      // truth contract §2.3), which every replay draw passes through.
      // Ensure any EFB/XFB copy textures sampled by this draw have been uploaded to VRAM.
      // In the deferred path, RT ops may appear before draws in the replay sequence, so a
      // draw can reference an EFB copy whose CompleteVRAMFramebufferCopy hasn't run yet.
      // We trigger the upload on-demand here so the texture has valid content at sample time.
      if (plan.data_target == DataTarget::TextureCache)
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
            g_texture_cache->CompleteVRAMFramebufferCopy(rt.cache_entry, rt.resolved_copy_data);
            found = true;
            break;
          }
          efb_copies_uploaded.insert(e->addr);
          if (found && telemetry)
          {
            ERROR_LOG_FMT(VIDEO,
                          "[FB-DBG]   On-demand EFB copy upload: addr={:#010x} ({}x{}) "
                          "triggered by draw (was not yet uploaded in sequence order)",
                          e->addr, e->native_width, e->native_height);
          }
        }
      }
      g_vertex_manager->DrawGraphicsData(plan.target, draw_call_data.draw_call_data);
      break;
    }
    case XFBCopyData::OperationType::RenderTarget:
    {
      const XFBCopyData::RenderTargetData& render_target_data =
          frame_view.render_targets[operation_reference.index];
      if (telemetry)
        telemetry_instance.LogRenderTargetOp(render_target_data.resolved_copy_data);
      // [FB-DBG] Log render target ops during replay to track where copies complete.
      if (telemetry)
      {
        const auto& rcd = render_target_data.resolved_copy_data;
        ERROR_LOG_FMT(
            VIDEO,
            "[FB-DBG] Replay RT op_idx={} cache_entry={:#x} gc_dest_addr={:#010x} "
            "src_rect=({},{},{}x{}) dest_stride={} replaying_xfb={} w={} h={} copy_to_vram={} "
            "is_xfb={} draws_before_rt={} total_draws_so_far={}",
            operation_reference.index, reinterpret_cast<u64>(render_target_data.cache_entry.get()),
            rcd.dest_address, rcd.src_rect.left, rcd.src_rect.top, rcd.src_rect.GetWidth(),
            rcd.src_rect.GetHeight(), rcd.dest_stride, plan.data_target == DataTarget::TextureCache,
            rcd.width, rcd.height, rcd.copy_to_vram, rcd.is_xfb, draws_since_last_rt_or_clear,
            total_draws_replayed);
        if (draws_since_last_rt_or_clear == 0 && plan.data_target == DataTarget::TextureCache)
          ERROR_LOG_FMT(VIDEO, "[FB-DBG]   !! WARNING: RT copy with ZERO draws since last clear/rt "
                               "- EFB may be empty/black at copy time");
      }
      draws_since_last_rt_or_clear = 0;
      if (render_target_data.cache_entry &&
          render_target_data.resolved_copy_data.dest_stride != 0 &&
          plan.data_target == DataTarget::TextureCache)
      {
        if (telemetry)
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
    case XFBCopyData::OperationType::ScissorViewport:
      if (telemetry)
        telemetry_instance.LogScissorViewportOp(
            frame_view.scissors_viewports[operation_reference.index]);
      emulated_graphics_handler.SetScissorAndViewport(
          frame_view.scissors_viewports[operation_reference.index]);
      break;
    case XFBCopyData::OperationType::Clear:
    {
      const EmulatedGraphics::ClearCopyData& clear_data =
          frame_view.clears[operation_reference.index];
      if (telemetry)
        telemetry_instance.LogClearOp(clear_data);
      // [FB-DBG] Log clear ops to confirm z_enable state during replay.
      if (telemetry)
      {
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
      }
      BPFunctions::ClearScreen(plan.target, clear_data.src_rect, clear_data.color_enable,
                               clear_data.alpha_enable, clear_data.z_enable,
                               clear_data.pixel_format, clear_data.color_ar, clear_data.color_gb,
                               clear_data.z_value);
      draws_since_last_rt_or_clear = 0;
      break;
    }
    case XFBCopyData::OperationType::PixelEngine:
    {
      const auto& pe = frame_view.pixel_engine[operation_reference.index];
      if (telemetry)
        telemetry_instance.LogPixelEngineOp(pe);
      // [FB-DBG] Log PE ops during replay to see what fires before the floor copy.
      if (telemetry)
        ERROR_LOG_FMT(VIDEO,
                      "[FB-DBG] Replay PE op_idx={} type={} reset={} new_value={:#06x} "
                      "cycles_into_future={} (notify_gpu triggers FlushEFBCopies if reset)",
                      operation_reference.index, static_cast<int>(pe.type), pe.reset ? 1 : 0,
                      static_cast<u32>(pe.new_value) & 0xFFFF, pe.cycles_into_the_future);
      if (!plan.impacts_emulation_state)
        continue;
      emulated_graphics_handler.PixelEngineOperation(pe, false, true);
      pe_ops_executed++;
      break;
    }
    case XFBCopyData::OperationType::PixelFormatChange:
      if (telemetry)
        telemetry_instance.LogPixelFormatChangeOp(
            frame_view.pixel_format_changes[operation_reference.index]);
      emulated_graphics_handler.PixelFormatChange(
          frame_view.pixel_format_changes[operation_reference.index]);
      break;
    case XFBCopyData::OperationType::ReinterpretTexture:
      // Class-B (texture-cache-mutating): at most once per (session, copy, op) — P3-b.
      if (executed_texture_ops)
      {
        ASSERT(!executed_texture_ops->contains(op_index));
        executed_texture_ops->insert(op_index);
      }
      if (telemetry)
        telemetry_instance.LogReinterpretTextureOp(
            frame_view.reinterpret_textures[operation_reference.index]);
      emulated_graphics_handler.OnReinterpretTexture(
          frame_view.reinterpret_textures[operation_reference.index]);
      break;
    case XFBCopyData::OperationType::PaletteTexture:
      // Class-B (texture-cache-mutating): at most once per (session, copy, op) — P3-b.
      if (executed_texture_ops)
      {
        ASSERT(!executed_texture_ops->contains(op_index));
        executed_texture_ops->insert(op_index);
      }
      if (telemetry)
        telemetry_instance.LogPaletteTextureOp(
            frame_view.palette_textures[operation_reference.index]);
      emulated_graphics_handler.OnPaletteTexture(
          frame_view.palette_textures[operation_reference.index]);
      break;
    case XFBCopyData::OperationType::PartialTextureUpdate:
    {
      // Class-B (texture-cache-mutating): at most once per (session, copy, op) — P3-b.
      if (executed_texture_ops)
      {
        ASSERT(!executed_texture_ops->contains(op_index));
        executed_texture_ops->insert(op_index);
      }
      if (telemetry)
        telemetry_instance.LogPartialTextureUpdateOp(
            frame_view.partial_texture_updates[operation_reference.index]);
      emulated_graphics_handler.OnPartialTextureUpdate(
          frame_view.partial_texture_updates[operation_reference.index]);
      break;
    }
    case XFBCopyData::OperationType::Max:
      break;  // Unreachable
    }
  }

  // P1-c: PE executes in ≤ 1 pass per frame (I2b seed)
  if (!plan.impacts_emulation_state)
    ASSERT(pe_ops_executed == 0);

  if (telemetry)
  {
    telemetry_instance.AddReplayUs(Common::Timer::NowUs() - replay_start);
    telemetry_instance.EndOpDump();
    telemetry_instance.EndReplay();
  }
}

void ReplayExecutor::UpdateTrackedScissorViewport(
    const XFBCopyData::View& view, std::size_t first, std::size_t last,
    std::optional<EmulatedGraphics::ScissorsAndViewportData>* out)
{
  // Walk backward from the end of the range to find the most recent ScissorViewport.
  // Draws do not modify viewport/scissor state, so this is the value the target was
  // left in after replaying [first, last).
  for (std::size_t i = last; i-- > first;)
  {
    const auto& op = view.operations[i];
    if (op.type == XFBCopyData::OperationType::ScissorViewport)
    {
      *out = view.scissors_viewports[op.index];
      return;
    }
  }
}
