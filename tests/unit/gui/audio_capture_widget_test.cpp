#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "gui/audio_capture_widget.hpp"
#include "signallab/io/wav_reader.hpp"

namespace {

class FakeAudioInput final : public signallab::repository::AudioInput {
public:
    signallab::core::Result<std::vector<signallab::repository::AudioInputDevice>>
    devices() const override
    {
        return signallab::core::Result<std::vector<signallab::repository::AudioInputDevice>>::success(availableDevices);
    }

    signallab::core::Result<bool> start(
        const std::string& deviceId,
        signallab::repository::AudioInputFormat format,
        std::function<void(std::span<const double>)> onSamples,
        std::function<void(std::string)> onError) override
    {
        ++startCount;
        selectedDevice = deviceId;
        selectedRate = format.sampleRate;
        samplesCallback = std::move(onSamples);
        errorCallback = std::move(onError);
        return signallab::core::Result<bool>::success(true);
    }

    void stop() override
    {
        ++stopCount;
        if (!finalSamples.empty() && samplesCallback) {
            samplesCallback(finalSamples);
            finalSamples.clear();
        }
    }

    std::vector<signallab::repository::AudioInputDevice> availableDevices{
        {"fake-mic", "测试麦克风"}};
    std::function<void(std::span<const double>)> samplesCallback;
    std::function<void(std::string)> errorCallback;
    std::vector<double> finalSamples;
    std::string selectedDevice;
    unsigned int selectedRate{0};
    int startCount{0};
    int stopCount{0};
};

class AudioCaptureWidgetTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto input = std::make_unique<FakeAudioInput>();
        backend = input.get();
        widget = std::make_unique<signallab::gui::AudioCaptureWidget>(std::move(input));
    }

    template<typename Widget>
    Widget* control(const char* name)
    {
        return widget->findChild<Widget*>(QString::fromLatin1(name));
    }

    void poll()
    {
        QMetaObject::invokeMethod(control<QTimer>("capturePollTimer"), "timeout", Qt::DirectConnection);
    }

    void startWithSamples()
    {
        control<QComboBox>("captureFilterCombo")->setCurrentIndex(0);
        control<QPushButton>("captureStartButton")->click();
        backend->samplesCallback(std::vector<double>{0.25, -0.5, 0.75, -0.125});
        poll();
    }

    bool waitForSave()
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (!control<QPushButton>("captureStartButton")->isEnabled()
            && elapsed.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            poll();
            QThread::msleep(1);
        }
        return control<QPushButton>("captureStartButton")->isEnabled();
    }

    FakeAudioInput* backend{nullptr};
    std::unique_ptr<signallab::gui::AudioCaptureWidget> widget;
};

TEST_F(AudioCaptureWidgetTest, StartsStopsAndRendersTheRecordedSamples)
{
    EXPECT_FALSE(control<QPushButton>("captureStopButton")->isEnabled());
    EXPECT_FALSE(control<QPushButton>("captureSaveButton")->isEnabled());
    EXPECT_TRUE(control<QComboBox>("captureSaveVersionCombo")->currentData().toBool());
    startWithSamples();

    EXPECT_EQ(backend->startCount, 1);
    EXPECT_EQ(backend->selectedDevice, "fake-mic");
    EXPECT_EQ(backend->selectedRate, 48000U);
    EXPECT_FALSE(control<QComboBox>("captureDeviceCombo")->isEnabled());
    EXPECT_FALSE(control<QPushButton>("captureStartButton")->isEnabled());
    EXPECT_FALSE(control<QPushButton>("captureSaveButton")->isEnabled());
    EXPECT_TRUE(control<QPushButton>("captureStopButton")->isEnabled());
    EXPECT_EQ(control<QProgressBar>("captureLevelBar")->value(), 75);
    auto* chartView = control<QChartView>("captureChartView");
    ASSERT_NE(chartView, nullptr);
    ASSERT_EQ(chartView->chart()->series().size(), 2);
    auto* raw = static_cast<QLineSeries*>(chartView->chart()->series().at(0));
    auto* filtered = static_cast<QLineSeries*>(chartView->chart()->series().at(1));
    EXPECT_EQ(raw->count(), 4);
    EXPECT_EQ(raw->points(), filtered->points());

    backend->finalSamples = {0.1, 0.2};
    control<QPushButton>("captureStopButton")->click();
    EXPECT_GE(backend->stopCount, 1);
    EXPECT_TRUE(control<QPushButton>("captureStartButton")->isEnabled());
    EXPECT_TRUE(control<QPushButton>("captureSaveButton")->isEnabled());
    EXPECT_FALSE(control<QPushButton>("captureStopButton")->isEnabled());
    EXPECT_TRUE(control<QLabel>("captureSampleCountLabel")->text().contains(QStringLiteral("6")));
    EXPECT_EQ(raw->count(), 6);
}

