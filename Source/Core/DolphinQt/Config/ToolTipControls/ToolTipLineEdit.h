// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "DolphinQt/Config/ToolTipControls/ToolTipWidget.h"

#include <QLineEdit>

class ToolTipLineEdit : public ToolTipWidget<QLineEdit>
{
private:
  QPoint GetToolTipPosition() const override;
};
