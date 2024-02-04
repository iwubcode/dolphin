// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/SmallVector.h"

#include "VideoCommon/GraphicsModSystem/Types.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/RenderState.h"

class AbstractTexture;

namespace GraphicsModSystem
{
struct DrawData;
}

namespace GraphicsModEditor
{
class SceneDumper
{
public:
  SceneDumper();
  ~SceneDumper();

  struct AdditionalDrawData
  {
    std::span<float> transform;
  };
  bool IsDrawCallInRecording(GraphicsModSystem::DrawCallID draw_call_id) const;
  void AddDataToRecording(GraphicsModSystem::DrawCallID draw_call_id,
                          const GraphicsModSystem::DrawDataView& draw_data,
                          AdditionalDrawData additional_draw_data);

  struct RecordingRequest
  {
    std::unordered_set<GraphicsModSystem::DrawCallID> m_draw_call_ids;

    bool m_enable_blending = false;
    bool m_apply_gpu_skinning = true;
    bool m_include_transform = true;
    bool m_include_materials = true;
  };
  void Record(const std::string& path, const RecordingRequest& request);
  bool IsRecording() const;

  void OnXFBCreated(const std::string& hash);
  void OnFramePresented(std::span<std::string> xfbs_presented);

private:
  enum RecordingState
  {
    NOT_RECORDING,
    WANTS_RECORDING,
    IS_RECORDING
  };
  RecordingState m_recording_state = RecordingState::NOT_RECORDING;

  std::map<std::string, int, std::less<>> m_materialhash_to_material_id;
  std::map<std::string, int, std::less<>> m_texturehash_to_texture_id;
  RecordingRequest m_record_request;
  std::string m_scene_save_path;
  bool m_saw_frame_end = false;

  struct SceneDumperImpl;
  std::unique_ptr<SceneDumperImpl> m_impl;
};
}  // namespace GraphicsModEditor