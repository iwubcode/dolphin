// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Config/GraphicsTarget.h"

#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/VariantUtil.h"

namespace GraphicsModSystem::Config
{
void SerializeTarget(picojson::object& json_obj, const AnyTarget& target)
{
  std::visit(overloaded{
                 [&](const IntTarget& the_target) {
                   json_obj.emplace("type", "int");
                   json_obj.emplace("id", static_cast<double>(the_target.m_target_id));
                   json_obj.emplace("name", the_target.m_name);
                   json_obj.emplace("tags", ToJsonArray(the_target.m_tag_names));
                 },
                 [&](const StringTarget& the_target) {
                   json_obj.emplace("type", "string");
                   json_obj.emplace("id", the_target.m_target_id);
                   json_obj.emplace("name", the_target.m_name);
                   json_obj.emplace("tags", ToJsonArray(the_target.m_tag_names));
                 },
             },
             target);
}
bool DeserializeTarget(const picojson::object& json_obj, AnyTarget& target)
{
  const auto type = ReadStringFromJson(json_obj, "type");
  if (!type)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, option 'type' was missing or invalid");
    return false;
  }

  if (*type == "int")
  {
    IntTarget i_target;
    i_target.m_name = ReadStringFromJson(json_obj, "name").value_or("");
    i_target.m_target_id = ReadNumericFromJson<u64>(json_obj, "id").value_or(0);
    if (i_target.m_target_id == 0)
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load graphics mod configuration file, option 'id' is invalid");
      return false;
    }
    if (const auto tags_iter = json_obj.find("tags"); tags_iter != json_obj.end())
    {
      if (!tags_iter->second.is<picojson::array>())
      {
        ERROR_LOG_FMT(
            VIDEO,
            "Failed to load graphics mod configuration file, option 'tags' is not an array type");
        return false;
      }

      auto& tags_json = tags_iter->second.get<picojson::array>();
      if (!std::all_of(tags_json.begin(), tags_json.end(),
                       [](const auto& value) { return value.is<std::string>(); }))
      {
        ERROR_LOG_FMT(VIDEO,
                      "Failed to load graphics mod configuration file, all tags are not strings");
        return false;
      }

      for (const auto& tag_json : tags_json)
      {
        i_target.m_tag_names.push_back(tag_json.to_str());
      }
    }
    target = i_target;
  }
  else if (*type == "string")
  {
    StringTarget s_target;
    s_target.m_name = ReadStringFromJson(json_obj, "name").value_or("");

    const auto id = ReadStringFromJson(json_obj, "id");
    if (!id)
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load graphics mod configuration file, option 'id' is invalid");
      return false;
    }
    s_target.m_target_id = *id;
    if (const auto tags_iter = json_obj.find("tags"); tags_iter != json_obj.end())
    {
      if (!tags_iter->second.is<picojson::array>())
      {
        ERROR_LOG_FMT(VIDEO,
                      "Failed to load graphics mod configuration file, option 'tags' is not an array type");
        return false;
      }

      auto& tags_json = tags_iter->second.get<picojson::array>();
      if (!std::all_of(tags_json.begin(), tags_json.end(),
        [](const auto& value) { return value.is<std::string>(); }))
      {
        ERROR_LOG_FMT(
            VIDEO,
            "Failed to load graphics mod configuration file, all tags are not strings");
        return false;
      }

      for (const auto& tag_json : tags_json)
      {
        s_target.m_tag_names.push_back(tag_json.to_str());
      }
    }
    target = s_target;
  }
  else
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Failed to load graphics mod configuration file, option 'type' is invalid value '{}'",
        *type);
    return false;
  }
  return true;
}
}  // namespace GraphicsModSystem::Config
