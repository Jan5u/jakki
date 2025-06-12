#include "gui.hpp"
#include "ui_connect.h"
#include "ui_main.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow), audioManager(networkManager), networkManager(audioManager) {
    ui->setupUi(this);
    connect(ui->actionQuit, &QAction::triggered, this, &QApplication::quit);
    connect(ui->actionDisconnect, &QAction::triggered, this, &MainWindow::disconnect);
    connect(ui->actionNew_Connection, &QAction::triggered, this, &MainWindow::showConnectDialog);
    connect(ui->treeView, &QTreeView::customContextMenuRequested, this, &MainWindow::showContextMenu);

    QStandardItemModel *model = new QStandardItemModel(this);
    model->setHorizontalHeaderLabels({"Channels"});
    QStandardItem *channel = new QStandardItem("test");
    channel->appendRow(new QStandardItem("#text"));
    channel->appendRow(new QStandardItem("voice"));
    model->appendRow(channel);
    ui->treeView->setModel(model);
    ui->treeView->expandAll();
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