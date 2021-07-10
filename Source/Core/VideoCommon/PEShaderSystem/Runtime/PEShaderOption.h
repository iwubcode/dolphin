// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>

#include <fmt/format.h>

#include "Common/Types.h"
#include "VideoCommon/PEShaderSystem/Config/PEShaderConfigOption.h"

namespace VideoCommon::PE
{
  class ShaderOption
  {
  public:
    static std::unique_ptr<ShaderOption> Create(const ShaderConfigOption& config_option);
	virtual ~ShaderOption() = default;
	ShaderOption(const ShaderOption&) = default;
	ShaderOption(ShaderOption&&) = default;
	ShaderOption& operator=(const ShaderOption&) = default;
	ShaderOption& operator=(ShaderOption&&) = default;
    virtual void WriteToMemory(u8*& buffer) const = 0;
    virtual void WriteToShaderSource(fmt::memory_buffer& buffer) const = 0;
    bool EvaluateAtCompileTime() const;
    std::size_t Size() const;
  protected:
    bool m_evaluate_at_compile_time = false;
    std::size_t m_size;
  };
}