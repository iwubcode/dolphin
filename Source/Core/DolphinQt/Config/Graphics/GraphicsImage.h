// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <QPushButton>
#include <QFileInfo>

class GraphicsImage final : public QPushButton
{
  Q_OBJECT
public:
  explicit GraphicsImage(QWidget* parent = nullptr);

  std::string GetPath() const { return m_file_info.filePath().toStdString(); }

signals:
  void PathIsUpdated();
public slots:
  void UpdateImage();
  void SelectImage();

private:
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  QFileInfo m_file_info;
};
