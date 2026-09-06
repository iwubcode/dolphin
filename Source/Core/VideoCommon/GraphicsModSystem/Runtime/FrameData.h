#pragma once

#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/IGraphicsHandler.h"
#include "VideoCommon/TextureCacheBase.h"

class XFBFrame
{
public:
  XFBFrame();

  enum class OperationType : u8
  {
    Draw = 0,
    RenderTarget,
    ScissorViewport,
    Clear,
    PixelEngine,
    PixelFormatChange,
    ReinterpretTexture,
    PaletteTexture,
    PartialTextureUpdate,
    Max,
  };

  struct OperationReference
  {
    OperationType type;
    u32 index;
  };

  struct RenderTargetData
  {
    EmulatedGraphics::FramebufferCopyResolvedData resolved_copy_data;
    RcTcacheEntry cache_entry;
  };

  struct DrawData
  {
    EmulatedGraphics::DrawCallData draw_call_data;

    // If not set, the state did not change from the last draw call
    // indexes into the UniformState
    std::optional<std::size_t> geometry_constant_index;
    std::optional<std::size_t> pixel_constant_index;
    std::optional<std::size_t> vertex_constant_index;
  };

  struct FrameView
  {
    std::span<const OperationReference> operations;
    std::span<const DrawData> draws;
    std::span<const RenderTargetData> render_targets;
    std::span<const EmulatedGraphics::ScissorsAndViewportData> scissors_viewports;
    std::span<const EmulatedGraphics::ClearCopyData> clears;
    std::span<const EmulatedGraphics::PixelEngineData> pixel_engine;
    std::span<const EmulatedGraphics::PixelFormatChangeData> pixel_format_changes;
    std::span<const EmulatedGraphics::ReinterpretTextureData> reinterpret_textures;
    std::span<const EmulatedGraphics::PaletteTextureData> palette_textures;
    std::span<const PartialTextureUpdateContext> partial_texture_updates;
  };

  // Operations
  void AddDrawData(EmulatedGraphics::DrawCallData data);
  void AddRenderTargetData(RenderTargetData data);
  void AddClearData(EmulatedGraphics::ClearCopyData data);
  void AddScissorsAndViewportData(EmulatedGraphics::ScissorsAndViewportData data);
  void AddPixelEngineOperationData(EmulatedGraphics::PixelEngineData data);
  void AddPixelFormatChangeData(EmulatedGraphics::PixelFormatChangeData data);
  void AddReinterpretTextureData(EmulatedGraphics::ReinterpretTextureData data);
  void AddPaletteTextureData(EmulatedGraphics::PaletteTextureData data);
  void AddPartialTextureUpdateData(PartialTextureUpdateContext data);

  FrameView GetView() const;
  std::size_t GetTotalOperations() const { return m_recorded_operations.size(); }

  using UniformState =
      std::variant<GeometryShaderConstants, PixelShaderConstants, VertexShaderConstants>;
  const std::vector<UniformState>& GetUniformStateChanges() const { return m_uniform_state_change; }

  void Reset()
  {
    m_recorded_operations.clear();
    m_draws.clear();
    m_render_targets.clear();
    m_scissors_viewports.clear();
    m_clears.clear();
    m_pixel_engine.clear();
    m_pixel_format_changes.clear();
    m_reinterpret_textures.clear();
    m_palette_textures.clear();
    m_partial_texture_updates.clear();
    m_uniform_state_change.clear();
  }

private:
  // The frame recorded operations.
  std::vector<OperationReference> m_recorded_operations;

  // Operations stored by type
  std::vector<DrawData> m_draws;
  std::vector<RenderTargetData> m_render_targets;
  std::vector<EmulatedGraphics::ScissorsAndViewportData> m_scissors_viewports;
  std::vector<EmulatedGraphics::ClearCopyData> m_clears;
  std::vector<EmulatedGraphics::PixelEngineData> m_pixel_engine;
  std::vector<EmulatedGraphics::PixelFormatChangeData> m_pixel_format_changes;
  std::vector<EmulatedGraphics::ReinterpretTextureData> m_reinterpret_textures;
  std::vector<EmulatedGraphics::PaletteTextureData> m_palette_textures;
  std::vector<PartialTextureUpdateContext> m_partial_texture_updates;

  std::vector<UniformState> m_uniform_state_change;
};
