#pragma once

#include <QMainWindow>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

#include "signallab/app/analysis_service.hpp"
#include "signallab/app/background_analysis_service.hpp"
#include "signallab/app/background_file_service.hpp"
#include "signallab/app/background_script_service.hpp"
#include "signallab/app/import_service.hpp"
#include "signallab/repository/file_repository.hpp"
#include "signallab/task/scheduler.hpp"

class InteractiveChartView;
class QLabel;
class QComboBox;
class QDoubleSpinBox;
class QLineSeries;
class QListView;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStandardItemModel;
class QTableView;
class QTabWidget;
class QTextEdit;
class QTimer;
class QValueAxis;

namespace signallab::gui {
class AnalysisHistoryModel;
class AnalysisResultModel;
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

private:
    [[nodiscard]] signallab::app::AnalysisOptions analysisOptions() const;
    [[nodiscard]] const signallab::app::SignalEntry* selectedSignal() const;
    [[nodiscard]] std::optional<std::size_t> selectedSignalIndex() const;

    void analyzeSelectedSignal();
    void cancelActiveScript();
    void cancelSelectedTask();
    void clearAnalysis();
    void clearScriptDiagnostics();
    void exportChart(InteractiveChartView* chart, const QString& baseName);
    void importCsv();
    void importWav();
    void jumpToScriptDiagnostic(const QModelIndex& index);
    void loadSelectedScriptHistory();
    void processBackgroundAnalysisEvents();
    void processBackgroundFileEvents();
    void processBackgroundScriptEvents();
    void refreshAnalysisHistory();
    void refreshScriptHistory();
    void refreshSignalList(std::optional<std::int64_t> selectRecordId = {});
    void refreshTaskList();
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
    void updateScriptButtons();

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
    QLabel* nameValue_;
    QLabel* sampleRateValue_;
    QLabel* sampleCountValue_;
    QLabel* durationValue_;
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
    InteractiveChartView* timeChartView_;
    QLineSeries* timeSeries_;
    QValueAxis* timeXAxis_;
    QValueAxis* timeYAxis_;
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
    QListView* taskListView_;
    QTextEdit* taskDetails_;
    QPushButton* cancelTaskButton_;
    QPushButton* retryTaskButton_;

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
        signallab::repository::CsvImportOptions> csvPreviewOptions_;
};
