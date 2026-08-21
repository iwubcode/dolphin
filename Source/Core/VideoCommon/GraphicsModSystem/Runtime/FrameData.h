#pragma once

#include <optional>
#include <variant>
#include <vector>

#include "VideoCommon/IGraphicsHandler.h"
#include "VideoCommon/TextureCacheBase.h"

class XFBFrame
{
public:
  XFBFrame();

  struct RenderTargetData
  {
    EmulatedGraphics::FrameBufferCopyData copy_data;
    RcTcacheEntry cache_entry;
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
  void AddScaleTextureData(EmulatedGraphics::ScaleTextureData data);

  struct DrawData
  {
    EmulatedGraphics::DrawCallData draw_call_data;

    // If not set, the state did not change from the last draw call
    // indexes into the UniformState
    std::optional<std::size_t> geometry_constant_index;
    std::optional<std::size_t> pixel_constant_index;
    std::optional<std::size_t> vertex_constant_index;
  };
  using Operations =
      std::variant<DrawData, RenderTargetData, EmulatedGraphics::ScissorsAndViewportData,
                   EmulatedGraphics::ClearCopyData, EmulatedGraphics::PixelEngineData,
                   EmulatedGraphics::PixelFormatChangeData,
                   EmulatedGraphics::ReinterpretTextureData, EmulatedGraphics::PaletteTextureData,
                   EmulatedGraphics::ScaleTextureData>;
  const std::vector<Operations>& GetOperations() const { return m_gpu_operations; }

  using UniformState =
      std::variant<GeometryShaderConstants, PixelShaderConstants, VertexShaderConstants>;
  const std::vector<UniformState>& GetUniformStateChanges() const { return m_uniform_state_change; }

  void Reset()
  {
    m_gpu_operations.clear();
    m_uniform_state_change.clear();
  }

private:
  std::vector<Operations> m_gpu_operations;
  std::vector<UniformState> m_uniform_state_change;
};
