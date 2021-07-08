// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

namespace VideoCommon::PE
{
  class ShaderOption
  {
  public:
    static std::unique_ptr<ShaderOption> Create(const ShaderConfigOption& config_option);
    virtual void WriteToMemory(u8* buffer) const = 0;
    virtual void WriteToShaderSource(fmt::memory_buffer& buffer) const = 0;
    bool EvaluateAtCompileTime() const;
    std::size_t Size() const;
  protected:
    bool m_evaluate_at_compile_time = false;
    std::size_t m_size;
  };
}