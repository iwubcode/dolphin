#include "VideoCommon/GraphicsModSystem/Runtime/FrameData.h"

XFBFrame::XFBFrame()
{
  m_gpu_operations.reserve(5000);
  m_uniform_state_change.reserve(5000);
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
  }
  if (data.pixel_shader_constants)
  {
    pixel_constant_index = m_uniform_state_change.size();
    m_uniform_state_change.push_back(*data.pixel_shader_constants);
  }
  if (data.vertex_shader_constants)
  {
    vertex_constant_index = m_uniform_state_change.size();
    m_uniform_state_change.push_back(*data.vertex_shader_constants);
  }
  DrawData draw_data{.draw_call_data = std::move(data),
                     .geometry_constant_index = geometry_constant_index,
                     .pixel_constant_index = pixel_constant_index,
                     .vertex_constant_index = vertex_constant_index};
  m_gpu_operations.push_back(std::move(draw_data));
}

void XFBFrame::AddRenderTargetData(RenderTargetData data)
{
  m_gpu_operations.push_back(std::move(data));
}

void XFBFrame::AddScissorsAndViewportData(EmulatedGraphics::ScissorsAndViewportData data)
{
  m_gpu_operations.push_back(std::move(data));
}

void XFBFrame::AddClearData(EmulatedGraphics::ClearCopyData data)
{
  m_gpu_operations.push_back(std::move(data));
}

void XFBFrame::AddPixelEngineOperationData(EmulatedGraphics::PixelEngineData data)
{
  m_gpu_operations.push_back(std::move(data));
}

void XFBFrame::AddPixelFormatChangeData(EmulatedGraphics::PixelFormatChangeData data)
{
  m_gpu_operations.push_back(std::move(data));
}

void XFBFrame::AddReinterpretTextureData(EmulatedGraphics::ReinterpretTextureData data)
{
  m_gpu_operations.push_back(std::move(data));
}

void XFBFrame::AddPaletteTextureData(EmulatedGraphics::PaletteTextureData data)
{
  m_gpu_operations.push_back(std::move(data));
}

void XFBFrame::AddScaleTextureData(EmulatedGraphics::ScaleTextureData data)
{
  m_gpu_operations.push_back(std::move(data));
}
