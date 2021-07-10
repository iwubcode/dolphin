// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <string>
#include <variant>
#include <vector>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/CommonTypes.h"

namespace VideoCommon::PE
{
  struct CommonOptionContext
  {
    std::string m_ui_name;
    std::string m_ui_description;
    std::string m_shader_name;
    std::string m_dependent_option;
    std::string m_group_name;

    bool m_compile_time_constant = false;
    bool m_is_pass_dependent_option = false;
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
    CommonOptionContext m_common;

  OptionType m_default_value;
  OptionType m_min_value;
  OptionType m_max_value;
  OptionType m_value;
  
  History<OptionType> m_history;
  Snapshots<OptionType> m_snapshots;
};

template <typename Size>
struct IntOption<Size>
{
  using OptionType = std::array<u32, Size>;
  CommonOptionContext m_common;

  OptionType m_default_value;
  OptionType m_min_value;
  OptionType m_max_value;
  OptionType m_value;
  
  History<OptionType> m_history;
  Snapshots<OptionType> m_snapshots;
};

struct ColorOption final : public FloatOption<3>
{
};

struct ColorAlphaOption final : public FloatOption<4>
{
};

// TODO: game-memory option...

using ShaderConfigOption = std::variant<EnumChoiceOption, FloatOption<1>, FloatOption<2>, FloatOption<3>, FloatOption<4>, IntOption<1>, IntOption<2>, IntOption<3>, IntOption<4>, ColorOption, ColorAlphaOption>;

bool IsValid(const ShaderConfigOption& option);
void SerializeToJSON(const ShaderConfigOption& option, picojson::object& obj);
ShaderConfigOption DeserializeFromJSON(picojson::object& obj);
}
