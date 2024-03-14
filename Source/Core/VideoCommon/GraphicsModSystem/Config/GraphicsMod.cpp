// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include "VideoCommon/GraphicsModSystem/Constants.h"

namespace GraphicsModSystem::Config
{
std::optional<GraphicsMod> GraphicsMod::Create(const std::string& file_path)
{
  std::string json_data;
  if (!File::ReadFileToString(file_path, json_data))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load graphics mod json file '{}'", file_path);
    return std::nullopt;
  }

  picojson::value root;
  const auto error = picojson::parse(root, json_data);

  if (!error.empty())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load graphics mod json file '{}' due to parse error: {}",
                  file_path, error);
    return std::nullopt;
  }

  GraphicsMod result;
  if (!result.Deserialize(root))
  {
    return std::nullopt;
  }
  return result;
}

void GraphicsMod::Serialize(picojson::object& json_obj) const
{
  picojson::object serialized_metadata;
  serialized_metadata.emplace("schema_version", static_cast<double>(m_schema_version));
  serialized_metadata.emplace("title", m_title);
  serialized_metadata.emplace("author", m_author);
  serialized_metadata.emplace("description", m_description);
  serialized_metadata.emplace("mod_version", m_mod_version);
  json_obj.emplace("meta", std::move(serialized_metadata));

  picojson::array serialized_assets;
  for (const auto& asset : m_assets)
  {
    picojson::object serialized_asset;
    asset.Serialize(serialized_asset);
    serialized_assets.emplace_back(std::move(serialized_asset));
  }
  json_obj.emplace("assets", std::move(serialized_assets));

  picojson::array serialized_tags;
  for (const auto& tag : m_tags)
  {
    picojson::object serialized_tag;
    tag.Serialize(serialized_tag);
    serialized_tags.emplace_back(std::move(serialized_tag));
  }
  json_obj.emplace("tags", std::move(serialized_tags));

  picojson::array serialized_targets;
  for (const auto& target : m_targets)
  {
    picojson::object serialized_target;
    SerializeTarget(serialized_target, target);
    serialized_targets.emplace_back(std::move(serialized_target));
  }
  json_obj.emplace("targets", std::move(serialized_targets));

  picojson::array serialized_actions;
  for (const auto& action : m_actions)
  {
    picojson::object serialized_action;
    action.Serialize(serialized_action);
    serialized_actions.emplace_back(std::move(serialized_action));
  }
  json_obj.emplace("actions", std::move(serialized_actions));

  // TODO...
  json_obj.emplace("default_hash_policy", HashAttributesToString(m_default_hash_policy.attributes));
}

bool GraphicsMod::Deserialize(const picojson::value& value)
{
  const auto& meta = value.get("meta");
  if (meta.is<picojson::object>())
  {
    const auto& schema_version = meta.get("schema_version");
    if (!schema_version.is<double>())
    {
      ERROR_LOG_FMT(VIDEO, "Failed to deserialize graphics mod data, schema_version is not valid");
      return false;
    }
    m_schema_version = static_cast<u16>(schema_version.get<double>());
    if (m_schema_version != LATEST_SCHEMA_VERSION)
    {
      // For now error, we can handle schema migrations in the future?
      ERROR_LOG_FMT(VIDEO,
                    "Failed to deserialize graphics mod data, schema_version was '{}' but latest "
                    "version is '{}'",
                    m_schema_version, LATEST_SCHEMA_VERSION);
      return false;
    }

    const auto& title = meta.get("title");
    if (title.is<std::string>())
    {
      m_title = title.to_str();
    }

    const auto& author = meta.get("author");
    if (author.is<std::string>())
    {
      m_author = author.to_str();
    }

    const auto& description = meta.get("description");
    if (description.is<std::string>())
    {
      m_description = description.to_str();
    }

    const auto& mod_version = meta.get("mod_version");
    if (mod_version.is<std::string>())
    {
      m_mod_version = mod_version.to_str();
    }
  }

  const auto& assets = value.get("assets");
  if (assets.is<picojson::array>())
  {
    for (const auto& asset_val : assets.get<picojson::array>())
    {
      if (!asset_val.is<picojson::object>())
      {
        ERROR_LOG_FMT(
            VIDEO, "Failed to load mod configuration file, specified asset is not a json object");
        return false;
      }
      GraphicsModAsset asset;
      if (!asset.Deserialize(asset_val.get<picojson::object>()))
      {
        return false;
      }

      m_assets.push_back(std::move(asset));
    }
  }

  const auto& tags = value.get("tags");
  if (tags.is<picojson::array>())
  {
    for (const auto& tag_val : tags.get<picojson::array>())
    {
      if (!tag_val.is<picojson::object>())
      {
        ERROR_LOG_FMT(VIDEO,
                      "Failed to load mod configuration file, specified tag is not a json object");
        return false;
      }
      GraphicsModTag tag;
      if (!tag.Deserialize(tag_val.get<picojson::object>()))
      {
        return false;
      }

      m_tags.push_back(std::move(tag));
    }
  }

  const auto& targets = value.get("targets");
  if (targets.is<picojson::array>())
  {
    for (const auto& target_val : targets.get<picojson::array>())
    {
      if (!target_val.is<picojson::object>())
      {
        ERROR_LOG_FMT(
            VIDEO, "Failed to load mod configuration file, specified target is not a json object");
        return false;
      }

      AnyTarget target;
      if (!DeserializeTarget(target_val.get<picojson::object>(), target))
        return false;

      m_targets.push_back(std::move(target));
    }
  }

  const auto& actions = value.get("actions");
  if (actions.is<picojson::array>())
  {
    for (const auto& action_val : actions.get<picojson::array>())
    {
      if (!action_val.is<picojson::object>())
      {
        ERROR_LOG_FMT(
            VIDEO, "Failed to load mod configuration file, specified action is not a json object");
        return false;
      }
      GraphicsModAction action;
      if (!action.Deserialize(action_val.get<picojson::object>()))
      {
        return false;
      }

      m_actions.push_back(std::move(action));
    }
  }

  return true;
}
}  // namespace GraphicsModSystem::Config
