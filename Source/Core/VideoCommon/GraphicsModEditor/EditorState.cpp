// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModEditor/EditorState.h"

#include <chrono>

#include <fmt/format.h>

#include "Common/EnumUtils.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/VariantUtil.h"

#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"
#include "VideoCommon/GraphicsModSystem/Constants.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionFactory.h"

namespace GraphicsModEditor
{
void WriteToGraphicsMod(const UserData& user_data, GraphicsModSystem::Config::GraphicsMod* config)
{
  config->m_title = user_data.m_title;
  config->m_author = user_data.m_author;
  config->m_description = user_data.m_description;

  config->m_assets = user_data.m_asset_library->GetAssets(user_data.m_current_mod_path);

  std::map<GraphicsModAction*, std::size_t> action_to_index;
  for (const auto& action : user_data.m_actions)
  {
    GraphicsModSystem::Config::GraphicsModAction action_config;
    action_config.m_factory_name = action->GetFactoryName();

    picojson::object serialized_data;
    action->SerializeToConfig(&serialized_data);
    action_config.m_data = picojson::value{serialized_data};

    config->m_actions.push_back(std::move(action_config));
    action_to_index[action.get()] = config->m_actions.size() - 1;
  }

  for (const auto& [draw_call_id, actions] : user_data.m_draw_call_id_to_actions)
  {
    GraphicsModSystem::Config::IntTarget i_target;
    i_target.m_target_id = Common::ToUnderlying(draw_call_id);

    if (const auto iter = user_data.m_draw_call_id_to_user_data.find(draw_call_id);
        iter != user_data.m_draw_call_id_to_user_data.end())
    {
      i_target.m_name = iter->second.m_friendly_name;
    }
    config->m_targets.push_back(std::move(i_target));

    auto& action_indexes = config->m_target_index_to_action_indexes[config->m_targets.size() - 1];
    for (const auto& action : actions)
    {
      action_indexes.push_back(action_to_index[action]);
    }
  }

  for (const auto& [texture_cache_id, actions] : user_data.m_texture_cache_id_to_actions)
  {
    GraphicsModSystem::Config::StringTarget s_target;
    s_target.m_target_id = texture_cache_id;

    if (const auto iter = user_data.m_texture_cache_id_to_user_data.find(texture_cache_id);
        iter != user_data.m_texture_cache_id_to_user_data.end())
    {
      s_target.m_name = iter->second.m_friendly_name;
    }
    config->m_targets.push_back(std::move(s_target));

    auto& action_indexes = config->m_target_index_to_action_indexes[config->m_targets.size() - 1];
    for (const auto& action : actions)
    {
      action_indexes.push_back(action_to_index[action]);
    }
  }
}

void ReadFromGraphicsMod(UserData* user_data, EditorData* editor_data,
                         const GraphicsModSystem::Config::GraphicsMod& config,
                         const std::string& mod_root)
{
  user_data->m_title = config.m_title;
  user_data->m_author = config.m_author;
  user_data->m_description = config.m_description;
  user_data->m_current_mod_path = mod_root;

  user_data->m_asset_library->AddAssets(config.m_assets, user_data->m_current_mod_path);

  const auto create_action =
      [&](const std::string_view& action_name,
          const picojson::value& json_data) -> std::unique_ptr<GraphicsModAction> {
    auto action =
        GraphicsModActionFactory::Create(action_name, json_data, user_data->m_asset_library);
    if (action == nullptr)
    {
      return nullptr;
    }
    return action;
  };

  for (const auto& action_config : config.m_actions)
  {
    if (auto action = create_action(action_config.m_factory_name, action_config.m_data))
    {
      user_data->m_actions.push_back(std::make_unique<EditorAction>(std::move(action)));
      user_data->m_actions.back().get()->SetID(editor_data->m_next_action_id);
      editor_data->m_next_action_id++;
    }
  }

  // TODO: ...
  for (const auto& tag_config : config.m_tags)
  {
    // Skip internal tags
    // in the future we may want to handle if internal tags
    // change or are removed
    if (tag_config.m_name.starts_with(GraphicsModSystem::internal_tag_prefix))
    {
      continue;
    }
  }

  for (const auto& target : config.m_targets)
  {
    std::visit(
        overloaded{
            [&](const GraphicsModSystem::Config::IntTarget& int_target) {
              const GraphicsModSystem::DrawCallID draw_call_id{int_target.m_target_id};
              auto& actions = user_data->m_draw_call_id_to_actions[draw_call_id];
              actions = {};

              if (int_target.m_name != "")
              {
                user_data->m_draw_call_id_to_user_data[draw_call_id].m_friendly_name =
                    int_target.m_name;
              }
            },
            [&](const GraphicsModSystem::Config::StringTarget& str_target) {
              const GraphicsModSystem::TextureCacheID texture_cache_id{str_target.m_target_id};
              auto& actions = user_data->m_texture_cache_id_to_actions[texture_cache_id];
              actions = {};

              if (str_target.m_name != "")
              {
                user_data->m_texture_cache_id_to_user_data[texture_cache_id].m_friendly_name =
                    str_target.m_name;
              }
            }},
        target);
  }

  for (const auto& [target_index, action_indexes] : config.m_target_index_to_action_indexes)
  {
    std::visit(
        overloaded{[&](const GraphicsModSystem::Config::IntTarget& int_target) {
                     const GraphicsModSystem::DrawCallID draw_call_id{int_target.m_target_id};
                     auto& actions = user_data->m_draw_call_id_to_actions[draw_call_id];

                     for (const auto& action_index : action_indexes)
                     {
                       actions.push_back(user_data->m_actions[action_index].get());
                     }
                   },
                   [&](const GraphicsModSystem::Config::StringTarget& str_target) {
                     const GraphicsModSystem::TextureCacheID texture_cache_id{
                         str_target.m_target_id};
                     auto& actions = user_data->m_texture_cache_id_to_actions[texture_cache_id];

                     for (const auto& action_index : action_indexes)
                     {
                       actions.push_back(user_data->m_actions[action_index].get());
                     }
                   }},
        config.m_targets[target_index]);
  }
}
}  // namespace GraphicsModEditor
