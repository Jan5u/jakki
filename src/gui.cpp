#include "audio/audio_impl.hpp"
#include "gui.hpp"
#include "ui_connect.h"
#include "ui_main.h"
#include "ui_textchannel.h"
#include "ui_settings.h"
#include "ui_adminpanel.h"
#include "ui_screenshare.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow), audioManager(networkManager, config), networkManager(audioManager, authManager), videoManager(config, networkManager), textManager(networkManager), emoteManager(networkManager) {
    ui->setupUi(this);
    connect(ui->actionQuit, &QAction::triggered, this, &QApplication::quit);
    connect(ui->actionDisconnect, &QAction::triggered, this, &MainWindow::disconnect);
    connect(ui->actionNew_Connection, &QAction::triggered, this, &MainWindow::showConnectDialog);
    connect(ui->actionAbout_Qt, &QAction::triggered, this, &QApplication::aboutQt);
    connect(ui->treeView, &QTreeView::customContextMenuRequested, this, &MainWindow::showContextMenu);
    connect(ui->treeView, &QTreeView::clicked, this, &MainWindow::onTreeViewItemClicked);
    connect(&networkManager, &Network::channelsReceived, this, &MainWindow::addChannels);
    connect(&networkManager, &Network::userJoinedChannel, this, &MainWindow::onUserJoinedChannel);
    connect(&networkManager, &Network::authenticationFailed, this, [this](const QString& reason) {
        qDebug() << "Authentication failed:" << reason;
        // TODO: Show error dialog to user
    });
    connect(&networkManager, &Network::adminResponseReceived, this, &MainWindow::handleAdminResponse);
    connect(&textManager, &Text::messageReceived, this, &MainWindow::displayMessage);
    connect(&textManager, &Text::historyReceived, this, &MainWindow::onHistoryReceived);
    connect(&textManager, &Text::typingIndicatorReceived, this, [this](const QString &channel, const QString &user) {
        for (int i = 0; i < ui->tabWidget->count(); ++i) {
            if (ui->tabWidget->tabText(i) == channel) {
                QWidget *tab = ui->tabWidget->widget(i);
                QLabel *typingLabel = tab->findChild<QLabel *>("typingLabel");
                if (!typingLabel)
                    break;

                auto &userTimers = typingUserTimers[channel];
                if (!userTimers.contains(user)) {
                    QTimer *timer = new QTimer(tab);
                    timer->setSingleShot(true);
                    connect(timer, &QTimer::timeout, this, [this, channel, user]() {
                        typingUserTimers[channel].remove(user);
                        updateTypingLabel(channel);
                    });
                    userTimers[user] = timer;
                }
                userTimers[user]->start(kTypingTimeoutMs);
                updateTypingLabel(channel);
                break;
            }
        }
    });
    connect(&networkManager, &Network::channelsReceived, &emoteManager, [this](const QStringList&) {
        emoteManager.requestEmotes();
    });
    connect(&audioManager, &Audio::deviceListChanged, this, &MainWindow::updateAudioDeviceComboBox);
    connect(&audioManager, &Audio::defaultDeviceChanged, this, &MainWindow::onDefaultDeviceChanged);
    connect(&audioManager, &Audio::volumeChanged, this, &MainWindow::onVolumeChanged);
    connect(ui->actionShare_Screen, &QAction::triggered, this, &MainWindow::showScreenShareDialog);

    model = new QStandardItemModel(this);
    model->setHorizontalHeaderLabels({"Channels"});

    ui->treeView->setModel(model);
    ui->treeView->expandAll();

    settingsTab = new QWidget;
    uiSettings = new Ui::SettingsTab;
    uiSettings->setupUi(settingsTab);
    ui->tabWidget->addTab(settingsTab, "Settings");

    // Setup admin panel tab
    adminPanelTab = new QWidget;
    uiAdminPanel = new Ui::adminPanelTab;
    uiAdminPanel->setupUi(adminPanelTab);
    ui->tabWidget->addTab(adminPanelTab, "Admin Panel");
    
    // Connect admin panel signals
    connect(uiAdminPanel->accountspushButton, &QPushButton::clicked, this, &MainWindow::requestUsersDatabase);
    connect(uiAdminPanel->approvepushButton, &QPushButton::clicked, this, &MainWindow::approveSelectedUser);
    
    // Setup accounts table
    accountsModel = new QStandardItemModel(this);
    accountsModel->setHorizontalHeaderLabels({"ID", "Username", "Public Key", "Admin", "Approved", "Created", "Last Auth"});
    uiAdminPanel->accountstableView->setModel(accountsModel);
    

    vulkanWindow = videoManager.createVulkanWindow();
    connect(vulkanWindow, &VulkanWindow::frameQueued, this, &MainWindow::onFrameQueued);
    vulkanTab = videoManager.createVulkanTab(this);
    if (vulkanTab) {
        ui->tabWidget->addTab(vulkanTab, "Screen");
    }
    videoManager.startDecodeThread();
    
    networkManager.setVideoManager(&videoManager);
    
    // Initialize audio device combo boxes
    updateAudioDeviceComboBox();

    // Connect audio device combobox signals
    connect(uiSettings->InputDeviceComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::setInputDevice);
    connect(uiSettings->OutputDeviceComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::setOutputDevice);

    // Initialize volume sliders with current volumes from audio manager
    float inputVolume = audioManager.getVolume(true);
    float outputVolume = audioManager.getVolume(false);
    uiSettings->captureVolumeSlider->setValue(static_cast<int>(inputVolume * 100));
    uiSettings->playbackVolumeSlider->setValue(static_cast<int>(outputVolume * 100));

    // Connect volume slider signals
    connect(uiSettings->playbackVolumeSlider, &QSlider::valueChanged, this, &MainWindow::onPlaybackVolumeChanged);
    connect(uiSettings->captureVolumeSlider, &QSlider::valueChanged, this, &MainWindow::onCaptureVolumeChanged);

    uiSettings->StyleSelectComboBox->clear();
    QDirIterator it(":/styles", QStringList() << "*.qss", QDir::Files);
    while (it.hasNext()) {
        QString filePath = it.next();
        QFileInfo fileInfo(filePath);
        QString themeName = fileInfo.baseName();
        if (!themeName.isEmpty()) {
            themeName[0] = themeName[0].toUpper();
        }
        uiSettings->StyleSelectComboBox->addItem(themeName, filePath);
    }
    QStringList availableStyles = QStyleFactory::keys();
    for (const QString &style : availableStyles) {
        uiSettings->StyleSelectComboBox->addItem(style);
    }
    
    // Load saved theme from config
    QString savedTheme = QString::fromStdString(config.getTheme());
    int themeIndex = uiSettings->StyleSelectComboBox->findText(savedTheme, Qt::MatchFixedString);
    if (themeIndex >= 0) {
        uiSettings->StyleSelectComboBox->setCurrentIndex(themeIndex);
    } else {
        uiSettings->StyleSelectComboBox->setCurrentIndex(0);
    }
    onStyleChanged(uiSettings->StyleSelectComboBox->currentIndex());
    connect(uiSettings->StyleSelectComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onStyleChanged);
}

