// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>

#include "Common/CommonTypes.h"
#include "VideoCommon/PEShaderSystem/Config/PEShaderConfigInput.h"
#include "VideoCommon/RenderState.h"

class AbstractTexture;

namespace VideoCommon::PE
{
enum class InputType : u32
{
  ExternalImage,       // external image loaded from file
  ColorBuffer,         // colorbuffer at internal resolution
  DepthBuffer,         // depthbuffer at internal resolution
  PreviousPassOutput,  // output of the previous pass
  PreviousShaderOutput,  // output of the previous shader or the color buffer
  PassOutput           // output of a previous pass
};

struct BaseShaderPass;

class ShaderInput
{
public:
  static std::unique_ptr<ShaderInput> Create(const ShaderConfigInput& input_config);
  ShaderInput(InputType type, u32 texture_unit, SamplerState state);
  virtual ~ShaderInput() = default;
  ShaderInput(const ShaderInput&) = default;
  ShaderInput(ShaderInput&&) = default;
  ShaderInput& operator=(const ShaderInput&) = default;
  ShaderInput& operator=(ShaderInput&&) = default;
  virtual void Update(const AbstractTexture* previous_texture,
                      const std::vector<BaseShaderPass*>& passes) = 0;

  InputType GetType() const;
  u32 GetTextureUnit() const;
  SamplerState GetSamplerState() const;
  virtual const AbstractTexture* GetTexture() const = 0;

private:
  InputType m_type;
  u32 m_texture_unit;
  SamplerState m_sampler_state;
};
}  // namespace VideoCommon::PE
