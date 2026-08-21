// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/Matrix.h"
#include "VideoCommon/GraphicsModSystem/Runtime/SkeletonManager.h"
#include "VideoCommon/GraphicsModSystem/Types.h"
#include "VideoCommon/Resources/MaterialResource.h"
#include "VideoCommon/Resources/MeshResource.h"

class IMeshRenderer
{
public:
  virtual void
  DrawEmulatedMesh(const GraphicsModSystem::DrawDataView& draw_data,
                   const Common::Matrix44& custom_transform = Common::Matrix44::Identity()) = 0;
  virtual void
  DrawEmulatedMesh(const GraphicsModSystem::DrawDataView& draw_data,
                   const VideoCommon::MaterialResource::Data& material_data,
                   const Common::Matrix44& custom_transform = Common::Matrix44::Identity()) = 0;
  virtual void DrawCustomMesh(const GraphicsModSystem::DrawDataView& draw_data,
                              GraphicsModSystem::DrawCallID draw_call,
                              const VideoCommon::MeshResource::Data& mesh_data,
                              const Common::Matrix44& custom_transform, bool ignore_mesh_transform,
                              VideoCommon::SkeletonInstance* skeleton_instance) = 0;
};
