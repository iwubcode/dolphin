// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt/Config/Graphics/Trigger/NewDrawCall2DTriggerDialog.h"

#include <fmt/format.h>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "DolphinQt/Config/ToolTipControls/ToolTipComboBox.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipLineEdit.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipSpinBox.h"

NewDrawCall2DTriggerDialog::NewDrawCall2DTriggerDialog(QWidget* parent) : QDialog(parent)
{
  CreateMainLayout();

  setWindowTitle(tr("New DrawCall2D Trigger"));
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
}

const DrawCall2DGraphicsTrigger& NewDrawCall2DTriggerDialog::GetTrigger() const
{
  return m_trigger;
}

QString NewDrawCall2DTriggerDialog::GetName() const
{
  return m_name->text();
}

void NewDrawCall2DTriggerDialog::CreateMainLayout()
{
  auto* main_layout = new QVBoxLayout();

  m_button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(m_button_box, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(m_button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto form_layout = new QFormLayout;

  m_name = new ToolTipLineEdit;

  m_width = new ToolTipSpinBox;
  connect(m_width, qOverload<int>(&QSpinBox::valueChanged), this,
          [this](int value) { m_trigger.width = value; });
  m_width_operation = new ToolTipComboBox;
  for (u32 i = 0; i < static_cast<u32>(NumericOperation::Less_Equal); i++)
  {
    m_width_operation->addItem(QString::fromStdString(fmt::to_string(NumericOperation(i))));
  }
  connect(m_width_operation, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int value) {
            m_trigger.width_operation = static_cast<NumericOperation>(value);
            m_width->setEnabled(m_trigger.width_operation != NumericOperation::Any);
          });

  m_height = new ToolTipSpinBox;
  connect(m_width, qOverload<int>(&QSpinBox::valueChanged), this,
          [this](int value) { m_trigger.height = value; });
  m_height_operation = new ToolTipComboBox;
  for (u32 i = 0; i < static_cast<u32>(NumericOperation::Less_Equal); i++)
  {
    m_height_operation->addItem(QString::fromStdString(fmt::to_string(NumericOperation(i))));
  }
  connect(m_height_operation, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int value) {
            m_trigger.height_operation = static_cast<NumericOperation>(value);
            m_height->setEnabled(m_trigger.height_operation != NumericOperation::Any);
          });

  m_format = new ToolTipSpinBox;
  m_format->setMinimum(static_cast<int>(TextureFormat::I4));
  m_format->setMaximum(static_cast<int>(TextureFormat::CMPR));
  m_format_operation = new ToolTipComboBox;
  for (u32 i = 0; i < static_cast<u32>(MultiGenericOperation::Any); i++)
  {
    m_format_operation->addItem(QString::fromStdString(fmt::to_string(MultiGenericOperation(i))));
  }
  connect(m_format_operation, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int value) {
            m_trigger.format_operation = static_cast<MultiGenericOperation>(value);
            m_format->setEnabled(m_trigger.format_operation != MultiGenericOperation::Any);
          });

  m_tlut = new ToolTipLineEdit;
  m_tlut_operation = new ToolTipComboBox;
  for (u32 i = 0; i < static_cast<u32>(GenericOperation::Any); i++)
  {
    m_tlut_operation->addItem(QString::fromStdString(fmt::to_string(GenericOperation(i))));
  }
  connect(m_tlut_operation, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int value) {
            m_trigger.tlut_operation = static_cast<GenericOperation>(value);
            m_tlut->setEnabled(m_trigger.tlut_operation != GenericOperation::Any);
          });

  m_hash = new ToolTipLineEdit;
  m_hash_operation = new ToolTipComboBox;
  for (u32 i = 0; i < static_cast<u32>(GenericOperation::Any); i++)
  {
    m_hash_operation->addItem(QString::fromStdString(fmt::to_string(GenericOperation(i))));
  }
  connect(m_hash_operation, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int value) {
            m_trigger.hash_operation = static_cast<GenericOperation>(value);
            m_hash->setEnabled(m_trigger.hash_operation != GenericOperation::Any);
          });

  form_layout->addRow(tr("Name:"), m_name);
  form_layout->addRow(tr("Width:"), m_width);
  form_layout->addRow(tr("Width Operation:"), m_width_operation);
  form_layout->addRow(tr("Height:"), m_height);
  form_layout->addRow(tr("Height Operation:"), m_height_operation);
  form_layout->addRow(tr("Format:"), m_format);
  form_layout->addRow(tr("Format Operation:"), m_format_operation);
  form_layout->addRow(tr("Tlut:"), m_tlut);
  form_layout->addRow(tr("Tlut Operation:"), m_tlut_operation);
  form_layout->addRow(tr("Hash:"), m_hash);
  form_layout->addRow(tr("Hash Operation:"), m_hash_operation);

  main_layout->addLayout(form_layout);
  main_layout->addWidget(m_button_box);
  setLayout(main_layout);
}
