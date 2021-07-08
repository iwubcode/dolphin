// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

class AbstractFramebuffer;
class AbstractTexture;

namespace VideoCommon::PE
{
	struct ShaderApplyOptions
	{
		AbstractFramebuffer* dest_fb
		MathUtil::Rectangle<int>& dest_rect;
		AbstractTexture* source_color_tex;
		AbstractTexture* source_depth_tex;
		MathUtil::Rectangle<int>& source_rect;
		int source_layer;
	};
}