#include <QApplication>
#include <QMessageBox>
#include <QString>

#include <utility>

#include "main_window.hpp"
#include "signallab/core/signal_generator.hpp"

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);

    signallab::core::SignalMetadata metadata;
    metadata.name = "1 kHz 正弦信号";
    metadata.unit = "幅值";
    metadata.source = "内置模拟器";

    auto signalResult = signallab::core::generateSineWave(
        1000.0,
        1.0,
        48000.0,
        0.02,
        metadata);

    if (!signalResult) {
        QMessageBox::critical(
            nullptr,
            "SignalLab",
            QString::fromStdString(signalResult.error().message));
        return 1;
    }

    auto signal = std::move(signalResult).value();
    MainWindow mainWindow(signal);
    mainWindow.show();

    return application.exec();
}
