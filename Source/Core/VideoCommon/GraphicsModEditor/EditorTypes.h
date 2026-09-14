// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/Matrix.h"

#include "VideoCommon/Assets/CustomAssetLibrary.h"
#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/Assets/DirectFilesystemAssetLibrary.h"
#include "VideoCommon/Assets/MaterialAsset.h"
#include "VideoCommon/Assets/MeshAsset.h"
#include "VideoCommon/Assets/ShaderAsset.h"
#include "VideoCommon/Assets/TextureAsset.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"
#include "VideoCommon/GraphicsModSystem/Types.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/XFMemory.h"

namespace GraphicsModEditor
{
enum class TextureType
{
  Normal,
  EFB,
  XFB
};

// Derives the texture type from immutable TCacheEntry flags. Only usable while the entry is alive.
inline TextureType GetTextureType(const TCacheEntry* entry)
{
  if (!entry)
    return TextureType::Normal;
  if (entry->is_efb_copy)
    return TextureType::EFB;
  if (entry->is_xfb_copy)
    return TextureType::XFB;
  return TextureType::Normal;
}

// Persistent per-texture record for a draw call. Unlike the transient
// GraphicsModSystem::TextureRef, this outlives the TCacheEntry (the editor's draw-call map is never
// pruned), so it stores only entry-independent data: the hash name and a snapshot of the texture
// type taken while the entry was still alive (it cannot be re-derived once the entry has been
// invalidated and pooled).
struct Texture
{
  TextureType texture_type = TextureType::Normal;
  std::string hash_name;
  u8 unit = 0;
};

// Persistent draw-call data, retained for the whole editor session.
struct DrawData
{
  Common::SmallVector<Texture, 8> textures;
  std::array<SamplerState, 8> samplers;

  std::size_t vertex_count = 0;
  std::size_t index_count = 0;

  bool is_gpu_skinning = false;
  u32 pos_normal_index = 0;

  ProjectionType projection_type;
  Projection::Raw projection_mat;
  RasterizationState rasterization_state;
  DepthState depth_state;
  BlendingState blending_state;
  u64 xfb_counter = 0;
};

struct DrawCallData
{
  GraphicsModSystem::DrawCallID id;
  GraphicsModSystem::MaterialID material_id;
  GraphicsModSystem::MeshID mesh_id;
  std::chrono::steady_clock::time_point create_time;
  std::chrono::steady_clock::time_point last_update_time;

  DrawData draw_data;
};

struct TextureCacheData
{
  GraphicsModSystem::TextureCacheID id;
  std::chrono::steady_clock::time_point create_time;
  std::chrono::steady_clock::time_point last_load_time;

  // The shared pointer keeps the TCacheEntry alive, but entry->texture may be null once the GPU
  // texture has been released back to the pool. "Is it viewable" == entry && entry->texture.
  RcTcacheEntry entry;
  u8 unit = 0;
};

struct LightData
{
  GraphicsModSystem::LightID m_id;
  std::chrono::steady_clock::time_point m_create_time;
  std::chrono::steady_clock::time_point m_last_update_time;

