// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/DeferredGraphicsHandler.h"

#include "Common/VariantUtil.h"
#include "Core/System.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/GraphicsPathTracer.h"
#include "VideoCommon/VertexManagerBase.h"

namespace
{
void ReplayOperations(EmulatedGraphicsHandler& emulated_graphics_handler,
                      FramebufferManager* frame_buffer_manager,
                      std::span<const XFBFrame::Operations> operations,
                      std::span<const XFBFrame::UniformState> uniform_state, auto indices)
{
  auto& system = Core::System::GetInstance();
  auto& geometry_shader_manager = system.GetGeometryShaderManager();
  auto& pixel_shader_manager = system.GetPixelShaderManager();
  auto& vertex_shader_manager = system.GetVertexShaderManager();

  for (const std::size_t index : indices)
  {
    const auto& operation = operations[index];
    std::visit(
        overloaded{[&](const XFBFrame::DrawData& draw_call_data) {
                     if (draw_call_data.geometry_constant_index)
                     {
                       geometry_shader_manager.constants = std::get<GeometryShaderConstants>(
                           uniform_state[*draw_call_data.geometry_constant_index]);
                       geometry_shader_manager.dirty = true;
                     }
                     if (draw_call_data.pixel_constant_index)
                     {
                       pixel_shader_manager.constants = std::get<PixelShaderConstants>(
                           uniform_state[*draw_call_data.pixel_constant_index]);
                       pixel_shader_manager.dirty = true;
                     }
                     if (draw_call_data.vertex_constant_index)
                     {
                       vertex_shader_manager.constants = std::get<VertexShaderConstants>(
                           uniform_state[*draw_call_data.vertex_constant_index]);
                       vertex_shader_manager.dirty = true;
                     }
                     // Determine if it is a custom or emulated mesh
                     g_vertex_manager->DrawGraphicsData(frame_buffer_manager,
                                                        draw_call_data.draw_call_data);
                   },
                   [&](const EmulatedGraphics::ScissorsAndViewportData& scissors_and_viewport) {
                     emulated_graphics_handler.SetScissorAndViewport(scissors_and_viewport);
                   },
                   [&](const EmulatedGraphics::ClearCopyData& clear_data) {
                     BPFunctions::ClearScreen(
                         frame_buffer_manager, clear_data.src_rect, clear_data.color_enable,
                         clear_data.alpha_enable, clear_data.z_enable, clear_data.pixel_format,
                         clear_data.color_ar, clear_data.color_gb, clear_data.z_value);
                   },
                   [&](const XFBFrame::RenderTargetData& render_target_data) {
                     if (render_target_data.cache_entry &&
                         render_target_data.copy_data.dest_stride != 0)
                     {
                       g_texture_cache->CompleteFramebufferCopy(render_target_data.cache_entry,
                                                                render_target_data.copy_data);
                     }
                   },
                   [&](const EmulatedGraphics::PixelEngineData& pixel_engine_data) {
                     emulated_graphics_handler.PixelEngineOperation(pixel_engine_data, false, true);
                   },
                   [&](const EmulatedGraphics::PixelFormatChangeData& pixel_format_change_data) {
                     emulated_graphics_handler.PixelFormatChange(pixel_format_change_data);
                   },
                   [&](const EmulatedGraphics::ReinterpretTextureData& reinterpret_texture_data) {
                     emulated_graphics_handler.OnReinterpretTexture(reinterpret_texture_data);
                   },
                   [&](const EmulatedGraphics::PaletteTextureData& palette_texture_data) {
                     emulated_graphics_handler.OnPaletteTexture(palette_texture_data);
                   },
                   [&](const EmulatedGraphics::ScaleTextureData& scale_texture_data) {
                     emulated_graphics_handler.OnScaleTexture(scale_texture_data);
                   }},
        operation);
  }
}
}  // namespace

