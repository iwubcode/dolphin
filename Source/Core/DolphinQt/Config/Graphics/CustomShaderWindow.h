// Copyright 2020 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <QDialog>
#include <QString>

class QComboBox;
class QGroupBox;
class QDialogButtonBox;
class QHBoxLayout;
class QLabel;
class QVBoxLayout;
class QPushButton;
class QWidget;
class PostProcessingShaderListWidget;

class CustomShaderWindow final : public QWidget
{
  Q_OBJECT
public:
  explicit CustomShaderWindow(QWidget* parent = nullptr);

private:
  void CreateProfilesLayout();
  void CreateTriggersLayout();
  void CreateMainLayout();
  void ConnectWidgets();

  void OnSelectProfile(int index);
  void OnProfileTextChanged(const QString& text);
  void OnDeleteProfilePressed();
  void OnLoadProfilePressed();
  void OnSaveProfilePressed();
  void UpdateProfileIndex();
  void UpdateProfileButtonState();
  void PopulateProfileSelection();

  void OnSelectTriggerPressed();
  void OnTriggerPointChanged();

  bool eventFilter(QObject* object, QEvent* event) override;
  
  // Main
  QVBoxLayout* m_main_layout;
  QHBoxLayout* m_config_layout;
  QDialogButtonBox* m_button_box;

  // Profiles
  QGroupBox* m_profiles_box;
  QHBoxLayout* m_profiles_layout;
  QComboBox* m_profiles_combo;
  QPushButton* m_profiles_load;
  QPushButton* m_profiles_save;
  QPushButton* m_profiles_delete;

  // Triggers
  QGroupBox* m_triggers_box;
  QHBoxLayout* m_triggers_layout;
  QLabel* m_selected_trigger_name;
  QPushButton* m_select_trigger;

  PostProcessingShaderListWidget* m_pp_list_widget = nullptr;
};
