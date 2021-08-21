// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <map>
#include <string>
#include <variant>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"
#include "VideoCommon/TextureDecoder.h"

enum class NumericOperation : u32
{
  Exact = 0,
  Any = 1,
  Greater = 2,
  Greater_Equal = 3,
  Less = 4,
  Less_Equal = 5
};

template <>
struct fmt::formatter<NumericOperation> : EnumFormatter<NumericOperation::Less_Equal>
{
  formatter()
      : EnumFormatter({"Exact", "Any", "Greater than", "Greater than or equal to", "Less than",
                       "Less than or equal to"})
  {
  }
};

enum class GenericOperation : u32
{
  Exact = 0,
  Any = 1
};

template <>
struct fmt::formatter<GenericOperation> : EnumFormatter<GenericOperation::Any>
{
  formatter() : EnumFormatter({"Exact", "Any"}) {}
};

enum class MultiGenericOperation : u32
{
  Exact = 0,
  Any = 1,
  OneOf = 2
};

template <>
struct fmt::formatter<MultiGenericOperation> : EnumFormatter<MultiGenericOperation::OneOf>
{
  formatter() : EnumFormatter({"Exact", "Any", "OneOf"}) {}
};

struct EFBGraphicsTrigger
{
  u32 width;
  NumericOperation width_operation;

  u32 height;
  NumericOperation height_operation;

  std::vector<TextureFormat> formats;
  MultiGenericOperation format_operation;
};

struct TextureLoadGraphicsTrigger
{
  std::string texture_id;
};

struct DrawCall2DGraphicsTrigger
{
  u32 width;
  NumericOperation width_operation;

  u32 height;
  NumericOperation height_operation;

  std::vector<TextureFormat> formats;
  MultiGenericOperation format_operation;

  std::string tlut;
  GenericOperation tlut_operation;

  std::string hash;
  GenericOperation hash_operation;
};

struct DrawCall3DGraphicsTrigger
{
  u32 width;
  NumericOperation width_operation;

  u32 height;
  NumericOperation height_operation;

  std::vector<TextureFormat> formats;
  MultiGenericOperation format_operation;

  std::string tlut;
  GenericOperation tlut_operation;

  std::string hash;
  GenericOperation hash_operation;
};

struct PostGraphicsTrigger
{
};

using GraphicsTrigger =
    std::variant<EFBGraphicsTrigger, TextureLoadGraphicsTrigger, DrawCall2DGraphicsTrigger,
                 DrawCall3DGraphicsTrigger, PostGraphicsTrigger>;

bool LoadFromFile(GraphicsTrigger& trigger, const std::string& file);
void SaveToFile(const GraphicsTrigger& trigger, const std::string& file);
