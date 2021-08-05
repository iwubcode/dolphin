#include "DolphinQt/Config/Graphics/Trigger/GraphicsTriggerWidget.h"

#include <QAction>
#include <QBoxLayout>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>

#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "DolphinQt/Config/Graphics/Trigger/NewDrawCall2DTriggerDialog.h"
#include "DolphinQt/Config/Graphics/Trigger/NewDrawCall3DTriggerDialog.h"
#include "DolphinQt/Config/Graphics/Trigger/NewEFBTriggerDialog.h"
#include "DolphinQt/Config/Graphics/Trigger/NewTextureLoadTriggerDialog.h"
#include "VideoCommon/GraphicsTriggerManager.h"

GraphicsTriggerWidget::GraphicsTriggerWidget(QWidget* parent) : QWidget(parent)
{
  CreateLayout();
  LoadSettings();
  ReloadTriggerList();
}

void GraphicsTriggerWidget::LoadSettings()
{
  const std::string profiles_path =
      File::GetUserPath(D_CONFIG_IDX) + GraphicsTriggerManager::directory_path;
  for (const auto& filename : Common::DoFileSearch({profiles_path}, {".ini"}))
  {
    GraphicsTrigger trigger;
    if (!LoadFromFile(trigger, filename))
    {
      continue;
    }

    std::string basename;
    SplitPath(filename, nullptr, &basename, nullptr);
    m_user_triggers.insert_or_assign(basename, trigger);
  }
}

void GraphicsTriggerWidget::SaveSettings()
{
}

void GraphicsTriggerWidget::CreateLayout()
{
  auto* main_layout = new QVBoxLayout;

  m_triggers = new QListWidget();
  main_layout->addWidget(m_triggers);

  m_add_trigger = new QToolButton();
  m_add_trigger_menu = new QMenu(m_add_trigger);

  auto add_efb = new QAction(tr("EFB..."), m_add_trigger_menu);
  connect(add_efb, &QAction::triggered, [this] { AddEFBTrigger(); });

  auto add_texture_load = new QAction(tr("Texture Load..."), m_add_trigger_menu);
  connect(add_texture_load, &QAction::triggered, [this] { AddTextureLoadTrigger(); });

  auto add_3ddraw = new QAction(tr("3D Draw Call..."), m_add_trigger_menu);
  connect(add_3ddraw, &QAction::triggered, [this] { Add3DDrawCallTrigger(); });

  auto add_2ddraw = new QAction(tr("2D Draw Call..."), m_add_trigger_menu);
  connect(add_2ddraw, &QAction::triggered, [this] { Add2DDrawCallTrigger(); });

  m_add_trigger_menu->addAction(add_efb);
  m_add_trigger_menu->addAction(add_texture_load);
  m_add_trigger_menu->addAction(add_3ddraw);
  m_add_trigger_menu->addAction(add_2ddraw);
  m_add_trigger_menu->setDefaultAction(add_efb);
  m_add_trigger->setPopupMode(QToolButton::MenuButtonPopup);
  m_add_trigger->setMenu(m_add_trigger_menu);

  m_remove_trigger = new QPushButton(tr("Remove"));
  QHBoxLayout* hlayout = new QHBoxLayout;
  hlayout->addStretch();
  hlayout->addWidget(m_add_trigger);
  hlayout->addWidget(m_remove_trigger);

  main_layout->addItem(hlayout);
  setLayout(main_layout);
}

void GraphicsTriggerWidget::ReloadTriggerList()
{
  m_triggers->clear();
  for (const auto& [name, trigger] : m_user_triggers)
  {
    m_triggers->addItem(QString::fromStdString(name));
  }
}

void GraphicsTriggerWidget::AddEFBTrigger()
{
  NewEFBTriggerDialog trigger_dialog(this);
  if (trigger_dialog.exec() == QDialog::Accepted)
  {
    m_user_triggers.insert_or_assign(trigger_dialog.GetName().toStdString(),
                                     trigger_dialog.GetTrigger());
    ReloadTriggerList();
  }
}

void GraphicsTriggerWidget::Add2DDrawCallTrigger()
{
  NewDrawCall2DTriggerDialog trigger_dialog(this);
  if (trigger_dialog.exec() == QDialog::Accepted)
  {
    m_user_triggers.insert_or_assign(trigger_dialog.GetName().toStdString(),
                                     trigger_dialog.GetTrigger());
    ReloadTriggerList();
  }
}

void GraphicsTriggerWidget::Add3DDrawCallTrigger()
{
  NewDrawCall3DTriggerDialog trigger_dialog(this);
  if (trigger_dialog.exec() == QDialog::Accepted)
  {
    m_user_triggers.insert_or_assign(trigger_dialog.GetName().toStdString(),
                                     trigger_dialog.GetTrigger());
    ReloadTriggerList();
  }
}

void GraphicsTriggerWidget::AddTextureLoadTrigger()
{
  NewTextureLoadTriggerDialog trigger_dialog(this);
  if (trigger_dialog.exec() == QDialog::Accepted)
  {
    m_user_triggers.insert_or_assign(trigger_dialog.GetName().toStdString(),
                                     trigger_dialog.GetTrigger());
    ReloadTriggerList();
  }
}