TEST_F(AudioCaptureWidgetTest, ValidatesCutoffAndOddTapCountBeforeOpeningDevice)
{
    control<QComboBox>("captureSampleRateCombo")->setCurrentIndex(0);
    control<QDoubleSpinBox>("captureCutoffSpin")->setValue(8000.0);
    EXPECT_FALSE(control<QPushButton>("captureStartButton")->isEnabled());
    EXPECT_TRUE(control<QLabel>("captureValidationLabel")->text().contains(QStringLiteral("截止频率")));
    control<QDoubleSpinBox>("captureCutoffSpin")->setValue(1000.0);
    control<QSpinBox>("captureTapCountSpin")->setValue(64);
    EXPECT_FALSE(control<QPushButton>("captureStartButton")->isEnabled());
    EXPECT_TRUE(control<QLabel>("captureValidationLabel")->text().contains(QStringLiteral("奇数")));
    EXPECT_EQ(backend->startCount, 0);
    control<QComboBox>("captureFilterCombo")->setCurrentIndex(0);
    EXPECT_TRUE(control<QPushButton>("captureStartButton")->isEnabled());
    EXPECT_FALSE(control<QDoubleSpinBox>("captureCutoffSpin")->isEnabled());
    EXPECT_FALSE(control<QSpinBox>("captureTapCountSpin")->isEnabled());
}

TEST_F(AudioCaptureWidgetTest, RefreshHandlesDisconnectedAndReconnectedDevices)
{
    backend->availableDevices.clear();
    control<QPushButton>("captureRefreshButton")->click();
    EXPECT_EQ(control<QComboBox>("captureDeviceCombo")->count(), 0);
    EXPECT_FALSE(control<QPushButton>("captureStartButton")->isEnabled());
    EXPECT_TRUE(control<QLabel>("captureValidationLabel")->text().contains(QStringLiteral("未发现麦克风")));

    backend->availableDevices.push_back({"new-mic", "重新连接的麦克风"});
    control<QPushButton>("captureRefreshButton")->click();
    EXPECT_TRUE(control<QPushButton>("captureStartButton")->isEnabled());
    EXPECT_EQ(control<QComboBox>("captureDeviceCombo")->currentData().toString(), QStringLiteral("new-mic"));
}

TEST_F(AudioCaptureWidgetTest, DeviceFailureStopsCaptureAndKeepsRecordingAvailable)
{
    startWithSamples();
    backend->errorCallback("Device disconnected");
    poll();
    EXPECT_FALSE(control<QPushButton>("captureStopButton")->isEnabled());
    EXPECT_TRUE(control<QPushButton>("captureSaveButton")->isEnabled());
    EXPECT_TRUE(control<QLabel>("captureStatusLabel")->text().contains(QStringLiteral("Device disconnected")));
    EXPECT_TRUE(control<QLabel>("captureSampleCountLabel")->text().contains(QStringLiteral("4")));
    EXPECT_EQ(QApplication::activeModalWidget(), nullptr);
}