DeferredGraphicsHandler::DeferredGraphicsHandler(XFBFrame* frame,
                                                 FramebufferManager* native_efb_framebuffer_manager)
    : m_xfb_frame(frame), m_native_efb_framebuffer_manager(native_efb_framebuffer_manager),
      m_graphics_mod_handler(g_framebuffer_manager.get(), true)
{
}

RcTcacheEntry
DeferredGraphicsHandler::OnFrameBufferCopy(IFramebufferRenderer* renderer,
                                           PixelShaderManager& pixel_shader_manager,
                                           EmulatedGraphics::FrameBufferCopyData copy_data)
{
  if (copy_data.pe_copy.copy_to_xfb != 0)
  {
    if (copy_data.dest_stride != 0)
    {
      DrawXFBFrame();
    }

    return m_graphics_mod_handler.OnFrameBufferCopy(renderer, pixel_shader_manager,
                                                    std::move(copy_data));
  }

  if (m_native_efb_framebuffer_manager && copy_data.dest_stride != 0)
  {
    DrawEFBCopy();
  }
  return m_graphics_mod_handler.OnFrameBufferCopy(this, pixel_shader_manager, std::move(copy_data));
}

void DeferredGraphicsHandler::OnFrameBufferClear(
    EmulatedGraphics::ClearCopyData clear_data,
    EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data, bool is_xfb)
{
  if (m_native_efb_framebuffer_manager)
  {
    const bool attempt_copy_efb_to_ram = false;
    EmulatedGraphicsHandler emulated_graphics_handler(m_native_efb_framebuffer_manager,
                                                      attempt_copy_efb_to_ram);
    emulated_graphics_handler.OnFrameBufferClear(clear_data, scissors_and_viewport_data, is_xfb);
  }

  if (is_xfb)
  {
    m_graphics_mod_handler.OnFrameBufferClear(clear_data, scissors_and_viewport_data, is_xfb);
    return;
  }

  m_xfb_frame->AddClearData(std::move(clear_data));
  m_xfb_frame->AddScissorsAndViewportData(std::move(scissors_and_viewport_data));
}

void DeferredGraphicsHandler::SetScissorAndViewport(
    EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data)
{
  if (m_native_efb_framebuffer_manager)
  {
    m_native_efb_operations.push_back(m_xfb_frame->GetOperations().size());
  }
  m_xfb_frame->AddScissorsAndViewportData(std::move(scissors_and_viewport_data));
}

void DeferredGraphicsHandler::PixelEngineOperation(
    EmulatedGraphics::PixelEngineData pixel_engine_data, bool notify_cpu, bool notify_gpu)
{
  if (m_native_efb_framebuffer_manager)
  {
    m_native_efb_operations.push_back(m_xfb_frame->GetOperations().size());
  }
  // Need to still notify the CPU that this operation occurs,
  // just hide it from the GPU until later
  const bool force_notify_cpu = true;
  const bool force_notify_gpu = false;
  m_graphics_mod_handler.PixelEngineOperation(pixel_engine_data, force_notify_cpu,
                                              force_notify_gpu);

  m_xfb_frame->AddPixelEngineOperationData(std::move(pixel_engine_data));
}

void DeferredGraphicsHandler::PixelFormatChange(
    EmulatedGraphics::PixelFormatChangeData pixel_format_change_data)
{
  if (m_native_efb_framebuffer_manager)
  {
    m_native_efb_operations.push_back(m_xfb_frame->GetOperations().size());
  }
  m_xfb_frame->AddPixelFormatChangeData(std::move(pixel_format_change_data));
}

void DeferredGraphicsHandler::OnDraw(GraphicsModSystem::DrawDataView draw_data,
                                     IMeshRenderer* renderer)
{
  m_graphics_mod_handler.OnDraw(draw_data, this);
}

