#pragma once

#include <QMainWindow>
#include <QStandardItemModel>
#include <QWidgetAction>
#include <QLabel>
#include "network.hpp"
#include "audio/audio.hpp"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
  Q_OBJECT

  public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

  private:
    Ui::MainWindow *ui;
    Audio audioManager;
    Network networkManager;
    QStandardItemModel *model;
    QWidget *settingsTab;
    void openTextChannelTab(const QString &channelName);
    void updateAudioDeviceComboBox();
  
  private slots:
    void disconnect();
    void showConnectDialog();
    void showContextMenu(const QPoint &pos);
    void sendMessage();
    void sendMessage(const QString &channelName);
    void addChannels(const QStringList& channels);
    void onTreeViewItemClicked(const QModelIndex &index);
    void closeTab(int index);
    void onUserJoinedChannel(const QString& user, const QString& channel);
};