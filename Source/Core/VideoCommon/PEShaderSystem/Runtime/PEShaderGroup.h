// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/PEShaderSystem/Config/PEShaderConfigGroup.h"
#include "VideoCommon/PEShaderSystem/Runtime/PEBaseShader.h"
#include "VideoCommon/PEShaderSystem/Runtime/PEShaderApplyOptions.h"
#include "VideoCommon/TextureConfig.h"

namespace VideoCommon::PE
{
class ShaderGroup
{
public:
  static void Initialize();
  static void Shutdown();
  void UpdateConfig(const ShaderConfigGroup& group);
  void Apply(const ShaderApplyOptions& options);

private:
  bool CreateShaders(const ShaderConfigGroup& group);
  std::optional<u32> m_last_change_count = 0;
  std::vector<std::unique_ptr<BaseShader>> m_shaders;
  bool m_skip = false;

  static inline std::unique_ptr<BaseShader> s_default_shader;
  u32 m_default_target_width;
  u32 m_default_target_height;
  u32 m_default_target_layers;
  AbstractTextureFormat m_default_target_format;

  u32 m_target_width;
  u32 m_target_height;
  u32 m_target_layers;
  AbstractTextureFormat m_target_format;
};
}  // namespace VideoCommon::PE
