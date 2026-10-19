// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/NativeEFBSync.h"

#include "Common/Logging/Log.h"

#include "VideoCommon/EmulatedGraphicsHandler.h"
#include "VideoCommon/GraphicsModSystem/Runtime/DeferredPerfTelemetry.h"
#include "VideoCommon/GraphicsModSystem/Runtime/ReplayExecutor.h"
#include "VideoCommon/GraphicsModSystem/Runtime/ScopedRenderState.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoConfig.h"

NativeEFBSync::NativeEFBSync(FramebufferManager* fbm) : m_fbm(fbm)
{
}

void NativeEFBSync::SyncTo(const XFBCopyData::View& view,
                           const std::vector<XFBCopyData::UniformState>& uniforms,
                           std::size_t target_op_index)
{
  if (m_synced_up_to >= target_op_index)
    return;

  // Begin viewport-write capture BEFORE Execute so that the seed application done by
  // the executor's scope-entry guard is captured under this pass name.
  if (DeferredPerf::kTelemetryEnabled && DeferredPerf::kViewportWriteCapture)
    DeferredPerf::Telemetry::Get().BeginViewportWriteCapture("native_efb");

  const bool attempt_copy_efb_to_ram = false;
  EmulatedGraphicsHandler emulated_graphics_handler(m_fbm, attempt_copy_efb_to_ram);

  // Build the plan: cursor range, native EFB target, Skip, impacts=false.
  // impacts=false because the plan's only CPU-visible effect (the EFB->RAM copy) is
  // performed by FlushCopy outside the executor, and false makes the copy-path
  // re-entrancy hazard (FlushEFBCopies firing inside SyncTo) structurally impossible.
  ReplayPassPlan plan;
  const std::size_t op_count = target_op_index - m_synced_up_to;
  plan.op_indices.resize(op_count);
  for (std::size_t i = 0; i < op_count; ++i)
    plan.op_indices[i] = m_synced_up_to + i;
  plan.target = m_fbm;
  plan.sv_seed.reset();
  if (m_last_sv.has_value())
    plan.sv_seed.emplace(*m_last_sv);
  plan.data_target = DataTarget::Skip;
  plan.impacts_emulation_state = false;
  plan.interpolation_phase = std::nullopt;
  plan.variant = PipelineVariant::Emulated;

  ReplayExecutor executor;
  executor.Execute(emulated_graphics_handler, plan, view, uniforms, "native_efb");

  if (DeferredPerf::kTelemetryEnabled && DeferredPerf::kViewportWriteCapture)
    DeferredPerf::Telemetry::Get().EndViewportWriteCapture();

  // Persist the viewport/scissor this target was left in, for the next replay.
  ReplayExecutor::UpdateTrackedScissorViewport(view, m_synced_up_to, target_op_index, &m_last_sv);

  m_synced_up_to = target_op_index;
}

void NativeEFBSync::OnClear(EmulatedGraphics::ClearCopyData data,
                            EmulatedGraphics::ScissorsAndViewportData sv, bool is_xfb)
{
  ScopedRenderState guard(m_fbm, m_last_sv);

  const bool attempt_copy_efb_to_ram = false;
  EmulatedGraphicsHandler emulated_graphics_handler(m_fbm, attempt_copy_efb_to_ram);
  emulated_graphics_handler.OnFramebufferClear(data, sv, is_xfb);

  // The native target's GPU state is now at this scissor/viewport. Copy (not move)
  // because the non-XFB path in the handler moves it into the frame record.
  m_last_sv = sv;
}

void NativeEFBSync::DeferCopy(TCacheEntry* entry, std::size_t op_index)
{
  m_deferred_copies[entry] = op_index;
}

void NativeEFBSync::FlushCopy(const XFBCopyData::View& view,
                              const std::vector<XFBCopyData::UniformState>& uniforms,
                              const std::shared_ptr<TCacheEntry>& cache_entry)
{
  auto it = m_deferred_copies.find(cache_entry.get());
  if (it == m_deferred_copies.end())
    return;
  const std::size_t last_operation_index = it->second;
  m_deferred_copies.erase(it);
  if (m_synced_up_to > last_operation_index)
    return;

  // Replay the ops up to the copy's op index into the native EFB.
  SyncTo(view, uniforms, last_operation_index);

  // Complete the RAM copy now that the native EFB has the right content.
  const auto& operation_reference = view.operations[last_operation_index - 1];
  const XFBCopyData::RenderTargetData& render_target_data =
      view.render_targets[operation_reference.index];
  g_texture_cache->CompleteRAMFramebufferCopy(m_fbm, render_target_data.resolved_copy_data,
                                              cache_entry);

  static_cast<IFramebufferRenderer*>(g_texture_cache.get())->FlushFramebufferCopy(cache_entry);
}

void NativeEFBSync::Reset()
{
  m_deferred_copies.clear();
  m_synced_up_to = 0;
}
