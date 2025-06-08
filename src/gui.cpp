#include "gui.hpp"
#include "ui_connect.h"
#include "ui_main.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow), audioManager(networkManager), networkManager(audioManager) {
    ui->setupUi(this);
    connect(ui->actionQuit, &QAction::triggered, this, &QApplication::quit);
    connect(ui->actionDisconnect, &QAction::triggered, this, &MainWindow::disconnect);
    connect(ui->actionNew_Connection, &QAction::triggered, this, &MainWindow::showConnectDialog);
}

MainWindow::~MainWindow() {
    delete ui;
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
        qDebug() << "Connect to:" << address;
        networkManager.connectToServer();
        // initOpus(pwdata);
        // initPipewire(pwdata);
        audioManager.startAudioThread();
    }
}