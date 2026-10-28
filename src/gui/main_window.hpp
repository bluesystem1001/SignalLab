#pragma once

#include <QMainWindow>
#include <QPointer>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

#include "signallab/app/analysis_service.hpp"
#include "signallab/app/background_analysis_service.hpp"
#include "signallab/app/background_file_service.hpp"
#include "signallab/app/background_script_service.hpp"
#include "signallab/app/backup_service.hpp"
#include "signallab/app/import_service.hpp"
#include "signallab/repository/file_repository.hpp"
#include "signallab/task/scheduler.hpp"

class InteractiveChartView;
class QCloseEvent;
class QLabel;
class QComboBox;
class QDoubleSpinBox;
class QLineSeries;
class QListView;
class QPlainTextEdit;
class QPushButton;
class QScrollBar;
class QSpinBox;
class QStandardItemModel;
class QTableView;
class QTabWidget;
class QTextEdit;
class QTimer;
class QValueAxis;

namespace signallab::gui {
class AudioCaptureWidget;
class DatabaseBackupWidget;
class AnalysisHistoryModel;
class AnalysisResultModel;
class FileImportDialog;
class ScriptHistoryModel;
class SignalListModel;
class TaskListModel;
}  // namespace signallab::gui

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(
        signallab::app::ImportService& importService,
        signallab::app::AnalysisService& analysisService,
        signallab::app::BackgroundFileService& backgroundFileService,
        signallab::app::BackgroundAnalysisService& backgroundAnalysisService,
        signallab::task::Scheduler& scheduler,
        signallab::app::BackgroundScriptService*
            backgroundScriptService = nullptr,
        QWidget* parent = nullptr);
    ~MainWindow() override;
    void setBackupService(std::shared_ptr<signallab::app::BackupService> service);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct PendingSignalDeletion {
        std::int64_t recordId{0};
        std::optional<std::int64_t> adjacentRecordId;
    };

    [[nodiscard]] signallab::app::AnalysisOptions analysisOptions() const;
    [[nodiscard]] signallab::app::FirFilterOptions firFilterOptions() const;
    [[nodiscard]] bool isSignalDeletionPending(
        std::int64_t recordId) const;
    [[nodiscard]] const signallab::app::SignalEntry*
        comparisonSignal() const;
    [[nodiscard]] const signallab::app::SignalEntry* selectedSignal() const;
    [[nodiscard]] std::optional<std::size_t> selectedSignalIndex() const;
    [[nodiscard]] double timeDomainMaximum() const;

    void analyzeSelectedSignal();
    void cancelActiveScript();
    void cancelSelectedTask();
    void clearAnalysis();
    void clearScriptDiagnostics();
    void deleteSelectedAnalysisHistory();
    void deleteSelectedSignal();
    void deleteSelectedTask();
    void exportChart(InteractiveChartView* chart, const QString& baseName);
    void filterSelectedSignal();
    void importCsv();
    void importWav();
    void jumpToScriptDiagnostic(const QModelIndex& index);
    void loadSelectedScriptHistory();
    void processBackgroundAnalysisEvents();
    void processBackgroundFileEvents();
    void processBackgroundScriptEvents();
    void refreshAnalysisHistory(
        std::optional<std::int64_t> selectRecordId = {});
    void refreshComparisonSignalChoices();
    void refreshScriptHistory();
    void refreshSignalList(std::optional<std::int64_t> selectRecordId = {});
    void refreshTaskList(
        std::optional<signallab::task::TaskId> selectTaskId = {});
    void renderSpectrum(bool resetAxes);
    void renderTimeSignal(bool resetAxes);
    void retrySelectedTask();
    void runScript();
    void showAnalysisHistory(const QModelIndex& index);
    void showAnalysisResult(
        std::shared_ptr<const signallab::app::AnalysisResult> result,
        const signallab::app::AnalysisOptions& options,
        bool focusSpectrum);
    void showFailure(
        const QString& title,
        const QString& reason,
        const QString& suggestion);
    void showScriptHistory(const QModelIndex& index);
    void showScriptResult(
        const signallab::app::BackgroundScriptEvent& event);
    void showSignal(const QModelIndex& index);
    void showTaskDetails(const QModelIndex& index);
    void updateAnalysisValidation();
    void updateFirValidation();
    void updateSignalDeleteButton();
    void updateScriptButtons();
    void updateTimeAxisScrollBar();

    signallab::app::ImportService& importService_;
    signallab::app::AnalysisService& analysisService_;
    signallab::app::BackgroundFileService& backgroundFileService_;
    signallab::app::BackgroundAnalysisService& backgroundAnalysisService_;
    signallab::task::Scheduler& scheduler_;
    signallab::app::BackgroundScriptService* backgroundScriptService_;

    signallab::gui::SignalListModel* signalModel_;
    signallab::gui::TaskListModel* taskModel_;
    signallab::gui::AnalysisHistoryModel* analysisHistoryModel_;
    signallab::gui::AnalysisResultModel* resultModel_;
    signallab::gui::ScriptHistoryModel* scriptHistoryModel_;

    QListView* signalListView_;
    QPushButton* deleteSignalButton_;
    QLabel* nameValue_;
    QLabel* sampleRateValue_;
    QLabel* sampleCountValue_;
    QLabel* durationValue_;
    QComboBox* firTypeCombo_;
    QDoubleSpinBox* firCutoffSpin_;
    QSpinBox* firTapCountSpin_;
    QLabel* firValidationLabel_;
    QPushButton* applyFirButton_;
    QLabel* validationLabel_;
    QComboBox* windowTypeCombo_;
    QDoubleSpinBox* minimumPeakRatioSpin_;
    QDoubleSpinBox* minimumPeakDistanceSpin_;
    QSpinBox* maximumPeakCountSpin_;
    QSpinBox* metricGuardBinsSpin_;
    QSpinBox* thdHarmonicsSpin_;
    QPushButton* analyzeButton_;
    QLabel* resultVersionLabel_;
    QLabel* resultParametersLabel_;
    QTableView* resultView_;

    QTabWidget* workspaceTabs_;
    signallab::gui::AudioCaptureWidget* audioCaptureWidget_{nullptr};
    signallab::gui::DatabaseBackupWidget* databaseBackupWidget_{nullptr};
    InteractiveChartView* timeChartView_;
    QLineSeries* timeSeries_;
    QLineSeries* comparisonTimeSeries_;
    QValueAxis* timeXAxis_;
    QValueAxis* timeYAxis_;
    QComboBox* comparisonSignalCombo_;
    QScrollBar* timeAxisScrollBar_;
    QLabel* timeAxisRangeLabel_;
    QLabel* timeCursorLabel_;
    QLabel* timeSelectionLabel_;
    QTimer* timeRedrawTimer_;
    InteractiveChartView* spectrumChartView_;
    QLineSeries* spectrumSeries_;
    QValueAxis* spectrumXAxis_;
    QValueAxis* spectrumYAxis_;
    QLabel* spectrumCursorLabel_;
    QLabel* spectrumSelectionLabel_;
    QTimer* spectrumRedrawTimer_;
    std::shared_ptr<const signallab::app::AnalysisResult>
        displayedAnalysis_;
    signallab::app::AnalysisOptions displayedAnalysisOptions_;

    QListView* analysisHistoryView_;
    QTextEdit* analysisHistoryDetails_;
    QPushButton* deleteAnalysisHistoryButton_;
    QListView* taskListView_;
    QTextEdit* taskDetails_;
    QPushButton* cancelTaskButton_;
    QPushButton* retryTaskButton_;
    QPushButton* deleteTaskButton_;

    QPlainTextEdit* scriptEditor_;
    QPushButton* runScriptButton_;
    QPushButton* cancelScriptButton_;
    QLabel* scriptStatusLabel_;
    QListView* scriptDiagnosticView_;
    QStandardItemModel* scriptDiagnosticModel_;
    QPlainTextEdit* scriptOutput_;
    QTabWidget* scriptPlotTabs_;
    QListView* scriptHistoryView_;
    QTextEdit* scriptHistoryDetails_;
    QPushButton* loadScriptHistoryButton_;
    std::optional<signallab::task::TaskId> activeScriptTaskId_;
    std::shared_ptr<const std::string> displayedScriptSource_;

    std::unordered_map<
        signallab::task::TaskId,
        std::uint64_t> importPreviewRevisions_;
    QPointer<signallab::gui::FileImportDialog> activeImportDialog_;
    std::optional<signallab::task::TaskId> activeImportPreviewTaskId_;
    std::unordered_map<
        signallab::task::TaskId,
        PendingSignalDeletion> pendingSignalDeletions_;
    std::unordered_map<
        signallab::task::TaskId,
        std::int64_t> signalTaskRecords_;
};