void DeferredGraphicsHandler::OnReinterpretTexture(
    EmulatedGraphics::ReinterpretTextureData reinterpret_texture_data)
{
  if (reinterpret_texture_data.existing_texture_entry->IsEfbCopy())
  {
    // Need to wait...not sure what this means for our native EFB buffer?
    // We only have one texture cache
    m_xfb_frame->AddReinterpretTextureData(std::move(reinterpret_texture_data));
    return;
  }

  m_graphics_mod_handler.OnReinterpretTexture(std::move(reinterpret_texture_data));
}

void DeferredGraphicsHandler::OnPaletteTexture(
    EmulatedGraphics::PaletteTextureData palette_texture_data)
{
  if (palette_texture_data.existing_texture_entry->IsEfbCopy())
  {
    // Need to wait...not sure what this means for our native EFB buffer?
    // We only have one texture cache
    m_xfb_frame->AddPaletteTextureData(std::move(palette_texture_data));
    return;
  }

  m_graphics_mod_handler.OnPaletteTexture(std::move(palette_texture_data));
}

void DeferredGraphicsHandler::OnScaleTexture(EmulatedGraphics::ScaleTextureData scale_texture_data)
{
  if (scale_texture_data.existing_entry_scaled->IsEfbCopy())
  {
    // Need to wait...not sure what this means for our native EFB buffer?
    // We only have one texture cache
    m_xfb_frame->AddScaleTextureData(std::move(scale_texture_data));
    return;
  }

  m_graphics_mod_handler.OnScaleTexture(std::move(scale_texture_data));
}

std::shared_ptr<TCacheEntry>
DeferredGraphicsHandler::RenderFramebufferCopy(PixelShaderManager& pixel_shader_manager,
                                               EmulatedGraphics::FrameBufferCopyData copy_data)
{
  RcTcacheEntry deferred_entry = g_texture_cache->DeferFramebufferCopy(
      m_native_efb_framebuffer_manager != nullptr ? m_native_efb_framebuffer_manager :
                                                    g_framebuffer_manager.get(),
      copy_data);

  m_xfb_frame->AddRenderTargetData(
      XFBFrame::RenderTargetData{.copy_data = std::move(copy_data), .cache_entry = deferred_entry});

  return deferred_entry;
}

void DeferredGraphicsHandler::UninitializeFramebufferMemory(
    const EmulatedGraphics::FrameBufferCopyData& copy_data)
{
  g_texture_cache->UninitializeFramebufferMemory(copy_data);
}

void DeferredGraphicsHandler::DrawEmulatedMesh(const GraphicsModSystem::DrawDataView& draw_data,
                                               const Common::Matrix44& custom_transform)
{
  // Defer: upload vertices + capture shader state for replay at frame boundary
  std::vector<EmulatedGraphics::DrawCallData> deferred_data;
  g_vertex_manager->DeferEmulatedMesh(draw_data, custom_transform, &deferred_data);
  for (auto& dcd : deferred_data)
  {
    if (m_native_efb_framebuffer_manager)
    {
      m_native_efb_operations.push_back(m_xfb_frame->GetOperations().size());
    }
    m_xfb_frame->AddDrawData(std::move(dcd));
  }
}

void DeferredGraphicsHandler::DrawEmulatedMesh(
    const GraphicsModSystem::DrawDataView& draw_data,
    const VideoCommon::MaterialResource::Data& material_data,
    const Common::Matrix44& custom_transform)
{
  // Defer: upload vertices + capture shader state for replay at frame boundary
  std::vector<EmulatedGraphics::DrawCallData> deferred_data;
  g_vertex_manager->DeferEmulatedMesh(draw_data, material_data, custom_transform, &deferred_data);
  for (auto& dcd : deferred_data)
  {
    if (m_native_efb_framebuffer_manager)
    {
      m_native_efb_operations.push_back(m_xfb_frame->GetOperations().size());
    }
    m_xfb_frame->AddDrawData(std::move(dcd));
  }
}

