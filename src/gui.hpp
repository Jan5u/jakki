#pragma once

#include <QMainWindow>
#include "network.hpp"
#include "audio.hpp"

namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
  public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

  private:
    Ui::MainWindow *ui;
    Audio audioManager;
    Network networkManager;
  
  private slots:
    void buttonclick();
    void disconnect();
    void showConnectDialog();
};