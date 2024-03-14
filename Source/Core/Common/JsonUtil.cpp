// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/JsonUtil.h"

picojson::object ToJsonObject(const Common::Vec3& vec)
{
  picojson::object obj;
  obj.emplace("x", vec.x);
  obj.emplace("y", vec.y);
  obj.emplace("z", vec.z);
  return obj;
}

void FromJson(const picojson::object& obj, Common::Vec3& vec)
{
  vec.x = ReadNumericOrDefault<float>(obj, "x");
  vec.y = ReadNumericOrDefault<float>(obj, "y");
  vec.z = ReadNumericOrDefault<float>(obj, "z");
}

std::string ReadStringOrDefault(const picojson::object& obj, const std::string& key,
                                std::string default_value)
{
  const auto it = obj.find(key);
  if (it == obj.end())
    return default_value;
  if (!it->second.is<std::string>())
    return default_value;
  return it->second.to_str();
}

bool ReadBoolOrDefault(const picojson::object& obj, const std::string& key, bool default_value)
{
  const auto it = obj.find(key);
  if (it == obj.end())
    return default_value;
  if (!it->second.is<bool>())
    return default_value;
  return it->second.get<bool>();
}
