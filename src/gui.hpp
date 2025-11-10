#pragma once

#include "audio/audio.hpp"
#include "config.hpp"
#include "network.hpp"
#include <QDebug>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QStandardItemModel>
#include <QStyleFactory>
#include <QWidgetAction>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
class SettingsTab;
} // namespace Ui
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

  public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

  private:
    Ui::MainWindow *ui;
    Ui::SettingsTab *uiSettings;
    Config config;
    Audio audioManager;
    Network networkManager;
    QStandardItemModel *model;
    QWidget *settingsTab;
    bool isInitialDeviceSetup = true;
    QString currentInputDeviceId;
    QString currentOutputDeviceId;
    void openTextChannelTab(const QString &channelName);
    void updateAudioDeviceComboBox();
    bool validateInputDevice();
    bool validateOutputDevice();

  private slots:
    void disconnect();
    void showConnectDialog();
    void showContextMenu(const QPoint &pos);
    void sendMessage();
    void sendMessage(const QString &channelName);
    void addChannels(const QStringList &channels);
    void onTreeViewItemClicked(const QModelIndex &index);
    void closeTab(int index);
    void onUserJoinedChannel(const QString &user, const QString &channel);
    void setInputDevice();
    void setOutputDevice();
    void setInputDeviceWithoutSaving();
    void setOutputDeviceWithoutSaving();
    void onDefaultDeviceChanged(bool isInput);
    void onPlaybackVolumeChanged(int value);
    void onCaptureVolumeChanged(int value);
    void onVolumeChanged(bool isInput, float volume);
    void onStyleChanged(int index);
};