TEST_F(AudioCaptureWidgetTest, ReachingTheDurationLimitAutomaticallyStopsAndBoundsTheChart)
{
    control<QComboBox>("captureFilterCombo")->setCurrentIndex(0);
    control<QComboBox>("captureSampleRateCombo")->setCurrentIndex(0);
    control<QSpinBox>("captureMaximumDurationSpin")->setValue(30);
    control<QPushButton>("captureStartButton")->click();
    const std::vector<double> samples(16000U * 31U, 0.25);
    backend->samplesCallback(samples);
    poll();
    EXPECT_FALSE(control<QPushButton>("captureStopButton")->isEnabled());
    EXPECT_TRUE(control<QPushButton>("captureSaveButton")->isEnabled());
    EXPECT_TRUE(control<QLabel>("captureDurationLabel")->text().contains(QStringLiteral("30.0")));
    EXPECT_TRUE(control<QLabel>("captureStatusLabel")->text().contains(QStringLiteral("自动停止")));
    auto* chartView = control<QChartView>("captureChartView");
    auto* raw = static_cast<QLineSeries*>(chartView->chart()->series().at(0));
    EXPECT_LE(raw->count(), 2000);
    ASSERT_GT(raw->count(), 0);
    EXPECT_GE(raw->points().first().x(), 28.0);
}

TEST_F(AudioCaptureWidgetTest, CancellingReplacementPreservesThePreviousRecording)
{
    startWithSamples();
    control<QPushButton>("captureStopButton")->click();
    QTimer::singleShot(0, widget.get(), []() {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        dialog->done(QMessageBox::Cancel);
    });
    control<QPushButton>("captureStartButton")->click();
    EXPECT_EQ(backend->startCount, 1);
    EXPECT_TRUE(control<QPushButton>("captureSaveButton")->isEnabled());
    EXPECT_TRUE(control<QLabel>("captureSampleCountLabel")->text().contains(QStringLiteral("4")));
}

TEST_F(AudioCaptureWidgetTest, CancellingCloseKeepsAnActiveCaptureRunning)
{
    startWithSamples();
    QTimer::singleShot(0, widget.get(), []() {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        dialog->done(QMessageBox::Cancel);
    });
    EXPECT_FALSE(widget->requestClose());
    EXPECT_TRUE(control<QPushButton>("captureStopButton")->isEnabled());
    control<QPushButton>("captureStopButton")->click();
}

TEST_F(AudioCaptureWidgetTest, SavesUnicodeWavAsynchronouslyAndCallsImportAfterSuccess)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = std::filesystem::path(
        (directory.path() + QStringLiteral("/麦克风录音.wav")).toStdWString());
    std::filesystem::path importedPath;
    widget->setImportCallback([&importedPath](const std::filesystem::path& savedPath) {
        importedPath = savedPath;
    });
    startWithSamples();
    control<QPushButton>("captureStopButton")->click();
    EXPECT_TRUE(widget->saveRecording(path, true));
    EXPECT_FALSE(control<QPushButton>("captureStartButton")->isEnabled());
    EXPECT_FALSE(control<QPushButton>("captureSaveButton")->isEnabled());
    ASSERT_TRUE(waitForSave());
    EXPECT_EQ(importedPath, path);
    EXPECT_TRUE(std::filesystem::exists(path));
    const auto recorded = signallab::io::readWav(path, {});
    ASSERT_TRUE(recorded) << recorded.error().message;
    EXPECT_EQ(recorded.value().sampleCount(), 4U);
    EXPECT_DOUBLE_EQ(recorded.value().sampleRate(), 48000.0);
    EXPECT_NEAR(recorded.value().samples()[1], -0.5, 1.0 / 32768.0);
    EXPECT_TRUE(control<QLabel>("captureStatusLabel")->text().contains(QStringLiteral("已保存")));
    EXPECT_TRUE(widget->requestClose());
}

TEST_F(AudioCaptureWidgetTest, FailedSaveKeepsTheBufferAndDoesNotImport)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    int importCount = 0;
    widget->setImportCallback([&importCount](const std::filesystem::path&) { ++importCount; });
    startWithSamples();
    control<QPushButton>("captureStopButton")->click();
    const auto path = std::filesystem::path(directory.path().toStdWString());
    ASSERT_TRUE(widget->saveRecording(path, true));
    ASSERT_TRUE(waitForSave());
    EXPECT_EQ(importCount, 0);
    EXPECT_TRUE(control<QPushButton>("captureSaveButton")->isEnabled());
    EXPECT_TRUE(control<QLabel>("captureStatusLabel")->text().contains(QStringLiteral("保存失败")));
    QTimer::singleShot(0, widget.get(), []() {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        dialog->done(QMessageBox::Cancel);
    });
    EXPECT_FALSE(widget->requestClose());
}

}
