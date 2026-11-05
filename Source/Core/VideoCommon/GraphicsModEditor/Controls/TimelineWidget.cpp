// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModEditor/Controls/TimelineWidget.h"

#include <algorithm>

#include <fmt/format.h>
#include <imgui.h>

namespace GraphicsModEditor
{
void TimelineWidget::SetEntries(std::vector<TimelineEntry> entries)
{
  m_entries = std::move(entries);
  m_hover_index = -1;
  m_dragging = false;
  m_drag_start.reset();
  m_drag_current.reset();
  m_last_scrolled_to.reset();
}

void TimelineWidget::SetSelected(std::optional<u32> index)
{
  m_selected = index;
}

ImU32 TimelineWidget::CategoryColor(const std::string& category)
{
  // ABGR (ImGui color convention)
  if (category == "draw")
    return IM_COL32(80, 160, 255, 255);    // blue
  if (category == "clear")
    return IM_COL32(200, 80, 80, 255);     // red
  if (category == "sv")
    return IM_COL32(120, 120, 120, 255);   // gray
  if (category == "copy")
    return IM_COL32(255, 160, 60, 255);    // orange
  if (category == "pe")
    return IM_COL32(200, 200, 80, 255);    // yellow
  if (category == "data")
    return IM_COL32(160, 80, 200, 255);    // purple
  return IM_COL32(100, 100, 100, 255);     // other
}

const char* TimelineWidget::CategoryGlyph(const std::string& category)
{
  if (category == "draw")
    return "D";
  if (category == "clear")
    return "C";
  if (category == "sv")
    return "S";
  if (category == "copy")
    return "R";
  if (category == "pe")
    return "P";
  if (category == "data")
    return "T";
  return ".";
}

void TimelineWidget::DrawImGui()
{
  if (m_entries.empty())
  {
    ImGui::TextDisabled("No operations recorded");
    return;
  }

  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 mouse_pos = io.MousePos;
  const bool mouse_down = io.MouseDown[0];

  const float cell_width = 18.0f;
  const float cell_height = 36.0f;
  const float content_width = cell_width * static_cast<float>(m_entries.size());

  // Horizontally scrolling region for the strip.
  ImGui::BeginChild("##TimelineStrip", ImVec2(0, cell_height + 12.0f), ImGuiChildFlags_Borders);

  const ImVec2 strip_start = ImGui::GetCursorScreenPos();
  // Reserve the full content width so ImGui provides a horizontal scrollbar when the
  // strip is wider than the window (without this, long frames are simply clipped).
  ImGui::Dummy(ImVec2(content_width, cell_height));

  // Determine which cell (if any) the mouse is over.
  int hover = -1;
  if (mouse_pos.x >= strip_start.x && mouse_pos.y >= strip_start.y &&
      mouse_pos.y <= strip_start.y + cell_height)
  {
    const float rel_x = mouse_pos.x - strip_start.x;
    const int candidate = static_cast<int>(rel_x / cell_width);
    if (candidate >= 0 && candidate < static_cast<int>(m_entries.size()))
      hover = candidate;
  }

  // Drag selection: track the drag start and current cell while the mouse is down.
  if (hover >= 0 && mouse_down)
  {
    if (!m_dragging)
    {
      m_dragging = true;
      m_drag_start = m_entries[hover].index;
      m_drag_current = m_entries[hover].index;
    }
    else
    {
      m_drag_current = m_entries[hover].index;
    }
  }
  else if (m_dragging && !mouse_down)
  {
    // Release: emit the selection.
    m_dragging = false;
    if (m_drag_start && m_drag_current)
    {
      const u32 first = std::min(*m_drag_start, *m_drag_current);
      const u32 last = std::max(*m_drag_start, *m_drag_current);
      if (first == last)
        entry_selected_event.Trigger(first);
      else
        range_selected_event.Trigger({first, last});
      m_selected = first;
    }
    m_drag_start.reset();
    m_drag_current.reset();
  }

  // Draw each cell.
  for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
  {
    const TimelineEntry& entry = m_entries[i];
    const ImVec2 cell_pos(strip_start.x + i * cell_width, strip_start.y);
    const ImVec2 cell_size(cell_width, cell_height);

    // Copy-boundary marker: a thin vertical line before this cell.
    if (entry.is_boundary)
    {
      const ImVec2 line_pos(cell_pos.x, cell_pos.y);
      ImGui::GetWindowDrawList()->AddLine(line_pos, ImVec2(line_pos.x, line_pos.y + cell_height),
                                          IM_COL32(255, 255, 255, 128), 1.0f);
    }

    // Cell background: category color, brightened when hovered/selected.
    ImU32 color = CategoryColor(entry.category);
    if (i == hover || (m_selected && *m_selected == entry.index))
    {
      // ImU32 is packed ABGR; brighten RGB by 1.5x, keep alpha.
      const u32 a = (color >> 24) & 0xFF;
      const u32 b = (color >> 16) & 0xFF;
      const u32 g = (color >> 8) & 0xFF;
      const u32 r = color & 0xFF;
      color = IM_COL32(static_cast<int>(std::min<u32>(255, r * 3 / 2)),
                       static_cast<int>(std::min<u32>(255, g * 3 / 2)),
                       static_cast<int>(std::min<u32>(255, b * 3 / 2)),
                       static_cast<int>(a));
    }
    if (m_dragging && m_drag_start && m_drag_current)
    {
      const u32 lo = std::min(*m_drag_start, *m_drag_current);
      const u32 hi = std::max(*m_drag_start, *m_drag_current);
      if (entry.index >= lo && entry.index <= hi)
        color = IM_COL32(255, 255, 255, 100);
    }
    ImGui::GetWindowDrawList()->AddRectFilled(cell_pos, ImVec2(cell_pos.x + cell_size.x,
                                                               cell_pos.y + cell_size.y),
                                              color);

    // Category glyph centered in the cell.
    const char* glyph = CategoryGlyph(entry.category);
    const ImVec2 text_size = ImGui::CalcTextSize(glyph);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(cell_pos.x + (cell_size.x - text_size.x) * 0.5f,
               cell_pos.y + (cell_size.y - text_size.y) * 0.5f),
        IM_COL32(255, 255, 255, 255), glyph);
  }

