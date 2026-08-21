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
struct FrameBufferCopyData
{
  // Raw PE copy register data (kept for mod hooks that may inspect it):
  UPE_Copy pe_copy;

  // Destination address and dimensions:
  u32 dest_address;
  u32 copy_width;   // srcRect.GetWidth() = bpmem.copyTexSrcWH.x + 1
  u32 copy_height;  // Final height after y_scale applied (for XFB) or srcRect.GetHeight() for EFB

  // Stride and format:
  u32 dest_stride;
  EFBCopyFormat efbcopy_format;  // Pre-computed: EFBCopyFormat::XFB or tp_realFormat()

  // Derived parameters (pre-computed in BPStructs):
  bool is_depth_copy;
  bool is_intensity;  // pe_copy.intensity_fmt && pe_copy.auto_conv for EFB, false for XFB
  bool half_scale;    // pe_copy.half_scale for EFB, false for XFB
  float y_scale;      // For XFB: computed from scale_invert + dispcopyyscale. For EFB: 1.0f
  float gamma;        // Pre-looked up from s_gammaLUT[pe_copy.gamma] - ready to use directly

  MathUtil::Rectangle<int> src_rect;

  bool clamp_top;
  bool clamp_bottom;
  CopyFilterCoefficients::Values filter_coefficients;
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
  AbstractTexture* ptr;
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
  TextureFormat new_format;
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

  virtual RcTcacheEntry OnFrameBufferCopy(IFramebufferRenderer* renderer,
                                          PixelShaderManager& pixel_shader_manager,
                                          EmulatedGraphics::FrameBufferCopyData copy_data) = 0;
  virtual void
  SetScissorAndViewport(EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data) = 0;

  virtual void
  OnFrameBufferClear(EmulatedGraphics::ClearCopyData clear_data,
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
  virtual void OnScaleTexture(EmulatedGraphics::ScaleTextureData scale_texture_data) = 0;
};
