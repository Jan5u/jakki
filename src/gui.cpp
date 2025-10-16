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
    connect(uiSettings->InputDeviceComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onInputDeviceChanged);
    connect(uiSettings->OutputDeviceComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onOutputDeviceChanged);
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
            if (inputIndex != currentInputIndex) {
                if (!isInitialDeviceSetup) {
                    onInputDeviceChanged(inputIndex);
                }
            }
        }
    }
    
    if (!savedOutputDevice.isEmpty()) {
        int outputIndex = uiSettings->OutputDeviceComboBox->findData(savedOutputDevice);
        if (outputIndex >= 0) {
            uiSettings->OutputDeviceComboBox->setCurrentIndex(outputIndex);
            if (outputIndex != currentOutputIndex) {
                if (!isInitialDeviceSetup) {
                    onOutputDeviceChanged(outputIndex);
                }
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

void MainWindow::onInputDeviceChanged(int index) {
    // Get device ID from combo box data (or empty string for "Default")
    QString deviceId = uiSettings->InputDeviceComboBox->currentData().toString();
    
    // Save to config
    config.setInputDevice(deviceId.toStdString());

    // Apply device change to audio manager
    audioManager.setInputDevice(deviceId.toStdString());
    
    qDebug() << "Input device changed to:" << uiSettings->InputDeviceComboBox->currentText() << "ID:" << deviceId;
}

void MainWindow::onOutputDeviceChanged(int index) {
    // Get device ID from combo box data (or empty string for "Default")
    QString deviceId = uiSettings->OutputDeviceComboBox->currentData().toString();
    
    // Save to config
    config.setOutputDevice(deviceId.toStdString());

    // Apply device change to audio manager
    audioManager.setOutputDevice(deviceId.toStdString());
    
    qDebug() << "Output device changed to:" << uiSettings->OutputDeviceComboBox->currentText() << "ID:" << deviceId;
}