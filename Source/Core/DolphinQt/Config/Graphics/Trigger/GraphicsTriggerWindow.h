// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <QDialog>

class GraphicsTriggerWidget;
class QDialogButtonBox;

class GraphicsTriggerWindow final : public QDialog
{
  Q_OBJECT
public:
  explicit GraphicsTriggerWindow(QWidget* parent);

private:
  void CreateMainLayout();

  GraphicsTriggerWidget* m_graphics_trigger_widget;
  QDialogButtonBox* m_button_box;
};
