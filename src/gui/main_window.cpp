#include "main_window.hpp"

#include <QLabel>
#include <QStatusBar>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("SignalLab " SIGNALLAB_VERSION_STRING);
    resize(960, 600);

    auto* welcomeLabel = new QLabel("SignalLab is ready", this);
    welcomeLabel->setAlignment(Qt::AlignCenter);
    setCentralWidget(welcomeLabel);

    statusBar()->showMessage("Ready");
}
