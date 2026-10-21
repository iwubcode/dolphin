// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/CameraModifierService.h"

#include <cstring>

#include "Core/System.h"
#include "VideoCommon/FreeLookCamera.h"
#include "VideoCommon/VertexShaderManager.h"

namespace GraphicsModSystem::Runtime
{
CameraModifierService g_camera_modifier_service;

bool CameraModifierService::IsActive() const
{
  return g_freelook_camera.IsActive();
}

Common::Matrix44 CameraModifierService::CurrentView() const
{
  return g_freelook_camera.IsActive() ? g_freelook_camera.GetView() : Common::Matrix44::Identity();
}

Common::Vec2 CameraModifierService::CurrentFov() const
{
  return g_freelook_camera.IsActive() ? g_freelook_camera.GetFieldOfViewMultiplier() :
                                        Common::Vec2{1.0f, 1.0f};
}

void CameraModifierService::OnProjectionChanged()
{
  m_base = Core::System::GetInstance().GetVertexShaderManager().GetRawProjectionBase();
  m_base_changed = true;
  m_have_base = true;
}

void CameraModifierService::OnProjectionRestored(const std::array<float4, 4>& raw_projection)
{
  // float4 is std::array<float, 4>, so the 4x4 layout is identical to
  // std::array<float, 16> (row-major); flatten it for FromArray.
  std::array<float, 16> flat;
  std::memcpy(flat.data(), raw_projection.data(), sizeof(flat));
  m_base = Common::Matrix44::FromArray(flat);
  m_base_changed = true;
  m_have_base = true;
}

bool CameraModifierService::NeedsRecomposition() const
{
  if (!m_have_base)
    return false;
  if (m_base_changed)
    return true;
  return m_last_view.data != CurrentView().data || m_last_fov.data != CurrentFov().data;
}

void CameraModifierService::Apply(VertexShaderConstants& vs_constants, bool& vs_dirty,
                                  PixelShaderConstants& ps_constants, bool& ps_dirty)
{
  const auto view = CurrentView();
  const auto fov = CurrentFov();

  // Perspective-ness is derived from the base (m[15]), never from live
  // xfmem.projection.type (end-of-frame stale at replay time). Ortho bases
  // ignore view/fov, as today.
  const auto composed =
      m_base.data[15] == 0.0f ? Common::Matrix44::Scale(m_base * view, fov) : m_base;

  std::memcpy(vs_constants.projection.data(), composed.data.data(), 4 * sizeof(float4));
  vs_dirty = true;

  const auto freelook = m_base.data[15] == 0.0f ? view : Common::Matrix44::Identity();
  if (std::memcmp(freelook.data.data(), ps_constants.freelook.data(), 4 * sizeof(float4)) != 0)
  {
    std::memcpy(ps_constants.freelook.data(), freelook.data.data(), 4 * sizeof(float4));
    ps_dirty = true;
  }

  m_last_view = view;
  m_last_fov = fov;
  m_base_changed = false;
}

}  // namespace GraphicsModSystem::Runtime
