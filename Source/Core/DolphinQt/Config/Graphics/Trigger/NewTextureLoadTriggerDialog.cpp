// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt/Config/Graphics/Trigger/NewTextureLoadTriggerDialog.h"

#include <fmt/format.h>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "DolphinQt/Config/ToolTipControls/ToolTipLineEdit.h"

NewTextureLoadTriggerDialog::NewTextureLoadTriggerDialog(QWidget* parent) : QDialog(parent)
{
  CreateMainLayout();

  setWindowTitle(tr("New Texture Load Trigger"));
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
}

const TextureLoadGraphicsTrigger& NewTextureLoadTriggerDialog::GetTrigger() const
{
  return m_trigger;
}

QString NewTextureLoadTriggerDialog::GetName() const
{
  return m_name->text();
}

void NewTextureLoadTriggerDialog::CreateMainLayout()
{
  auto* main_layout = new QVBoxLayout();

  m_button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(m_button_box, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(m_button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto form_layout = new QFormLayout;

  m_name = new ToolTipLineEdit;

  m_texture_id = new ToolTipLineEdit;
  connect(m_texture_id, &QLineEdit::textChanged, this,
          [this](const QString& value) { m_trigger.texture_id = value.toStdString(); });

  form_layout->addRow(tr("Name:"), m_name);
  form_layout->addRow(tr("Texture id:"), m_texture_id);

  main_layout->addLayout(form_layout);
  main_layout->addWidget(m_button_box);
  setLayout(main_layout);
}
