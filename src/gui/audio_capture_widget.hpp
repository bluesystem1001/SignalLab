#pragma once

#include <QWidget>

#include <filesystem>
#include <functional>
#include <future>
#include <memory>

#include "signallab/app/audio_capture_service.hpp"

class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineSeries;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;
class QValueAxis;

namespace signallab::gui {

class AudioCaptureWidget final : public QWidget
{
public:
    explicit AudioCaptureWidget(
        std::unique_ptr<repository::AudioInput> input,
        QWidget* parent = nullptr);
    ~AudioCaptureWidget() override;

    void setImportCallback(
        std::function<void(const std::filesystem::path&)> callback);
    [[nodiscard]] bool requestClose();
    [[nodiscard]] bool saveRecording(
        const std::filesystem::path& path,
        bool importAfterSave = false);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    [[nodiscard]] app::AudioCaptureOptions options() const;
    [[nodiscard]] QString validationMessage() const;
    void refreshDevices();
    void startCapture();
    void stopCapture();
    void chooseSavePath(bool importAfterSave);
    void poll();
    void updateControls();

    app::AudioCaptureService service_;
    bool capturing_{false};
    bool unsaved_{false};
    bool importAfterSave_{false};
    std::size_t sampleCount_{0};
    std::filesystem::path savingPath_;
    std::future<core::Result<bool>> saveFuture_;
    std::function<void(const std::filesystem::path&)> importCallback_;

    QComboBox* deviceCombo_{nullptr};
    QPushButton* refreshButton_{nullptr};
    QComboBox* sampleRateCombo_{nullptr};
    QComboBox* filterCombo_{nullptr};
    QDoubleSpinBox* cutoffSpin_{nullptr};
    QSpinBox* tapCountSpin_{nullptr};
    QSpinBox* maximumDurationSpin_{nullptr};
    QPushButton* startButton_{nullptr};
    QPushButton* stopButton_{nullptr};
    QComboBox* saveVersionCombo_{nullptr};
    QPushButton* saveButton_{nullptr};
    QPushButton* saveImportButton_{nullptr};
    QLabel* validationLabel_{nullptr};
    QLabel* statusLabel_{nullptr};
    QLabel* durationLabel_{nullptr};
    QLabel* sampleCountLabel_{nullptr};
    QProgressBar* levelBar_{nullptr};
    QLineSeries* rawSeries_{nullptr};
    QLineSeries* filteredSeries_{nullptr};
    QValueAxis* timeAxis_{nullptr};
    QValueAxis* amplitudeAxis_{nullptr};
    QTimer* timer_{nullptr};
};

}
