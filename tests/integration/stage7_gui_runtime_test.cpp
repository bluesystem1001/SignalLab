#include <QAbstractItemModel>
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QStringListModel>
#include <QTableView>
#include <QTabWidget>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QSpinBox>
#include <QWidget>

#include <QtCharts/QChart>
#include <QtCharts/QLegend>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

void answerNextQuestion(QMessageBox::StandardButton answer)
{
    QTimer::singleShot(0, [answer]() {
        auto* messageBox = qobject_cast<QMessageBox*>(
            QApplication::activeModalWidget());
        if (messageBox == nullptr) {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                messageBox = qobject_cast<QMessageBox*>(widget);
                if (messageBox != nullptr) {
                    break;
                }
            }
        }
        if (messageBox == nullptr) {
            ADD_FAILURE() << "The expected confirmation dialog did not open.";
            QApplication::closeAllWindows();
            return;
        }
        auto* button = messageBox->button(answer);
        if (button == nullptr) {
            ADD_FAILURE() << "The confirmation dialog has no requested button.";
            messageBox->reject();
            return;
        }
        button->click();
    });
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
    auto comparisonSignal = signallab::core::generateSineWave(
        3.0,
        0.5,
        100.0,
        30.0,
        signallab::core::SignalMetadata{
            "Stage 7 comparison signal",
            "mV",
            "GUI shared time-axis test"});
    ASSERT_TRUE(comparisonSignal) << comparisonSignal.error().message;
    auto comparisonStored = database.saveSignal(
        signallab::repository::SignalRegistration{
            signallab::repository::SignalSourceType::Generated,
            "stage7-comparison-signal",
            {},
            "{}",
            std::nullopt,
            std::move(comparisonSignal).value()});
    ASSERT_TRUE(comparisonStored) << comparisonStored.error().message;

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
    auto* comparisonCombo =
        window.findChild<QComboBox*>("comparisonSignalCombo");
    auto* timeScrollBar =
        window.findChild<QScrollBar*>("timeAxisScrollBar");
    auto* timeSelectionLabel =
        window.findChild<QLabel*>("timeSelectionLabel");
    ASSERT_NE(chartView, nullptr);
    ASSERT_NE(chartView->chart(), nullptr);
    ASSERT_NE(comparisonCombo, nullptr);
    ASSERT_NE(timeScrollBar, nullptr);
    ASSERT_NE(timeSelectionLabel, nullptr);
    timeSelectionLabel->setText(QStringLiteral("旧选择摘要"));
    chartView->clearSelection();
    EXPECT_EQ(
        timeSelectionLabel->text(),
        QStringLiteral("局部选择：按住 Shift 拖动"));
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
    ASSERT_EQ(chartSeries.size(), 2);
    auto* primarySeries = qobject_cast<QLineSeries*>(chartSeries.at(0));
    auto* comparisonSeries = qobject_cast<QLineSeries*>(chartSeries.at(1));
    ASSERT_NE(primarySeries, nullptr);
    ASSERT_NE(comparisonSeries, nullptr);
    const int pixelWidth = std::max(1, chartView->viewport()->width());
    EXPECT_GT(primarySeries->count(), 0);
    EXPECT_LE(primarySeries->count(), pixelWidth * 2);
    EXPECT_EQ(comparisonSeries->count(), 0);
    EXPECT_FALSE(chartView->chart()->legend()->isVisible());
    EXPECT_FALSE(timeScrollBar->isEnabled());
    EXPECT_EQ(timeScrollBar->maximum(), 0);

    int comparisonIndex = -1;
    for (int index = 0; index < comparisonCombo->count(); ++index) {
        if (comparisonCombo->itemData(index).toLongLong()
            == comparisonStored.value().recordId) {
            comparisonIndex = index;
            break;
        }
    }
    ASSERT_GT(comparisonIndex, 0);
    comparisonCombo->setCurrentIndex(comparisonIndex);
    ASSERT_TRUE(processEventsUntil(
        [comparisonSeries]() { return comparisonSeries->count() > 0; },
        2'000));
    EXPECT_TRUE(chartView->chart()->legend()->isVisible());
    EXPECT_TRUE(primarySeries->name().contains(QStringLiteral("主信号")));
    EXPECT_TRUE(
        comparisonSeries->name().contains(QStringLiteral("对比")));
    EXPECT_LE(primarySeries->count(), pixelWidth * 2);
    EXPECT_LE(comparisonSeries->count(), pixelWidth * 2);

    EXPECT_TRUE(chartView->zoomOrientations().testFlag(Qt::Horizontal));
    EXPECT_FALSE(chartView->zoomOrientations().testFlag(Qt::Vertical));
    auto* zoomInButton =
        window.findChild<QPushButton*>("timeZoomInButton");
    auto* zoomOutButton =
        window.findChild<QPushButton*>("timeZoomOutButton");
    auto* resetButton =
        window.findChild<QPushButton*>("timeResetButton");
    ASSERT_NE(zoomInButton, nullptr);
    ASSERT_NE(zoomOutButton, nullptr);
    ASSERT_NE(resetButton, nullptr);
    const auto horizontalAxes = chartView->chart()->axes(Qt::Horizontal);
    const auto verticalAxes = chartView->chart()->axes(Qt::Vertical);
    ASSERT_FALSE(horizontalAxes.empty());
    ASSERT_FALSE(verticalAxes.empty());
    auto* horizontalAxis = qobject_cast<QValueAxis*>(
        horizontalAxes.front());
    auto* verticalAxis = qobject_cast<QValueAxis*>(verticalAxes.front());
    ASSERT_NE(horizontalAxis, nullptr);
    ASSERT_NE(verticalAxis, nullptr);
    EXPECT_EQ(
        verticalAxis->titleText(), QStringLiteral("幅值（V / mV）"));
    EXPECT_TRUE(primarySeries->attachedAxes().contains(horizontalAxis));
    EXPECT_TRUE(comparisonSeries->attachedAxes().contains(horizontalAxis));
    EXPECT_NEAR(horizontalAxis->min(), 0.0, 1.0e-12);
    EXPECT_NEAR(horizontalAxis->max(), 30.0, 1.0e-12);
    for (const QPointF& point : comparisonSeries->points()) {
        EXPECT_NEAR(
            point.x() * 100.0,
            std::round(point.x() * 100.0),
            1.0e-9);
    }
    const double originalHorizontalSpan =
        horizontalAxis->max() - horizontalAxis->min();
    const double originalVerticalMinimum = verticalAxis->min();
    const double originalVerticalMaximum = verticalAxis->max();
    zoomInButton->click();
    QCoreApplication::processEvents();
    EXPECT_LT(
        horizontalAxis->max() - horizontalAxis->min(),
        originalHorizontalSpan);
    EXPECT_DOUBLE_EQ(verticalAxis->min(), originalVerticalMinimum);
    EXPECT_DOUBLE_EQ(verticalAxis->max(), originalVerticalMaximum);
    ASSERT_TRUE(timeScrollBar->isEnabled());
    ASSERT_GT(timeScrollBar->maximum(), 0);
    const double zoomedSpan =
        horizontalAxis->max() - horizontalAxis->min();
    const int scrollMaximum = timeScrollBar->maximum();
    timeScrollBar->setSliderDown(true);
    for (int step = 1; step <= 25; ++step) {
        timeScrollBar->setValue(scrollMaximum * step / 25);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(5);
    }
    ASSERT_GT(comparisonSeries->count(), 0);
    EXPECT_GT(comparisonSeries->points().front().x(), 0.0);
    timeScrollBar->setSliderDown(false);
    QCoreApplication::processEvents();
    EXPECT_NEAR(
        horizontalAxis->max() - horizontalAxis->min(),
        zoomedSpan,
        originalHorizontalSpan * 1.0e-12);
    EXPECT_NEAR(
        horizontalAxis->max(),
        originalHorizontalSpan,
        originalHorizontalSpan * 1.0e-12);
    EXPECT_DOUBLE_EQ(verticalAxis->min(), originalVerticalMinimum);
    EXPECT_DOUBLE_EQ(verticalAxis->max(), originalVerticalMaximum);
    ASSERT_TRUE(processEventsUntil(
        [primarySeries, comparisonSeries, pixelWidth]() {
            return primarySeries->count() > 0
                && comparisonSeries->count() > 0
                && primarySeries->count() <= pixelWidth * 2
                && comparisonSeries->count() <= pixelWidth * 2;
        },
        2'000));

    zoomOutButton->click();
    QCoreApplication::processEvents();
    EXPECT_NEAR(
        horizontalAxis->max() - horizontalAxis->min(),
        originalHorizontalSpan,
        originalHorizontalSpan * 1.0e-12);
    EXPECT_NEAR(horizontalAxis->min(), 0.0, 1.0e-12);
    EXPECT_FALSE(timeScrollBar->isEnabled());

    zoomInButton->click();
    QCoreApplication::processEvents();
    ASSERT_TRUE(timeScrollBar->isEnabled());
    resetButton->click();
    QCoreApplication::processEvents();
    EXPECT_NEAR(horizontalAxis->min(), 0.0, 1.0e-12);
    EXPECT_NEAR(horizontalAxis->max(), 30.0, 1.0e-12);
    EXPECT_EQ(timeScrollBar->value(), 0);
    EXPECT_FALSE(timeScrollBar->isEnabled());

    zoomInButton->click();
    zoomInButton->click();
    QCoreApplication::processEvents();
    ASSERT_TRUE(timeScrollBar->isEnabled());
    timeScrollBar->setValue(timeScrollBar->maximum());
    ASSERT_TRUE(processEventsUntil(
        [primarySeries, comparisonSeries]() {
            return primarySeries->count() == 0
                && comparisonSeries->count() > 0;
        },
        2'000));
    EXPECT_DOUBLE_EQ(verticalAxis->min(), originalVerticalMinimum);
    EXPECT_DOUBLE_EQ(verticalAxis->max(), originalVerticalMaximum);
    resetButton->click();
    QCoreApplication::processEvents();

    comparisonCombo->setCurrentIndex(0);
    QCoreApplication::processEvents();
    EXPECT_EQ(comparisonSeries->count(), 0);
    EXPECT_FALSE(chartView->chart()->legend()->isVisible());
    EXPECT_NEAR(
        horizontalAxis->max(),
        1'000'000.0 / 48'000.0,
        1.0e-12);

    window.close();
    backgroundFiles.clearEventNotifier();
    backgroundAnalysis.clearEventNotifier();
    scheduler.shutdownAndWait();
    ASSERT_TRUE(database.shutdown());
}

TEST(Stage7GuiRuntimeTest,
     GeneratesPersistedFirSignalFromTheSidePanel)
{
    TemporaryProject project;
    signallab::database::DatabaseExecutor database(
        project.databasePath(), migrationsDirectory());
    ASSERT_TRUE(database.start());

    signallab::core::SignalMetadata metadata{
        "FIR GUI source", "V", "Stage 7 FIR GUI test"};
    auto signal = signallab::core::generateSineWave(
        64.0, 1.0, 1'024.0, 0.25, std::move(metadata));
    ASSERT_TRUE(signal) << signal.error().message;
    auto stored = database.saveSignal(
        signallab::repository::SignalRegistration{
            signallab::repository::SignalSourceType::Generated,
            "stage7-fir-gui-source",
            {},
            "{}",
            std::nullopt,
            std::move(signal).value()});
    ASSERT_TRUE(stored) << stored.error().message;

    signallab::io::FilesystemFileRepository files(project.root());
    signallab::app::ImportService imports(files, database);
    ASSERT_TRUE(imports.restoreSignals());
    ASSERT_EQ(imports.entries().size(), 1U);
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

    auto* type = window.findChild<QComboBox*>("firTypeCombo");
    auto* cutoff = window.findChild<QDoubleSpinBox*>("firCutoffSpin");
    auto* taps = window.findChild<QSpinBox*>("firTapCountSpin");
    auto* validation =
        window.findChild<QLabel*>("firValidationLabel");
    auto* apply = window.findChild<QPushButton*>("applyFirButton");
    auto* signalView = window.findChild<QListView*>("signalListView");
    ASSERT_NE(type, nullptr);
    ASSERT_NE(cutoff, nullptr);
    ASSERT_NE(taps, nullptr);
    ASSERT_NE(validation, nullptr);
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(signalView, nullptr);
    ASSERT_TRUE(processEventsUntil(
        [apply]() { return apply->isEnabled(); }, 2'000));

    taps->setValue(32);
    EXPECT_FALSE(apply->isEnabled());
    EXPECT_TRUE(validation->text().contains(QStringLiteral("奇数")));
    taps->setValue(31);
    cutoff->setValue(64.0);
    type->setCurrentIndex(type->findData(static_cast<int>(
        signallab::app::FirFilterType::HighPass)));
    ASSERT_TRUE(apply->isEnabled());

    apply->click();
    ASSERT_TRUE(processEventsUntil(
        [&imports, signalView]() {
            return imports.entries().size() == 2U
                && signalView->model()->rowCount() == 2;
        },
        5'000));

    const auto& filtered = imports.entries().back();
    EXPECT_EQ(
        filtered.sourceType,
        signallab::repository::SignalSourceType::Generated);
    EXPECT_EQ(filtered.signal.sampleCount(), 256U);
    EXPECT_NE(filtered.displayName.find("FIR high-pass"), std::string::npos);
    EXPECT_NE(
        filtered.parametersJson.find("\"cutoff_hz\":64"),
        std::string::npos);
    EXPECT_NE(
        filtered.parametersJson.find("\"tap_count\":31"),
        std::string::npos);
    EXPECT_EQ(signalView->currentIndex().row(), 1);

    const auto snapshots = scheduler.snapshots();
    const auto firTask = std::find_if(
        snapshots.begin(),
        snapshots.end(),
        [](const signallab::task::TaskSnapshot& snapshot) {
            return snapshot.name.starts_with("FIR filter:");
        });
    ASSERT_NE(firTask, snapshots.end());
    EXPECT_EQ(firTask->lane, signallab::task::TaskLane::Compute);
    EXPECT_EQ(firTask->status, signallab::task::TaskStatus::Succeeded);

    window.close();
    backgroundFiles.clearEventNotifier();
    backgroundAnalysis.clearEventNotifier();
    scheduler.shutdownAndWait();
    ASSERT_TRUE(database.shutdown());
}

TEST(Stage7GuiRuntimeTest,
     DeletesTaskCenterAndAnalysisHistoryWithIndependentScopes)
{
    TemporaryProject project;
    signallab::database::DatabaseExecutor database(
        project.databasePath(), migrationsDirectory());
    ASSERT_TRUE(database.start());

    signallab::core::SignalMetadata metadata{
        "Deletion boundary signal", "V", "Stage 7 GUI deletion test"};
    auto signal = signallab::core::generateSineWave(
        64.0, 1.0, 1'024.0, 0.25, std::move(metadata));
    ASSERT_TRUE(signal) << signal.error().message;
    auto stored = database.saveSignal(
        signallab::repository::SignalRegistration{
            signallab::repository::SignalSourceType::Generated,
            "stage7-deletion-boundary-signal",
            {},
            "{}",
            std::nullopt,
            std::move(signal).value()});
    ASSERT_TRUE(stored) << stored.error().message;
    const std::int64_t signalRecordId = stored.value().recordId;

    signallab::io::FilesystemFileRepository files(project.root());
    signallab::app::ImportService imports(files, database);
    ASSERT_TRUE(imports.restoreSignals());
    signallab::app::AnalysisService analysis(database, database);
    for (int run = 0; run < 2; ++run) {
        auto result = analysis.analyze(signalRecordId);
        ASSERT_TRUE(result) << result.error().message;
    }

    signallab::task::Scheduler scheduler;
    const auto submitFinishedTask = [&scheduler](std::string name) {
        return scheduler.submit(signallab::task::TaskSpec{
            std::move(name),
            signallab::task::TaskLane::Compute,
            [](const signallab::task::TaskContext&) {
                return signallab::core::Result<bool>::success(true);
            },
            {}});
    };
    auto firstTask = submitFinishedTask("first deletable task");
    auto secondTask = submitFinishedTask("second deletable task");
    ASSERT_TRUE(firstTask) << firstTask.error().message;
    ASSERT_TRUE(secondTask) << secondTask.error().message;
    ASSERT_TRUE(scheduler.waitForTerminal(firstTask.value(), 2s));
    ASSERT_TRUE(scheduler.waitForTerminal(secondTask.value(), 2s));

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
    QCoreApplication::processEvents();

    auto* taskView = window.findChild<QListView*>("taskListView");
    auto* historyView =
        window.findChild<QListView*>("analysisHistoryView");
    auto* deleteTaskButton =
        window.findChild<QPushButton*>("deleteTaskButton");
    auto* deleteHistoryButton =
        window.findChild<QPushButton*>("deleteAnalysisHistoryButton");
    ASSERT_NE(taskView, nullptr);
    ASSERT_NE(historyView, nullptr);
    ASSERT_NE(deleteTaskButton, nullptr);
    ASSERT_NE(deleteHistoryButton, nullptr);

    auto* taskModel = dynamic_cast<signallab::gui::TaskListModel*>(
        taskView->model());
    auto* historyModel =
        dynamic_cast<signallab::gui::AnalysisHistoryModel*>(
            historyView->model());
    ASSERT_NE(taskModel, nullptr);
    ASSERT_NE(historyModel, nullptr);
    ASSERT_EQ(taskModel->rowCount(), 2);
    ASSERT_EQ(historyModel->rowCount(), 2);

    historyView->setCurrentIndex(historyModel->index(0, 0));
    QCoreApplication::processEvents();
    ASSERT_TRUE(deleteHistoryButton->isEnabled());
    const auto* selectedHistory = historyModel->recordAt(0);
    const auto* adjacentHistory = historyModel->recordAt(1);
    ASSERT_NE(selectedHistory, nullptr);
    ASSERT_NE(adjacentHistory, nullptr);
    const std::int64_t deletedHistoryId = selectedHistory->id;
    const std::int64_t adjacentHistoryId = adjacentHistory->id;

    answerNextQuestion(QMessageBox::No);
    deleteHistoryButton->click();
    EXPECT_EQ(historyModel->rowCount(), 2);
    auto historyAfterDecline = analysis.history();
    ASSERT_TRUE(historyAfterDecline) << historyAfterDecline.error().message;
    EXPECT_EQ(historyAfterDecline.value().size(), 2U);

    answerNextQuestion(QMessageBox::Yes);
    deleteHistoryButton->click();
    ASSERT_EQ(historyModel->rowCount(), 1);
    ASSERT_NE(historyModel->recordAt(historyView->currentIndex()), nullptr);
    EXPECT_EQ(
        historyModel->recordAt(historyView->currentIndex())->id,
        adjacentHistoryId);
    auto remainingHistory = analysis.history();
    ASSERT_TRUE(remainingHistory) << remainingHistory.error().message;
    ASSERT_EQ(remainingHistory.value().size(), 1U);
    EXPECT_EQ(remainingHistory.value().front().id, adjacentHistoryId);
    auto deletedFeatures = analysis.featureResults(deletedHistoryId);
    ASSERT_TRUE(deletedFeatures) << deletedFeatures.error().message;
    EXPECT_TRUE(deletedFeatures.value().empty());
    auto preservedSignal = database.findByRecordId(signalRecordId);
    ASSERT_TRUE(preservedSignal) << preservedSignal.error().message;
    EXPECT_TRUE(preservedSignal.value().has_value());
    EXPECT_EQ(scheduler.snapshots().size(), 2U);

    taskView->setCurrentIndex(taskModel->index(0, 0));
    QCoreApplication::processEvents();
    ASSERT_TRUE(deleteTaskButton->isEnabled());
    const auto deletedTaskId = taskModel->taskIdAt(0);
    const auto adjacentTaskId = taskModel->taskIdAt(1);
    ASSERT_TRUE(deletedTaskId.has_value());
    ASSERT_TRUE(adjacentTaskId.has_value());

    answerNextQuestion(QMessageBox::No);
    deleteTaskButton->click();
    EXPECT_EQ(taskModel->rowCount(), 2);
    EXPECT_TRUE(scheduler.snapshot(*deletedTaskId).has_value());

    answerNextQuestion(QMessageBox::Yes);
    deleteTaskButton->click();
    ASSERT_EQ(taskModel->rowCount(), 1);
    EXPECT_FALSE(scheduler.snapshot(*deletedTaskId).has_value());
    EXPECT_EQ(taskModel->taskIdAt(taskView->currentIndex()), adjacentTaskId);
    remainingHistory = analysis.history();
    ASSERT_TRUE(remainingHistory) << remainingHistory.error().message;
    EXPECT_EQ(remainingHistory.value().size(), 1U);

    answerNextQuestion(QMessageBox::Yes);
    deleteTaskButton->click();
    EXPECT_EQ(taskModel->rowCount(), 0);
    EXPECT_FALSE(deleteTaskButton->isEnabled());
    EXPECT_TRUE(scheduler.snapshots().empty());
    remainingHistory = analysis.history();
    ASSERT_TRUE(remainingHistory) << remainingHistory.error().message;
    EXPECT_EQ(remainingHistory.value().size(), 1U);

    answerNextQuestion(QMessageBox::Yes);
    deleteHistoryButton->click();
    EXPECT_EQ(historyModel->rowCount(), 0);
    EXPECT_FALSE(deleteHistoryButton->isEnabled());
    remainingHistory = analysis.history();
    ASSERT_TRUE(remainingHistory) << remainingHistory.error().message;
    EXPECT_TRUE(remainingHistory.value().empty());
    preservedSignal = database.findByRecordId(signalRecordId);
    ASSERT_TRUE(preservedSignal) << preservedSignal.error().message;
    EXPECT_TRUE(preservedSignal.value().has_value());

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

TEST(Stage7GuiRuntimeTest,
     PermanentlyDeletesProjectSignalsAndKeepsAnalysisHistory)
{
    TemporaryProject project;
    signallab::database::DatabaseExecutor database(
        project.databasePath(), migrationsDirectory());
    ASSERT_TRUE(database.start());

    const auto saveGenerated = [&database](
                                   std::string key,
                                   std::string name,
                                   double frequency) {
        auto signal = signallab::core::generateSineWave(
            frequency,
            1.0,
            1'024.0,
            0.25,
            signallab::core::SignalMetadata{
                std::move(name), "V", "GUI signal deletion test"});
        if (!signal) {
            return signallab::core::Result<
                signallab::repository::StoredSignal>::failure(
                signal.error());
        }
        return database.saveSignal(
            signallab::repository::SignalRegistration{
                signallab::repository::SignalSourceType::Generated,
                std::move(key),
                {},
                "{}",
                std::nullopt,
                std::move(signal).value()});
    };

    auto first = saveGenerated(
        "gui-delete-first", "First deletable signal", 64.0);
    auto second = saveGenerated(
        "gui-delete-second", "Second deletable signal", 96.0);
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    const std::int64_t firstId = first.value().recordId;
    const std::int64_t secondId = second.value().recordId;

    signallab::io::FilesystemFileRepository files(project.root());
    signallab::app::ImportService imports(files, database);
    ASSERT_TRUE(imports.restoreSignals());
    signallab::app::AnalysisService analysis(database, database);
    auto analyzed = analysis.analyze(firstId);
    ASSERT_TRUE(analyzed) << analyzed.error().message;
    auto historyBefore = analysis.history();
    ASSERT_TRUE(historyBefore) << historyBefore.error().message;
    ASSERT_EQ(historyBefore.value().size(), 1U);
    const std::int64_t analysisId = historyBefore.value().front().id;

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
    QCoreApplication::processEvents();

    auto* signalView = window.findChild<QListView*>("signalListView");
    auto* deleteButton =
        window.findChild<QPushButton*>("deleteSignalButton");
    auto* analyzeButton = window.findChild<QPushButton*>("analyzeButton");
    auto* firButton = window.findChild<QPushButton*>("applyFirButton");
    auto* taskView = window.findChild<QListView*>("taskListView");
    auto* retryTaskButton =
        window.findChild<QPushButton*>("retryTaskButton");
    auto* chartView =
        window.findChild<InteractiveChartView*>("timeChartView");
    auto* comparisonCombo =
        window.findChild<QComboBox*>("comparisonSignalCombo");
    auto* timeScrollBar =
        window.findChild<QScrollBar*>("timeAxisScrollBar");
    ASSERT_NE(signalView, nullptr);
    ASSERT_NE(deleteButton, nullptr);
    ASSERT_NE(analyzeButton, nullptr);
    ASSERT_NE(firButton, nullptr);
    ASSERT_NE(taskView, nullptr);
    ASSERT_NE(retryTaskButton, nullptr);
    ASSERT_NE(chartView, nullptr);
    ASSERT_NE(comparisonCombo, nullptr);
    ASSERT_NE(timeScrollBar, nullptr);
    auto* signalModel = dynamic_cast<signallab::gui::SignalListModel*>(
        signalView->model());
    auto* taskModel = dynamic_cast<signallab::gui::TaskListModel*>(
        taskView->model());
    ASSERT_NE(signalModel, nullptr);
    ASSERT_NE(taskModel, nullptr);
    ASSERT_EQ(signalModel->rowCount(), 2);

    signalView->setCurrentIndex(signalModel->index(0, 0));
    QCoreApplication::processEvents();
    ASSERT_EQ(signalModel->recordIdAt(0), firstId);
    ASSERT_TRUE(deleteButton->isEnabled());
    ASSERT_TRUE(analyzeButton->isEnabled());
    ASSERT_TRUE(firButton->isEnabled());

    int comparisonIndex = -1;
    for (int index = 0; index < comparisonCombo->count(); ++index) {
        if (comparisonCombo->itemData(index).toLongLong() == secondId) {
            comparisonIndex = index;
            break;
        }
    }
    ASSERT_GT(comparisonIndex, 0);
    comparisonCombo->setCurrentIndex(comparisonIndex);
    QCoreApplication::processEvents();
    ASSERT_EQ(chartView->chart()->series().size(), 2);
    auto* comparisonLine = qobject_cast<QLineSeries*>(
        chartView->chart()->series().at(1));
    ASSERT_NE(comparisonLine, nullptr);
    ASSERT_GT(comparisonLine->count(), 0);

    answerNextQuestion(QMessageBox::No);
    deleteButton->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(signalModel->rowCount(), 2);
    auto firstStillStored = database.findByRecordId(firstId);
    ASSERT_TRUE(firstStillStored) << firstStillStored.error().message;
    EXPECT_TRUE(firstStillStored.value().has_value());

    auto releaseIoLane = std::make_shared<std::atomic_bool>(false);
    auto ioLaneEntered = std::make_shared<std::atomic_bool>(false);
    signallab::task::TaskSpec ioLaneBlocker;
    ioLaneBlocker.name = "Hold signal deletion I/O lane";
    ioLaneBlocker.lane = signallab::task::TaskLane::Io;
    ioLaneBlocker.action = [releaseIoLane, ioLaneEntered](
                               const signallab::task::TaskContext& context) {
        ioLaneEntered->store(true, std::memory_order_release);
        while (!releaseIoLane->load(std::memory_order_acquire)) {
            if (context.stopToken().stop_requested()) {
                return signallab::core::Result<bool>::failure(
                    signallab::core::ErrorCode::Cancelled,
                    "The deterministic I/O lane blocker was cancelled.");
            }
            std::this_thread::sleep_for(1ms);
        }
        return signallab::core::Result<bool>::success(true);
    };
    auto blockerSubmission = scheduler.submit(std::move(ioLaneBlocker));
    ASSERT_TRUE(blockerSubmission) << blockerSubmission.error().message;
    ASSERT_TRUE(processEventsUntil(
        [ioLaneEntered]() {
            return ioLaneEntered->load(std::memory_order_acquire);
        },
        5'000));

    answerNextQuestion(QMessageBox::Yes);
    deleteButton->click();
    std::optional<signallab::task::TaskId> deletionTaskId;
    ASSERT_TRUE(processEventsUntil(
        [&scheduler, &deletionTaskId]() {
            for (const auto& snapshot : scheduler.snapshots()) {
                if (snapshot.name.starts_with("Delete signal:")
                    && snapshot.status
                        == signallab::task::TaskStatus::Pending) {
                    deletionTaskId = snapshot.id;
                    return true;
                }
            }
            return false;
        },
        2'000));
    ASSERT_TRUE(deletionTaskId.has_value());
    EXPECT_FALSE(deleteButton->isEnabled());
    EXPECT_FALSE(analyzeButton->isEnabled());
    EXPECT_FALSE(firButton->isEnabled());

    const std::size_t pendingTaskCount = scheduler.snapshots().size();
    analyzeButton->click();
    firButton->click();
    deleteButton->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(scheduler.snapshots().size(), pendingTaskCount);

    releaseIoLane->store(true, std::memory_order_release);
    ASSERT_TRUE(processEventsUntil(
        [&scheduler, deletionTaskId, signalModel]() {
            const auto deletion = scheduler.snapshot(*deletionTaskId);
            return deletion.has_value()
                && deletion->status
                    == signallab::task::TaskStatus::Succeeded
                && signalModel->rowCount() == 1;
        },
        5'000));
    EXPECT_EQ(signalModel->recordIdAt(signalView->currentIndex()), secondId);
    EXPECT_EQ(comparisonCombo->currentData().toLongLong(), 0);
    EXPECT_FALSE(comparisonCombo->isEnabled());
    EXPECT_EQ(comparisonLine->count(), 0);
    EXPECT_FALSE(timeScrollBar->isEnabled());

    int deletionTaskRow = -1;
    for (int row = 0; row < taskModel->rowCount(); ++row) {
        if (taskModel->taskIdAt(row) == deletionTaskId) {
            deletionTaskRow = row;
            break;
        }
    }
    ASSERT_GE(deletionTaskRow, 0);
    taskView->setCurrentIndex(taskModel->index(deletionTaskRow, 0));
    QCoreApplication::processEvents();
    ASSERT_TRUE(retryTaskButton->isEnabled());
    const std::size_t taskCountBeforeRejectedRetry =
        scheduler.snapshots().size();
    retryTaskButton->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(
        scheduler.snapshots().size(), taskCountBeforeRejectedRetry);

    auto removedFirst = database.findByRecordId(firstId);
    ASSERT_TRUE(removedFirst) << removedFirst.error().message;
    EXPECT_FALSE(removedFirst.value().has_value());

    auto historyAfter = analysis.history();
    ASSERT_TRUE(historyAfter) << historyAfter.error().message;
    ASSERT_EQ(historyAfter.value().size(), 1U);
    EXPECT_EQ(historyAfter.value().front().id, analysisId);
    EXPECT_FALSE(historyAfter.value().front().signalRecordId.has_value());
    auto preservedFeatures = analysis.featureResults(analysisId);
    ASSERT_TRUE(preservedFeatures) << preservedFeatures.error().message;
    EXPECT_FALSE(preservedFeatures.value().empty());

    ASSERT_TRUE(processEventsUntil(
        [deleteButton, analyzeButton, firButton]() {
            return deleteButton->isEnabled()
                && analyzeButton->isEnabled()
                && firButton->isEnabled();
        },
        2'000));
    answerNextQuestion(QMessageBox::Yes);
    deleteButton->click();
    ASSERT_TRUE(processEventsUntil(
        [signalModel]() { return signalModel->rowCount() == 0; },
        5'000));
    EXPECT_FALSE(signalView->currentIndex().isValid());
    EXPECT_FALSE(deleteButton->isEnabled());
    EXPECT_FALSE(analyzeButton->isEnabled());
    EXPECT_FALSE(firButton->isEnabled());
    const auto chartSeries = chartView->chart()->series();
    ASSERT_FALSE(chartSeries.empty());
    auto* timeLine = qobject_cast<QLineSeries*>(chartSeries.front());
    ASSERT_NE(timeLine, nullptr);
    EXPECT_EQ(timeLine->count(), 0);
    ASSERT_EQ(chartSeries.size(), 2);
    auto* emptyComparisonLine = qobject_cast<QLineSeries*>(
        chartSeries.at(1));
    ASSERT_NE(emptyComparisonLine, nullptr);
    EXPECT_EQ(emptyComparisonLine->count(), 0);
    EXPECT_FALSE(comparisonCombo->isEnabled());
    EXPECT_FALSE(timeScrollBar->isEnabled());
    auto remainingSignals = database.listSignals();
    ASSERT_TRUE(remainingSignals) << remainingSignals.error().message;
    EXPECT_TRUE(remainingSignals.value().empty());

    window.close();
    backgroundFiles.clearEventNotifier();
    backgroundAnalysis.clearEventNotifier();
    scheduler.shutdownAndWait();
    ASSERT_TRUE(database.shutdown());
}

}  // namespace
