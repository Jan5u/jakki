#pragma once

#include "audio/audio.hpp"
#include "auth.hpp"
#include "config.hpp"
#include "network.hpp"
#include "video/video.hpp"
#include "video/vulkan_renderer.hpp"
#include <QDebug>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QStandardItemModel>
#include <QStyleFactory>
#include <QWidgetAction>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
class SettingsTab;
class adminPanelTab;
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
    Ui::adminPanelTab *uiAdminPanel;
    Config config;
    Auth authManager;
    Audio audioManager;
    Network networkManager;
    Video videoManager;
    QStandardItemModel *model;
    QStandardItemModel *accountsModel;
    QWidget *settingsTab;
    QWidget *adminPanelTab;
    VulkanWindow *vulkanWindow;
    QWidget *vulkanTab;
    bool isInitialDeviceSetup = true;
    QString currentInputDeviceId;
    QString currentOutputDeviceId;
    void openTextChannelTab(const QString &channelName);
    void updateAudioDeviceComboBox();
    bool validateInputDevice();
    bool validateOutputDevice();
    void sendAdminRequest(const QString &requestType);

  public slots:
    void onFrameQueued(int colorValue);

  private slots:
    void disconnect();
    void showConnectDialog();
    void showContextMenu(const QPoint &pos);
    void sendMessage();
    void sendMessage(const QString &channelName);
    void showScreenShareDialog();
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
    void requestUsersDatabase();
    void handleAdminResponse(const QString& request, const QString& jsonData);
    void approveSelectedUser();
};