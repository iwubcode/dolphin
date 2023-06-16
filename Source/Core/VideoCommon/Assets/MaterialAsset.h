// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"
#include "VideoCommon/Assets/CustomAsset.h"

namespace VideoCommon
{
struct MaterialProperty
{
  using Value = std::variant<std::string>;
  enum class Type
  {
    Type_Undefined,
    Type_TextureAsset,
    Type_Max = Type_TextureAsset
  };
  std::string m_code_name;
  Type m_type = Type::Type_Undefined;
  std::optional<Value> m_value;
};

struct MaterialData
{
  static bool FromJson(const CustomAssetLibrary::AssetID& asset_id, const picojson::object& json,
                       MaterialData* data);
  std::string shader_asset;
  std::vector<MaterialProperty> properties;
};

class MaterialAsset final : public CustomLoadableAsset<MaterialData>
{
public:
  using CustomLoadableAsset::CustomLoadableAsset;

private:
  CustomAssetLibrary::LoadInfo LoadImpl(const CustomAssetLibrary::AssetID& asset_id) override;
};

}  // namespace VideoCommon

template <>
struct fmt::formatter<VideoCommon::MaterialProperty::Type>
    : EnumFormatter<VideoCommon::MaterialProperty::Type::Type_Max>
{
  constexpr formatter() : EnumFormatter({"Undefined", "Texture"}) {}
};
