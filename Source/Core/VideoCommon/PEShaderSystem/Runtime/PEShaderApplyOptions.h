// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"

class AbstractFramebuffer;
class AbstractTexture;

namespace VideoCommon::PE
{
	struct ShaderApplyOptions
	{
		AbstractFramebuffer* m_dest_fb;
		MathUtil::Rectangle<int> m_dest_rect;
		const AbstractTexture* m_source_color_tex;
		const AbstractTexture* m_source_depth_tex;
		MathUtil::Rectangle<int> m_source_rect;
		int m_source_layer;
		u64 m_time_elapsed;
		float m_depth_near;
		float m_depth_far;
	};
}
