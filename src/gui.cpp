#include "audio/audio_impl.hpp"
#include "gui.hpp"
#include "qdebug.h"
#include "ui_connect.h"
#include "ui_main.h"
#include "ui_textchannel.h"
#include "ui_settings.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow), audioManager(networkManager, config), networkManager(audioManager) {
    ui->setupUi(this);
    connect(ui->actionQuit, &QAction::triggered, this, &QApplication::quit);
    connect(ui->actionDisconnect, &QAction::triggered, this, &MainWindow::disconnect);
    connect(ui->actionNew_Connection, &QAction::triggered, this, &MainWindow::showConnectDialog);
    connect(ui->treeView, &QTreeView::customContextMenuRequested, this, &MainWindow::showContextMenu);
    connect(ui->treeView, &QTreeView::clicked, this, &MainWindow::onTreeViewItemClicked);
    connect(&networkManager, &Network::channelsReceived, this, &MainWindow::addChannels);
    connect(&networkManager, &Network::userJoinedChannel, this, &MainWindow::onUserJoinedChannel);
    connect(&audioManager, &Audio::deviceListChanged, this, &MainWindow::updateAudioDeviceComboBox);
    connect(&audioManager, &Audio::defaultDeviceChanged, this, &MainWindow::onDefaultDeviceChanged);
    connect(&audioManager, &Audio::volumeChanged, this, &MainWindow::onVolumeChanged);

    model = new QStandardItemModel(this);
    model->setHorizontalHeaderLabels({"Channels"});

    ui->treeView->setModel(model);
    ui->treeView->expandAll();

    settingsTab = new QWidget;
    uiSettings = new Ui::SettingsTab;
    uiSettings->setupUi(settingsTab);
    ui->tabWidget->addTab(settingsTab, "Settings");
    
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
}

MainWindow::~MainWindow() {
    delete ui;
    delete uiSettings;
    networkManager.disconnectQUIC();
}

void MainWindow::disconnect() {
    qDebug("disconnect");
    networkManager.disconnectQUIC();
}

void MainWindow::showConnectDialog() {
    QDialog dialog(this);
    Ui::connectDialog uiDialog;
    uiDialog.setupUi(&dialog);

    if (dialog.exec() == QDialog::Accepted) {
        QString address = uiDialog.lineEdit->text();
        QString port = uiDialog.lineEdit_2->text();
        qDebug() << "Connect to:" << address << ":" << port;
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
    QLineEdit *messageInput = currentTab->findChild<QLineEdit*>("messageLineEdit");
    if (messageInput) {
        QString message = messageInput->text();
        if (!message.isEmpty()) {
            qDebug() << "Message:" << message << "to channel:" << channelName;
            // TODO: send message
            messageInput->clear();
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
    connect(uiTextChannel.sendButton, &QPushButton::clicked, [this, channelName]() {
        sendMessage(channelName);
    });
    int tabIndex = ui->tabWidget->addTab(textChannelTab, channelName);
    ui->tabWidget->setCurrentIndex(tabIndex);
    
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
    QWidget *tab = ui->tabWidget->widget(index);
    ui->tabWidget->removeTab(index);
    delete tab;
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