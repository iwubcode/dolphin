#include "VideoCommon/GraphicsModSystem/Runtime/FrameData.h"

#include <cstring>

#include "Common/Timer.h"

#include "VideoCommon/GraphicsModSystem/Runtime/DeferredPerfTelemetry.h"

namespace
{
using namespace DeferredPerf;

template <typename T>
void AddOperation(std::vector<XFBFrame::OperationReference>& order, std::vector<T>& storage,
                  T&& data, XFBFrame::OperationType type)
{
  if (!kTelemetryEnabled)
  {
    storage.push_back(std::forward<T>(data));
    order.push_back({type, static_cast<u32>(storage.size() - 1)});
    return;
  }
  const u64 start = Common::Timer::NowUs();
  storage.push_back(std::forward<T>(data));
  order.push_back({type, static_cast<u32>(storage.size() - 1)});
  Telemetry::Get().AddRecordUs(Common::Timer::NowUs() - start);
  Telemetry::Get().AddOpRecorded(type);
}
}  // namespace

XFBFrame::XFBFrame()
{
  m_recorded_operations.reserve(5000);
  m_draws.reserve(5000);
  m_uniform_state_change.reserve(5000);
}

XFBFrame::FrameView XFBFrame::GetView() const
{
  return {m_recorded_operations,
          m_draws,
          m_render_targets,
          m_scissors_viewports,
          m_clears,
          m_pixel_engine,
          m_pixel_format_changes,
          m_reinterpret_textures,
          m_palette_textures,
          m_partial_texture_updates};
}

void XFBFrame::AddDrawData(EmulatedGraphics::DrawCallData data)
{
  std::optional<std::size_t> geometry_constant_index;
  std::optional<std::size_t> pixel_constant_index;
  std::optional<std::size_t> vertex_constant_index;
  if (data.geometry_shader_constants)
  {
    geometry_constant_index = m_uniform_state_change.size();
    m_uniform_state_change.push_back(*data.geometry_shader_constants);
    if (kTelemetryEnabled)
      Telemetry::Get().AddUniformStateGeometry();
  }
  if (data.pixel_shader_constants)
  {
    pixel_constant_index = m_uniform_state_change.size();
    m_uniform_state_change.push_back(*data.pixel_shader_constants);
    if (kTelemetryEnabled)
      Telemetry::Get().AddUniformStatePixel();
  }
  if (data.vertex_shader_constants)
  {
    vertex_constant_index = m_uniform_state_change.size();
    m_uniform_state_change.push_back(*data.vertex_shader_constants);
    if (kTelemetryEnabled)
      Telemetry::Get().AddUniformStateVertex();
  }

  if (kTelemetryEnabled)
  {
    u64 bytes = 0;
    if (data.geometry_shader_constants)
      bytes += sizeof(GeometryShaderConstants);
    if (data.pixel_shader_constants)
      bytes += sizeof(PixelShaderConstants);
    if (data.vertex_shader_constants)
      bytes += sizeof(VertexShaderConstants);
    Telemetry::Get().AddConstCopyBytes(bytes);
  }

  // THROWAWAY INVESTIGATION FIX: the constants are already stored in
  // m_uniform_state_change (and that is the only place the replay path reads
  // them from), so drop the redundant copies before moving DrawCallData into
  // the storage vector. VertexShaderConstants is ~3-4 KB and trivially
  // copyable, so the "move" above was a full memcpy stored twice per dirty draw.
  if (kStripDeferredConstants)
  {
    data.geometry_shader_constants.reset();
    data.pixel_shader_constants.reset();
    data.vertex_shader_constants.reset();
  }

  DrawData draw_data{.draw_call_data = std::move(data),
                     .geometry_constant_index = geometry_constant_index,
                     .pixel_constant_index = pixel_constant_index,
                     .vertex_constant_index = vertex_constant_index};
  AddOperation(m_recorded_operations, m_draws, std::move(draw_data), OperationType::Draw);
}

void XFBFrame::AddRenderTargetData(RenderTargetData data)
{
  AddOperation(m_recorded_operations, m_render_targets, std::move(data),
               OperationType::RenderTarget);
}

void XFBFrame::AddScissorsAndViewportData(EmulatedGraphics::ScissorsAndViewportData data)
{
  // Coalesce: a scissor/viewport change is idempotent state, so a change to the
  // same values as the last recorded one is a no-op. (Draws never modify this
  // state, so the previous entry is still the live value.) The struct is
  // trivially copyable with no padding, so a byte compare detects "unchanged".
  /*if (!m_scissors_viewports.empty() &&
      std::memcmp(&m_scissors_viewports.back(), &data, sizeof(data)) == 0)
  {
    if (kTelemetryEnabled)
      Telemetry::Get().AddCoalescedOp();
    return;
  }*/
  AddOperation(m_recorded_operations, m_scissors_viewports, std::move(data),
               OperationType::ScissorViewport);
}

void XFBFrame::AddClearData(EmulatedGraphics::ClearCopyData data)
{
  // Not coalesced: a draw between two identical clears can modify the
  // framebuffer, so the second clear is not a no-op.
  AddOperation(m_recorded_operations, m_clears, std::move(data), OperationType::Clear);
}

void XFBFrame::AddPixelEngineOperationData(EmulatedGraphics::PixelEngineData data)
{
  AddOperation(m_recorded_operations, m_pixel_engine, std::move(data), OperationType::PixelEngine);
}

void XFBFrame::AddPixelFormatChangeData(EmulatedGraphics::PixelFormatChangeData data)
{
  // Coalesce: the pixel format is idempotent state, so a change to the same
  // format as the last recorded one is a no-op. Games frequently re-issue the
  // same Z-control state (BPMEM_ZCOMPARE writes). The struct is trivially
  // copyable with no padding, so a byte compare detects "unchanged".
  /*if (!m_pixel_format_changes.empty() &&
      std::memcmp(&m_pixel_format_changes.back(), &data, sizeof(data)) == 0)
  {
    if (kTelemetryEnabled)
      Telemetry::Get().AddCoalescedOp();
    return;
  }*/
  AddOperation(m_recorded_operations, m_pixel_format_changes, std::move(data),
               OperationType::PixelFormatChange);
}

void XFBFrame::AddReinterpretTextureData(EmulatedGraphics::ReinterpretTextureData data)
{
  AddOperation(m_recorded_operations, m_reinterpret_textures, std::move(data),
               OperationType::ReinterpretTexture);
}

void XFBFrame::AddPaletteTextureData(EmulatedGraphics::PaletteTextureData data)
{
  AddOperation(m_recorded_operations, m_palette_textures, std::move(data),
               OperationType::PaletteTexture);
}

void XFBFrame::AddPartialTextureUpdateData(PartialTextureUpdateContext data)
{
  AddOperation(m_recorded_operations, m_partial_texture_updates, std::move(data),
               OperationType::PartialTextureUpdate);
}
