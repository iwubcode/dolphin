// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <set>

#include "Common/HookableEvent.h"
#include "VideoCommon/GraphicsModEditor/EditorState.h"
#include "VideoCommon/GraphicsModEditor/EditorTypes.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModBackend.h"

namespace GraphicsModEditor
{
class EditorBackend final : public GraphicsModSystem::Runtime::GraphicsModBackend
{
public:
  explicit EditorBackend(EditorState&);
  void OnDraw(const GraphicsModSystem::DrawData& draw_started,
              VertexManagerBase* vertex_manager) override;
  void OnTextureLoad(const GraphicsModSystem::Texture& texture) override;
  void OnTextureCreate(const GraphicsModSystem::Texture& texture) override;
  void OnLight() override;
  void OnFramePresented(const PresentInfo& present_info) override;

private:
  void SelectionOccurred(const std::set<SelectableType>& selection);

  EditorState& m_state;
  Common::EventHook m_selection_event;
  std::set<SelectableType> m_selected_objects;

  bool m_last_draw_gpu_skinned = false;
  GraphicsModSystem::DrawCallID m_last_draw_call_id = GraphicsModSystem::DrawCallID::INVALID;
  GraphicsModSystem::MaterialID m_last_material_id = GraphicsModSystem::MaterialID::INVALID;
};
}  // namespace GraphicsModEditor
