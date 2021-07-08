// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>

#include "VideoCommon/PEShaderSystem/Runtime/PEShaderApplyOptions.h"
#include "VideoCommon/PEShaderSystem/Runtime/PEShaderOption.h"
#include "VideoCommon/PEShaderSystem/Runtime/RuntimeShaderImpl.h"

class AbstractTexture;
class ShaderCode;

namespace VideoCommon::PE
{
class BuiltinUniforms
{
public:
  BuiltinUniforms();
  std::size_t Size() const;
  void WriteToMemory(u8*& buffer) const;
  void WriteShaderUniforms(ShaderCode& shader_source) const;
  void Update(const ShaderApplyOptions& options, const AbstractTexture* last_pass_texture,
              float last_pass_output_scale);

private:
  std::array<const ShaderOption*, 10> GetOptions() const;
  RuntimeFloatOption<4> m_prev_resolution;
  RuntimeFloatOption<4> m_prev_rect;
  RuntimeFloatOption<4> m_src_resolution;
  RuntimeFloatOption<4> m_window_resolution;
  RuntimeFloatOption<4> m_src_rect;
  RuntimeIntOption<1> m_time;
  RuntimeIntOption<1> m_layer;
  RuntimeFloatOption<1> m_z_depth_near;
  RuntimeFloatOption<1> m_z_depth_far;
  RuntimeFloatOption<1> m_output_scale;
};
}  // namespace VideoCommon::PE
