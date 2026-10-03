#pragma once

#include <QDialog>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

#include "signallab/repository/file_repository.hpp"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QWidget;

namespace signallab::gui {

class FileImportDialog final : public QDialog
{
public:
    enum class Mode {
        Csv,
        Wav
    };

    using PreviewRequestCallback = std::function<void(std::uint64_t)>;

    explicit FileImportDialog(
        std::filesystem::path sourcePath,
        repository::CsvImportOptions options,
        QWidget* parent = nullptr);

    explicit FileImportDialog(
        std::filesystem::path sourcePath,
        repository::WavImportOptions options,
        QWidget* parent = nullptr);

    [[nodiscard]] Mode mode() const noexcept;
    [[nodiscard]] const std::filesystem::path& sourcePath() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] bool isPreviewCurrent() const noexcept;
    [[nodiscard]] bool isPreviewPending() const noexcept;

    [[nodiscard]] std::optional<repository::CsvImportOptions>
    csvOptions() const;
    [[nodiscard]] std::optional<repository::WavImportOptions>
    wavOptions() const;

    void setPreviewRequestCallback(PreviewRequestCallback callback);

    // Starts a preview request for the current immutable revision. The
    // callback is expected to submit work asynchronously and later call
    // setPreview() or setPreviewError() with this same revision.
    [[nodiscard]] bool requestPreview();

    // Stale results are ignored. A false return means the supplied preview was
    // not accepted; a current but incompatible preview is rendered as an
    // inline error.
    [[nodiscard]] bool setPreview(
        std::uint64_t revision,
        const repository::ImportPreview& preview);
    [[nodiscard]] bool setPreviewError(
        std::uint64_t revision,
        const QString& message);

private:
    void buildUi();
    void buildCsvControls();
    void buildWavControls();
    void connectOptionChanges();
    void invalidatePreview();
    void updateTimeSourceControls();
    void updateButtons();
    void clearPreviewTable();
    void renderPreview(const repository::ImportPreview& preview);
    [[nodiscard]] bool validateOptions(QString* message) const;
    [[nodiscard]] bool applyPreviewError(const QString& message);

    Mode mode_;
    std::filesystem::path sourcePath_;
    std::optional<repository::CsvImportOptions> initialCsvOptions_;
    std::optional<repository::WavImportOptions> initialWavOptions_;
    PreviewRequestCallback previewRequestCallback_;
    std::uint64_t revision_{1};
    std::optional<std::uint64_t> pendingPreviewRevision_;
    std::optional<std::uint64_t> acceptedPreviewRevision_;

    QLineEdit* nameEdit_{nullptr};
    QLineEdit* unitEdit_{nullptr};

    QComboBox* delimiterCombo_{nullptr};
    QCheckBox* hasHeaderCheck_{nullptr};
    QSpinBox* valueColumnSpin_{nullptr};
    QComboBox* timeSourceCombo_{nullptr};
    QSpinBox* timeColumnSpin_{nullptr};
    QDoubleSpinBox* sampleRateSpin_{nullptr};

    QComboBox* wavChannelCombo_{nullptr};

    QLabel* previewSummaryLabel_{nullptr};
    QLabel* previewErrorLabel_{nullptr};
    QTableWidget* previewTable_{nullptr};
    QDialogButtonBox* buttonBox_{nullptr};
    QPushButton* previewButton_{nullptr};
    QPushButton* importButton_{nullptr};
};

}  // namespace signallab::gui
