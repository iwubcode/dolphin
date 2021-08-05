// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <QDialog>

#include "Common/CommonTypes.h"
#include "VideoCommon/GraphicsTrigger.h"

class QDialogButtonBox;
class ToolTipLineEdit;

class NewTextureLoadTriggerDialog final : public QDialog
{
  Q_OBJECT
public:
  explicit NewTextureLoadTriggerDialog(QWidget* parent);

  const TextureLoadGraphicsTrigger& GetTrigger() const;
  QString GetName() const;

private:
  void CreateMainLayout();

  ToolTipLineEdit* m_name;

  ToolTipLineEdit* m_texture_id;

  QDialogButtonBox* m_button_box;

  TextureLoadGraphicsTrigger m_trigger;
};
