// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <type_traits>

#include "Common/CommonTypes.h"

namespace GraphicsModSystem::Config
{
enum class HashAttributes : u8
{
  None = 0,
  Blending = 1 << 1,
  Projection = 1 << 2,
  VertexPosition = 1 << 3,
  VertexTexCoords = 1 << 4,
  VertexLayout = 1 << 5,
  Indices = 1 << 6
};

struct HashPolicy
{
  HashAttributes attributes;
  u64 version;
};

HashAttributes operator|(HashAttributes lhs, HashAttributes rhs);
HashAttributes operator&(HashAttributes lhs, HashAttributes rhs);

HashPolicy GetDefaultHashPolicy();
HashAttributes HashAttributesFromString(const std::string& str);
std::string HashAttributesToString(HashAttributes attributes);

}  // namespace GraphicsModSystem::Config
