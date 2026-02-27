#pragma once

#include "audio/audio.hpp"
#include "auth.hpp"
#include "config.hpp"
#include "network.hpp"
#include "chat/text.hpp"
#include "chat/emote_manager.hpp"
#include "chat/emote_completer.hpp"
#include "video/video.hpp"
#include "video/render/vulkan_renderer.hpp"
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
#include <QRegularExpression>
#include <QTextBrowser>
#include <QTextEdit>
#include <QScrollBar>
#include <QSet>
#include <QKeyEvent>
#include <QTimer>
#include <QStatusBar>
#include <QToolButton>
#include <QPainter>
#include <QTreeWidgetItem>
#include <QPushButton>

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

  protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

  private:
    struct ChannelHistoryState {
        int oldestMessageId = 0;
        bool hasMore = true;
        bool loading = false;
    };

    Ui::MainWindow *ui;
    Ui::SettingsTab *uiSettings;
    Ui::adminPanelTab *uiAdminPanel;
    Config config;
    Auth authManager;
    Audio audioManager;
    Network networkManager;
    Video videoManager;
    Text textManager;
    EmoteManager emoteManager;
    QStandardItemModel *model;
    QStandardItemModel *accountsModel;
    QWidget *settingsTab;
    QWidget *adminPanelTab;
    VulkanWindow *vulkanWindow;
    QWidget *vulkanTab;
    QToolButton *tabSettingsBtn;
    QToolButton *tabAdminPanelBtn;
    QWidget *welcomeTab;
    QPushButton *welcomeConnectButton;
    QToolButton *sbChannelsBtn;
    QToolButton *sbMicBtn;
    QToolButton *sbHeadphonesBtn;
    QToolButton *sbMonitorBtn;
    QToolButton *sbUsersBtn;
    bool isInitialDeviceSetup = true;
    QString currentInputDeviceId;
    QString currentOutputDeviceId;
    QMap<QString, ChannelHistoryState> channelHistoryState;
    QSet<QString> pendingScrollToBottom;
    QMap<QString, QTimer*> typingThrottleTimers;
    QMap<QString, QMap<QString, QTimer*>> typingUserTimers;
    static constexpr int kHistoryPageSize = 50;
    static constexpr int kTypingThrottleMs = 3000;
    static constexpr int kTypingTimeoutMs = 5000;
    void openTextChannelTab(const QString &channelName);
    void updateAudioDeviceComboBox();
    bool validateInputDevice();
    bool validateOutputDevice();
    void sendAdminRequest(const QString &requestType);
    void displayMessage(const QString &channel, const QString &sender, const QString &content, const QDateTime &timestamp);
    void onHistoryReceived(const QString &channel, const QList<Message> &messages);
    void onChatScrolled(int value);
    void updateTypingLabel(const QString &channel);
    QString formatMessage(const QString &text);

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
    void onUserLeftChannel(const QString &user, const QString &channel);
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
    void onUsersListReceived(const QStringList& onlineUsers, const QStringList& offlineUsers);
    void onUserStatusChanged(const QString& user, bool online);
};