// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "Common/CommonTypes.h"

#include "VideoCommon/Assets/MeshAsset.h"
#include "VideoCommon/GraphicsModSystem/Types.h"
#include "VideoCommon/Resources/MeshResource.h"

namespace VideoCommon
{
struct SkeletonInstance;
}

namespace GraphicsModSystem
{
void ApplyCPUSkinning(GraphicsModSystem::DrawCallID draw_call,
                      const VideoCommon::MeshResource::MeshChunk& replacement_chunk,
                      const VideoCommon::SkinningRig& replacement_rig,
                      const std::vector<Eigen::Vector3f>& original_positions,
                      const GraphicsModSystem::DrawDataView& native_draw_data,
                      VideoCommon::SkeletonInstance* skeleton, std::vector<u8>* out);
}
