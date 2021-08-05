// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "DolphinQt/Config/ToolTipControls/ToolTipWidget.h"

#include <QPushButton>

class ToolTipPushButton : public ToolTipWidget<QPushButton>
{
public:
  using ToolTipWidget<QPushButton>::ToolTipWidget;
private:
  QPoint GetToolTipPosition() const override;
};
