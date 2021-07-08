// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <string>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"

namespace VideoCommon::PE
{
  struct CommonOptionContext
  {
    std::string ui_name;
    std::string ui_description;
    std::string shader_name;
    std::string dependent_option;
    std::string group_name;

    bool compile_time_constant = false;
    bool is_pass_dependent_option = false;
  };

  template <typename T>
  struct History
  {
    std::vector<T> m_entries;
  }

  template <typename T>
  struct Snapshots
  {
    std::array<T, 5> m_values;
  }

  struct EnumChoiceOption
  {
    using OptionType = u32;
    CommonOptionContext m_common;
  
    std::vector<std::string> m_ui_choices;
    std::vector<OptionType> m_values_for_choices;
    u32 m_index;
  
    History<OptionType> m_history;
    Snapshots<OptionType> m_snapshots;
  };

  template <typename Size>
  struct FloatOption<Size>
  {
    using OptionType = std::array<float, Size>;
    CommonOptionContext common;

  OptionType default_value;
  OptionType min_value;
  OptionType max_value;
  
  History<OptionType> m_history;
  Snapshots<OptionType> m_snapshots;
};

template <typename Size>
struct IntOption<Size>
{
  using OptionType = std::array<int, Size>;
  CommonOptionContext common;

  OptionType default_value;
  OptionType min_value;
  OptionType max_value;
  OptionType value;
  
  History<OptionType> m_history;
  Snapshots<OptionType> m_snapshots;
};

struct ColorOption final : public FloatOption<3>
{
};

struct ColorAlphaOption final : public FloatOption<4>
{
};

// TODO: float matrix option

// TODO: game-memory option...

using ShaderConfigOption = std::variant<EnumChoiceOption, FloatOption<1>, FloatOption<2>, FloatOption<3>, FloatOption<4>, IntOption<1>, IntOption<2>, IntOption<3>, IntOption<4>, ColorOption, ColorAlphaOption>;

bool IsValid(const ShaderConfigOption& option);
std::size_t GetComponentCount(const ShaderConfigOption& option);
void SerializeToMemory(const ShaderConfigOption& option, u8* buffer);
void SerializeToString(const ShaderConfigOption& option, fmt::memory_buffer& buffer);
void SerializeToJSON(const ShaderConfigOption& option, picojson::object& obj);
ShaderConfigOption DeserializeFromJSON(picojson::object& obj);
}
