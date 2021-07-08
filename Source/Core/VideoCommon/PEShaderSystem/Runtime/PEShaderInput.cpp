// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/PEShaderSystem/Runtime/PEShaderInput.h"

#include <vector>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Image.h"
#include "Common/VariantUtil.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/PEShaderSystem/Runtime/PEBaseShaderPass.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/TextureConfig.h"

namespace VideoCommon::PE
{
namespace
{
class RuntimeImage final : public ShaderInput
{
public:
  static std::unique_ptr<RuntimeImage> Create(std::string path, u32 texture_unit,
                                              SamplerState state)
  {
    File::IOFile file(path, "rb");
    std::vector<u8> buffer(file.GetSize());
    if (!file.IsOpen() || !file.ReadBytes(buffer.data(), file.GetSize()))
      return nullptr;

    struct ExternalImage
    {
      std::vector<u8> data;
      u32 width;
      u32 height;
    };

    ExternalImage image;
    if (!Common::LoadPNG(buffer, &image.data, &image.width, &image.height))
      return nullptr;

    auto texture = g_renderer->CreateTexture(
        TextureConfig(image.width, image.height, 1, 1, 1, AbstractTextureFormat::RGBA8, 0));
    if (!texture)
      return nullptr;

    texture->Load(0, image.width, image.height, image.width, image.data.data(),
                  image.width * sizeof(u32) * image.height);

    return std::make_unique<RuntimeImage>(std::move(texture), texture_unit, state);
  }
  RuntimeImage(std::unique_ptr<AbstractTexture> texture, u32 texture_unit, SamplerState state)
      : ShaderInput(InputType::ExternalImage, texture_unit, state), m_texture(std::move(texture))
  {
  }
  void Update(const AbstractTexture*, const std::vector<BaseShaderPass*>&) override {}

  const AbstractTexture* GetTexture() const override { return m_texture.get(); }

private:
  std::unique_ptr<AbstractTexture> m_texture;
};

class RuntimeGenericInput final : public ShaderInput
{
public:
  RuntimeGenericInput(InputType type, u32 texture_unit, SamplerState state)
      : ShaderInput(type, texture_unit, state)
  {
  }

  void Update(const AbstractTexture*, const std::vector<BaseShaderPass*>&) override {}

  const AbstractTexture* GetTexture() const override { return nullptr; }
};

class RuntimePreviousPass final : public ShaderInput
{
public:
  RuntimePreviousPass(u32 texture_unit, SamplerState state)
      : ShaderInput(InputType::PreviousPassOutput, texture_unit, state)
  {
  }

  void Update(const AbstractTexture* previous_texture, const std::vector<BaseShaderPass*>&) override
  {
    m_texture = previous_texture;
  }

  const AbstractTexture* GetTexture() const override { return m_texture; }

private:
  const AbstractTexture* m_texture = nullptr;
};

class RuntimePass final : public ShaderInput
{
public:
  RuntimePass(u32 index, u32 texture_unit, SamplerState state)
      : ShaderInput(InputType::PassOutput, texture_unit, state), m_index(index)
  {
  }

  void Update(const AbstractTexture*, const std::vector<BaseShaderPass*>& passes) override
  {
    m_texture = passes[m_index]->m_output_texture.get();
  }

  const AbstractTexture* GetTexture() const override { return m_texture; }

private:
  u32 m_index;
  const AbstractTexture* m_texture = nullptr;
};
}  // namespace
std::unique_ptr<ShaderInput> ShaderInput::Create(const ShaderConfigInput& input_config)
{
  std::unique_ptr<ShaderInput> result;
  std::visit(overloaded{[&](UserImage i) {
                          result =
                              RuntimeImage::Create(i.m_path, i.m_texture_unit, i.m_sampler_state);
                        },
                        [&](InternalImage i) {
                          result =
                              RuntimeImage::Create(i.m_path, i.m_texture_unit, i.m_sampler_state);
                        },
                        [&](ColorBuffer i) {
                          result = std::make_unique<RuntimeGenericInput>(
                              InputType::ColorBuffer, i.m_texture_unit, i.m_sampler_state);
                        },
                        [&](DepthBuffer i) {
                          result = std::make_unique<RuntimeGenericInput>(
                              InputType::DepthBuffer, i.m_texture_unit, i.m_sampler_state);
                        },
                        [&](PreviousPass i) {
                          result = std::make_unique<RuntimeGenericInput>(
                              InputType::PreviousPassOutput, i.m_texture_unit, i.m_sampler_state);
                        },
                        [&](PreviousShader i) {
                          result = std::make_unique<RuntimeGenericInput>(
                              InputType::PreviousShaderOutput, i.m_texture_unit, i.m_sampler_state);
                        },
                        [&](ExplicitPass i) {
                          result = std::make_unique<RuntimePass>(i.m_pass_index, i.m_texture_unit,
                                                                 i.m_sampler_state);
                        }},
             input_config);
  return result;
}

ShaderInput::ShaderInput(InputType type, u32 texture_unit, SamplerState state)
    : m_type(type), m_texture_unit(texture_unit), m_sampler_state(state)
{
}

InputType ShaderInput::GetType() const
{
  return m_type;
}

u32 ShaderInput::GetTextureUnit() const
{
  return m_texture_unit;
}

SamplerState ShaderInput::GetSamplerState() const
{
  return m_sampler_state;
}
}  // namespace VideoCommon::PE
