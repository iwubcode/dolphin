// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModRuntimeBackend.h"

#include <variant>

#include "Common/VariantUtil.h"

#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionFactory.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModHash.h"

namespace GraphicsModSystem::Runtime
{
GraphicsModRuntimeBackend::GraphicsModRuntimeBackend(const Config::GraphicsModGroup& config_data)
{
  for (auto& config_mod : config_data.GetMods())
  {
    if (!config_mod.m_enabled)
      continue;

    GraphicsMod runtime_mod;

    runtime_mod.m_hash_policy = config_mod.m_mod.m_default_hash_policy;

    auto filesystem_library = std::make_shared<VideoCommon::DirectFilesystemAssetLibrary>();

    for (const auto& config_asset : config_mod.m_mod.m_assets)
    {
      auto asset_map = config_asset.m_map;
      for (auto& [k, v] : asset_map)
      {
        if (v.is_absolute())
        {
          WARN_LOG_FMT(VIDEO,
                       "Specified graphics mod asset '{}' for mod '{}' has an absolute path, you "
                       "shouldn't release this to users.",
                       config_asset.m_asset_id, config_mod.m_id);
        }
        else
        {
          v = std::filesystem::path{config_mod.m_path} / v;
        }
      }

      filesystem_library->SetAssetIDMapData(config_asset.m_asset_id, std::move(asset_map));
    }

    for (const auto& config_action : config_mod.m_mod.m_actions)
    {
      runtime_mod.m_actions.push_back(GraphicsModActionFactory::Create(
          config_action.m_factory_name, config_action.m_data, filesystem_library));
    }

    std::vector<std::variant<DrawCallID, std::string>> targets;
    for (const auto& target : config_mod.m_mod.m_targets)
    {
      std::visit(overloaded{[&](const Config::IntTarget& int_target) {
                              runtime_mod.m_draw_id_to_actions.insert_or_assign(
                                  DrawCallID{int_target.m_target_id},
                                  std::vector<GraphicsModAction*>{});
                              targets.push_back(DrawCallID{int_target.m_target_id});
                            },
                            [&](const Config::StringTarget& str_target) {
                              runtime_mod.m_str_id_to_actions.insert_or_assign(
                                  str_target.m_target_id, std::vector<GraphicsModAction*>{});
                              targets.push_back(str_target.m_target_id);
                            }},
                 target);
    }

    // Handle any specific actions set on targets
    for (const auto& [target_index, action_indexes] :
         config_mod.m_mod.m_target_index_to_action_indexes)
    {
      std::visit(overloaded{[&](DrawCallID draw_id) {
                              for (const auto& action_index : action_indexes)
                              {
                                runtime_mod.m_draw_id_to_actions[draw_id].push_back(
                                    runtime_mod.m_actions[action_index].get());
                              }
                            },
                            [&](const std::string& str_id) {
                              for (const auto& action_index : action_indexes)
                              {
                                runtime_mod.m_str_id_to_actions[str_id].push_back(
                                    runtime_mod.m_actions[action_index].get());
                              }
                            }},
                 targets[target_index]);
    }

    // Handle any actions set on a target from a global tag
    for (const auto& [target_index, tag_indexes] : config_mod.m_mod.m_target_index_to_tag_indexes)
    {
      std::vector<GraphicsModAction*> actions;
      for (const u64 tag_index : tag_indexes)
      {
        if (const auto iter = config_mod.m_mod.m_tag_index_to_action_indexes.find(tag_index);
            iter != config_mod.m_mod.m_tag_index_to_action_indexes.end())
        {
          for (const auto& action_index : iter->second)
          {
            actions.push_back(runtime_mod.m_actions[action_index].get());
          }
        }
      }
      std::visit(overloaded{[&](DrawCallID draw_id) {
                              auto& v = runtime_mod.m_draw_id_to_actions[draw_id];
                              v.insert(v.end(), std::make_move_iterator(actions.begin()),
                                       std::make_move_iterator(actions.end()));
                            },
                            [&](const std::string& str_id) {
                              auto& v = runtime_mod.m_str_id_to_actions[str_id];
                              v.insert(v.end(), std::make_move_iterator(actions.begin()),
                                       std::make_move_iterator(actions.end()));
                            }},
                 targets[target_index]);
    }
  }
}

void GraphicsModRuntimeBackend::OnDraw(const DrawData& draw_data, VertexManagerBase* vertex_manager)
{
  if (m_mods.empty())
  {
    vertex_manager->DrawEmulatedMesh();
    return;
  }

  for (auto& mod : m_mods)
  {
    const auto hash_output = GetDrawDataHash(mod.m_hash_policy, draw_data);

    if (const auto iter = mod.m_draw_id_to_actions.find(hash_output.draw_call_id);
        iter != mod.m_draw_id_to_actions.end())
    {
      CustomDraw(draw_data, vertex_manager, iter->second);
      break;
    }
  }
}

void GraphicsModRuntimeBackend::OnTextureLoad(const Texture& texture)
{
  for (auto& mod : m_mods)
  {
    if (const auto iter = mod.m_str_id_to_actions.find(texture.hash_name);
        iter != mod.m_str_id_to_actions.end())
    {
      GraphicsModActionData::TextureLoad load{.texture_name = texture.hash_name};
      for (const auto& action : iter->second)
      {
        action->OnTextureLoad(&load);
      }
      break;
    }
  }
}

void GraphicsModRuntimeBackend::OnTextureCreate(const Texture& texture)
{
  for (auto& mod : m_mods)
  {
    if (const auto iter = mod.m_str_id_to_actions.find(texture.hash_name);
        iter != mod.m_str_id_to_actions.end())
    {
      GraphicsModActionData::TextureCreate texture_create{.texture_name = texture.hash_name};
      for (const auto& action : iter->second)
      {
        action->OnTextureCreate(&texture_create);
      }
      break;
    }
  }
}

void GraphicsModRuntimeBackend::OnLight()
{
}

void GraphicsModRuntimeBackend::OnFramePresented(const PresentInfo&)
{
  for (auto& mod : m_mods)
  {
    for (const auto& action : mod.m_actions)
    {
      action->OnFrameEnd();
    }
  }
}
}  // namespace GraphicsModSystem::Runtime
