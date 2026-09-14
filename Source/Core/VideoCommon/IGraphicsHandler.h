// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/SmallVector.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/IMeshRenderer.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/XFMemory.h"

class IFramebufferRenderer;

namespace EmulatedGraphics
{
struct FramebufferCopyResolvedData
{
  // Source description
  u32 dest_address;
  u32 dest_stride;
  u32 width;
  u32 height;
  EFBCopyFormat efbcopy_format;
  PixelFormat pixel_format;
  bool is_depth_format;
  bool is_intensity_format;
  bool half_scale;
  float y_scale;
  float gamma;
  MathUtil::Rectangle<int> src_rect;
  bool clamp_top;
  bool clamp_bottom;
  CopyFilterCoefficients::Values filter_coefficients;

  // Derived
  bool is_xfb;
  bool copy_to_vram;
  bool copy_to_ram;
  u32 memory_width;
  u32 memory_height;
  u32 scaled_memory_width;
  u32 scaled_memory_height;
  TextureFormat memory_format;
  u32 bytes_per_row;
  u32 num_blocks_y;
  u32 covered_range;
  bool linear_filter;
};

struct ClearCopyData
{
  MathUtil::Rectangle<int> src_rect;
  bool color_enable;
  bool alpha_enable;
  bool z_enable;
  PixelFormat pixel_format;
  u32 color_ar;
  u32 color_gb;
  u32 z_value;
};

struct ScissorsAndViewportData
{
  ScissorPos scissor_top_left;
  ScissorPos scissor_bottom_right;
  ScissorOffset scissor_offset;
  Viewport viewport;
};

struct PixelEngineData
{
  enum class Type
  {
    SetToken,
    SetInterrupt,
    Finish
  };
  Type type;
  int new_value;
  int cycles_into_the_future;
  bool reset;
};

struct PixelFormatChangeData
{
  PixelFormat pixel_format;
  DepthFormat z_format;
};

struct Texture
{
  // Keeps the TCacheEntry alive for the duration of the frame. Note that entry->texture may be
  // null if the GPU texture has been released back to the pool (e.g. pending EFB copies).
  RcTcacheEntry entry;
  SamplerState sampler;
  u32 index;
};

struct DrawCallData
{
  const AbstractPipeline* pipeline;
  AbstractFramebuffer* frame_buffer;
  std::optional<GeometryShaderConstants> geometry_shader_constants;
  std::optional<PixelShaderConstants> pixel_shader_constants;
  std::optional<VertexShaderConstants> vertex_shader_constants;
  u32 base_index;
  u32 base_vertex;
  u32 index_size;
  u32 vertex_stride;
  PrimitiveType primitive_type;
  Common::SmallVector<Texture, 8> textures;
};

struct ReinterpretTextureData
{
  RcTcacheEntry existing_texture_entry;
  RcTcacheEntry reinterpreted_texture_entry;
};

struct PaletteTextureData
{
  RcTcacheEntry existing_texture_entry;
  RcTcacheEntry decoded_texture_entry;
  TLUTFormat tlutfmt;
  u32 texel_buffer_offset;
};

struct ScaleTextureData
{
  RcTcacheEntry existing_entry_scaled;
};
}  // namespace EmulatedGraphics

class IGraphicsHandler
{
public:
  virtual ~IGraphicsHandler() = default;

  virtual RcTcacheEntry
  OnFramebufferCopy(IFramebufferRenderer* renderer, PixelShaderManager& pixel_shader_manager,
                    EmulatedGraphics::FramebufferCopyResolvedData resolved_data) = 0;
  virtual void FlushFramebufferCopy(IFramebufferRenderer* renderer,
                                    const RcTcacheEntry& cache_entry) = 0;
  virtual void
  SetScissorAndViewport(EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data) = 0;

  virtual void
  OnFramebufferClear(EmulatedGraphics::ClearCopyData clear_data,
                     EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data,
                     bool is_xfb) = 0;

  virtual void PixelEngineOperation(EmulatedGraphics::PixelEngineData pixel_engine_data,
                                    bool notify_cpu, bool notify_gpu) = 0;
  virtual void
  PixelFormatChange(EmulatedGraphics::PixelFormatChangeData pixel_format_change_data) = 0;
  virtual void OnDraw(GraphicsModSystem::DrawDataView draw_call_data,
                      IMeshRenderer* mesh_renderer) = 0;

  virtual void
  OnReinterpretTexture(EmulatedGraphics::ReinterpretTextureData reinterpret_texture_data) = 0;
  virtual void OnPaletteTexture(EmulatedGraphics::PaletteTextureData palette_texture_data) = 0;
  virtual void
  OnPartialTextureUpdate(PartialTextureUpdateContext partial_texture_update_context) = 0;
};
