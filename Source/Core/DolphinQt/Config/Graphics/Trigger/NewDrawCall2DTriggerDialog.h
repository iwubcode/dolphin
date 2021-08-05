// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <QDialog>

#include "Common/CommonTypes.h"
#include "VideoCommon/GraphicsTrigger.h"

class QDialogButtonBox;
class ToolTipComboBox;
class ToolTipLineEdit;
class ToolTipSpinBox;

class NewDrawCall2DTriggerDialog final : public QDialog
{
  Q_OBJECT
public:
  explicit NewDrawCall2DTriggerDialog(QWidget* parent);

  const DrawCall2DGraphicsTrigger& GetTrigger() const;
  QString GetName() const;

private:
  void CreateMainLayout();
  ToolTipLineEdit* m_name;

  ToolTipSpinBox* m_width;
  ToolTipComboBox* m_width_operation;

  ToolTipSpinBox* m_height;
  ToolTipComboBox* m_height_operation;

  ToolTipSpinBox* m_format;
  ToolTipComboBox* m_format_operation;

  ToolTipLineEdit* m_tlut;
  ToolTipComboBox* m_tlut_operation;

  ToolTipLineEdit* m_hash;
  ToolTipComboBox* m_hash_operation;

  QDialogButtonBox* m_button_box;

  DrawCall2DGraphicsTrigger m_trigger;
};
