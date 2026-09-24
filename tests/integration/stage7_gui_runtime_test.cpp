#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QIcon>
#include <QListView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QStringListModel>
#include <QTableView>
#include <QTabWidget>
#include <QTextCursor>
#include <QThread>
#include <QWidget>

#include <QtCharts/QChart>
#include <QtCharts/QLineSeries>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "gui/interactive_chart_view.hpp"
#include "gui/main_window.hpp"
#include "gui/stage7_models.hpp"
#include "signallab/app/analysis_service.hpp"
#include "signallab/app/background_analysis_service.hpp"
#include "signallab/app/background_file_service.hpp"
#include "signallab/app/background_script_service.hpp"
#include "signallab/app/import_service.hpp"
#include "signallab/app/script_service.hpp"
#include "signallab/core/signal_generator.hpp"
#include "signallab/database/database_executor.hpp"
#include "signallab/io/filesystem_file_repository.hpp"
#include "signallab/io/filesystem_script_file_gateway.hpp"
#include "signallab/repository/signal_repository.hpp"
#include "signallab/task/scheduler.hpp"

namespace {

using namespace std::chrono_literals;

class TemporaryProject {
public:
    TemporaryProject()
    {
        const auto uniqueNumber =
            std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path()
            / ("signallab-stage7-gui-" + std::to_string(uniqueNumber));
        std::filesystem::create_directories(root_);
    }

    ~TemporaryProject()
    {
        std::error_code ignoredError;
        std::filesystem::remove_all(root_, ignoredError);
    }

