// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModEditor/Panels/ScrubberPanel.h"

#include <fmt/format.h>
#include <imgui.h>

#include "Core/Core.h"
#include "Core/System.h"

#include "VideoCommon/EmulatedGraphicsManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/PresentedFrameEvents.h"
#include "VideoCommon/VideoConfig.h"

namespace GraphicsModEditor::Panels
{
namespace
{
/// Builds the label/tooltip/category for one op from its payload. Display metadata
/// only — copied at publish time, never re-derived from the ring.
TimelineEntry MakeEntry(const XFBCopyData::View& view, const XFBCopyData::OperationReference& op,
                        u32 global_index, bool is_boundary)
{
  TimelineEntry entry;
  entry.index = global_index;
  entry.is_boundary = is_boundary;

  switch (op.type)
  {
  case XFBCopyData::OperationType::Draw:
  {
    const XFBCopyData::DrawData& draw = view.draws[op.index];
    entry.category = "draw";
    // Label from the first texture's hash name (display metadata; the RcTcacheEntry
    // inside the presented frame keeps it alive). No DrawCallID: the editor's
    // runtime state owns draw identity, not the scrubber.
    if (!draw.draw_call_data.textures.empty())
    {
      const auto& tex = draw.draw_call_data.textures[0];
      entry.label = fmt::format("Draw {}", tex.entry ? tex.entry->texture_info_name : "?");
    }
    else
    {
      entry.label = "Draw";
    }
    entry.tooltip = fmt::format(
        "index_size={} base_index={} base_vertex={} prim={} textures={}",
        draw.draw_call_data.index_size, draw.draw_call_data.base_index,
        draw.draw_call_data.base_vertex, fmt::format("{}", draw.draw_call_data.primitive_type),
        draw.draw_call_data.textures.size());
    break;
  }
  case XFBCopyData::OperationType::RenderTarget:
  {
    const auto& rt = view.render_targets[op.index];
    entry.category = "copy";
    entry.label = fmt::format("RT→{:#010x}", rt.resolved_copy_data.dest_address);
    entry.tooltip = fmt::format("src=({},{},{}x{}) stride={} xfb={}",
                                rt.resolved_copy_data.src_rect.left,
                                rt.resolved_copy_data.src_rect.top,
                                rt.resolved_copy_data.src_rect.GetWidth(),
                                rt.resolved_copy_data.src_rect.GetHeight(),
                                rt.resolved_copy_data.dest_stride,
                                rt.resolved_copy_data.is_xfb ? 1 : 0);
    break;
  }
  case XFBCopyData::OperationType::ScissorViewport:
    entry.category = "sv";
    entry.label = "SV";
    break;
  case XFBCopyData::OperationType::Clear:
  {
    const auto& clear = view.clears[op.index];
    entry.category = "clear";
    entry.label = "Clear";
    entry.tooltip = fmt::format("rect=({},{},{}x{}) C={} A={} Z={}", clear.src_rect.left,
                                clear.src_rect.top, clear.src_rect.GetWidth(),
                                clear.src_rect.GetHeight(), clear.color_enable ? 1 : 0,
                                clear.alpha_enable ? 1 : 0, clear.z_enable ? 1 : 0);
    break;
  }
  case XFBCopyData::OperationType::PixelEngine:
  {
    const auto& pe = view.pixel_engine[op.index];
    entry.category = "pe";
    entry.label = "PE";
    entry.tooltip = fmt::format("type={} reset={}", static_cast<int>(pe.type),
                                pe.reset ? 1 : 0);
    break;
  }
  case XFBCopyData::OperationType::PixelFormatChange:
    entry.category = "data";
    entry.label = "PF";
    break;
  case XFBCopyData::OperationType::ReinterpretTexture:
    entry.category = "data";
    entry.label = "RI";
    break;
  case XFBCopyData::OperationType::PaletteTexture:
    entry.category = "data";
    entry.label = "PA";
    break;
  case XFBCopyData::OperationType::PartialTextureUpdate:
    entry.category = "data";
    entry.label = "PU";
    break;
  case XFBCopyData::OperationType::Max:
    entry.category = "other";
    entry.label = "?";
    break;
  }
  return entry;
}
}  // namespace

ScrubberPanel::ScrubberPanel()
{
  m_presented_frame_hook =
      GraphicsModSystem::Runtime::GetPresentedFrameEvents()
          .presented_frame_published_event.Register(
              [this](const GraphicsModSystem::Runtime::PresentedFrame& frame) {
                OnPresentedFramePublished(frame);
              });

  m_entry_selected_hook =
      m_timeline.entry_selected_event.Register([this](u32 global_op) { ScrubTo(global_op); });
  m_range_selected_hook = m_timeline.range_selected_event.Register(
      [this](const std::pair<u32, u32>& range) {
        // v1: a drag range scrubs to its end (the furthest op).
        ScrubTo(range.second);
      });
}

ScrubberPanel::~ScrubberPanel() = default;

void ScrubberPanel::OnPresentedFramePublished(const GraphicsModSystem::Runtime::PresentedFrame& frame)
{
  // Copy display metadata in-callback — the presented-frame pointer is the ring
  // slot, valid only until the next push (contract §1).
  m_frame_number = frame.frame_number;
  m_total_ops = frame.TotalOperations();
  m_copy_starts.clear();
  m_copy_starts.reserve(frame.copies.size());

  std::vector<TimelineEntry> entries;
  entries.reserve(m_total_ops);
  u32 global_index = 0;
  for (std::size_t copy_index = 0; copy_index < frame.copies.size(); ++copy_index)
  {
    const GraphicsModSystem::Runtime::XFBCopy& copy = frame.copies[copy_index];
    const XFBCopyData::View view = copy.GetView();
    m_copy_starts.push_back(global_index);
    for (std::size_t i = 0; i < view.operations.size(); ++i)
    {
      // Boundary marker at each copy start (first op of the copy).
      entries.push_back(MakeEntry(view, view.operations[i], global_index, i == 0));
      ++global_index;
    }
  }

  m_timeline.SetEntries(std::move(entries));
  // A new presented frame invalidates the scrub position (D8); the handler clears
  // its own session on the next replay tick.
  m_selected.reset();
  m_timeline.SetSelected(std::nullopt);
}

void ScrubberPanel::ScrubTo(std::optional<u32> global_op)
{
  m_selected = global_op;
  m_timeline.SetSelected(global_op);
  Core::System::GetInstance()
      .GetEmulatedGraphicsManager()
      .RequestScrubPrefix(global_op ? std::optional<std::size_t>{*global_op} : std::nullopt);
}

void ScrubberPanel::DrawImGui()
{
  if (!ImGui::Begin("Frame Scrubber"))
  {
    ImGui::End();
    return;
  }

  const bool paused = Core::GetState(Core::System::GetInstance()) == Core::State::Paused;
  const bool deferred =
      Core::System::GetInstance().GetEmulatedGraphicsManager().IsDeferredActive();
  const bool scrub_enabled = paused && deferred;

  // ImGui::Text is printf-style; use fmt::format for {} placeholders.
  ImGui::TextUnformatted(
      fmt::format("Frame: {}  Ops: {}", m_frame_number, m_total_ops).c_str());
  if (!scrub_enabled)
  {
    if (!paused)
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Pause to scrub");
    else
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Deferred mode required");
  }

  m_timeline.DrawImGui();

  // Continuous scrub slider over the global op index (drag instead of stepping).
  if (scrub_enabled && m_total_ops > 0)
  {
    u32 slider_value = m_selected.value_or(0);
    u32 slider_min = 0;
    u32 slider_max = static_cast<u32>(m_total_ops - 1);
    if (ImGui::SliderScalar("##ScrubSlider", ImGuiDataType_U32, &slider_value, &slider_min,
                            &slider_max, "op %u"))
      ScrubTo(slider_value);
  }
  else
  {
    ImGui::BeginDisabled();
    u32 dummy_value = 0;
    u32 dummy_min = 0;
    u32 dummy_max = static_cast<u32>(m_total_ops > 0 ? m_total_ops - 1 : 0);
    ImGui::SliderScalar("##ScrubSlider", ImGuiDataType_U32, &dummy_value, &dummy_min,
                        &dummy_max, "op %u");
    ImGui::EndDisabled();
  }

  if (scrub_enabled)
  {
    if (ImGui::Button("Full frame"))
      ScrubTo(std::nullopt);
    ImGui::SameLine();
    if (m_selected)
      ImGui::TextUnformatted(fmt::format("Scrubbed to op {}", *m_selected).c_str());
    else
      ImGui::Text("Full frame");
  }
  else
  {
    ImGui::BeginDisabled();
    ImGui::Button("Full frame");
    ImGui::EndDisabled();
  }

  ImGui::End();
}
}  // namespace GraphicsModEditor::Panels