void DeferredGraphicsHandler::DrawCustomMesh(const GraphicsModSystem::DrawDataView& draw_data,
                                             GraphicsModSystem::DrawCallID draw_call,
                                             const VideoCommon::MeshResource::Data& mesh_data,
                                             const Common::Matrix44& custom_transform,
                                             bool ignore_mesh_transform,
                                             VideoCommon::SkeletonInstance* skeleton_instance)
{
  // Defer: upload custom mesh vertices + capture shader state for replay at frame boundary
  std::vector<EmulatedGraphics::DrawCallData> deferred_data;
  g_vertex_manager->DeferCustomMesh(draw_data, draw_call, mesh_data, custom_transform,
                                    ignore_mesh_transform, skeleton_instance, &deferred_data);
  for (auto& dcd : deferred_data)
  {
    if (m_native_efb_framebuffer_manager)
    {
      m_native_efb_operations.push_back(m_xfb_frame->GetOperations().size());
    }
    m_xfb_frame->AddDrawData(std::move(dcd));
  }
}

void DeferredGraphicsHandler::DrawXFBFrame()
{
  const AbstractGfx::ViewportAndScissor native_viewport_scissors =
      g_gfx->GetStoredViewportAndScissor();
  if (m_native_efb_framebuffer_manager)
  {
    // If we have a native EFB framebuffer, we would have drawn the
    // EFB by now, potentially modifying our scissors/viewport stored
    // to a different result than was actually at the start of our
    // XFB, restore it
    if (m_previous_frame_scissors_viewport)
    {
      g_gfx->SetViewport(m_previous_frame_scissors_viewport->viewport_x,
                         m_previous_frame_scissors_viewport->viewport_y,
                         m_previous_frame_scissors_viewport->viewport_width,
                         m_previous_frame_scissors_viewport->viewport_height,
                         m_previous_frame_scissors_viewport->viewport_near_depth,
                         m_previous_frame_scissors_viewport->viewport_far_depth);
      g_gfx->SetScissorRect(m_previous_frame_scissors_viewport->scissor_rect);
      g_gfx->StoreViewportAndScissor(std::move(*m_previous_frame_scissors_viewport));
    }
  }

  // Bind our normal (user IR) frame buffer
  g_framebuffer_manager->BindEFBFramebuffer();

  // If necessary, we already did our copy to ram, no need to attempt it again
  const bool attempt_copy_efb_to_ram = false;
  EmulatedGraphicsHandler emulated_graphics_handler(g_framebuffer_manager.get(),
                                                    attempt_copy_efb_to_ram);

  const auto& uniform_state = m_xfb_frame->GetUniformStateChanges();
  const auto& operations = m_xfb_frame->GetOperations();

  auto all_indices = std::views::iota(0ULL, operations.size());
  ReplayOperations(emulated_graphics_handler, g_framebuffer_manager.get(), operations,
                   uniform_state, all_indices);

  m_xfb_frame->Reset();

  // Clear native as well, we didn't have any more operations
  m_native_efb_operations.clear();

  if (m_native_efb_framebuffer_manager)
  {
    // Save off our high resolution scissors/viewport, so we can use that as a base for the next
    // frame
    m_previous_frame_scissors_viewport = g_gfx->GetStoredViewportAndScissor();
  }
}

void DeferredGraphicsHandler::DrawEFBCopy()
{
  if (m_native_efb_operations.empty())
    return;

  // Bind our native frame buffer
  m_native_efb_framebuffer_manager->BindEFBFramebuffer();

  const bool attempt_copy_efb_to_ram = false;
  EmulatedGraphicsHandler emulated_graphics_handler(m_native_efb_framebuffer_manager,
                                                    attempt_copy_efb_to_ram);

  const auto& uniform_state = m_xfb_frame->GetUniformStateChanges();
  const auto& operations = m_xfb_frame->GetOperations();
  ReplayOperations(emulated_graphics_handler, m_native_efb_framebuffer_manager, operations,
                   uniform_state, m_native_efb_operations);
  m_native_efb_operations.clear();
}
