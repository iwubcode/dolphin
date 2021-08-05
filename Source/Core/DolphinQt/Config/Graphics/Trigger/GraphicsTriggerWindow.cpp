// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt/Config/Graphics/Trigger/GraphicsTriggerWindow.h"

#include "DolphinQt/Config/Graphics/Trigger/GraphicsTriggerWidget.h"

#include <QDialogButtonBox>
#include <QVBoxLayout>

GraphicsTriggerWindow::GraphicsTriggerWindow(QWidget* parent)
    : QDialog(parent)
{
  CreateMainLayout();

  setWindowTitle(tr("Graphics Trigger Configuration"));
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
}

void GraphicsTriggerWindow::CreateMainLayout()
{
  m_button_box = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(m_button_box, &QDialogButtonBox::accepted, this, [this]() {
    m_graphics_trigger_widget->SaveSettings();
    accept();
  });
  connect(m_button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto* main_layout = new QVBoxLayout();

  m_graphics_trigger_widget = new GraphicsTriggerWidget(this);
  main_layout->addWidget(m_graphics_trigger_widget);

  main_layout->addWidget(m_button_box);
  setLayout(main_layout);
}
