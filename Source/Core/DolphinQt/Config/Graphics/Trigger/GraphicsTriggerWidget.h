// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <QWidget>

#include "VideoCommon/GraphicsTrigger.h"

class QListWidget;
class QMenu;
class QPushButton;
class QToolButton;

class GraphicsTriggerWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit GraphicsTriggerWidget(QWidget* parent);

  void LoadSettings();
  void SaveSettings();
private:
  void CreateLayout();

  void ReloadTriggerList();

  void AddEFBTrigger();
  void Add2DDrawCallTrigger();
  void Add3DDrawCallTrigger();
  void AddTextureLoadTrigger();

  QListWidget* m_triggers;
  QToolButton* m_add_trigger;
  QMenu* m_add_trigger_menu;
  QPushButton* m_remove_trigger;

  std::map<std::string, GraphicsTrigger> m_user_triggers;
};
