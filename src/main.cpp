#include "gui.hpp"
#include <QApplication>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(":/images/icon.ico"));
    MainWindow w;
    w.show();
    return app.exec(); 
}