static QString markdownToHtml(const QString &text) {
    QTextDocument doc;
    doc.setMarkdown(text, QTextDocument::MarkdownDialectGitHub);
    QString html = doc.toHtml();
    static QRegularExpression bodyRegex(R"(<body[^>]*>(.*)</body>)", QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatch match = bodyRegex.match(html);
    if (match.hasMatch()) {
        html = match.captured(1).trimmed();
    }
    html.replace(QLatin1String("<pre"), QLatin1String("<pre style=\"background-color:rgb(40,40,40); padding:8px; margin:4px 0;\""));
    html.replace(QLatin1String("<code"), QLatin1String("<code style=\"background-color:rgb(40,40,40); font-family:monospace;\""));
    static QRegularExpression singleParagraphRegex(R"(^<p[^>]*>(.*)</p>$)", QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatch pMatch = singleParagraphRegex.match(html);
    if (pMatch.hasMatch()) {
        return pMatch.captured(1);
    }
    return html;
}

static void appendMessage(QTextBrowser *browser, const QString &time, const QString &sender, const QString &formattedContent) {
    browser->append(QString("<p style=\"margin:0px; line-height:16px;\">&nbsp;</p><p style=\"margin:0px;\"><b>%1</b> <span "
                            "style=\"color:gray;\">%2</span></p><p style=\"margin:0px;\">%3</p>")
                        .arg(sender, time, formattedContent));
}

QString MainWindow::formatMessage(const QString &text) {
    QString result = markdownToHtml(text);
    result = emoteManager.processEmoteCodes(result);
    return result;
}

void MainWindow::showScreenShareDialog() {
    QDialog dialog(this);
    Ui::screenShareDialog uiDialog;
    uiDialog.setupUi(&dialog);

    videoManager.selectScreen();

    QMap<QString, QString> nvidiaMap = {
        { "H264", "h264_nvenc" },
        { "H265", "hevc_nvenc" },
        { "AV1",  "av1_nvenc" }
    };

    QMap<QString, QString> vulkanMap = {
        { "H264", "h264_vulkan" },
        { "H265", "hevc_vulkan" },
        { "AV1",  "av1_vulkan" }
    };

    uiDialog.encodersComboBox->clear();
    
    bool hasNVIDIA = !videoManager.supportedNVIDIAEncoders.empty();
    bool hasVulkan = !videoManager.supportedVulkanEncoders.empty();
    
    if (hasNVIDIA) {
        uiDialog.encodersComboBox->addItem("NVIDIA", "nvidia");
    }
    if (hasVulkan) {
        uiDialog.encodersComboBox->addItem("Vulkan", "vulkan");
    }
    
    auto updateFormatsComboBox = [&]() {
        uiDialog.formatsComboBox->clear();
        
        QString selectedEncoderType = uiDialog.encodersComboBox->currentData().toString();
        const auto& supportedEncoders = (selectedEncoderType == "nvidia") 
            ? videoManager.supportedNVIDIAEncoders 
            : videoManager.supportedVulkanEncoders;
        
        const auto& encoderMap = (selectedEncoderType == "nvidia") ? nvidiaMap : vulkanMap;
        
        for (auto it = encoderMap.constBegin(); it != encoderMap.constEnd(); ++it) {
            const QString& format = it.key();
            const QString& codecName = it.value();
            
            bool isSupported = std::any_of(
                supportedEncoders.begin(), 
                supportedEncoders.end(),
                [&codecName](const std::string& encoder) {
                    return QString::fromStdString(encoder) == codecName;
                }
            );
            
            if (isSupported) {
                uiDialog.formatsComboBox->addItem(format, codecName);
            }
        }
    };
    
    updateFormatsComboBox();
    
    connect(uiDialog.encodersComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [&updateFormatsComboBox]() { updateFormatsComboBox(); });

    if (dialog.exec() == QDialog::Accepted) {
        QString selectedEncoderType = uiDialog.encodersComboBox->currentText();
        QString selectedFormat = uiDialog.formatsComboBox->currentText();
        QString selectedCodec = uiDialog.formatsComboBox->currentData().toString();
        
        qDebug() << "Selected encoder type:" << selectedEncoderType;
        qDebug() << "Selected format:" << selectedFormat;
        qDebug() << "Selected codec:" << selectedCodec;
    }
}

MainWindow::~MainWindow() {
    delete ui;
    delete uiSettings;
    networkManager.disconnectQUIC();
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event) {
    if (event->type() == QEvent::KeyPress) {
        auto *textEdit = qobject_cast<QTextEdit *>(obj);
        if (textEdit && textEdit->objectName() == "messageLineEdit") {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) && !(keyEvent->modifiers() & Qt::ShiftModifier)) {
                QWidget *tab = textEdit->parentWidget();
                if (tab) {
                    auto *completer = tab->findChild<EmoteCompleter *>();
                    if (completer && completer->isVisible()) {
                        return false;
                    }
                }
                QString channelName = textEdit->property("channelName").toString();
                if (!channelName.isEmpty()) {
                    sendMessage(channelName);
                    return true;
                }
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::disconnect() {
    qDebug("disconnect");
    networkManager.disconnectQUIC();
    
    // Clear the channel list
    model->clear();
    model->setHorizontalHeaderLabels({"Channels"});
}

void MainWindow::showConnectDialog() {
    QDialog dialog(this);
    Ui::connectDialog uiDialog;
    uiDialog.setupUi(&dialog);

    if (dialog.exec() == QDialog::Accepted) {
        QString address = uiDialog.lineEdit->text();
        QString port = uiDialog.lineEdit_2->text();
        QString username = uiDialog.usernamelineEdit_3->text();
        
        qDebug() << "Connect to:" << address << ":" << port << "as user:" << username;
        
        // Set username in auth manager
        if (!username.isEmpty()) {
            authManager.setUsername(username);
        } else {
            qDebug() << "Warning: No username provided, using default";
            authManager.setUsername("default_user");
        }
        
        networkManager.connectToServer(address, port);
    }
}

void MainWindow::showContextMenu(const QPoint &pos) {
    QModelIndex index = ui->treeView->indexAt(pos);
    if (!index.isValid()) return;

    QMenu *menu = new QMenu(this);
    QWidget *volumeWidget = new QWidget;
    volumeWidget->setMinimumWidth(150);
    QVBoxLayout *layout = new QVBoxLayout(volumeWidget);
    layout->setContentsMargins(8, 4, 8, 4);
    QLabel *label = new QLabel("Volume");
    QSlider *volumeSlider = new QSlider(Qt::Horizontal);
    volumeSlider->setMinimum(0);
    volumeSlider->setMaximum(100);
    volumeSlider->setValue(50);
    layout->addWidget(label);
    layout->addWidget(volumeSlider);
    QWidgetAction *sliderAction = new QWidgetAction(menu);
    sliderAction->setDefaultWidget(volumeWidget);
    menu->addAction(sliderAction);
    menu->exec(ui->treeView->viewport()->mapToGlobal(pos));
}

void MainWindow::sendMessage() {
    int currentIndex = ui->tabWidget->currentIndex();
    QString channelName = ui->tabWidget->tabText(currentIndex);
    sendMessage(channelName);
}

void MainWindow::sendMessage(const QString &channelName) {
    QWidget *currentTab = ui->tabWidget->currentWidget();
    QTextEdit *messageInput = currentTab->findChild<QTextEdit*>("messageLineEdit");
    if (messageInput) {
        QString message = messageInput->toPlainText().trimmed();
        if (!message.isEmpty()) {
            qDebug() << "Message:" << message << "to channel:" << channelName;
            textManager.sendMessage(channelName, message);
            messageInput->clear();
            pendingScrollToBottom.insert(channelName);
        }
    }
}

void MainWindow::addChannels(const QStringList& channels) {
    qDebug() << "Adding channels:" << channels;
    
    for (const QString& channelName : channels) {
        QStandardItem *channel;
        if (channelName.startsWith("#")) {
            channel = new QStandardItem(channelName);
        } else {
            channel = new QStandardItem(channelName);
        }
        model->appendRow(channel);
    }

    ui->treeView->expandAll();
}

void MainWindow::onTreeViewItemClicked(const QModelIndex &index) {
    if (!index.isValid()) return;
    
    QStandardItem *item = model->itemFromIndex(index);
    if (!item) return;
    
    QString channelName = item->text();
    qDebug() << "User clicked on channel:" << channelName;

    if (item->parent()) {
        qDebug() << "item parent:" << item->parent()->text();
        networkManager.joinScreenShare(item->text());
        return;
    }
    
    if (channelName.startsWith("#")) {
        qDebug() << "Text channel selected:" << channelName;
        openTextChannelTab(channelName);
    } else {
        qDebug() << "Voice channel selected:" << channelName;
        networkManager.joinVoiceChannel(channelName);
    }
}

void MainWindow::openTextChannelTab(const QString &channelName) {
    for (int i = 0; i < ui->tabWidget->count(); ++i) {
        if (ui->tabWidget->tabText(i) == channelName) {
            ui->tabWidget->setCurrentIndex(i);
            return;
        }
    }
    QWidget *textChannelTab = new QWidget;
    Ui::TextChannelTab uiTextChannel;
    uiTextChannel.setupUi(textChannelTab);
    connect(uiTextChannel.sendButton, &QPushButton::clicked, [this, channelName]() { sendMessage(channelName); });

    QTextEdit *messageInput = uiTextChannel.messageLineEdit;
    messageInput->setTabChangesFocus(false);
    messageInput->setAcceptRichText(false);
    messageInput->installEventFilter(this);
    messageInput->setProperty("channelName", channelName);

    QTimer *typingThrottle = new QTimer(textChannelTab);
    typingThrottle->setSingleShot(true);
    typingThrottleTimers[channelName] = typingThrottle;
    connect(messageInput->document(), &QTextDocument::contentsChanged, messageInput, [this, messageInput, channelName]() {
        if (messageInput->toPlainText().isEmpty())
            return;
        QTimer *throttle = typingThrottleTimers.value(channelName);
        if (throttle && !throttle->isActive()) {
            textManager.sendTypingIndicator(channelName);
            throttle->start(kTypingThrottleMs);
        }
    });

    EmoteCompleter *completer = new EmoteCompleter(emoteManager, textChannelTab);
    completer->attachToInput(messageInput);

    QTextBrowser *textBrowser = uiTextChannel.textBrowser;

    const int inputMinH = 36;
    const int inputMaxH = 150;
    connect(messageInput->document(), &QTextDocument::contentsChanged, messageInput, [messageInput, inputMinH, inputMaxH]() {
        QTextDocument *doc = messageInput->document();
        doc->setTextWidth(messageInput->viewport()->width());
        int docHeight = static_cast<int>(doc->size().height());
        int frameMargins = messageInput->frameWidth() * 2;
        int contentMargins = messageInput->contentsMargins().top() + messageInput->contentsMargins().bottom();
        int newHeight = qBound(inputMinH, docHeight + frameMargins + contentMargins, inputMaxH);
        messageInput->setMinimumHeight(newHeight);
        messageInput->setMaximumHeight(newHeight);
    });

    QScrollBar *scrollBar = textBrowser->verticalScrollBar();
    scrollBar->setProperty("channelName", channelName);
    connect(scrollBar, &QScrollBar::valueChanged, this, &MainWindow::onChatScrolled);
    connect(scrollBar, &QScrollBar::rangeChanged, textBrowser, [scrollBar](int, int max) {
        if (scrollBar->value() >= max - 20 || max <= 0) {
            scrollBar->setValue(max);
        }
    });

    int tabIndex = ui->tabWidget->addTab(textChannelTab, channelName);
    ui->tabWidget->setCurrentIndex(tabIndex);

    channelHistoryState[channelName] = ChannelHistoryState{};
    textManager.requestHistory(channelName, kHistoryPageSize);

    static bool closeConnected = false;
    if (!closeConnected) {
        connect(ui->tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
        closeConnected = true;
    }
}

void MainWindow::updateAudioDeviceComboBox() {
    // Block signals during update to prevent spurious triggers
    uiSettings->InputDeviceComboBox->blockSignals(true);
    uiSettings->OutputDeviceComboBox->blockSignals(true);

    // Load saved devices from config
    QString savedInputDevice = QString::fromStdString(config.getInputDevice());
    QString savedOutputDevice = QString::fromStdString(config.getOutputDevice());
    
    // Get current index
    int currentInputIndex = uiSettings->InputDeviceComboBox->currentIndex();
    int currentOutputIndex = uiSettings->OutputDeviceComboBox->currentIndex();

    // Clear existing items
    uiSettings->InputDeviceComboBox->clear();
    uiSettings->OutputDeviceComboBox->clear();
    
    // Add "Default" option
    uiSettings->InputDeviceComboBox->addItem("Default");
    uiSettings->OutputDeviceComboBox->addItem("Default");
    
    // Get and add input devices
    auto inputDevices = audioManager.getInputDevices();
    for (const auto& device : inputDevices) {
        uiSettings->InputDeviceComboBox->addItem(QString::fromStdString(device.name), QString::fromStdString(device.id));
    }
    
    // Get and add output devices
    auto outputDevices = audioManager.getOutputDevices();
    for (const auto& device : outputDevices) {
        uiSettings->OutputDeviceComboBox->addItem(QString::fromStdString(device.name), QString::fromStdString(device.id));
    }
    
    // Restore saved device selections by device ID
    if (!savedInputDevice.isEmpty()) {
        int inputIndex = uiSettings->InputDeviceComboBox->findData(savedInputDevice);
        if (inputIndex >= 0) {
            uiSettings->InputDeviceComboBox->setCurrentIndex(inputIndex);
            if (isInitialDeviceSetup) {
                currentInputDeviceId = savedInputDevice;
            }
            if (inputIndex != currentInputIndex) {
                if (!isInitialDeviceSetup) {
                    setInputDevice();
                }
            }
        } else {
            // Fallback to default audio device
            uiSettings->InputDeviceComboBox->setCurrentIndex(0);
            if (isInitialDeviceSetup) {
                currentInputDeviceId = "";
            }
            if (!isInitialDeviceSetup) {
                setInputDeviceWithoutSaving();
            }
        }
    }
    
    if (!savedOutputDevice.isEmpty()) {
        int outputIndex = uiSettings->OutputDeviceComboBox->findData(savedOutputDevice);
        if (outputIndex >= 0) {
            uiSettings->OutputDeviceComboBox->setCurrentIndex(outputIndex);
            if (isInitialDeviceSetup) {
                currentOutputDeviceId = savedOutputDevice;
            }
            if (outputIndex != currentOutputIndex) {
                if (!isInitialDeviceSetup) {
                    setOutputDevice();
                }
            }
        } else {
            // Fallback to default audio device
            uiSettings->OutputDeviceComboBox->setCurrentIndex(0);
            if (isInitialDeviceSetup) {
                currentOutputDeviceId = "";
            }
            if (!isInitialDeviceSetup) {
                setOutputDeviceWithoutSaving();
            }
        }
    }

    // Re-enable signals after update is complete
    uiSettings->InputDeviceComboBox->blockSignals(false);
    uiSettings->OutputDeviceComboBox->blockSignals(false);

    // Mark initial setup as complete after first run
    if (isInitialDeviceSetup) {
        isInitialDeviceSetup = false;
    }
    
    qDebug() << "Updated audio devices: " << inputDevices.size() << "input," << outputDevices.size() << "output";
}

void MainWindow::closeTab(int index) {
    QString channelName = ui->tabWidget->tabText(index);
    channelHistoryState.remove(channelName);
    QWidget *tab = ui->tabWidget->widget(index);
    ui->tabWidget->removeTab(index);
    delete tab;
}

void MainWindow::displayMessage(const QString &channel, const QString &sender, const QString &content, const QDateTime &timestamp) {
    if (typingUserTimers.contains(channel) && typingUserTimers[channel].contains(sender)) {
        typingUserTimers[channel][sender]->stop();
        typingUserTimers[channel].remove(sender);
        updateTypingLabel(channel);
    }

    for (int i = 0; i < ui->tabWidget->count(); ++i) {
        if (ui->tabWidget->tabText(i) == channel) {
            QWidget *tab = ui->tabWidget->widget(i);
            QTextBrowser *textEdit = tab->findChild<QTextBrowser *>("textBrowser");
            if (textEdit) {
                QString time = timestamp.toString("yyyy-MM-dd hh:mm");
                appendMessage(textEdit, time, sender.toHtmlEscaped(), formatMessage(content));
                if (pendingScrollToBottom.remove(channel)) {
                    QScrollBar *sb = textEdit->verticalScrollBar();
                    sb->setValue(sb->maximum());
                }
            }
            return;
        }
    }
    openTextChannelTab(channel);
}

void MainWindow::onHistoryReceived(const QString &channel, const QList<Message> &messages) {
    for (int i = 0; i < ui->tabWidget->count(); ++i) {
        if (ui->tabWidget->tabText(i) != channel)
            continue;

        QWidget *tab = ui->tabWidget->widget(i);
        QTextBrowser *textBrowser = tab->findChild<QTextBrowser *>("textBrowser");
        if (!textBrowser)
            return;

        auto &state = channelHistoryState[channel];
        state.loading = false;

        if (messages.isEmpty()) {
            state.hasMore = false;
            return;
        }

        if (messages.size() < kHistoryPageSize) {
            state.hasMore = false;
        }

        int oldestId = messages.first().id;
        for (const auto &msg : messages) {
            if (msg.id < oldestId)
                oldestId = msg.id;
        }
        state.oldestMessageId = oldestId;

        bool isInitialLoad = textBrowser->document()->isEmpty();

        if (isInitialLoad) {
            for (const auto &msg : messages) {
                QString time = msg.timestamp.toString("yyyy-MM-dd hh:mm");
                appendMessage(textBrowser, time, msg.sender.toHtmlEscaped(), formatMessage(msg.content));
            }
            QScrollBar *sb = textBrowser->verticalScrollBar();
            sb->setValue(sb->maximum());
        } else {
            QScrollBar *sb = textBrowser->verticalScrollBar();
            int oldMax = sb->maximum();
            int oldValue = sb->value();

            QString prependHtml;
            for (const auto &msg : messages) {
                QString time = msg.timestamp.toString("yyyy-MM-dd hh:mm");
                QString formattedContent = formatMessage(msg.content);
                QString senderEscaped = msg.sender.toHtmlEscaped();
                prependHtml += QString("<p style=\"margin:0px; line-height:16px;\">&nbsp;</p><p style=\"margin:0px;\"><b>%1</b> <span "
                                       "style=\"color:gray;\">%2</span></p><p style=\"margin:0px;\">%3</p>")
                                   .arg(senderEscaped, time, formattedContent);
            }

            QTextCursor cursor(textBrowser->document());
            cursor.movePosition(QTextCursor::Start);
            cursor.insertHtml(prependHtml);
            cursor.insertBlock();

            int newMax = sb->maximum();
            int delta = newMax - oldMax;
            sb->setValue(oldValue + delta);
        }
        return;
    }
}

void MainWindow::updateTypingLabel(const QString &channel) {
    for (int i = 0; i < ui->tabWidget->count(); ++i) {
        if (ui->tabWidget->tabText(i) == channel) {
            QWidget *tab = ui->tabWidget->widget(i);
            QLabel *typingLabel = tab->findChild<QLabel *>("typingLabel");
            if (!typingLabel)
                return;

            QStringList users = typingUserTimers[channel].keys();
            if (users.isEmpty()) {
                typingLabel->clear();
            } else if (users.size() == 1) {
                typingLabel->setText(users.first() + " is typing...");
            } else if (users.size() == 2) {
                typingLabel->setText(users[0] + " and " + users[1] + " are typing...");
            } else {
                typingLabel->setText(QString::number(users.size()) + " people are typing...");
            }
            return;
        }
    }
}

void MainWindow::onChatScrolled(int value) {
    if (value != 0)
        return;

    QScrollBar *scrollBar = qobject_cast<QScrollBar *>(sender());
    if (!scrollBar)
        return;

    QString channelName = scrollBar->property("channelName").toString();
    if (channelName.isEmpty())
        return;

    auto it = channelHistoryState.find(channelName);
    if (it == channelHistoryState.end())
        return;

    auto &state = it.value();
    if (!state.hasMore || state.loading)
        return;

    state.loading = true;
    qDebug() << "Fetching older history for" << channelName << "before ID" << state.oldestMessageId;
    textManager.requestHistory(channelName, kHistoryPageSize, state.oldestMessageId);
}

void MainWindow::onUserJoinedChannel(const QString& user, const QString& channel) {
    qDebug() << "User" << user << "joined channel" << channel;
    QStandardItem* channelItem = nullptr;
    for (int i = 0; i < model->rowCount(); ++i) {
        QStandardItem* item = model->item(i);
        if (item && item->text() == channel) {
            channelItem = item;
            break;
        }
    }
    if (!channelItem) {
        qDebug() << "Channel" << channel << "not found in tree";
        return;
    }
    for (int i = 0; i < channelItem->rowCount(); ++i) {
        QStandardItem* userItem = channelItem->child(i);
        if (userItem && userItem->text() == user) {
            qDebug() << "User" << user << "already in channel" << channel;
            return;
        }
    }
    QStandardItem* userItem = new QStandardItem(user);
    channelItem->appendRow(userItem);
}

bool MainWindow::validateInputDevice() {
    QString deviceId = uiSettings->InputDeviceComboBox->currentData().toString();

    // Check if this is the same device as currently used
    if (deviceId == currentInputDeviceId) {
        qDebug() << "Input device unchanged, skipping:" << uiSettings->InputDeviceComboBox->currentText();
        return false;
    }
    currentInputDeviceId = deviceId;
    return true;
}

bool MainWindow::validateOutputDevice() {
    QString deviceId = uiSettings->OutputDeviceComboBox->currentData().toString();

    // Check if this is the same device as currently used
    if (deviceId == currentOutputDeviceId) {
        qDebug() << "Output device unchanged, skipping:" << uiSettings->OutputDeviceComboBox->currentText();
        return false;
    }
    currentOutputDeviceId = deviceId;
    return true;
}

void MainWindow::setInputDevice() {
    if (!validateInputDevice()) {
        return;
    }
    config.setInputDevice(currentInputDeviceId.toStdString());
    audioManager.setInputDevice(currentInputDeviceId.toStdString());
    qDebug() << "Input device changed to:" << uiSettings->InputDeviceComboBox->currentText() << "ID:" << currentInputDeviceId;
}

void MainWindow::setOutputDevice() {
    if (!validateOutputDevice()) {
        return;
    }
    config.setOutputDevice(currentOutputDeviceId.toStdString());
    audioManager.setOutputDevice(currentOutputDeviceId.toStdString());
    qDebug() << "Output device changed to:" << uiSettings->OutputDeviceComboBox->currentText() << "ID:" << currentOutputDeviceId;
}

void MainWindow::setInputDeviceWithoutSaving() {
    if (!validateInputDevice()) {
        return;
    }
    QString deviceId = uiSettings->InputDeviceComboBox->currentData().toString();
    audioManager.setInputDevice(deviceId.toStdString());
    qDebug() << "Input device applied (not saved) to:" << uiSettings->InputDeviceComboBox->currentText() << "ID:" << deviceId;
}

void MainWindow::setOutputDeviceWithoutSaving() {
    if (!validateOutputDevice()) {
        return;
    }
    QString deviceId = uiSettings->OutputDeviceComboBox->currentData().toString();
    audioManager.setOutputDevice(deviceId.toStdString());
    qDebug() << "Output device applied (not saved) to:" << uiSettings->OutputDeviceComboBox->currentText() << "ID:" << deviceId;
}

void MainWindow::onDefaultDeviceChanged(bool isInput) {
    qDebug() << "Default device changed:" << (isInput ? "Input" : "Output");
    if (isInput) {
        if (uiSettings->InputDeviceComboBox->currentIndex() == 0) {
            qDebug() << "Auto-switching to new default input device";
            audioManager.setInputDevice("");
        }
    } else {
        if (uiSettings->OutputDeviceComboBox->currentIndex() == 0) {
            qDebug() << "Auto-switching to new default output device";
            audioManager.setOutputDevice("");
        }
    }
}

void MainWindow::onPlaybackVolumeChanged(int value) {
    float volume = value / 100.0f;
    qDebug() << "Playback volume changed to:" << value << "%" << "(" << volume << ")";
    audioManager.setVolume(false, volume);
}

void MainWindow::onCaptureVolumeChanged(int value) {
    float volume = value / 100.0f;
    qDebug() << "Capture volume changed to:" << value << "%" << "(" << volume << ")";
    audioManager.setVolume(true, volume);
}

void MainWindow::onVolumeChanged(bool isInput, float volume) {
    int sliderValue = static_cast<int>(volume * 100);
    qDebug() << "Volume change event received for" << (isInput ? "input" : "output") << "- setting slider to:" << sliderValue;

    if (isInput) {
        uiSettings->captureVolumeSlider->blockSignals(true);
        uiSettings->captureVolumeSlider->setValue(sliderValue);
        uiSettings->captureVolumeSlider->blockSignals(false);
    } else {
        uiSettings->playbackVolumeSlider->blockSignals(true);
        uiSettings->playbackVolumeSlider->setValue(sliderValue);
        uiSettings->playbackVolumeSlider->blockSignals(false);
    }
}

void MainWindow::onStyleChanged(int index) {
    QString styleName = uiSettings->StyleSelectComboBox->itemText(index);
    QString styleData = uiSettings->StyleSelectComboBox->itemData(index).toString();
    qDebug() << "Changing style to:" << styleName;
    config.setTheme(styleName.toStdString());
    if (!styleData.isEmpty() && styleData.startsWith(":/styles/")) {
        QFile styleFile(styleData);
        if (styleFile.open(QFile::ReadOnly)) {
            QString styleSheet = QLatin1String(styleFile.readAll());
            qApp->setStyleSheet(styleSheet);
            styleFile.close();
        } else {
            qDebug() << "Failed to load custom stylesheet:" << styleData;
            qApp->setStyleSheet("");
        }
    } else {
        qApp->setStyleSheet("");
        QApplication::setStyle(QStyleFactory::create(styleName));
    }
}

void MainWindow::requestUsersDatabase() {
    qDebug() << "Requesting users database from server...";
    sendAdminRequest("get_users");
}

void MainWindow::sendAdminRequest(const QString &requestType) {
    if (!networkManager.isConnected()) {
        qDebug() << "Cannot send admin request: not connected to server";
        return;
    }
    
    qDebug() << "Sending admin request:" << requestType;
    networkManager.sendAdminMessage(requestType);
}

void MainWindow::handleAdminResponse(const QString& request, const QString& jsonData) {
    qDebug() << "Handling admin response for:" << request;
    
    if (request == "get_users") {
        // Parse JSON data
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(jsonData.toUtf8(), &error);
        
        if (error.error != QJsonParseError::NoError) {
            qDebug() << "JSON parse error:" << error.errorString();
            return;
        }
        
        if (!doc.isArray()) {
            qDebug() << "Expected JSON array for user data";
            return;
        }
        
        QJsonArray users = doc.array();
        
        // Clear existing data
        accountsModel->clear();
        accountsModel->setHorizontalHeaderLabels({"ID", "Username", "Public Key", "Admin", "Approved", "Created", "Last Auth"});
        
        // Populate table with user data
        for (const QJsonValue& userValue : users) {
            if (!userValue.isObject()) continue;
            
            QJsonObject user = userValue.toObject();
            
            QList<QStandardItem*> row;
            row << new QStandardItem(QString::number(user["id"].toInt()));
            row << new QStandardItem(user["username"].toString());
            row << new QStandardItem(user["public_key"].toString());
            row << new QStandardItem(user["is_admin"].toBool() ? "Yes" : "No");
            row << new QStandardItem(user["is_approved"].toBool() ? "Yes" : "No");
            row << new QStandardItem(user["created_at"].toString());
            row << new QStandardItem(user["last_auth"].toString());
            
            accountsModel->appendRow(row);
        }
        
        qDebug() << "Populated accounts table with" << users.size() << "users";
    }
}

void MainWindow::approveSelectedUser() {
    QModelIndexList selectedIndexes = uiAdminPanel->accountstableView->selectionModel()->selectedRows();
    
    if (selectedIndexes.isEmpty()) {
        qDebug() << "No user selected for approval";
        return;
    }
    
    // Get the ID from the first column of the selected row
    QModelIndex selectedIndex = selectedIndexes.first();
    QModelIndex idIndex = accountsModel->index(selectedIndex.row(), 0); // ID is in column 0
    QString userId = accountsModel->data(idIndex).toString();
    
    qDebug() << "Approving user with ID:" << userId;
    sendAdminRequest("approve_user:" + userId);
}

void MainWindow::onFrameQueued(int colorValue) {
    // qDebug() << "Frame queued:" << colorValue;
}
