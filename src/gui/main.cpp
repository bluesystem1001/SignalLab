#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include <QString>

#include <filesystem>
#include <system_error>
#include <utility>

#include "main_window.hpp"
#include "signallab/core/signal_generator.hpp"

namespace {

std::filesystem::path findProjectRoot()
{
    const auto applicationDirectory = std::filesystem::path(
        QCoreApplication::applicationDirPath().toStdWString());
    auto candidate = applicationDirectory;

    while (!candidate.empty()) {
        std::error_code error;
        if (std::filesystem::exists(candidate / "CMakeLists.txt", error)
            && !error) {
            return candidate;
        }

        const auto parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }

    // A packaged program stores its data beside the executable.
    return applicationDirectory;
}

}  // namespace

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

    signallab::app::ImportService importService(findProjectRoot());
    static_cast<void>(
        importService.addGeneratedSignal(std::move(signalResult).value()));

    MainWindow mainWindow(importService);
    mainWindow.show();

    return application.exec();
}
