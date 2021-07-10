// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/PEShaderSystem/Runtime/PEShaderOption.h"

#include <array>
#include <string>

#include "Common/VariantUtil.h"

namespace VideoCommon::PE
{
namespace
{
  template <typename Size>
  struct RuntimeFloatOption<Size> final : public ShaderOption
  {
    using OptionType = std::array<float, Size>;
	OptionType m_value;
  public:
    RuntimeFloatOption(std::string name, bool compile_time_constant, OptionType value)
	  : m_name(std::move(name)), m_evaluate_at_compile_time(compile_time_constant), m_value(std::move(value))
	  {
		  m_size = Size * sizeof(float);
		  m_padding = 4 - Size;
	  }
	  
	  void WriteToMemory(u8*& buffer) const override
	  {
		  std::memcpy(buffer, m_value.data(), m_size);
		  buffer += m_size;
		  buffer += (m_padding * sizeof(float));
	  }
	  
	  void WriteToShaderSource(fmt::memory_buffer& shader_source) const override
	  {
		  if constexpr(Size == 1)
		  {
			  fmt::format_to(shader_source, "float {};\n", m_name);
		  }
		  else
		  {
			  fmt::format_to(shader_source, "float{} {};\n", Size, m_name);
		  }
		 for (int i = 0; i < m_padding; i++)
		 {
			 fmt::format_to(shader_source, "float _{}{}_padding;\n", m_name, i);
		 }
	  }
  };

  template <typename Size>
  struct RuntimeIntOption<Size> final : public ShaderOption
  {
    using OptionType = std::array<u32, Size>;
	OptionType m_value;
  public:
    RuntimeIntOption(std::string name, bool compile_time_constant, OptionType value)
	  : m_name(std::move(name)), m_evaluate_at_compile_time(compile_time_constant), m_value(std::move(value))
	  {
		  m_size = Size * sizeof(u32);
		  m_padding = 4 - Size;
	  }
	  
	  void WriteToMemory(u8*& buffer) const override
	  {
		  std::memcpy(buffer, m_value.data(), m_size);
		  buffer += m_size;
		  buffer += (m_padding * sizeof(u32));
	  }
	  
	  void WriteToShaderSource(fmt::memory_buffer& shader_source) const override
	  {
		  if constexpr(Size == 1)
		  {
			  fmt::format_to(shader_source, "int {};\n", m_name);
		  }
		  else
		  {
			  fmt::format_to(shader_source, "int{} {};\n", Size, m_name);
		  }
		 for (int i = 0; i < m_padding; i++)
		 {
			 fmt::format_to(shader_source, "int _{}{}_padding;\n", m_name, i);
		 }
	  }
  };
}
std::unique_ptr<ShaderOption> ShaderOption::Create(const ShaderConfigOption& config_option)
{
  std::unique_ptr<ShaderOption> result;
std::visit(overloaded{
 [&](EnumChoiceOption o) { result = std::make_unique<RuntimeIntOption<1>>(o.m_common.m_shader_name, o.m_common.m_compile_time_constant, std::array<u32, 1>{o.m_values_for_choices[o.m_index]}); },
 [&](FloatOption<1> o) { result = std::make_unique<RuntimeFloatOption<1>>(o.m_common.m_shader_name, o.m_common.m_compile_time_constant, o.m_value); },
 [&](FloatOption<2> o) { result = std::make_unique<RuntimeFloatOption<2>>(o.m_common.m_shader_name, o.m_common.m_compile_time_constant, o.m_value); },
 [&](FloatOption<3> o) { result = std::make_unique<RuntimeFloatOption<3>>(o.m_common.m_shader_name, o.m_common.m_compile_time_constant, o.m_value); },
 [&](FloatOption<4> o) { result = std::make_unique<RuntimeFloatOption<4>>(o.m_common.m_shader_name, o.m_common.m_compile_time_constant, o.m_value); },
 [&](IntOption<1> o) { result = std::make_unique<RuntimeIntOption<1>>(o.m_common.m_shader_name, o.m_common.m_compile_time_constant, o.m_value); },
 [&](IntOption<2> o) { result = std::make_unique<RuntimeIntOption<2>>(o.m_common.m_shader_name, o.m_common.m_compile_time_constant, o.m_value); },
 [&](IntOption<3> o) { result = std::make_unique<RuntimeIntOption<3>>(o.m_common.m_shader_name, o.m_common.m_compile_time_constant, o.m_value); },
 [&](IntOption<4> o) { result = std::make_unique<RuntimeIntOption<4>>(o.m_common.m_shader_name, o.m_common.m_compile_time_constant, o.m_value); },
 [&](ColorOption o) { result = std::make_unique<RuntimeFloatOption<3>>(o.m_common.m_shader_name, o.m_common.m_compile_time_constant, o.m_value); },
 [&](ColorAlphaOption o) { result = std::make_unique<RuntimeFloatOption<4>>(o.m_common.m_shader_name, o.m_common.m_compile_time_constant, o.m_value); }
}, config_option);
return result;
}

bool ShaderOption::EvaluateAtCompileTime() const
{
	return m_evaluate_at_compile_time;
}

std::size_t ShaderOption::Size() const
{
	return m_size;
}
