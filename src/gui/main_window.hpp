#pragma once

#include <QMainWindow>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "signallab/algorithm/spectrum.hpp"
#include "signallab/app/analysis_service.hpp"
#include "signallab/app/background_analysis_service.hpp"
#include "signallab/app/background_file_service.hpp"
#include "signallab/app/import_service.hpp"
#include "signallab/task/scheduler.hpp"

class QLabel;
class QListWidget;
class QPushButton;
class QChartView;
class QTabWidget;
class QTextEdit;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(
        signallab::app::ImportService& importService,
        signallab::app::AnalysisService& analysisService,
        signallab::app::BackgroundFileService& backgroundFileService,
        signallab::app::BackgroundAnalysisService& backgroundAnalysisService,
        signallab::task::Scheduler& scheduler,
        QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void importCsv();
    void importWav();
    void analyzeSelectedSignal();
    void cancelSelectedTask();
    void clearAnalysis();
    void processBackgroundAnalysisEvents();
    void processBackgroundFileEvents();
    void refreshHistory();
    void refreshSignalList();
    void refreshTaskList();
    void retrySelectedTask();
    void showAnalysisResult(
        const signallab::app::AnalysisResult& result,
        bool focusSpectrum);
    void showHistoryTask(int row);
    void showSignal(std::size_t index);
    void showSpectrum(const signallab::algorithm::Spectrum& spectrum);

    signallab::app::ImportService& importService_;
    signallab::app::AnalysisService& analysisService_;
    signallab::app::BackgroundFileService& backgroundFileService_;
    signallab::app::BackgroundAnalysisService& backgroundAnalysisService_;
    signallab::task::Scheduler& scheduler_;
    QListWidget* signalList_;
    QLabel* nameValue_;
    QLabel* sampleRateValue_;
    QLabel* sampleCountValue_;
    QLabel* durationValue_;
    QLabel* meanValue_;
    QLabel* varianceValue_;
    QLabel* rmsValue_;
    QLabel* peakToPeakValue_;
    QLabel* dominantFrequencyValue_;
    QLabel* snrValue_;
    QLabel* thdValue_;
    QTabWidget* chartTabs_;
    QChartView* timeChartView_;
    QChartView* spectrumChartView_;
    QListWidget* historyList_;
    QTextEdit* historyDetails_;
    QListWidget* taskList_;
    QPushButton* cancelTaskButton_;
    QPushButton* retryTaskButton_;
    std::vector<signallab::repository::AnalysisTaskRecord> historyTasks_;
    std::vector<signallab::task::TaskId> visibleTaskIds_;
    std::unordered_map<
        signallab::task::TaskId,
        signallab::repository::CsvImportOptions> csvPreviewOptions_;
};
