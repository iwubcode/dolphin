// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModEditor/Panels/ActiveTargetsPanel.h"

#include <algorithm>
#include <unordered_set>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/EnumUtils.h"

#include "VideoCommon/GraphicsModEditor/Controls/MeshExtractWindow.h"
#include "VideoCommon/GraphicsModEditor/EditorEvents.h"
#include "VideoCommon/GraphicsModEditor/EditorTypes.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/CustomMeshAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/CustomPipelineAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/ModifyLight.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/SkipAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/TransformAction.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoEvents.h"

namespace GraphicsModEditor::Panels
{
ActiveTargetsPanel::ActiveTargetsPanel(EditorState& state) : m_state(state)
{
  m_selection_event = EditorEvents::ItemsSelectedEvent::Register(
      [this](const std::set<SelectableType>& selected_targets) {
        if (selected_targets.size() == 1 &&
            std::holds_alternative<EditorAsset*>(*selected_targets.begin()))
        {
          m_selected_nodes.clear();
        }
      },
      "EditorActiveTargetsPanelSelection");
}

void ActiveTargetsPanel::DrawImGui()
{
  // Set the active target panel first use size and position
  const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
  u32 default_window_height = g_presenter->GetTargetRectangle().GetHeight() -
                              ((float)g_presenter->GetTargetRectangle().GetHeight() * 0.1);
  u32 default_window_width = ((float)g_presenter->GetTargetRectangle().GetWidth() * 0.15);
  ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x + default_window_width / 4,
                                 main_viewport->WorkPos.y +
                                     ((float)g_presenter->GetTargetRectangle().GetHeight() * 0.05)),
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(default_window_width, default_window_height),
                           ImGuiCond_FirstUseEver);

  m_selection_list_changed = false;
  ImGui::Begin("Scene Panel");

  std::vector<GraphicsModEditor::RuntimeState::XFBData*> xfbs;

  if (m_freeze_scene_details)
  {
    if (ImGui::Button("Unfreeze Scene List"))
      m_freeze_scene_details = false;
  }
  else
  {
    ImGui::SetItemTooltip("All existing scene elements will stay and no new ones will be added, "
                          "making it easier to view some content");
    if (ImGui::Button("Freeze Scene List"))
      m_freeze_scene_details = true;
  }
  if (m_freeze_scene_details && !m_frozen_xfbs_presented.empty())
  {
    for (const auto& xfb_presented : m_frozen_xfbs_presented)
    {
      xfbs.push_back(&m_frozen_xfb_to_data[xfb_presented]);
    }
  }
  else
  {
    for (const auto& xfb_presented : m_state.m_runtime_data.m_xfbs_presented)
    {
      xfbs.push_back(&m_state.m_runtime_data.m_xfb_to_data[xfb_presented]);
    }

    if (m_freeze_scene_details)
    {
      m_frozen_xfbs_presented = m_state.m_runtime_data.m_xfbs_presented;
      m_frozen_xfb_to_data = m_state.m_runtime_data.m_xfb_to_data;
    }
  }

  ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
  if (ImGui::BeginTabBar("SceneTabs", tab_bar_flags))
  {
    if (ImGui::BeginTabItem("Objects"))
    {
      DrawCallPanel(xfbs);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("EFBs"))
    {
      EFBPanel(xfbs);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Lights"))
    {
      LightPanel(xfbs);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::End();

  if (m_selection_list_changed)
  {
    EditorEvents::ItemsSelectedEvent::Trigger(m_selected_nodes);
  }

  if (m_open_mesh_dump_export_window)
  {
    if (Controls::ShowMeshExtractWindow(m_state.m_scene_dumper, m_last_mesh_dump_request))
    {
      m_open_mesh_dump_export_window = false;
      m_last_mesh_dump_request = {};
    }
  }
}

void ActiveTargetsPanel::DrawCallPanel(
    const std::vector<GraphicsModEditor::RuntimeState::XFBData*>& xfbs)
{
  auto& imgui_io = ImGui::GetIO();
  static constexpr ImGuiTreeNodeFlags base_flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                   ImGuiTreeNodeFlags_SpanAvailWidth;

  std::unordered_set<GraphicsModSystem::DrawCallID> draw_calls_seen;
  for (const auto xfb_data : xfbs)
  {
    for (const auto& draw_call_id_with_time : xfb_data->m_draw_call_ids)
    {
      const auto draw_call_id = draw_call_id_with_time.draw_call_id;
      if (draw_calls_seen.contains(draw_call_id))
        continue;
      draw_calls_seen.insert(draw_call_id);
      const auto& user_data = m_state.m_user_data.m_draw_call_id_to_user_data[draw_call_id];
      const auto target_actions_iter =
          m_state.m_user_data.m_draw_call_id_to_actions.find(draw_call_id);

      ImGuiTreeNodeFlags node_flags;

      if (target_actions_iter == m_state.m_user_data.m_draw_call_id_to_actions.end())
        node_flags = ImGuiTreeNodeFlags_Leaf;
      else
        node_flags = base_flags;

      if (m_selected_nodes.contains(draw_call_id))
        node_flags |= ImGuiTreeNodeFlags_Selected;

      ImGui::Image(m_state.m_editor_data.m_name_to_texture["filled_cube"].get(), ImVec2{25, 25});
      ImGui::SameLine();

      ImGui::SetNextItemOpen(m_open_draw_call_nodes.contains(draw_call_id));
      const std::string id = fmt::to_string(Common::ToUnderlying(draw_call_id));
      std::string_view name;
      if (user_data.m_friendly_name.empty())
        name = id;
      else
        name = user_data.m_friendly_name;
      const bool node_open = ImGui::TreeNodeEx(id.c_str(), node_flags, "%s", name.data());

      bool ignore_target_context_menu = false;
      if (ImGui::IsItemClicked(ImGuiMouseButton_::ImGuiMouseButton_Left) || ImGui::IsItemFocused())
      {
        if (!imgui_io.KeyCtrl)
          m_selected_nodes.clear();
        m_selected_nodes.insert(draw_call_id);
        m_selection_list_changed = true;
      }

      // Normally we would use 'BeginPopupContextItem' but unfortunately we can't logically do this
      // after handling the node state because it gets the _last_ item clicked
      const bool potential_popup =
          ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right);
      if (!node_open)
      {
        m_open_draw_call_nodes.erase(draw_call_id);
      }
      else
      {
        m_open_draw_call_nodes.insert(draw_call_id);
        if (target_actions_iter != m_state.m_user_data.m_draw_call_id_to_actions.end())
        {
          std::vector<GraphicsModAction*> actions_to_delete;
          for (const auto& action : target_actions_iter->second)
          {
            node_flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                         ImGuiTreeNodeFlags_SpanAvailWidth;

            if (m_selected_nodes.contains(action))
              node_flags |= ImGuiTreeNodeFlags_Selected;
            const std::string action_name =
                fmt::format("{}-{}", action->GetFactoryName(), action->GetID());
            ImGui::TreeNodeEx(action_name.c_str(), node_flags, "%s", action_name.c_str());
            if (ImGui::IsItemClicked(ImGuiMouseButton_::ImGuiMouseButton_Left) ||
                ImGui::IsItemFocused())
            {
              if (!imgui_io.KeyCtrl)
                m_selected_nodes.clear();
              m_selected_nodes.insert(action);
              m_selection_list_changed = true;
            }

            if (ImGui::BeginPopupContextItem())
            {
              ignore_target_context_menu = true;
              if (ImGui::Selectable("Delete"))
              {
                actions_to_delete.push_back(action);
              }
              ImGui::EndPopup();
            }
          }

          for (const auto& action_to_delete : actions_to_delete)
          {
            // Removal from reference memory container
            std::erase_if(target_actions_iter->second, [action_to_delete](auto& action_own) {
              return action_own == action_to_delete;
            });
            if (target_actions_iter->second.empty())
            {
              m_state.m_user_data.m_draw_call_id_to_actions.erase(target_actions_iter);
            }

            // Removal from owning memory container
            std::erase_if(m_state.m_user_data.m_actions, [action_to_delete](auto&& action) {
              return action.get() == action_to_delete;
            });

            m_selected_nodes.erase(action_to_delete);
            m_selection_list_changed = true;
          }
        }
        ImGui::TreePop();
      }

      if (!ignore_target_context_menu)
      {
        if (potential_popup)
        {
          if (!ImGui::IsPopupOpen(id.data()))
          {
            ImGui::OpenPopup(id.data());
          }
        }
        if (ImGui::BeginPopup(id.data()))
        {
          if (ImGui::BeginMenu("Add Action"))
          {
            if (ImGui::MenuItem("Transform"))
            {
              auto action = std::make_unique<EditorAction>(std::make_unique<TransformAction>());
              action->SetID(m_state.m_editor_data.m_next_action_id);

              auto& action_refs = m_state.m_user_data.m_draw_call_id_to_actions[draw_call_id];
              action_refs.push_back(action.get());

              m_state.m_user_data.m_actions.push_back(std::move(action));
              m_open_draw_call_nodes.insert(draw_call_id);
              m_state.m_editor_data.m_next_action_id++;
            }

            if (ImGui::MenuItem("Skip Draw"))
            {
              auto action = std::make_unique<EditorAction>(std::make_unique<SkipAction>());
              action->SetID(m_state.m_editor_data.m_next_action_id);

              auto& action_refs = m_state.m_user_data.m_draw_call_id_to_actions[draw_call_id];
              action_refs.push_back(action.get());

              m_state.m_user_data.m_actions.push_back(std::move(action));
              m_open_draw_call_nodes.insert(draw_call_id);
              m_state.m_editor_data.m_next_action_id++;
            }

            if (ImGui::MenuItem("Custom Mesh"))
            {
              auto action = std::make_unique<EditorAction>(
                  std::make_unique<CustomMeshAction>(m_state.m_user_data.m_asset_library));
              action->SetID(m_state.m_editor_data.m_next_action_id);

              auto& action_refs = m_state.m_user_data.m_draw_call_id_to_actions[draw_call_id];
              action_refs.push_back(action.get());

              m_state.m_user_data.m_actions.push_back(std::move(action));
              m_open_draw_call_nodes.insert(draw_call_id);
              m_state.m_editor_data.m_next_action_id++;
            }

            if (ImGui::MenuItem("Custom Pipeline"))
            {
              auto action = std::make_unique<EditorAction>(
                  CustomPipelineAction::Create(m_state.m_user_data.m_asset_library));
              action->SetID(m_state.m_editor_data.m_next_action_id);

              auto& action_refs = m_state.m_user_data.m_draw_call_id_to_actions[draw_call_id];
              action_refs.push_back(action.get());

              m_state.m_user_data.m_actions.push_back(std::move(action));
              m_open_draw_call_nodes.insert(draw_call_id);
              m_state.m_editor_data.m_next_action_id++;
            }
            ImGui::EndMenu();
          }

          if (ImGui::BeginMenu("Export"))
          {
            if (ImGui::MenuItem("Mesh"))
            {
              if (!m_open_mesh_dump_export_window)
              {
                m_last_mesh_dump_request.m_draw_call_ids.insert(draw_call_id);
              }
              m_open_mesh_dump_export_window = true;
            }
            ImGui::EndMenu();
          }
          ImGui::EndPopup();
        }
      }
    }
  }
}

void ActiveTargetsPanel::EFBPanel(
    const std::vector<GraphicsModEditor::RuntimeState::XFBData*>& xfbs)
{
  auto& imgui_io = ImGui::GetIO();
  static constexpr ImGuiTreeNodeFlags base_flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                   ImGuiTreeNodeFlags_SpanAvailWidth;

  for (const auto xfb_data : xfbs)
  {
    for (const auto texture_cache_id : xfb_data->m_texture_cache_ids)
    {
      const auto& runtime_data =
          m_state.m_runtime_data.m_texture_cache_id_to_data[texture_cache_id];

      // TODO: don't skip, just present differently...
      if (runtime_data.texture.texture_type != GraphicsModSystem::TextureType::EFB)
        continue;

      const auto& user_data = m_state.m_user_data.m_texture_cache_id_to_user_data[texture_cache_id];
      const auto target_actions_iter =
          m_state.m_user_data.m_texture_cache_id_to_actions.find(texture_cache_id);

      ImGuiTreeNodeFlags node_flags;

      if (target_actions_iter == m_state.m_user_data.m_texture_cache_id_to_actions.end())
        node_flags = ImGuiTreeNodeFlags_Leaf;
      else
        node_flags = base_flags;

      if (m_selected_nodes.contains(texture_cache_id))
        node_flags |= ImGuiTreeNodeFlags_Selected;

      ImGui::Image(m_state.m_editor_data.m_name_to_texture["filled_cube"].get(), ImVec2{25, 25});
      ImGui::SameLine();

      ImGui::SetNextItemOpen(m_open_texture_call_nodes.contains(texture_cache_id));
      std::string_view name;
      if (user_data.m_friendly_name.empty())
        name = texture_cache_id;
      else
        name = user_data.m_friendly_name;
      const bool node_open =
          ImGui::TreeNodeEx(texture_cache_id.c_str(), node_flags, "%s", name.data());

      bool ignore_target_context_menu = false;
      if (ImGui::IsItemClicked(ImGuiMouseButton_::ImGuiMouseButton_Left) || ImGui::IsItemFocused())
      {
        if (!imgui_io.KeyCtrl)
          m_selected_nodes.clear();
        m_selected_nodes.insert(texture_cache_id);
        m_selection_list_changed = true;
      }
      if (!node_open)
      {
        m_open_texture_call_nodes.erase(texture_cache_id);
      }
      else
      {
        m_open_texture_call_nodes.insert(texture_cache_id);
        if (target_actions_iter != m_state.m_user_data.m_texture_cache_id_to_actions.end())
        {
          std::vector<GraphicsModAction*> actions_to_delete;
          for (const auto& action : target_actions_iter->second)
          {
            node_flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                         ImGuiTreeNodeFlags_SpanAvailWidth;

            if (m_selected_nodes.contains(action))
              node_flags |= ImGuiTreeNodeFlags_Selected;
            const std::string action_name =
                fmt::format("{}-{}", action->GetFactoryName(), action->GetID());
            ImGui::TreeNodeEx(action_name.c_str(), node_flags, "%s", action_name.c_str());
            if (ImGui::IsItemClicked(ImGuiMouseButton_::ImGuiMouseButton_Left) ||
                ImGui::IsItemFocused())
            {
              if (!imgui_io.KeyCtrl)
                m_selected_nodes.clear();
              m_selected_nodes.insert(action);
              m_selection_list_changed = true;
            }

            if (ImGui::BeginPopupContextItem())
            {
              ignore_target_context_menu = true;
              if (ImGui::Selectable("Delete"))
              {
                actions_to_delete.push_back(action);
              }
              ImGui::EndPopup();
            }
          }

          for (const auto& action_to_delete : actions_to_delete)
          {
            // Removal from reference memory container
            std::erase_if(target_actions_iter->second, [action_to_delete](auto& action_own) {
              return action_own == action_to_delete;
            });
            if (target_actions_iter->second.empty())
            {
              m_state.m_user_data.m_texture_cache_id_to_actions.erase(target_actions_iter);
            }

            // Removal from owning memory container
            std::erase_if(m_state.m_user_data.m_actions, [action_to_delete](auto&& action) {
              return action.get() == action_to_delete;
            });

            m_selected_nodes.erase(action_to_delete);
            m_selection_list_changed = true;
          }
        }
        ImGui::TreePop();
      }

      if (!ignore_target_context_menu &&
          runtime_data.texture.texture_type == GraphicsModSystem::TextureType::EFB)
      {
        if (ImGui::BeginPopupContextItem(texture_cache_id.c_str()))
        {
          if (ImGui::MenuItem("Skip Draw"))
          {
            auto action = std::make_unique<EditorAction>(std::make_unique<SkipAction>());
            action->SetID(m_state.m_editor_data.m_next_action_id);

            auto& action_refs = m_state.m_user_data.m_texture_cache_id_to_actions[texture_cache_id];
            action_refs.push_back(action.get());

            m_state.m_user_data.m_actions.push_back(std::move(action));
            m_open_texture_call_nodes.insert(texture_cache_id);
            m_state.m_editor_data.m_next_action_id++;
          }
          ImGui::EndPopup();
        }
        ImGui::OpenPopupOnItemClick(texture_cache_id.c_str(), ImGuiPopupFlags_MouseButtonRight);
      }
    }
  }
}

void ActiveTargetsPanel::LightPanel(
    const std::vector<GraphicsModEditor::RuntimeState::XFBData*>& xfbs)
{
  auto& imgui_io = ImGui::GetIO();
  static constexpr ImGuiTreeNodeFlags base_flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                   ImGuiTreeNodeFlags_SpanAvailWidth;
  for (const auto xfb_data : xfbs)
  {
    for (const auto light_id : xfb_data->m_light_ids)
    {
      const auto& runtime_data = m_state.m_runtime_data.m_light_id_to_data[light_id];
      const auto& user_data = m_state.m_user_data.m_light_id_to_user_data[light_id];
      const auto target_actions_iter =
          m_state.m_user_data.m_light_id_to_reference_actions.find(light_id);

      ImGuiTreeNodeFlags node_flags;

      if (target_actions_iter == m_state.m_user_data.m_light_id_to_reference_actions.end())
        node_flags = ImGuiTreeNodeFlags_Leaf;
      else
        node_flags = base_flags;

      if (m_selected_nodes.contains(light_id))
        node_flags |= ImGuiTreeNodeFlags_Selected;

      // ImGui::SetNextItemWidth(25.0f);
      ImGui::Image(m_state.m_editor_data.m_name_to_texture["filled_cube"].get(), ImVec2{25, 25});
      ImGui::SameLine();

      ImGui::SetNextItemOpen(m_open_light_nodes.contains(light_id));
      const std::string id = fmt::to_string(Common::ToUnderlying(light_id));
      std::string_view name;
      if (user_data.m_friendly_name.empty())
        name = id;
      else
        name = user_data.m_friendly_name;
      const bool node_open = ImGui::TreeNodeEx(id.c_str(), node_flags, "%s", name.data());

      bool ignore_target_context_menu = false;
      if (ImGui::IsItemClicked(ImGuiMouseButton_::ImGuiMouseButton_Left) || ImGui::IsItemFocused())
      {
        if (!imgui_io.KeyCtrl)
          m_selected_nodes.clear();
        m_selected_nodes.insert(light_id);
        m_selection_list_changed = true;
      }
      if (!node_open)
      {
        m_open_light_nodes.erase(light_id);
      }
      else
      {
        m_open_light_nodes.insert(light_id);
        std::vector<GraphicsModAction*> actions_to_delete;
        for (const auto& action : target_actions_iter->second)
        {
          node_flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                       ImGuiTreeNodeFlags_SpanAvailWidth;

          if (m_selected_nodes.contains(action))
            node_flags |= ImGuiTreeNodeFlags_Selected;
          const std::string action_name =
              fmt::format("{}-{}", action->GetFactoryName(), action->GetID());
          ImGui::TreeNodeEx(action_name.c_str(), node_flags, "%s", action_name.c_str());
          if (ImGui::IsItemClicked(ImGuiMouseButton_::ImGuiMouseButton_Left) ||
              ImGui::IsItemFocused())
          {
            if (!imgui_io.KeyCtrl)
              m_selected_nodes.clear();
            m_selected_nodes.insert(action);
            m_selection_list_changed = true;
          }

          if (ImGui::BeginPopupContextItem())
          {
            ignore_target_context_menu = true;
            if (ImGui::Selectable("Delete"))
            {
              actions_to_delete.push_back(action);
            }
            ImGui::EndPopup();
          }
        }

        for (const auto& action_to_delete : actions_to_delete)
        {
          // Removal from reference memory container
          std::erase_if(target_actions_iter->second, [action_to_delete](auto& action_own) {
            return action_own == action_to_delete;
          });
          if (target_actions_iter->second.empty())
          {
            m_state.m_user_data.m_light_id_to_reference_actions.erase(target_actions_iter);
          }

          // Removal from owning memory container
          std::erase_if(m_state.m_user_data.m_actions, [action_to_delete](auto&& action) {
            return action.get() == action_to_delete;
          });

          m_selected_nodes.erase(action_to_delete);
          m_selection_list_changed = true;
        }
        ImGui::TreePop();
      }

      if (!ignore_target_context_menu)
      {
        if (ImGui::BeginPopupContextItem(id.c_str()))
        {
          if (ImGui::MenuItem("Skip"))
          {
            auto action = std::make_unique<EditorAction>(std::make_unique<SkipAction>());
            action->SetID(m_state.m_editor_data.m_next_action_id);

            auto& action_refs = m_state.m_user_data.m_light_id_to_reference_actions[light_id];
            action_refs.push_back(action.get());

            m_state.m_user_data.m_actions.push_back(std::move(action));
            m_open_light_nodes.insert(light_id);
            m_state.m_editor_data.m_next_action_id++;
          }

          if (ImGui::MenuItem("Modify light"))
          {
            float4 color_as_editor;
            color_as_editor[0] = runtime_data.m_color[0] / 255.0;
            color_as_editor[1] = runtime_data.m_color[1] / 255.0;
            color_as_editor[2] = runtime_data.m_color[2] / 255.0;
            color_as_editor[3] = runtime_data.m_color[3] / 255.0;
            auto action = std::make_unique<EditorAction>(std::make_unique<ModifyLightAction>(
                color_as_editor, runtime_data.m_cosatt, runtime_data.m_distatt, runtime_data.m_pos,
                runtime_data.m_dir));
            action->SetID(m_state.m_editor_data.m_next_action_id);

            auto& action_refs = m_state.m_user_data.m_light_id_to_reference_actions[light_id];
            action_refs.push_back(action.get());

            m_state.m_user_data.m_actions.push_back(std::move(action));
            m_open_light_nodes.insert(light_id);
            m_state.m_editor_data.m_next_action_id++;
          }
          ImGui::EndPopup();
        }
        ImGui::OpenPopupOnItemClick(id.c_str(), ImGuiPopupFlags_MouseButtonRight);
      }
    }
  }
}
}  // namespace GraphicsModEditor::Panels
