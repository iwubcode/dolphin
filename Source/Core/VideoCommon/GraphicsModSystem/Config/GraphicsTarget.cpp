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
                 },
                 [&](const StringTarget& the_target) {
                   json_obj.emplace("type", "string");
                   json_obj.emplace("id", the_target.m_target_id);
                   json_obj.emplace("name", the_target.m_name);
                 },
             },
             target);
}
bool DeserializeTarget(const picojson::object& json_obj, AnyTarget& target)
{
  const auto type_iter = json_obj.find("type");
  if (type_iter == json_obj.end())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, option 'type' not found");
    return false;
  }
  if (!type_iter->second.is<std::string>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load mod configuration file, option 'type' is not a string type");
    return false;
  }
  const std::string& type = type_iter->second.get<std::string>();
  if (type == "int")
  {
    IntTarget i_target;
    i_target.m_name = ReadStringOrDefault(json_obj, "name");
    i_target.m_target_id = ReadNumericOrDefault<u64>(json_obj, "id");
    if (i_target.m_target_id == 0)
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load graphics mod configuration file, option 'id' is invalid");
      return false;
    }
    target = i_target;
  }
  else if (type == "string")
  {
    StringTarget s_target;
    s_target.m_name = ReadStringOrDefault(json_obj, "name");
    s_target.m_target_id = ReadStringOrDefault(json_obj, "id");
    if (s_target.m_target_id == "")
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load graphics mod configuration file, option 'id' is invalid");
      return false;
    }
    target = s_target;
  }
  else
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Failed to load graphics mod configuration file, option 'type' is invalid value '{}'",
        type);
    return false;
  }
  return true;
}
}  // namespace GraphicsModSystem::Config
