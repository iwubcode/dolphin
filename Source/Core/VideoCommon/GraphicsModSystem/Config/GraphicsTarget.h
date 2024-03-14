// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <variant>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace GraphicsModSystem::Config
{
struct StringTarget
{
  std::string m_target_id;
  std::string m_name;
  std::vector<std::string> m_tag_names;
};

struct IntTarget
{
  u64 m_target_id;
  std::string m_name;
  std::vector<std::string> m_tag_names;
};

using AnyTarget = std::variant<StringTarget, IntTarget>;

void SerializeTarget(picojson::object& json_obj, const AnyTarget& target);
bool DeserializeTarget(const picojson::object& json_obj, AnyTarget& target);
}  // namespace GraphicsModSystem::Config