  // Tooltip for the hovered cell.
  if (hover >= 0)
  {
    const TimelineEntry& entry = m_entries[hover];
    ImGui::SetTooltip("%s",
                      fmt::format("op {} [{}] {}\n{}", entry.index, entry.category,
                                  entry.label, entry.tooltip)
                          .c_str());
  }

  // Keyboard stepping: left/right arrows move the selection by one op.
  if (ImGui::IsItemHovered() && m_selected)
  {
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
    {
      if (*m_selected > 0)
      {
        --(*m_selected);
        entry_selected_event.Trigger(*m_selected);
      }
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
    {
      if (*m_selected + 1 < m_entries.size())
      {
        ++(*m_selected);
        entry_selected_event.Trigger(*m_selected);
      }
    }
  }

  // Keep the selected cell visible: center it whenever the selection changes
  // (click, keyboard step, or panel-driven slider scrub).
  if (m_selected && m_selected != m_last_scrolled_to)
  {
    m_last_scrolled_to = m_selected;
    const auto it = std::ranges::find(m_entries, *m_selected, &TimelineEntry::index);
    if (it != m_entries.end())
    {
      const float cell_x =
          static_cast<float>(std::distance(m_entries.begin(), it)) * cell_width;
      const float visible_width = ImGui::GetWindowWidth();
      if (content_width > visible_width)
      {
        const float target = cell_x - (visible_width - cell_width) * 0.5f;
        ImGui::SetScrollX(std::clamp(target, 0.0f, content_width - visible_width));
      }
    }
  }

  ImGui::EndChild();
}
}  // namespace GraphicsModEditor
