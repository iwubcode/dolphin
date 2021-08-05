// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt/Config/Graphics/Trigger/GraphicsTriggerSelectionDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "DolphinQt/Config/ToolTipControls/ToolTipComboBox.h"

#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "VideoCommon/GraphicsTrigger.h"
#include "VideoCommon/GraphicsTriggerManager.h"

GraphicsTriggerSelectionDialog::GraphicsTriggerSelectionDialog(QWidget* parent) : QDialog(parent)
{
  CreateMainLayout();
  ConnectWidgets();

  setWindowTitle(tr("Select Trigger"));
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
}

QString GraphicsTriggerSelectionDialog::GetTriggerName() const
{
  return m_triggers->currentText();
}

void GraphicsTriggerSelectionDialog::CreateMainLayout()
{
  auto* main_layout = new QVBoxLayout();

  m_button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(m_button_box, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(m_button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  //m_button_box->button(QDialogButtonBox::Ok)->setEnabled(false);

  auto form_layout = new QFormLayout;

  m_triggers = new ToolTipComboBox;
  BuildComboBox();

  form_layout->addRow(tr("Trigger:"), m_triggers);

  main_layout->addLayout(form_layout);
  main_layout->addWidget(m_button_box);
  setLayout(main_layout);
}

void GraphicsTriggerSelectionDialog::BuildComboBox()
{
  m_triggers->clear();

  std::vector<std::string> items;

  const std::string user_profiles_path =
      File::GetUserPath(D_CONFIG_IDX) + GraphicsTriggerManager::directory_path;
  for (const auto& filename : Common::DoFileSearch({user_profiles_path}, {".ini"}))
  {
    std::string basename;
    SplitPath(filename, nullptr, &basename, nullptr);
    items.push_back(basename);
  }

  const std::string system_profiles_path =
      File::GetSysDirectory() + GraphicsTriggerManager::directory_path;
  for (const auto& filename : Common::DoFileSearch({system_profiles_path}, {".ini"}))
  {
    std::string basename;
    SplitPath(filename, nullptr, &basename, nullptr);
    items.push_back(basename);
  }

  std::sort(items.begin(), items.end());

  for (const auto& item : items)
  {
    m_triggers->addItem(QString::fromStdString(item));
  }
}

void GraphicsTriggerSelectionDialog::ConnectWidgets()
{
  connect(m_triggers, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int) { m_button_box->button(QDialogButtonBox::Ok)->setEnabled(true); });
}

