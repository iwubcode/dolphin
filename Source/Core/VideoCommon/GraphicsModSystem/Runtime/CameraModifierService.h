// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "Common/Matrix.h"
#include "VideoCommon/ConstantManager.h"

namespace GraphicsModSystem::Runtime
{
/// The single seam between camera controllers (v1: the FreeLook camera) and the
/// projection-composition site (the draw-time hook in
/// VertexManagerBase::DrawGraphicsData). Backends never learn the camera exists.
///
/// The service owns the raw projection base (stashed via the On* notifications)
/// and composes it with the live camera view/FOV only at draw time. At rest,
/// constants.projection holds the raw base on every path (camera truth contract
/// §1).
class CameraModifierService
{
public:
  /// True when a camera controller is active (v1: FreeLook enabled).
  bool IsActive() const;

  /// The camera view matrix, or identity when inactive.
  Common::Matrix44 CurrentView() const;

  /// The camera FOV multiplier, or {1, 1} when inactive.
  Common::Vec2 CurrentFov() const;

  /// The raw projection base in constants.projection just changed (live write site).
  void OnProjectionChanged();

  /// The executor restored recorded constants; the field holds this raw base.
  void OnProjectionRestored(const std::array<float4, 4>& raw_projection);

  /// False until the first notification; then: base changed OR the camera view/fov
  /// differ (value-compare, 16+2 floats) from the values passed to the last Apply.
  bool NeedsRecomposition() const;

  /// Compose from the stashed base (never from the field):
  ///   composed = compose(m_base, CurrentView(), CurrentFov())
  ///   write composed -> vs_constants.projection ; vs_dirty = true
  ///   freelook = perspective ? CurrentView() : identity
  ///   if freelook != ps_constants.freelook: write it ; ps_dirty = true
  ///   remember view/fov as composed ; clear the base-changed flag
  void Apply(VertexShaderConstants& vs_constants, bool& vs_dirty,
             PixelShaderConstants& ps_constants, bool& ps_dirty);

private:
  Common::Matrix44 m_base = Common::Matrix44::Identity();
  bool m_base_changed = false;  // set by On* notifications
  bool m_have_base = false;     // false => NeedsRecomposition() is false (inert)
  Common::Matrix44 m_last_view = Common::Matrix44::Identity();
  Common::Vec2 m_last_fov{1.0f, 1.0f};
};

extern CameraModifierService g_camera_modifier_service;

}  // namespace GraphicsModSystem::Runtime
