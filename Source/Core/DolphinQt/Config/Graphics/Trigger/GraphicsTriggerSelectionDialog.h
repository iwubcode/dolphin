// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <QDialog>

class QDialogButtonBox;
class ToolTipComboBox;

class GraphicsTriggerSelectionDialog final : public QDialog
{
  Q_OBJECT
public:
  explicit GraphicsTriggerSelectionDialog(QWidget* parent);

  QString GetTriggerName() const;

private:
  void CreateMainLayout();
  void BuildComboBox();
  void ConnectWidgets();

  ToolTipComboBox* m_triggers;
  QDialogButtonBox* m_button_box;
};