    TemporaryProject(const TemporaryProject&) = delete;
    TemporaryProject& operator=(const TemporaryProject&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept
    {
        return root_;
    }

    [[nodiscard]] std::filesystem::path databasePath() const
    {
        return root_ / "runtime.sqlite3";
    }

    [[nodiscard]] std::filesystem::path inputRoot() const
    {
        return root_ / "script-input";
    }

    [[nodiscard]] std::filesystem::path outputRoot() const
    {
        return root_ / "script-output";
    }

private:
    std::filesystem::path root_;
};

std::filesystem::path migrationsDirectory()
{
    return std::filesystem::path(SIGNALLAB_SOURCE_DIR) / "migrations";
}

signallab::core::Signal makeMillionPointSignal()
{
    constexpr double sampleRate = 48'000.0;
    constexpr std::size_t sampleCount = 1'000'000U;
    signallab::core::SignalMetadata metadata{
        "Stage 7 million point signal", "V", "GUI runtime test"};
    auto signal = signallab::core::generateSineWave(
        1'000.0,
        1.0,
        sampleRate,
        static_cast<double>(sampleCount) / sampleRate,
        std::move(metadata));
    return std::move(signal).value();
}

template <typename Predicate>
bool processEventsUntil(Predicate&& predicate, int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return predicate();
}

std::size_t taskCount(
    const signallab::task::Scheduler& scheduler,
    std::string_view name,
    signallab::task::TaskStatus status)
{
    const auto snapshots = scheduler.snapshots();
    return static_cast<std::size_t>(std::count_if(
        snapshots.begin(),
        snapshots.end(),
        [name, status](const signallab::task::TaskSnapshot& snapshot) {
            return snapshot.name == name && snapshot.status == status;
        }));
}

void writeScriptCsv(const std::filesystem::path& destination)
{
    std::filesystem::create_directories(destination.parent_path());
    std::ofstream stream(destination, std::ios::binary);
    ASSERT_TRUE(stream.is_open());
    stream << "time,value\n";
    constexpr double cycle[]{1.0, 0.0, -1.0, 0.0};
    for (std::size_t index = 0; index < 128U; ++index) {
        stream << static_cast<double>(index) / 1'024.0
               << ',' << cycle[index % 4U] << '\n';
    }
    stream.close();
    ASSERT_TRUE(stream.good());
}

TEST(Stage7ModelTest, ExposesStatusAccessibilityAndResultUnits)
{
    signallab::gui::TaskListModel taskModel;
    signallab::task::TaskSnapshot failedTask;
    failedTask.id = 7;
    failedTask.name = "FFT analysis";
    failedTask.status = signallab::task::TaskStatus::Failed;
    failedTask.progressPercent = 35;
    failedTask.errorMessage = "invalid input";
    taskModel.setSnapshots({failedTask});

    const QModelIndex taskIndex = taskModel.index(0, 0);
    ASSERT_TRUE(taskIndex.isValid());
    EXPECT_TRUE(taskIndex.data(Qt::DisplayRole).toString().contains(
        QStringLiteral("\u5931\u8d25")));
    const QIcon statusIcon =
        qvariant_cast<QIcon>(taskIndex.data(Qt::DecorationRole));
    EXPECT_FALSE(statusIcon.isNull());
    EXPECT_FALSE(
        taskIndex.data(Qt::AccessibleTextRole).toString().isEmpty());
    const QString accessibleDescription =
        taskIndex.data(Qt::AccessibleDescriptionRole).toString();
    EXPECT_FALSE(accessibleDescription.isEmpty());
    EXPECT_TRUE(accessibleDescription.contains(
        QStringLiteral("\u5efa\u8bae")));

    signallab::core::SignalMetadata metadata{
        "Model result signal", "V", "Stage 7 test"};
    auto signal = signallab::core::generateSineWave(
        64.0, 1.0, 1'024.0, 0.25, std::move(metadata));
    ASSERT_TRUE(signal) << signal.error().message;
    const signallab::app::AnalysisOptions options;
    auto result = signallab::app::analyzeSignal(signal.value(), options);
    ASSERT_TRUE(result) << result.error().message;

    signallab::gui::AnalysisResultModel resultModel;
    resultModel.setResult(
        result.value(),
        QStringLiteral("V"),
        options,
        QStringLiteral("stage7-test-version"));
    EXPECT_FALSE(resultModel.algorithmVersion().isEmpty());
    EXPECT_FALSE(resultModel.parameterSummary().isEmpty());

    QSet<QString> units;
    for (int row = 0; row < resultModel.rowCount(); ++row) {
        units.insert(resultModel.index(
            row,
            signallab::gui::AnalysisResultModel::UnitColumn)
                         .data(Qt::DisplayRole)
                         .toString());
    }
    EXPECT_TRUE(units.contains(QStringLiteral("V")));
    EXPECT_TRUE(units.contains(QStringLiteral("V\u00b2")));
    EXPECT_TRUE(units.contains(QStringLiteral("Hz")));
    EXPECT_TRUE(units.contains(QStringLiteral("dB")));
    EXPECT_TRUE(units.contains(QStringLiteral("%")));
}

TEST(Stage7GuiRuntimeTest,
     UsesDedicatedModelsAndBoundsMillionPointChartSubmission)
{
    TemporaryProject project;
    signallab::database::DatabaseExecutor database(
        project.databasePath(), migrationsDirectory());
    ASSERT_TRUE(database.start());

    auto stored = database.saveSignal(
        signallab::repository::SignalRegistration{
            signallab::repository::SignalSourceType::Generated,
            "stage7-million-point-signal",
            {},
            "{}",
            std::nullopt,
            makeMillionPointSignal()});
    ASSERT_TRUE(stored) << stored.error().message;

    signallab::io::FilesystemFileRepository files(project.root());
    signallab::app::ImportService imports(files, database);
    ASSERT_TRUE(imports.restoreSignals());
    signallab::app::AnalysisService analysis(database, database);
    signallab::task::Scheduler scheduler;
    signallab::app::BackgroundAnalysisService backgroundAnalysis(
        analysis, scheduler);
    signallab::app::BackgroundFileService backgroundFiles(
        imports, scheduler);
    MainWindow window(
        imports,
        analysis,
        backgroundFiles,
        backgroundAnalysis,
        scheduler);
    window.resize(1'280, 800);
    window.show();

    auto* signalView = window.findChild<QListView*>("signalListView");
    auto* taskView = window.findChild<QListView*>("taskListView");
    auto* historyView =
        window.findChild<QListView*>("analysisHistoryView");
    auto* resultView = window.findChild<QTableView*>("analysisResultView");
    ASSERT_NE(signalView, nullptr);
    ASSERT_NE(taskView, nullptr);
    ASSERT_NE(historyView, nullptr);
    ASSERT_NE(resultView, nullptr);
    ASSERT_NE(signalView->model(), nullptr);
    ASSERT_NE(taskView->model(), nullptr);
    ASSERT_NE(historyView->model(), nullptr);
    ASSERT_NE(resultView->model(), nullptr);

    EXPECT_NE(
        dynamic_cast<signallab::gui::SignalListModel*>(signalView->model()),
        nullptr);
    EXPECT_NE(
        dynamic_cast<signallab::gui::TaskListModel*>(taskView->model()),
        nullptr);
    EXPECT_NE(
        dynamic_cast<signallab::gui::AnalysisHistoryModel*>(
            historyView->model()),
        nullptr);
    EXPECT_NE(
        dynamic_cast<signallab::gui::AnalysisResultModel*>(
            resultView->model()),
        nullptr);
    EXPECT_EQ(dynamic_cast<QStringListModel*>(signalView->model()), nullptr);
    EXPECT_EQ(dynamic_cast<QStringListModel*>(taskView->model()), nullptr);
    EXPECT_EQ(dynamic_cast<QStringListModel*>(historyView->model()), nullptr);
    EXPECT_NE(signalView->model(), taskView->model());
    EXPECT_NE(taskView->model(), historyView->model());
    EXPECT_NE(historyView->model(), resultView->model());

    EXPECT_NE(window.findChild<QWidget*>("analysisParameters"), nullptr);
    EXPECT_NE(window.findChild<QPlainTextEdit*>("scriptEditor"), nullptr);
    EXPECT_NE(window.findChild<QPushButton*>("runScriptButton"), nullptr);
    EXPECT_NE(window.findChild<QListView*>("scriptDiagnosticView"), nullptr);
    EXPECT_NE(window.findChild<QListView*>("scriptHistoryView"), nullptr);

    auto* chartView =
        window.findChild<InteractiveChartView*>("timeChartView");
    ASSERT_NE(chartView, nullptr);
    ASSERT_NE(chartView->chart(), nullptr);
    ASSERT_TRUE(processEventsUntil(
        [chartView]() {
            const auto series = chartView->chart()->series();
            if (series.isEmpty()) {
                return false;
            }
            const auto* line = qobject_cast<QLineSeries*>(series.front());
            return line != nullptr && line->count() > 0;
        },
        2'000));

    const auto chartSeries = chartView->chart()->series();
    ASSERT_FALSE(chartSeries.isEmpty());
    auto* lineSeries = qobject_cast<QLineSeries*>(chartSeries.front());
    ASSERT_NE(lineSeries, nullptr);
    const int pixelWidth = std::max(1, chartView->viewport()->width());
    EXPECT_GT(lineSeries->count(), 0);
    EXPECT_LE(lineSeries->count(), pixelWidth * 2);

    window.close();
    backgroundFiles.clearEventNotifier();
    backgroundAnalysis.clearEventNotifier();
    scheduler.shutdownAndWait();
    ASSERT_TRUE(database.shutdown());
}

TEST(Stage7GuiRuntimeTest,
     RunsRepeatableScriptsAndSurfacesPlotsHistoryAndDiagnostics)
{
    TemporaryProject project;
    writeScriptCsv(project.inputRoot() / "small.csv");
    std::filesystem::create_directories(project.outputRoot());

    signallab::database::DatabaseExecutor database(
        project.databasePath(), migrationsDirectory());
    ASSERT_TRUE(database.start());
    signallab::io::FilesystemFileRepository files(project.root());
    signallab::app::ImportService imports(files, database);
    ASSERT_TRUE(imports.restoreSignals());
    signallab::app::AnalysisService analysis(database, database);
    signallab::io::FilesystemScriptFileGateway scriptFiles(
        project.inputRoot(), project.outputRoot());
    signallab::app::ScriptService scripts(
        imports, database, scriptFiles);
    signallab::task::Scheduler scheduler;
    signallab::app::BackgroundAnalysisService backgroundAnalysis(
        analysis, scheduler);
    signallab::app::BackgroundFileService backgroundFiles(
        imports, scheduler);
    signallab::app::BackgroundScriptService backgroundScripts(
        scripts, scheduler);
    MainWindow window(
        imports,
        analysis,
        backgroundFiles,
        backgroundAnalysis,
        scheduler,
        &backgroundScripts);
    window.resize(1'280, 800);
    window.show();

    auto* editor = window.findChild<QPlainTextEdit*>("scriptEditor");
    auto* runButton = window.findChild<QPushButton*>("runScriptButton");
    auto* output = window.findChild<QPlainTextEdit*>("scriptOutput");
    auto* plotTabs = window.findChild<QTabWidget*>("scriptPlotTabs");
    auto* workspaceTabs = window.findChild<QTabWidget*>("workspaceTabs");
    auto* loadHistoryButton =
        window.findChild<QPushButton*>("loadScriptHistoryButton");
    auto* historyView = window.findChild<QListView*>("scriptHistoryView");
    auto* diagnosticView =
        window.findChild<QListView*>("scriptDiagnosticView");
    auto* signalView = window.findChild<QListView*>("signalListView");
    ASSERT_NE(editor, nullptr);
    ASSERT_NE(runButton, nullptr);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(plotTabs, nullptr);
    ASSERT_NE(workspaceTabs, nullptr);
    ASSERT_NE(loadHistoryButton, nullptr);
    ASSERT_NE(historyView, nullptr);
    ASSERT_NE(diagnosticView, nullptr);
    ASSERT_NE(signalView, nullptr);
    ASSERT_NE(historyView->model(), nullptr);
    ASSERT_NE(diagnosticView->model(), nullptr);
    ASSERT_NE(signalView->model(), nullptr);
    EXPECT_NE(
        dynamic_cast<signallab::gui::ScriptHistoryModel*>(
            historyView->model()),
        nullptr);

    int scriptTabIndex = -1;
    for (int index = 0; index < workspaceTabs->count(); ++index) {
        if (workspaceTabs->tabText(index)
                == QStringLiteral("SignalScript")) {
            scriptTabIndex = index;
            break;
        }
    }
    ASSERT_GE(scriptTabIndex, 0);
    workspaceTabs->setCurrentIndex(scriptTabIndex);

    const QString successfulSource = QStringLiteral(
        "load \"small.csv\" as raw;\n"
        "spec = raw |> fft(window=\"hann\");\n"
        "result = features(spec);\n"
        "plot raw as \"Waveform\";\n"
        "plot spec as \"Spectrum\";\n");
    editor->setPlainText(successfulSource);

    for (std::size_t expectedSuccesses = 1;
         expectedSuccesses <= 2;
         ++expectedSuccesses) {
        ASSERT_TRUE(processEventsUntil(
            [runButton]() { return runButton->isEnabled(); },
            2'000));
        runButton->click();
        ASSERT_TRUE(processEventsUntil(
            [&scheduler, runButton, expectedSuccesses]() {
                return runButton->isEnabled()
                    && taskCount(
                           scheduler,
                           "Run SignalScript",
                           signallab::task::TaskStatus::Succeeded)
                        >= expectedSuccesses;
            },
            10'000));
    }

    ASSERT_TRUE(processEventsUntil(
        [historyView]() { return historyView->model()->rowCount() >= 2; },
        5'000));
    EXPECT_EQ(workspaceTabs->currentIndex(), scriptTabIndex);
    historyView->setCurrentIndex(historyView->model()->index(0, 0));
    editor->setPlainText(QStringLiteral("temporary editor contents"));
    loadHistoryButton->click();
    EXPECT_EQ(editor->toPlainText(), successfulSource);
    EXPECT_GE(signalView->model()->rowCount(), 1);
    EXPECT_GE(plotTabs->count(), 2);
    const QString outputText = output->toPlainText();
    EXPECT_FALSE(outputText.isEmpty());
    EXPECT_TRUE(outputText.contains(
        QStringLiteral("\u72b6\u6001\uff1a\u5df2\u5b8c\u6210")));
    EXPECT_TRUE(outputText.contains(QStringLiteral("raw =")));
    EXPECT_TRUE(outputText.contains(QStringLiteral("spec =")));
    EXPECT_TRUE(outputText.contains(QStringLiteral("result =")));
    EXPECT_EQ(
        taskCount(
            scheduler,
            "Run SignalScript",
            signallab::task::TaskStatus::Failed),
        0U);

    editor->setPlainText(QString::fromUtf8(
        "# UTF-16 cursor check: \xF0\x9F\x98\x80\n"
        "answer = missing;\n"));
    ASSERT_TRUE(runButton->isEnabled());
    runButton->click();
    ASSERT_TRUE(processEventsUntil(
        [&scheduler, runButton, diagnosticView]() {
            return runButton->isEnabled()
                && taskCount(
                       scheduler,
                       "Run SignalScript",
                       signallab::task::TaskStatus::Failed)
                    >= 1U
                && diagnosticView->model()->rowCount() > 0;
        },
        10'000));

    const QModelIndex diagnosticIndex =
        diagnosticView->model()->index(0, 0);
    ASSERT_TRUE(diagnosticIndex.isValid());
    diagnosticView->clicked(diagnosticIndex);
    EXPECT_EQ(editor->textCursor().blockNumber(), 1);
    EXPECT_EQ(editor->textCursor().positionInBlock(), 9);
    ASSERT_TRUE(processEventsUntil(
        [historyView]() { return historyView->model()->rowCount() >= 3; },
        5'000));

    window.close();
    backgroundFiles.clearEventNotifier();
    backgroundAnalysis.clearEventNotifier();
    backgroundScripts.clearEventNotifier();
    scheduler.shutdownAndWait();
    ASSERT_TRUE(database.shutdown());
}

}  // namespace
