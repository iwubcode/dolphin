// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/PEShaderSystem/Config/PETriggerConfig.h"

#include <picojson.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

namespace VideoCommon::PE
{
namespace
{
std::string GetStreamAsString(std::ifstream& stream)
{
  std::stringstream ss;
  ss << stream.rdbuf();
  return ss.str();
}
}  // namespace
void TriggerConfig::LoadFromProfile(const std::string& file_path)
{
  std::ifstream json_stream;
  File::OpenFStream(json_stream, file_path, std::ios_base::in);
  if (!json_stream.is_open())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load trigger json file '{}'", file_path);
    return;
  }

  picojson::value root;
  const auto error = picojson::parse(root, GetStreamAsString(json_stream));

  if (!error.empty())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load trigger json file '{}' due to parse error: {}", file_path,
                  error);
    return;
  }

  std::map<std::string, ShaderConfigGroup> result;
  if (root.is<picojson::array>())
  {
    const auto serialized_triggers = root.get<picojson::array>();

    for (std::size_t i = 0; i < serialized_triggers.size(); i++)
    {
      const auto& serialized_trigger_val = serialized_triggers[i];
      if (serialized_trigger_val.is<picojson::object>())
      {
        const auto& serialized_trigger = serialized_trigger_val.get<picojson::object>();

        if (const auto trigger_name_it = serialized_trigger.find("name"); trigger_name_it != serialized_trigger.end())
        {
          if (const auto shaders_it = serialized_trigger.find("shaders");
              shaders_it != serialized_trigger.end())
          {
            if (shaders_it->second.is<picojson::array>())
            {
              result[trigger_name_it->second.to_str()].DeserializeFromProfile(
                  shaders_it->second.get<picojson::array>());
            }
          }
        }
      }
    }
  }

  if (!result.empty())
  {
    m_trigger_name_to_shader_groups = std::move(result);
    m_changes = 0;
  }
}

void TriggerConfig::SaveToProfile(const std::string& file_path) const
{
  std::ofstream json_stream;
  File::OpenFStream(json_stream, file_path, std::ios_base::out);
  if (!json_stream.is_open())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to open trigger json file '{}' for writing", file_path);
    return;
  }

  picojson::array serialized_triggers;
  for (const auto& [trigger_name, shader_group] : m_trigger_name_to_shader_groups)
  {
    picojson::object serialized_trigger;
    serialized_trigger["name"] = picojson::value{trigger_name};
    shader_group.SerializeToProfile(serialized_trigger["shaders"]);

    serialized_triggers.push_back(picojson::value{serialized_trigger});
  }

  const auto output = picojson::value{serialized_triggers}.serialize(true);
  json_stream << output;
}
}  // namespace VideoCommon::PE