  int4 m_color;
  float4 m_cosatt;
  float4 m_distatt;
  float4 m_pos;
  float4 m_dir;
};

struct DrawCallUserData
{
  std::string m_friendly_name;
  std::string m_group;
  Common::Flags<GraphicsModSystem::DrawCallTag> tag_flags;
};

struct TextureCacheUserData
{
  std::string m_friendly_name;
  std::string m_group;
  Common::Flags<GraphicsModSystem::TextureTag> tag_flags;
};

struct LightUserData
{
  std::string m_friendly_name;
  std::vector<std::string> m_tag_names;
};

struct NativeMeshData
{
  struct NativeMeshChunkForDrawCall
  {
    PortableVertexDeclaration vertex_declaration;
    PrimitiveType primitive_type;
    std::vector<u16> index_data;
    std::vector<u8> vertex_data;
    std::vector<Common::Vec3> positions_transformed;
    std::vector<Common::Matrix44> position_inverse_transforms;  // view -> object space
    std::vector<Common::Matrix33> normal_inverse_transforms;    // view -> object space
  };
  std::map<GraphicsModSystem::DrawCallID, NativeMeshChunkForDrawCall> m_draw_call_to_chunk;
};

using EditorAssetData = std::variant<
    std::unique_ptr<VideoCommon::MaterialData>, std::unique_ptr<VideoCommon::TextureAndSamplerData>,
    std::unique_ptr<VideoCommon::MeshData>, std::unique_ptr<VideoCommon::RasterSurfaceShaderData>,
    std::unique_ptr<VideoCommon::RenderTargetData>>;
enum class AssetDataType
{
  Material,
  Mesh,
  PixelShader,
  RenderTarget,
  Shader,
  Texture
};
struct EditorAsset
{
  VideoCommon::CustomAssetLibrary::AssetID m_asset_id;
  std::filesystem::path m_asset_path;
  EditorAssetData m_data;
  AssetDataType m_data_type;
  VideoCommon::CustomAsset::TimeType m_last_data_write;
  VideoCommon::Assets::AssetMap m_asset_map;
  bool m_valid = false;
};

struct TextureCacheWrapper
{
  auto operator<=>(const TextureCacheWrapper&) const = default;
  GraphicsModSystem::TextureCacheID id;
};

struct GroupWrapper
{
  auto operator<=>(const GroupWrapper&) const = default;
  std::string id;
};

using SelectableType =
    std::variant<GraphicsModSystem::DrawCallID, TextureCacheWrapper, GroupWrapper,
                 GraphicsModSystem::LightID, GraphicsModAction*, EditorAsset*>;

class EditorAction final : public GraphicsModAction
{
public:
  explicit EditorAction(std::unique_ptr<GraphicsModAction> action) : m_action(std::move(action)) {}
  void OnDrawStarted(GraphicsModActionData::DrawStarted* draw) override
  {
    if (m_active)
      m_action->OnDrawStarted(draw);
  }
  void BeforeEFB(GraphicsModActionData::PreEFB* efb) override
  {
    if (m_active)
      m_action->BeforeEFB(efb);
  }
  void AfterEFB(GraphicsModActionData::PostEFB* efb) override
  {
    if (m_active)
      m_action->AfterEFB(efb);
  }
  void BeforeXFB() override
  {
    if (m_active)
      m_action->BeforeXFB();
  }
  void AfterXFB() override
  {
    if (m_active)
      m_action->AfterXFB();
  }

  void OnProjection(GraphicsModActionData::Projection* projection) override
  {
    if (m_active)
      m_action->OnProjection(projection);
  }
  void OnProjectionAndTexture(GraphicsModActionData::Projection* projection) override
  {
    if (m_active)
      m_action->OnProjectionAndTexture(projection);
  }
  void OnTextureLoad(GraphicsModActionData::TextureLoad* texture_load) override
  {
    if (m_active)
      m_action->OnTextureLoad(texture_load);
  }
  void OnTextureCreate(GraphicsModActionData::TextureCreate* texture_create) override
  {
    if (m_active)
      m_action->OnTextureCreate(texture_create);
  }
  void OnLight(GraphicsModActionData::Light* light) override
  {
    if (m_active)
      m_action->OnLight(light);
  }
  void OnFrameEnd() override
  {
    if (m_active)
      m_action->OnFrameEnd();
  }

  void SetAllDrawCall(GraphicsModSystem::DrawCallID draw_call)
  {
    m_draw_call = draw_call;
    m_action->SetDrawCall(draw_call);
  }

  void DrawImGui() override
  {
    ImGui::Checkbox("##EmptyCheckbox", &m_active);
    m_action->DrawImGui();
  }

  void SerializeToConfig(picojson::object* obj) override
  {
    if (!obj) [[unlikely]]
      return;

    auto& json_obj = *obj;
    json_obj["active"] = picojson::value{m_active};
    m_action->SerializeToConfig(&json_obj);
  }

  std::string GetFactoryName() const override { return m_action->GetFactoryName(); }

  Common::Matrix44* GetTransform() override { return m_action->GetTransform(); }

  void SetActive(bool active) { m_active = active; }

private:
  bool m_active = true;
  std::unique_ptr<GraphicsModAction> m_action;
};
}  // namespace GraphicsModEditor
