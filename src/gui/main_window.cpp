#include "main_window.hpp"

#include "audio_capture_widget.hpp"
#include "database_backup_widget.hpp"
#include "file_import_dialog.hpp"
#include "interactive_chart_view.hpp"
#include "stage7_models.hpp"

#include <QAbstractItemView>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QMessageBox>
#include <QMetaObject>
#include <QModelIndex>
#include <QPen>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QTabWidget>
#include <QTableView>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <QtCharts/QChart>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "signallab/app/display_downsampler.hpp"
#include "signallab/core/error.hpp"
#include "signallab/io/windows_audio_input.hpp"

namespace {

constexpr int kDiagnosticLineRole = Qt::UserRole + 1;
constexpr int kDiagnosticColumnRole = Qt::UserRole + 2;
constexpr int kTimeScrollResolution = 1'000'000;

std::filesystem::path toPath(const QString& text)
{
    return std::filesystem::path(text.toStdWString());
}

QString analysisStatusText(signallab::repository::AnalysisTaskStatus status)
{
    using signallab::repository::AnalysisTaskStatus;
    switch (status) {
    case AnalysisTaskStatus::Pending:
        return QStringLiteral("等待中");
    case AnalysisTaskStatus::Running:
        return QStringLiteral("运行中");
    case AnalysisTaskStatus::Succeeded:
        return QStringLiteral("已完成");
    case AnalysisTaskStatus::Failed:
        return QStringLiteral("失败");
    case AnalysisTaskStatus::Interrupted:
        return QStringLiteral("已中断");
    case AnalysisTaskStatus::Cancelled:
        return QStringLiteral("已取消");
    }
    return QStringLiteral("未知");
}

bool isTerminalAnalysisStatus(
    signallab::repository::AnalysisTaskStatus status)
{
    using signallab::repository::AnalysisTaskStatus;
    return status == AnalysisTaskStatus::Succeeded
        || status == AnalysisTaskStatus::Failed
        || status == AnalysisTaskStatus::Interrupted
        || status == AnalysisTaskStatus::Cancelled;
}

QString scriptStatusText(signallab::app::ScriptExecutionStatus status)
{
    using signallab::app::ScriptExecutionStatus;
    switch (status) {
    case ScriptExecutionStatus::Succeeded:
        return QStringLiteral("已完成");
    case ScriptExecutionStatus::Failed:
        return QStringLiteral("失败");
    case ScriptExecutionStatus::Cancelled:
        return QStringLiteral("已取消");
    }
    return QStringLiteral("未知");
}

QString textOrDash(const std::string& text)
{
    return text.empty()
        ? QStringLiteral("—")
        : QString::fromStdString(text);
}

QString actionableSuggestion(const QString& reason)
{
    const QString lower = reason.toLower();
    if (lower.contains(QStringLiteral("path"))
        || lower.contains(QStringLiteral("file"))
        || lower.contains(QStringLiteral("文件"))
        || lower.contains(QStringLiteral("路径"))) {
        return QStringLiteral(
            "检查文件是否存在、是否位于允许目录内，以及当前用户是否有读取或写入权限。");
    }
    if (lower.contains(QStringLiteral("csv"))
        || lower.contains(QStringLiteral("wav"))
        || lower.contains(QStringLiteral("format"))
        || lower.contains(QStringLiteral("格式"))) {
        return QStringLiteral(
            "检查文件格式、CSV 列设置或 WAV 编码，然后在任务中心重试。");
    }
    if (lower.contains(QStringLiteral("cancel"))
        || lower.contains(QStringLiteral("取消"))) {
        return QStringLiteral("如需继续，请选中任务并重试。");
    }
    if (lower.contains(QStringLiteral("lex"))
        || lower.contains(QStringLiteral("par"))
        || lower.contains(QStringLiteral("sem"))
        || lower.contains(QStringLiteral("run"))) {
        return QStringLiteral(
            "在脚本诊断列表中单击错误位置，修正对应语句后重新运行。");
    }
    return QStringLiteral("检查输入信号和参数；详细信息确认后可在任务中心重试。");
}

QString scriptValueText(
    const signallab::app::script_runtime::ScriptValue& value)
{
    using namespace signallab::app::script_runtime;
    return std::visit(
        [](const auto& item) -> QString {
            using Value = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Value, std::monostate>) {
                return QStringLiteral("Unit");
            } else if constexpr (std::is_same_v<Value, double>) {
                return QString::number(item, 'g', 12);
            } else if constexpr (std::is_same_v<Value, std::string>) {
                return QStringLiteral("\"%1\"")
                    .arg(QString::fromStdString(item));
            } else if constexpr (std::is_same_v<Value, bool>) {
                return item ? QStringLiteral("true")
                            : QStringLiteral("false");
            } else if constexpr (std::is_same_v<Value, SignalValue>) {
                if (!item) {
                    return QStringLiteral("Signal(null)");
                }
                return QStringLiteral("Signal(%1, %2 点, %3 Hz)")
                    .arg(QString::fromStdString(item->metadata().name))
                    .arg(static_cast<qulonglong>(item->sampleCount()))
                    .arg(item->sampleRate(), 0, 'g', 12);
            } else if constexpr (
                std::is_same_v<Value, SpectrumValuePtr>) {
                if (!item || !item->analysis) {
                    return QStringLiteral("Spectrum(null)");
                }
                return QStringLiteral("Spectrum(FFT %1 点, %2 个频点)")
                    .arg(static_cast<qulonglong>(
                        item->analysis->spectrum.fftSize))
                    .arg(static_cast<qulonglong>(
                        item->analysis->spectrum.amplitudes.size()));
            } else {
                if (!item) {
                    return QStringLiteral("FeatureSet(null)");
                }
                return QStringLiteral("FeatureSet(%1 项)")
                    .arg(static_cast<qulonglong>(item->values.size()));
            }
        },
        value);
}

int utf16OffsetForCodePointColumn(
    const QString& line,
    std::size_t oneBasedColumn)
{
    const std::size_t targetCodePoints =
        oneBasedColumn > 0 ? oneBasedColumn - 1U : 0U;
    std::size_t seen = 0;
    int utf16Offset = 0;
    while (utf16Offset < line.size() && seen < targetCodePoints) {
        const QChar current = line.at(utf16Offset);
        if (current.isHighSurrogate()
            && utf16Offset + 1 < line.size()
            && line.at(utf16Offset + 1).isLowSurrogate()) {
            utf16Offset += 2;
        } else {
            ++utf16Offset;
        }
        ++seen;
    }
    return utf16Offset;
}

QList<QPointF> toTimePoints(
    const std::vector<signallab::app::DisplayPoint>& points)
{
    QList<QPointF> converted;
    converted.reserve(static_cast<qsizetype>(points.size()));
    for (const auto& point : points) {
        converted.push_back(QPointF(point.timeSeconds, point.value));
    }
    return converted;
}

QList<QPointF> toSpectrumPoints(
    const std::vector<signallab::app::SpectrumDisplayPoint>& points)
{
    QList<QPointF> converted;
    converted.reserve(static_cast<qsizetype>(points.size()));
    for (const auto& point : points) {
        converted.push_back(QPointF(point.frequencyHz, point.amplitude));
    }
    return converted;
}

std::pair<double, double> paddedRange(double minimum, double maximum)
{
    if (minimum == maximum) {
        const double padding = std::max(std::abs(minimum) * 0.05, 0.5);
        minimum -= padding;
        maximum += padding;
    } else {
        const double padding = (maximum - minimum) * 0.05;
        minimum -= padding;
        maximum += padding;
    }
    return {minimum, maximum};
}

std::pair<double, double> yRange(const QList<QPointF>& points)
{
    if (points.empty()) {
        return {0.0, 1.0};
    }
    double minimum = points.front().y();
    double maximum = minimum;
    for (const QPointF& point : points) {
        minimum = std::min(minimum, point.y());
        maximum = std::max(maximum, point.y());
    }
    return paddedRange(minimum, maximum);
}

QString signalName(const signallab::app::SignalEntry& entry)
{
    if (!entry.displayName.empty()) {
        return QString::fromStdString(entry.displayName);
    }
    if (!entry.signal.metadata().name.empty()) {
        return QString::fromStdString(entry.signal.metadata().name);
    }
    return QStringLiteral("信号 #%1").arg(entry.recordId);
}

QString signalLegendLabel(const signallab::app::SignalEntry& entry)
{
    const QString unit = QString::fromStdString(entry.signal.metadata().unit);
    return unit.isEmpty()
        ? signalName(entry)
        : QStringLiteral("%1 [%2]").arg(signalName(entry), unit);
}

bool hasDisplayableTimeDomain(const signallab::core::Signal& signal)
{
    const double duration = signal.durationSeconds();
    const double sampleInterval = 1.0 / signal.sampleRate();
    return signal.sampleCount() > 0U && std::isfinite(duration)
        && duration > 0.0 && std::isfinite(sampleInterval)
        && sampleInterval > 0.0;
}

std::pair<std::size_t, std::size_t> visibleSampleRange(
    const signallab::core::Signal& signal,
    double firstTime,
    double lastTime)
{
    const std::size_t sampleCount = signal.sampleCount();
    const double duration = signal.durationSeconds();
    if (sampleCount == 0U || !std::isfinite(firstTime)
        || !std::isfinite(lastTime) || !std::isfinite(duration)
        || duration <= 0.0 || firstTime > lastTime
        || lastTime < 0.0 || firstTime >= duration) {
        return {0U, 0U};
    }

    const long double sampleRate = signal.sampleRate();
    const long double clampedFirst = std::clamp(firstTime, 0.0, duration);
    const long double clampedLast = std::clamp(lastTime, 0.0, duration);
    const long double firstScaled = clampedFirst * sampleRate;
    const long double lastScaled = clampedLast * sampleRate;
    const long double lastIndex = static_cast<long double>(sampleCount - 1U);
    const auto firstIndex = firstScaled >= lastIndex
        ? sampleCount - 1U
        : static_cast<std::size_t>(std::floor(firstScaled));
    const long double pastScaled = std::ceil(lastScaled) + 1.0L;
    const auto pastLastIndex = pastScaled
            >= static_cast<long double>(sampleCount)
        ? sampleCount
        : static_cast<std::size_t>(pastScaled);
    return pastLastIndex > firstIndex
        ? std::pair<std::size_t, std::size_t>{
              firstIndex, pastLastIndex}
        : std::pair<std::size_t, std::size_t>{0U, 0U};
}

std::optional<std::size_t> nearestSampleAtTime(
    const signallab::core::Signal& signal,
    double timeSeconds)
{
    const double duration = signal.durationSeconds();
    if (!std::isfinite(timeSeconds) || !std::isfinite(duration)
        || timeSeconds < 0.0 || timeSeconds >= duration
        || signal.sampleCount() == 0U) {
        return std::nullopt;
    }
    const long double scaled = static_cast<long double>(timeSeconds)
        * static_cast<long double>(signal.sampleRate());
    const long double rounded = std::floor(scaled + 0.5L);
    return rounded >= static_cast<long double>(signal.sampleCount() - 1U)
        ? signal.sampleCount() - 1U
        : static_cast<std::size_t>(rounded);
}

std::size_t sampleCountInTimeRange(
    const signallab::core::Signal& signal,
    double firstTime,
    double lastTime)
{
    const double duration = signal.durationSeconds();
    if (!std::isfinite(firstTime) || !std::isfinite(lastTime)
        || !std::isfinite(duration) || firstTime >= lastTime
        || lastTime <= 0.0 || firstTime >= duration) {
        return 0U;
    }
    const long double overlap = std::max(
        std::min(lastTime, duration) - std::max(firstTime, 0.0),
        0.0);
    const long double estimate = overlap
        * static_cast<long double>(signal.sampleRate());
    return estimate >= static_cast<long double>(signal.sampleCount())
        ? signal.sampleCount()
        : static_cast<std::size_t>(std::floor(estimate + 0.5L));
}

}  // namespace

MainWindow::MainWindow(
    signallab::app::ImportService& importService,
    signallab::app::AnalysisService& analysisService,
    signallab::app::BackgroundFileService& backgroundFileService,
    signallab::app::BackgroundAnalysisService& backgroundAnalysisService,
    signallab::task::Scheduler& scheduler,
    signallab::app::BackgroundScriptService* backgroundScriptService,
    QWidget* parent)
    : QMainWindow(parent),
      importService_(importService),
      analysisService_(analysisService),
      backgroundFileService_(backgroundFileService),
      backgroundAnalysisService_(backgroundAnalysisService),
      scheduler_(scheduler),
      backgroundScriptService_(backgroundScriptService),
      signalModel_(new signallab::gui::SignalListModel(this)),
      taskModel_(new signallab::gui::TaskListModel(this)),
      analysisHistoryModel_(
          new signallab::gui::AnalysisHistoryModel(this)),
      resultModel_(new signallab::gui::AnalysisResultModel(this)),
      scriptHistoryModel_(
          new signallab::gui::ScriptHistoryModel(this)),
      signalListView_(nullptr),
      deleteSignalButton_(nullptr),
      nameValue_(nullptr),
      sampleRateValue_(nullptr),
      sampleCountValue_(nullptr),
      durationValue_(nullptr),
      firTypeCombo_(nullptr),
      firCutoffSpin_(nullptr),
      firTapCountSpin_(nullptr),
      firValidationLabel_(nullptr),
      applyFirButton_(nullptr),
      validationLabel_(nullptr),
      windowTypeCombo_(nullptr),
      minimumPeakRatioSpin_(nullptr),
      minimumPeakDistanceSpin_(nullptr),
      maximumPeakCountSpin_(nullptr),
      metricGuardBinsSpin_(nullptr),
      thdHarmonicsSpin_(nullptr),
      analyzeButton_(nullptr),
      resultVersionLabel_(nullptr),
      resultParametersLabel_(nullptr),
      resultView_(nullptr),
      workspaceTabs_(nullptr),
      timeChartView_(nullptr),
      timeSeries_(nullptr),
      comparisonTimeSeries_(nullptr),
      timeXAxis_(nullptr),
      timeYAxis_(nullptr),
      comparisonSignalCombo_(nullptr),
      timeAxisScrollBar_(nullptr),
      timeAxisRangeLabel_(nullptr),
      timeCursorLabel_(nullptr),
      timeSelectionLabel_(nullptr),
      timeRedrawTimer_(new QTimer(this)),
      spectrumChartView_(nullptr),
      spectrumSeries_(nullptr),
      spectrumXAxis_(nullptr),
      spectrumYAxis_(nullptr),
      spectrumCursorLabel_(nullptr),
      spectrumSelectionLabel_(nullptr),
      spectrumRedrawTimer_(new QTimer(this)),
      analysisHistoryView_(nullptr),
      analysisHistoryDetails_(nullptr),
      deleteAnalysisHistoryButton_(nullptr),
      taskListView_(nullptr),
      taskDetails_(nullptr),
      cancelTaskButton_(nullptr),
      retryTaskButton_(nullptr),
      deleteTaskButton_(nullptr),
      scriptEditor_(nullptr),
      runScriptButton_(nullptr),
      cancelScriptButton_(nullptr),
      scriptStatusLabel_(nullptr),
      scriptDiagnosticView_(nullptr),
      scriptDiagnosticModel_(nullptr),
      scriptOutput_(nullptr),
      scriptPlotTabs_(nullptr),
      scriptHistoryView_(nullptr),
      scriptHistoryDetails_(nullptr),
      loadScriptHistoryButton_(nullptr)
{
    setWindowTitle(QStringLiteral("SignalLab %1")
        .arg(QStringLiteral(SIGNALLAB_VERSION_STRING)));
    resize(1440, 860);
    setMinimumSize(980, 640);

    auto* centralWidget = new QWidget(this);
    auto* centralLayout = new QVBoxLayout(centralWidget);
    centralLayout->setContentsMargins(6, 6, 6, 6);
    auto* mainSplitter = new QSplitter(Qt::Horizontal, centralWidget);
    mainSplitter->setObjectName(QStringLiteral("mainWorkspaceSplitter"));
    centralLayout->addWidget(mainSplitter);

    auto* projectPanel = new QWidget(mainSplitter);
    auto* projectLayout = new QVBoxLayout(projectPanel);
    auto* projectTitle = new QLabel(QStringLiteral("项目信号"), projectPanel);
    QFont projectFont = projectTitle->font();
    projectFont.setBold(true);
    projectTitle->setFont(projectFont);
    projectLayout->addWidget(projectTitle);
    signalListView_ = new QListView(projectPanel);
    signalListView_->setObjectName(QStringLiteral("signalListView"));
    signalListView_->setModel(signalModel_);
    signalListView_->setSelectionMode(
        QAbstractItemView::SingleSelection);
    signalListView_->setAccessibleName(QStringLiteral("项目信号列表"));
    projectLayout->addWidget(signalListView_, 1);
    deleteSignalButton_ = new QPushButton(
        QStringLiteral("删除所选信号"), projectPanel);
    deleteSignalButton_->setObjectName(
        QStringLiteral("deleteSignalButton"));
    deleteSignalButton_->setEnabled(false);
    projectLayout->addWidget(deleteSignalButton_);
    auto* importCsvButton = new QPushButton(
        QStringLiteral("导入 CSV"), projectPanel);
    importCsvButton->setObjectName(QStringLiteral("importCsvButton"));
    auto* importWavButton = new QPushButton(
        QStringLiteral("导入 WAV"), projectPanel);
    importWavButton->setObjectName(QStringLiteral("importWavButton"));
    projectLayout->addWidget(importCsvButton);
    projectLayout->addWidget(importWavButton);
    auto* captureButton = new QPushButton(QStringLiteral("麦克风实时采集"), projectPanel);
    captureButton->setObjectName(QStringLiteral("openAudioCaptureButton"));
    projectLayout->addWidget(captureButton);
    projectPanel->setMinimumWidth(210);
    mainSplitter->addWidget(projectPanel);

    workspaceTabs_ = new QTabWidget(mainSplitter);
    workspaceTabs_->setObjectName(QStringLiteral("workspaceTabs"));

    auto* timePage = new QWidget(workspaceTabs_);
    auto* timeLayout = new QVBoxLayout(timePage);
    auto* timeTools = new QHBoxLayout;
    auto* resetTimeButton = new QPushButton(
        QStringLiteral("复位视图"), timePage);
    auto* zoomInTimeButton = new QPushButton(
        QStringLiteral("横向放大"), timePage);
    auto* zoomOutTimeButton = new QPushButton(
        QStringLiteral("横向缩小"), timePage);
    auto* exportTimeButton = new QPushButton(
        QStringLiteral("导出 PNG"), timePage);
    auto* comparisonSignalLabel = new QLabel(
        QStringLiteral("对比信号："), timePage);
    comparisonSignalCombo_ = new QComboBox(timePage);
    comparisonSignalCombo_->setObjectName(
        QStringLiteral("comparisonSignalCombo"));
    comparisonSignalCombo_->setMinimumContentsLength(18);
    comparisonSignalCombo_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    comparisonSignalCombo_->setAccessibleName(
        QStringLiteral("时域对比信号"));
    comparisonSignalCombo_->setToolTip(QStringLiteral(
        "选择第二个信号，以相对 t=0 的共同秒轴叠加显示；不改变原始采样。"));
    resetTimeButton->setObjectName(QStringLiteral("timeResetButton"));
    zoomInTimeButton->setObjectName(QStringLiteral("timeZoomInButton"));
    zoomOutTimeButton->setObjectName(QStringLiteral("timeZoomOutButton"));
    timeCursorLabel_ = new QLabel(
        QStringLiteral("游标：移入图表查看精确样本"), timePage);
    timeCursorLabel_->setObjectName(QStringLiteral("timeCursorLabel"));
    timeSelectionLabel_ = new QLabel(
        QStringLiteral("局部选择：按住 Shift 拖动"), timePage);
    timeSelectionLabel_->setObjectName(
        QStringLiteral("timeSelectionLabel"));
    timeTools->addWidget(resetTimeButton);
    timeTools->addWidget(zoomInTimeButton);
    timeTools->addWidget(zoomOutTimeButton);
    timeTools->addSpacing(8);
    timeTools->addWidget(comparisonSignalLabel);
    timeTools->addWidget(comparisonSignalCombo_, 1);
    timeTools->addWidget(exportTimeButton);
    timeLayout->addLayout(timeTools);
    auto* timeReadout = new QHBoxLayout;
    timeReadout->addWidget(timeCursorLabel_, 1);
    timeReadout->addWidget(timeSelectionLabel_);
    timeLayout->addLayout(timeReadout);
    auto* timeChart = new QChart;
    timeSeries_ = new QLineSeries(timeChart);
    comparisonTimeSeries_ = new QLineSeries(timeChart);
    timeXAxis_ = new QValueAxis(timeChart);
    timeYAxis_ = new QValueAxis(timeChart);
    timeXAxis_->setTitleText(QStringLiteral("时间 (s)"));
    timeYAxis_->setTitleText(QStringLiteral("幅值"));
    timeChart->addSeries(timeSeries_);
    timeChart->addSeries(comparisonTimeSeries_);
    QPen comparisonPen = comparisonTimeSeries_->pen();
    comparisonPen.setStyle(Qt::DashLine);
    comparisonPen.setWidthF(2.0);
    comparisonTimeSeries_->setPen(comparisonPen);
    timeChart->addAxis(timeXAxis_, Qt::AlignBottom);
    timeChart->addAxis(timeYAxis_, Qt::AlignLeft);
    timeSeries_->attachAxis(timeXAxis_);
    timeSeries_->attachAxis(timeYAxis_);
    comparisonTimeSeries_->attachAxis(timeXAxis_);
    comparisonTimeSeries_->attachAxis(timeYAxis_);
    timeChart->legend()->hide();
    timeChart->setTitle(QStringLiteral("请选择一个信号"));
    timeChartView_ = new InteractiveChartView(timeChart, timePage);
    timeChartView_->setObjectName(QStringLiteral("timeChartView"));
    timeChartView_->setAccessibleName(QStringLiteral("时域波形图"));
    timeChartView_->setZoomOrientations(Qt::Horizontal);
    timeLayout->addWidget(timeChartView_, 1);
    auto* timeAxisControls = new QHBoxLayout;
    auto* timeAxisLabel = new QLabel(QStringLiteral("时间轴："), timePage);
    timeAxisScrollBar_ = new QScrollBar(Qt::Horizontal, timePage);
    timeAxisScrollBar_->setObjectName(
        QStringLiteral("timeAxisScrollBar"));
    timeAxisScrollBar_->setAccessibleName(
        QStringLiteral("波形时间轴滑动条"));
    timeAxisScrollBar_->setEnabled(false);
    timeAxisScrollBar_->setRange(0, 0);
    timeAxisRangeLabel_ = new QLabel(
        QStringLiteral("无可用波形"), timePage);
    timeAxisRangeLabel_->setObjectName(
        QStringLiteral("timeAxisRangeLabel"));
    timeAxisRangeLabel_->setMinimumWidth(190);
    timeAxisControls->addWidget(timeAxisLabel);
    timeAxisControls->addWidget(timeAxisScrollBar_, 1);
    timeAxisControls->addWidget(timeAxisRangeLabel_);
    timeLayout->addLayout(timeAxisControls);
    workspaceTabs_->addTab(timePage, QStringLiteral("时域"));

    auto* spectrumPage = new QWidget(workspaceTabs_);
    auto* spectrumLayout = new QVBoxLayout(spectrumPage);
    auto* spectrumTools = new QHBoxLayout;
    auto* resetSpectrumButton = new QPushButton(
        QStringLiteral("复位视图"), spectrumPage);
    auto* exportSpectrumButton = new QPushButton(
        QStringLiteral("导出 PNG"), spectrumPage);
    spectrumCursorLabel_ = new QLabel(
        QStringLiteral("游标：完成分析后查看精确频点"), spectrumPage);
    spectrumCursorLabel_->setObjectName(
        QStringLiteral("spectrumCursorLabel"));
    spectrumSelectionLabel_ = new QLabel(
        QStringLiteral("局部选择：按住 Shift 拖动"), spectrumPage);
    spectrumSelectionLabel_->setObjectName(
        QStringLiteral("spectrumSelectionLabel"));
    spectrumTools->addWidget(resetSpectrumButton);
    spectrumTools->addWidget(exportSpectrumButton);
    spectrumTools->addWidget(spectrumCursorLabel_, 1);
    spectrumTools->addWidget(spectrumSelectionLabel_);
    spectrumLayout->addLayout(spectrumTools);
    auto* spectrumChart = new QChart;
    spectrumSeries_ = new QLineSeries(spectrumChart);
    spectrumXAxis_ = new QValueAxis(spectrumChart);
    spectrumYAxis_ = new QValueAxis(spectrumChart);
    spectrumXAxis_->setTitleText(QStringLiteral("频率 (Hz)"));
    spectrumYAxis_->setTitleText(QStringLiteral("幅值"));
    spectrumChart->addSeries(spectrumSeries_);
    spectrumChart->addAxis(spectrumXAxis_, Qt::AlignBottom);
    spectrumChart->addAxis(spectrumYAxis_, Qt::AlignLeft);
    spectrumSeries_->attachAxis(spectrumXAxis_);
    spectrumSeries_->attachAxis(spectrumYAxis_);
    spectrumChart->legend()->hide();
    spectrumChart->setTitle(QStringLiteral("完成分析后显示单边幅度谱"));
    spectrumChartView_ = new InteractiveChartView(
        spectrumChart, spectrumPage);
    spectrumChartView_->setObjectName(QStringLiteral("spectrumChartView"));
    spectrumChartView_->setAccessibleName(QStringLiteral("单边频谱图"));
    spectrumLayout->addWidget(spectrumChartView_, 1);
    workspaceTabs_->addTab(spectrumPage, QStringLiteral("频域"));

    auto* taskPage = new QWidget(workspaceTabs_);
    auto* taskLayout = new QVBoxLayout(taskPage);
    taskListView_ = new QListView(taskPage);
    taskListView_->setObjectName(QStringLiteral("taskListView"));
    taskListView_->setModel(taskModel_);
    taskListView_->setSelectionMode(QAbstractItemView::SingleSelection);
    taskListView_->setAccessibleName(QStringLiteral("后台任务列表"));
    taskLayout->addWidget(taskListView_, 1);
    taskDetails_ = new QTextEdit(taskPage);
    taskDetails_->setObjectName(QStringLiteral("taskDetails"));
    taskDetails_->setReadOnly(true);
    taskDetails_->setMaximumHeight(150);
    taskDetails_->setPlaceholderText(
        QStringLiteral("选择任务可查看状态、错误原因和可执行建议。"));
    taskLayout->addWidget(taskDetails_);
    auto* taskButtons = new QHBoxLayout;
    cancelTaskButton_ = new QPushButton(
        QStringLiteral("取消所选任务"), taskPage);
    retryTaskButton_ = new QPushButton(
        QStringLiteral("重试所选任务"), taskPage);
    deleteTaskButton_ = new QPushButton(
        QStringLiteral("删除所选任务"), taskPage);
    cancelTaskButton_->setObjectName(QStringLiteral("cancelTaskButton"));
    retryTaskButton_->setObjectName(QStringLiteral("retryTaskButton"));
    deleteTaskButton_->setObjectName(QStringLiteral("deleteTaskButton"));
    cancelTaskButton_->setEnabled(false);
    retryTaskButton_->setEnabled(false);
    deleteTaskButton_->setEnabled(false);
    taskButtons->addWidget(cancelTaskButton_);
    taskButtons->addWidget(retryTaskButton_);
    taskButtons->addWidget(deleteTaskButton_);
    taskButtons->addStretch();
    taskLayout->addLayout(taskButtons);
    workspaceTabs_->addTab(taskPage, QStringLiteral("任务中心"));

    auto* historyPage = new QWidget(workspaceTabs_);
    auto* historyLayout = new QHBoxLayout(historyPage);
    auto* historyListPanel = new QWidget(historyPage);
    auto* historyListLayout = new QVBoxLayout(historyListPanel);
    historyListLayout->setContentsMargins(0, 0, 0, 0);
    analysisHistoryView_ = new QListView(historyListPanel);
    analysisHistoryView_->setObjectName(
        QStringLiteral("analysisHistoryView"));
    analysisHistoryView_->setModel(analysisHistoryModel_);
    analysisHistoryView_->setMinimumWidth(300);
    deleteAnalysisHistoryButton_ = new QPushButton(
        QStringLiteral("删除所选分析记录"), historyListPanel);
    deleteAnalysisHistoryButton_->setObjectName(
        QStringLiteral("deleteAnalysisHistoryButton"));
    deleteAnalysisHistoryButton_->setEnabled(false);
    historyListLayout->addWidget(analysisHistoryView_, 1);
    historyListLayout->addWidget(deleteAnalysisHistoryButton_);
    analysisHistoryDetails_ = new QTextEdit(historyPage);
    analysisHistoryDetails_->setObjectName(
        QStringLiteral("analysisHistoryDetails"));
    analysisHistoryDetails_->setReadOnly(true);
    analysisHistoryDetails_->setPlaceholderText(
        QStringLiteral("选择分析记录查看参数、版本、哈希和特征结果。"));
    historyLayout->addWidget(historyListPanel);
    historyLayout->addWidget(analysisHistoryDetails_, 1);
    workspaceTabs_->addTab(historyPage, QStringLiteral("分析历史"));

    auto* scriptPage = new QWidget(workspaceTabs_);
    auto* scriptLayout = new QVBoxLayout(scriptPage);
    auto* scriptButtons = new QHBoxLayout;
    runScriptButton_ = new QPushButton(
        QStringLiteral("运行脚本"), scriptPage);
    cancelScriptButton_ = new QPushButton(
        QStringLiteral("取消运行"), scriptPage);
    loadScriptHistoryButton_ = new QPushButton(
        QStringLiteral("载入所选历史源码"), scriptPage);
    runScriptButton_->setObjectName(QStringLiteral("runScriptButton"));
    cancelScriptButton_->setObjectName(
        QStringLiteral("cancelScriptButton"));
    loadScriptHistoryButton_->setObjectName(
        QStringLiteral("loadScriptHistoryButton"));
    scriptStatusLabel_ = new QLabel(
        backgroundScriptService_ == nullptr
            ? QStringLiteral("脚本服务未装配")
            : QStringLiteral("就绪：脚本在后台计算通道运行"),
        scriptPage);
    scriptStatusLabel_->setObjectName(
        QStringLiteral("scriptStatusLabel"));
    scriptButtons->addWidget(runScriptButton_);
    scriptButtons->addWidget(cancelScriptButton_);
    scriptButtons->addWidget(loadScriptHistoryButton_);
    scriptButtons->addWidget(scriptStatusLabel_, 1);
    scriptLayout->addLayout(scriptButtons);
    auto* scriptSplitter = new QSplitter(Qt::Horizontal, scriptPage);
    auto* editorPanel = new QWidget(scriptSplitter);
    auto* editorLayout = new QVBoxLayout(editorPanel);
    editorLayout->addWidget(new QLabel(
        QStringLiteral("SignalScript 编辑器"), editorPanel));
    scriptEditor_ = new QPlainTextEdit(editorPanel);
    scriptEditor_->setObjectName(QStringLiteral("scriptEditor"));
    scriptEditor_->setAccessibleName(
        QStringLiteral("SignalScript 源码编辑器"));
    scriptEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    scriptEditor_->setPlainText(QStringLiteral(
        "# 可重复运行的分析演示\n"
        "load \"data/samples/example_signal.csv\" as raw;\n"
        "spec = raw |> fft(window=\"hann\");\n"
        "result = features(spec);\n"
        "plot raw as \"Imported waveform\";\n"
        "plot spec as \"One-sided amplitude spectrum\";\n"));
    editorLayout->addWidget(scriptEditor_, 1);
    editorLayout->addWidget(new QLabel(
        QStringLiteral("诊断（单击可跳转）"), editorPanel));
    scriptDiagnosticModel_ = new QStandardItemModel(this);
    scriptDiagnosticModel_->setHorizontalHeaderLabels(
        {QStringLiteral("级别"), QStringLiteral("代码"),
         QStringLiteral("位置"), QStringLiteral("消息")});
    scriptDiagnosticView_ = new QListView(editorPanel);
    scriptDiagnosticView_->setObjectName(
        QStringLiteral("scriptDiagnosticView"));
    scriptDiagnosticView_->setModel(scriptDiagnosticModel_);
    scriptDiagnosticView_->setMaximumHeight(150);
    scriptDiagnosticView_->setAccessibleName(
        QStringLiteral("脚本诊断列表"));
    editorLayout->addWidget(scriptDiagnosticView_);

    auto* scriptResultTabs = new QTabWidget(scriptSplitter);
    scriptResultTabs->setObjectName(QStringLiteral("scriptResultTabs"));
    scriptOutput_ = new QPlainTextEdit(scriptResultTabs);
    scriptOutput_->setObjectName(QStringLiteral("scriptOutput"));
    scriptOutput_->setReadOnly(true);
    scriptOutput_->setPlaceholderText(
        QStringLiteral("执行状态、变量和导出文件会显示在这里。"));
    scriptResultTabs->addTab(scriptOutput_, QStringLiteral("执行结果"));
    scriptPlotTabs_ = new QTabWidget(scriptResultTabs);
    scriptPlotTabs_->setObjectName(QStringLiteral("scriptPlotTabs"));
    scriptResultTabs->addTab(scriptPlotTabs_, QStringLiteral("绘图"));
    auto* scriptHistoryPage = new QWidget(scriptResultTabs);
    auto* scriptHistoryLayout = new QVBoxLayout(scriptHistoryPage);
    scriptHistoryView_ = new QListView(scriptHistoryPage);
    scriptHistoryView_->setObjectName(
        QStringLiteral("scriptHistoryView"));
    scriptHistoryView_->setModel(scriptHistoryModel_);
    scriptHistoryView_->setAccessibleName(
        QStringLiteral("脚本执行历史列表"));
    scriptHistoryDetails_ = new QTextEdit(scriptHistoryPage);
    scriptHistoryDetails_->setObjectName(
        QStringLiteral("scriptHistoryDetails"));
    scriptHistoryDetails_->setReadOnly(true);
    scriptHistoryLayout->addWidget(scriptHistoryView_, 1);
    scriptHistoryLayout->addWidget(scriptHistoryDetails_, 1);
    scriptResultTabs->addTab(
        scriptHistoryPage, QStringLiteral("执行历史"));
    scriptSplitter->addWidget(editorPanel);
    scriptSplitter->addWidget(scriptResultTabs);
    scriptSplitter->setStretchFactor(0, 1);
    scriptSplitter->setStretchFactor(1, 1);
    scriptLayout->addWidget(scriptSplitter, 1);
    workspaceTabs_->addTab(scriptPage, QStringLiteral("SignalScript"));
    audioCaptureWidget_ = new signallab::gui::AudioCaptureWidget(
        std::make_unique<signallab::io::WindowsAudioInput>(), workspaceTabs_);
    workspaceTabs_->addTab(audioCaptureWidget_, QStringLiteral("实时采集"));
    audioCaptureWidget_->setImportCallback([this](const std::filesystem::path& path) {
        signallab::repository::WavImportOptions options;
        options.metadata.name = QString::fromStdWString(path.stem().wstring()).toStdString();
        options.metadata.unit = "FS";
        options.metadata.source = "Microphone capture";
        const auto submitted = backgroundFileService_.importWav(path, std::move(options));
        if (!submitted) {
            showFailure(QStringLiteral("录音已保存，但导入失败"),
                QString::fromStdString(submitted.error().message),
                QStringLiteral("录音文件已保留，可以通过“导入 WAV”重新导入。"));
            return;
        }
        refreshTaskList(submitted.value());
        statusBar()->showMessage(QStringLiteral("录音已保存，正在后台导入项目信号。"));
    });
    mainSplitter->addWidget(workspaceTabs_);

    auto* detailsScroll = new QScrollArea(mainSplitter);
    detailsScroll->setObjectName(QStringLiteral("analysisSidePanel"));
    detailsScroll->setWidgetResizable(true);
    auto* detailsPanel = new QWidget(detailsScroll);
    auto* detailsLayout = new QVBoxLayout(detailsPanel);
    auto* signalGroup = new QGroupBox(
        QStringLiteral("信号信息"), detailsPanel);
    auto* signalForm = new QFormLayout(signalGroup);
    nameValue_ = new QLabel(QStringLiteral("—"), signalGroup);
    sampleRateValue_ = new QLabel(QStringLiteral("—"), signalGroup);
    sampleCountValue_ = new QLabel(QStringLiteral("—"), signalGroup);
    durationValue_ = new QLabel(QStringLiteral("—"), signalGroup);
    signalForm->addRow(QStringLiteral("名称："), nameValue_);
    signalForm->addRow(QStringLiteral("采样率："), sampleRateValue_);
    signalForm->addRow(QStringLiteral("样本数："), sampleCountValue_);
    signalForm->addRow(QStringLiteral("持续时间："), durationValue_);
    detailsLayout->addWidget(signalGroup);

    auto* firGroup = new QGroupBox(
        QStringLiteral("FIR 滤波器"), detailsPanel);
    firGroup->setObjectName(QStringLiteral("firFilterParameters"));
    auto* firForm = new QFormLayout(firGroup);
    firTypeCombo_ = new QComboBox(firGroup);
    firTypeCombo_->setObjectName(QStringLiteral("firTypeCombo"));
    firTypeCombo_->addItem(
        QStringLiteral("低通"),
        static_cast<int>(signallab::app::FirFilterType::LowPass));
    firTypeCombo_->addItem(
        QStringLiteral("高通"),
        static_cast<int>(signallab::app::FirFilterType::HighPass));
    firCutoffSpin_ = new QDoubleSpinBox(firGroup);
    firCutoffSpin_->setObjectName(QStringLiteral("firCutoffSpin"));
    firCutoffSpin_->setRange(0.001, 500'000'000.0);
    firCutoffSpin_->setDecimals(3);
    firCutoffSpin_->setValue(100.0);
    firCutoffSpin_->setSuffix(QStringLiteral(" Hz"));
    firTapCountSpin_ = new QSpinBox(firGroup);
    firTapCountSpin_->setObjectName(QStringLiteral("firTapCountSpin"));
    firTapCountSpin_->setRange(
        3, static_cast<int>(signallab::app::kMaximumFirTapCount));
    firTapCountSpin_->setSingleStep(2);
    firTapCountSpin_->setValue(63);
    firValidationLabel_ = new QLabel(firGroup);
    firValidationLabel_->setObjectName(
        QStringLiteral("firValidationLabel"));
    firValidationLabel_->setWordWrap(true);
    applyFirButton_ = new QPushButton(
        QStringLiteral("后台生成滤波信号"), firGroup);
    applyFirButton_->setObjectName(QStringLiteral("applyFirButton"));
    firForm->addRow(QStringLiteral("类型："), firTypeCombo_);
    firForm->addRow(QStringLiteral("截止频率："), firCutoffSpin_);
    firForm->addRow(QStringLiteral("抽头数："), firTapCountSpin_);
    firForm->addRow(QStringLiteral("校验："), firValidationLabel_);
    firForm->addRow(applyFirButton_);
    detailsLayout->addWidget(firGroup);

    auto* parameterGroup = new QGroupBox(
        QStringLiteral("分析参数"), detailsPanel);
    parameterGroup->setObjectName(QStringLiteral("analysisParameters"));
    auto* parameterForm = new QFormLayout(parameterGroup);
    windowTypeCombo_ = new QComboBox(parameterGroup);
    windowTypeCombo_->setObjectName(QStringLiteral("windowTypeCombo"));
    windowTypeCombo_->addItem(
        QStringLiteral("Hann 窗"),
        static_cast<int>(signallab::algorithm::WindowType::Hann));
    windowTypeCombo_->addItem(
        QStringLiteral("Hamming 窗"),
        static_cast<int>(signallab::algorithm::WindowType::Hamming));
    windowTypeCombo_->addItem(
        QStringLiteral("矩形窗"),
        static_cast<int>(signallab::algorithm::WindowType::Rectangular));
    minimumPeakRatioSpin_ = new QDoubleSpinBox(parameterGroup);
    minimumPeakRatioSpin_->setObjectName(
        QStringLiteral("minimumPeakRatioSpin"));
    minimumPeakRatioSpin_->setRange(0.0, 1.0);
    minimumPeakRatioSpin_->setDecimals(4);
    minimumPeakRatioSpin_->setSingleStep(0.005);
    minimumPeakRatioSpin_->setValue(0.01);
    minimumPeakDistanceSpin_ = new QDoubleSpinBox(parameterGroup);
    minimumPeakDistanceSpin_->setObjectName(
        QStringLiteral("minimumPeakDistanceSpin"));
    minimumPeakDistanceSpin_->setRange(0.0, 500'000'000.0);
    minimumPeakDistanceSpin_->setDecimals(3);
    minimumPeakDistanceSpin_->setValue(0.0);
    minimumPeakDistanceSpin_->setSuffix(QStringLiteral(" Hz"));
    maximumPeakCountSpin_ = new QSpinBox(parameterGroup);
    maximumPeakCountSpin_->setObjectName(
        QStringLiteral("maximumPeakCountSpin"));
    maximumPeakCountSpin_->setRange(1, 100'000);
    maximumPeakCountSpin_->setValue(10);
    metricGuardBinsSpin_ = new QSpinBox(parameterGroup);
    metricGuardBinsSpin_->setObjectName(
        QStringLiteral("metricGuardBinsSpin"));
    metricGuardBinsSpin_->setRange(0, 1'000'000);
    metricGuardBinsSpin_->setValue(1);
    thdHarmonicsSpin_ = new QSpinBox(parameterGroup);
    thdHarmonicsSpin_->setObjectName(
        QStringLiteral("thdHarmonicsSpin"));
    thdHarmonicsSpin_->setRange(2, 1'000);
    thdHarmonicsSpin_->setValue(5);
    parameterForm->addRow(QStringLiteral("窗函数："), windowTypeCombo_);
    parameterForm->addRow(
        QStringLiteral("最小峰值比："), minimumPeakRatioSpin_);
    parameterForm->addRow(
        QStringLiteral("最小峰间距："), minimumPeakDistanceSpin_);
    parameterForm->addRow(
        QStringLiteral("最大峰数："), maximumPeakCountSpin_);
    parameterForm->addRow(
        QStringLiteral("保护频点："), metricGuardBinsSpin_);
    parameterForm->addRow(
        QStringLiteral("THD 谐波数："), thdHarmonicsSpin_);
    validationLabel_ = new QLabel(parameterGroup);
    validationLabel_->setObjectName(QStringLiteral("analysisValidation"));
    validationLabel_->setWordWrap(true);
    parameterForm->addRow(QStringLiteral("校验："), validationLabel_);
    analyzeButton_ = new QPushButton(
        QStringLiteral("后台分析所选信号"), parameterGroup);
    analyzeButton_->setObjectName(QStringLiteral("analyzeButton"));
    parameterForm->addRow(analyzeButton_);
    detailsLayout->addWidget(parameterGroup);

    auto* resultGroup = new QGroupBox(
        QStringLiteral("分析结果"), detailsPanel);
    auto* resultLayout = new QVBoxLayout(resultGroup);
    resultVersionLabel_ = new QLabel(
        QStringLiteral("算法版本：—"), resultGroup);
    resultVersionLabel_->setObjectName(QStringLiteral("resultVersion"));
    resultParametersLabel_ = new QLabel(
        QStringLiteral("参数：—"), resultGroup);
    resultParametersLabel_->setObjectName(
        QStringLiteral("resultParameters"));
    resultParametersLabel_->setWordWrap(true);
    resultView_ = new QTableView(resultGroup);
    resultView_->setObjectName(QStringLiteral("analysisResultView"));
    resultView_->setModel(resultModel_);
    resultView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    resultView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    resultView_->horizontalHeader()->setStretchLastSection(true);
    resultView_->verticalHeader()->setVisible(false);
    resultView_->setAccessibleName(QStringLiteral("分析结果表"));
    resultLayout->addWidget(resultVersionLabel_);
    resultLayout->addWidget(resultParametersLabel_);
    resultLayout->addWidget(resultView_);
    detailsLayout->addWidget(resultGroup, 1);
    detailsScroll->setWidget(detailsPanel);
    detailsScroll->setMinimumWidth(330);
    mainSplitter->addWidget(detailsScroll);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setStretchFactor(2, 0);
    mainSplitter->setSizes({220, 880, 340});
    setCentralWidget(centralWidget);

    timeRedrawTimer_->setSingleShot(true);
    timeRedrawTimer_->setInterval(80);
    spectrumRedrawTimer_->setSingleShot(true);
    spectrumRedrawTimer_->setInterval(80);

    connect(importCsvButton, &QPushButton::clicked, this, [this]() {
        importCsv();
    });
    connect(importWavButton, &QPushButton::clicked, this, [this]() {
        importWav();
    });
    connect(captureButton, &QPushButton::clicked, this, [this]() {
        workspaceTabs_->setCurrentWidget(audioCaptureWidget_);
    });
    connect(deleteSignalButton_, &QPushButton::clicked, this, [this]() {
        deleteSelectedSignal();
    });
    connect(
        signalListView_->selectionModel(),
        &QItemSelectionModel::currentChanged,
        this,
        [this](const QModelIndex& current, const QModelIndex&) {
            showSignal(current);
        });
    connect(analyzeButton_, &QPushButton::clicked, this, [this]() {
        analyzeSelectedSignal();
    });
    connect(applyFirButton_, &QPushButton::clicked, this, [this]() {
        filterSelectedSignal();
    });
    connect(cancelTaskButton_, &QPushButton::clicked, this, [this]() {
        cancelSelectedTask();
    });
    connect(retryTaskButton_, &QPushButton::clicked, this, [this]() {
        retrySelectedTask();
    });
    connect(deleteTaskButton_, &QPushButton::clicked, this, [this]() {
        deleteSelectedTask();
    });
    connect(
        taskListView_->selectionModel(),
        &QItemSelectionModel::currentChanged,
        this,
        [this](const QModelIndex& current, const QModelIndex&) {
            showTaskDetails(current);
        });
    connect(
        analysisHistoryView_->selectionModel(),
        &QItemSelectionModel::currentChanged,
        this,
        [this](const QModelIndex& current, const QModelIndex&) {
            showAnalysisHistory(current);
        });
    connect(
        deleteAnalysisHistoryButton_,
        &QPushButton::clicked,
        this,
        [this]() { deleteSelectedAnalysisHistory(); });
    connect(
        comparisonSignalCombo_,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this](int) {
            if (selectedSignal() != nullptr) {
                renderTimeSignal(true);
                workspaceTabs_->setCurrentWidget(
                    timeChartView_->parentWidget());
            }
        });
    connect(resetTimeButton, &QPushButton::clicked, this, [this]() {
        timeChartView_->resetView();
        renderTimeSignal(false);
        updateTimeAxisScrollBar();
    });
    connect(zoomInTimeButton, &QPushButton::clicked, this, [this]() {
        static_cast<void>(timeChartView_->zoomHorizontally(0.5));
    });
    connect(zoomOutTimeButton, &QPushButton::clicked, this, [this]() {
        static_cast<void>(timeChartView_->zoomHorizontally(2.0));
    });
    connect(resetSpectrumButton, &QPushButton::clicked, this, [this]() {
        spectrumChartView_->resetView();
        renderSpectrum(false);
    });
    connect(exportTimeButton, &QPushButton::clicked, this, [this]() {
        exportChart(timeChartView_, QStringLiteral("time-domain"));
    });
    connect(exportSpectrumButton, &QPushButton::clicked, this, [this]() {
        exportChart(spectrumChartView_, QStringLiteral("spectrum"));
    });
    connect(
        timeAxisScrollBar_,
        &QScrollBar::valueChanged,
        this,
        [this](int value) {
            const double fullDuration = timeDomainMaximum();
            const double visibleSpan = std::clamp(
                timeXAxis_->max() - timeXAxis_->min(),
                0.0,
                fullDuration);
            const double movableDuration = fullDuration - visibleSpan;
            const int maximum = timeAxisScrollBar_->maximum();
            if (!std::isfinite(fullDuration) || movableDuration <= 0.0
                || maximum <= 0) {
                return;
            }
            const long double ratio = value <= 0
                ? 0.0L
                : value >= maximum
                    ? 1.0L
                    : static_cast<long double>(value)
                        / static_cast<long double>(maximum);
            const double first = static_cast<double>(
                ratio * static_cast<long double>(movableDuration));
            timeXAxis_->setRange(first, first + visibleSpan);
        });
    connect(
        timeAxisScrollBar_,
        &QScrollBar::sliderReleased,
        this,
        [this]() {
            timeRedrawTimer_->stop();
            renderTimeSignal(false);
        });
    connect(timeRedrawTimer_, &QTimer::timeout, this, [this]() {
        renderTimeSignal(false);
    });
    connect(spectrumRedrawTimer_, &QTimer::timeout, this, [this]() {
        renderSpectrum(false);
    });
    connect(
        timeXAxis_,
        &QValueAxis::rangeChanged,
        this,
        [this](double minimum, double maximum) {
            const double fullDuration = timeDomainMaximum();
            if (std::isfinite(fullDuration) && fullDuration > 0.0) {
                double span = maximum - minimum;
                double first = minimum;
                bool needsNormalization = false;
                if (!std::isfinite(minimum) || !std::isfinite(maximum)
                    || !std::isfinite(span) || span <= 0.0
                    || span >= fullDuration) {
                    span = fullDuration;
                    first = 0.0;
                    needsNormalization = true;
                } else if (first < 0.0) {
                    first = 0.0;
                    needsNormalization = true;
                } else if (maximum > fullDuration) {
                    first = fullDuration - span;
                    needsNormalization = true;
                }
                if (needsNormalization) {
                    const QSignalBlocker blocker(timeXAxis_);
                    timeXAxis_->setRange(first, first + span);
                }
            }
            updateTimeAxisScrollBar();
            if (!timeRedrawTimer_->isActive()) {
                timeRedrawTimer_->start();
            }
        });
    connect(
        spectrumXAxis_,
        &QValueAxis::rangeChanged,
        this,
        [this](double, double) { spectrumRedrawTimer_->start(); });
    timeChartView_->setResizeSettledCallback([this]() {
        renderTimeSignal(false);
    });
    spectrumChartView_->setResizeSettledCallback([this]() {
        renderSpectrum(false);
    });
    timeChartView_->setCursorCallback([this](QPointF point, bool visible) {
        const auto* entry = selectedSignal();
        if (!visible || entry == nullptr) {
            timeCursorLabel_->setText(
                QStringLiteral("游标：移入图表查看精确样本"));
            return;
        }
        const auto readout = [point](
                                 const signallab::app::SignalEntry& item) {
            const auto index = nearestSampleAtTime(item.signal, point.x());
            if (!index.has_value()) {
                return QStringLiteral("%1=超出范围")
                    .arg(signalName(item));
            }
            return QStringLiteral("%1 #%2=%3")
                .arg(signalName(item))
                .arg(static_cast<qulonglong>(*index))
                .arg(item.signal.samples()[*index], 0, 'g', 9);
        };
        QString text = QStringLiteral("游标：t=%1 s；%2")
            .arg(point.x(), 0, 'g', 9)
            .arg(readout(*entry));
        if (const auto* comparison = comparisonSignal()) {
            text += QStringLiteral("；%1").arg(readout(*comparison));
        }
        timeCursorLabel_->setText(text);
    });
    timeChartView_->setSelectionCallback(
        [this](double first, double last) {
            const auto* entry = selectedSignal();
            if (entry == nullptr) {
                return;
            }
            QString counts = QStringLiteral("%1 约 %2 点")
                .arg(signalName(*entry))
                .arg(static_cast<qulonglong>(sampleCountInTimeRange(
                    entry->signal, first, last)));
            if (const auto* comparison = comparisonSignal()) {
                counts += QStringLiteral("；%1 约 %2 点")
                    .arg(signalName(*comparison))
                    .arg(static_cast<qulonglong>(sampleCountInTimeRange(
                        comparison->signal, first, last)));
            }
            timeSelectionLabel_->setText(
                QStringLiteral("选择：%1–%2 s（%3）")
                    .arg(first, 0, 'g', 7)
                    .arg(last, 0, 'g', 7)
                    .arg(counts));
        });
    timeChartView_->setSelectionClearedCallback([this]() {
        timeSelectionLabel_->setText(
            QStringLiteral("局部选择：按住 Shift 拖动"));
    });
    spectrumChartView_->setCursorCallback(
        [this](QPointF point, bool visible) {
            if (!visible || !displayedAnalysis_
                || displayedAnalysis_->spectrum.frequenciesHz.empty()) {
                spectrumCursorLabel_->setText(
                    QStringLiteral("游标：完成分析后查看精确频点"));
                return;
            }
            const auto& frequencies =
                displayedAnalysis_->spectrum.frequenciesHz;
            const auto& amplitudes =
                displayedAnalysis_->spectrum.amplitudes;
            auto found = std::lower_bound(
                frequencies.begin(), frequencies.end(), point.x());
            std::size_t index = found == frequencies.end()
                ? frequencies.size() - 1U
                : static_cast<std::size_t>(
                      std::distance(frequencies.begin(), found));
            if (index > 0U
                && std::abs(frequencies[index - 1U] - point.x())
                    < std::abs(frequencies[index] - point.x())) {
                --index;
            }
            spectrumCursorLabel_->setText(
                QStringLiteral("游标：f=%1 Hz，频点 #%2，幅值=%3")
                    .arg(frequencies[index], 0, 'g', 9)
                    .arg(static_cast<qulonglong>(index))
                    .arg(amplitudes[index], 0, 'g', 9));
        });
    spectrumChartView_->setSelectionCallback(
        [this](double first, double last) {
            spectrumSelectionLabel_->setText(
                QStringLiteral("选择：%1–%2 Hz")
                    .arg(first, 0, 'g', 7)
                    .arg(last, 0, 'g', 7));
        });
    spectrumChartView_->setSelectionClearedCallback([this]() {
        spectrumSelectionLabel_->setText(
            QStringLiteral("局部选择：按住 Shift 拖动"));
    });

    const auto validate = [this]() { updateAnalysisValidation(); };
    connect(
        windowTypeCombo_,
        &QComboBox::currentIndexChanged,
        this,
        [validate](int) { validate(); });
    connect(
        minimumPeakRatioSpin_,
        qOverload<double>(&QDoubleSpinBox::valueChanged),
        this,
        [validate](double) { validate(); });
    connect(
        minimumPeakDistanceSpin_,
        qOverload<double>(&QDoubleSpinBox::valueChanged),
        this,
        [validate](double) { validate(); });
    connect(
        maximumPeakCountSpin_,
        qOverload<int>(&QSpinBox::valueChanged),
        this,
        [validate](int) { validate(); });
    connect(
        metricGuardBinsSpin_,
        qOverload<int>(&QSpinBox::valueChanged),
        this,
        [validate](int) { validate(); });
    connect(
        thdHarmonicsSpin_,
        qOverload<int>(&QSpinBox::valueChanged),
        this,
        [validate](int) { validate(); });

    const auto validateFir = [this]() { updateFirValidation(); };
    connect(
        firTypeCombo_,
        &QComboBox::currentIndexChanged,
        this,
        [validateFir](int) { validateFir(); });
    connect(
        firCutoffSpin_,
        qOverload<double>(&QDoubleSpinBox::valueChanged),
        this,
        [validateFir](double) { validateFir(); });
    connect(
        firTapCountSpin_,
        qOverload<int>(&QSpinBox::valueChanged),
        this,
        [validateFir](int) { validateFir(); });

    connect(runScriptButton_, &QPushButton::clicked, this, [this]() {
        runScript();
    });
    connect(cancelScriptButton_, &QPushButton::clicked, this, [this]() {
        cancelActiveScript();
    });
    connect(
        loadScriptHistoryButton_,
        &QPushButton::clicked,
        this,
        [this]() { loadSelectedScriptHistory(); });
    connect(
        scriptDiagnosticView_,
        &QListView::clicked,
        this,
        [this](const QModelIndex& index) {
            jumpToScriptDiagnostic(index);
        });
    connect(
        scriptHistoryView_->selectionModel(),
        &QItemSelectionModel::currentChanged,
        this,
        [this](const QModelIndex& current, const QModelIndex&) {
            showScriptHistory(current);
        });
    connect(
        scriptEditor_,
        &QPlainTextEdit::textChanged,
        this,
        [this]() {
            if (!activeScriptTaskId_.has_value()) {
                clearScriptDiagnostics();
                displayedScriptSource_.reset();
            }
        });

    const QPointer<MainWindow> guardedWindow(this);
    backgroundAnalysisService_.setEventNotifier([guardedWindow]() {
        if (!guardedWindow) {
            return;
        }
        QMetaObject::invokeMethod(
            guardedWindow.data(),
            [guardedWindow]() {
                if (guardedWindow) {
                    guardedWindow->processBackgroundAnalysisEvents();
                }
            },
            Qt::QueuedConnection);
    });
    backgroundFileService_.setEventNotifier([guardedWindow]() {
        if (!guardedWindow) {
            return;
        }
        QMetaObject::invokeMethod(
            guardedWindow.data(),
            [guardedWindow]() {
                if (guardedWindow) {
                    guardedWindow->processBackgroundFileEvents();
                }
            },
            Qt::QueuedConnection);
    });
    if (backgroundScriptService_ != nullptr) {
        backgroundScriptService_->setEventNotifier([guardedWindow]() {
            if (!guardedWindow) {
                return;
            }
            QMetaObject::invokeMethod(
                guardedWindow.data(),
                [guardedWindow]() {
                    if (guardedWindow) {
                        guardedWindow->processBackgroundScriptEvents();
                    }
                },
                Qt::QueuedConnection);
        });
    }

    refreshSignalList();
    refreshAnalysisHistory();
    refreshTaskList();
    refreshScriptHistory();
    updateAnalysisValidation();
    updateFirValidation();
    updateScriptButtons();
    if (signalModel_->rowCount() > 0) {
        QTimer::singleShot(0, this, [this]() {
            signalListView_->setCurrentIndex(signalModel_->index(0, 0));
        });
    }
    statusBar()->showMessage(QStringLiteral(
        "文件读取、FFT/FIR、脚本执行和 SQL 写入均在后台运行；"
        "时域图支持双信号同轴对比、横向缩放与时间轴滑动。"));
}

MainWindow::~MainWindow()
{
    if (activeImportPreviewTaskId_.has_value()) {
        static_cast<void>(scheduler_.cancel(*activeImportPreviewTaskId_));
        activeImportPreviewTaskId_.reset();
    }
    importPreviewRevisions_.clear();
    activeImportDialog_ = nullptr;
    if (backgroundScriptService_ != nullptr) {
        backgroundScriptService_->clearEventNotifier();
    }
    backgroundAnalysisService_.clearEventNotifier();
    backgroundFileService_.clearEventNotifier();
}

void MainWindow::setBackupService(std::shared_ptr<signallab::app::BackupService> service)
{
    if (!service || databaseBackupWidget_ != nullptr) {
        return;
    }
    databaseBackupWidget_ = new signallab::gui::DatabaseBackupWidget(
        std::move(service), std::filesystem::path(), workspaceTabs_);
    workspaceTabs_->addTab(databaseBackupWidget_, QStringLiteral("云端备份"));
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (databaseBackupWidget_ != nullptr && !databaseBackupWidget_->requestClose()) {
        event->ignore();
        workspaceTabs_->setCurrentWidget(databaseBackupWidget_);
        return;
    }
    if (!audioCaptureWidget_->requestClose()) {
        event->ignore();
        workspaceTabs_->setCurrentWidget(audioCaptureWidget_);
        return;
    }
    QMainWindow::closeEvent(event);
}

signallab::app::AnalysisOptions MainWindow::analysisOptions() const
{
    signallab::app::AnalysisOptions options;
    options.windowType = static_cast<signallab::algorithm::WindowType>(
        windowTypeCombo_->currentData().toInt());
    options.minimumPeakAmplitudeRatio = minimumPeakRatioSpin_->value();
    options.minimumPeakDistanceHz = minimumPeakDistanceSpin_->value();
    options.maximumPeakCount = static_cast<std::size_t>(
        maximumPeakCountSpin_->value());
    options.metricGuardBins = static_cast<std::size_t>(
        metricGuardBinsSpin_->value());
    options.thdHarmonicCount = static_cast<std::size_t>(
        thdHarmonicsSpin_->value());
    return options;
}

signallab::app::FirFilterOptions MainWindow::firFilterOptions() const
{
    signallab::app::FirFilterOptions options;
    options.type = static_cast<signallab::app::FirFilterType>(
        firTypeCombo_->currentData().toInt());
    options.cutoffHz = firCutoffSpin_->value();
    options.tapCount = static_cast<std::size_t>(
        firTapCountSpin_->value());
    return options;
}

bool MainWindow::isSignalDeletionPending(std::int64_t recordId) const
{
    return std::any_of(
        pendingSignalDeletions_.begin(),
        pendingSignalDeletions_.end(),
        [recordId](const auto& item) {
            return item.second.recordId == recordId;
        });
}

std::optional<std::size_t> MainWindow::selectedSignalIndex() const
{
    const QModelIndex current = signalListView_->currentIndex();
    if (!current.isValid() || current.row() < 0) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(current.row());
    if (index >= importService_.entries().size()) {
        return std::nullopt;
    }
    return index;
}

const signallab::app::SignalEntry* MainWindow::selectedSignal() const
{
    const auto index = selectedSignalIndex();
    return index.has_value()
        ? &importService_.entries()[*index]
        : nullptr;
}

const signallab::app::SignalEntry* MainWindow::comparisonSignal() const
{
    if (comparisonSignalCombo_ == nullptr
        || !comparisonSignalCombo_->currentData().isValid()) {
        return nullptr;
    }
    bool converted = false;
    const auto recordId = comparisonSignalCombo_->currentData()
        .toLongLong(&converted);
    const auto* primary = selectedSignal();
    if (!converted || recordId <= 0
        || (primary != nullptr && primary->recordId == recordId)) {
        return nullptr;
    }
    const auto& entries = importService_.entries();
    const auto found = std::find_if(
        entries.begin(),
        entries.end(),
        [recordId](const signallab::app::SignalEntry& entry) {
            return entry.recordId == recordId;
        });
    return found == entries.end() ? nullptr : &*found;
}

double MainWindow::timeDomainMaximum() const
{
    const auto* primary = selectedSignal();
    if (primary == nullptr) {
        return 0.0;
    }
    double maximum = primary->signal.durationSeconds();
    if (const auto* comparison = comparisonSignal()) {
        maximum = std::max(
            maximum, comparison->signal.durationSeconds());
    }
    return std::isfinite(maximum) && maximum > 0.0 ? maximum : 0.0;
}

void MainWindow::importCsv()
{
    if (activeImportDialog_) {
        activeImportDialog_->raise();
        activeImportDialog_->activateWindow();
        return;
    }

    const QString fileName = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择 CSV 文件"),
        {},
        QStringLiteral(
            "CSV 文件 (*.csv *.tsv *.txt);;所有文件 (*.*)"));
    if (fileName.isEmpty()) {
        return;
    }

    signallab::repository::CsvImportOptions options;
    const QFileInfo information(fileName);
    options.delimiter = information.suffix().compare(
            QStringLiteral("tsv"), Qt::CaseInsensitive) == 0
        ? signallab::repository::CsvDelimiter::Tab
        : signallab::repository::CsvDelimiter::Comma;
    options.metadata.name =
        information.completeBaseName().toStdString();
    options.metadata.unit = "幅值";
    options.metadata.source = fileName.toStdString();

    signallab::gui::FileImportDialog dialog(
        toPath(fileName), options, this);
    activeImportDialog_ = &dialog;
    dialog.setPreviewRequestCallback(
        [this, &dialog](std::uint64_t revision) {
            if (activeImportPreviewTaskId_.has_value()) {
                static_cast<void>(scheduler_.cancel(
                    *activeImportPreviewTaskId_));
                importPreviewRevisions_.erase(
                    *activeImportPreviewTaskId_);
                activeImportPreviewTaskId_.reset();
            }
            const auto currentOptions = dialog.csvOptions();
            if (!currentOptions.has_value()) {
                static_cast<void>(dialog.setPreviewError(
                    revision, QStringLiteral("CSV 参数不可用。")));
                return;
            }
            auto submitted = backgroundFileService_.previewCsv(
                dialog.sourcePath(), *currentOptions);
            if (!submitted) {
                static_cast<void>(dialog.setPreviewError(
                    revision,
                    QString::fromStdString(
                        submitted.error().message)));
                return;
            }
            activeImportPreviewTaskId_ = submitted.value();
            importPreviewRevisions_.insert_or_assign(
                submitted.value(), revision);
            refreshTaskList();
            statusBar()->showMessage(
                QStringLiteral("CSV 预览任务 #%1 已排队")
                    .arg(static_cast<qulonglong>(submitted.value())),
                4000);
        });
    QTimer::singleShot(0, &dialog, [&dialog]() {
        static_cast<void>(dialog.requestPreview());
    });

    const int result = dialog.exec();
    if (activeImportPreviewTaskId_.has_value()) {
        static_cast<void>(scheduler_.cancel(*activeImportPreviewTaskId_));
        importPreviewRevisions_.erase(*activeImportPreviewTaskId_);
        activeImportPreviewTaskId_.reset();
    }
    activeImportDialog_ = nullptr;
    if (result != QDialog::Accepted || !dialog.isPreviewCurrent()) {
        return;
    }

    const auto acceptedOptions = dialog.csvOptions();
    if (!acceptedOptions.has_value()) {
        return;
    }
    auto submitted = backgroundFileService_.importCsv(
        dialog.sourcePath(), *acceptedOptions);
    if (!submitted) {
        const QString reason =
            QString::fromStdString(submitted.error().message);
        showFailure(
            QStringLiteral("CSV 导入无法排队"),
            reason,
            actionableSuggestion(reason));
        return;
    }
    refreshTaskList();
    statusBar()->showMessage(
        QStringLiteral("CSV 导入任务 #%1 已排队")
            .arg(static_cast<qulonglong>(submitted.value())),
        4000);
}

void MainWindow::importWav()
{
    if (activeImportDialog_) {
        activeImportDialog_->raise();
        activeImportDialog_->activateWindow();
        return;
    }

    const QString fileName = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择 WAV 文件"),
        {},
        QStringLiteral("WAV 文件 (*.wav);;所有文件 (*.*)"));
    if (fileName.isEmpty()) {
        return;
    }

    const QFileInfo information(fileName);
    signallab::repository::WavImportOptions options;
    options.metadata.name =
        information.completeBaseName().toStdString();
    options.metadata.unit = "幅值";
    options.metadata.source = fileName.toStdString();

    signallab::gui::FileImportDialog dialog(
        toPath(fileName), options, this);
    activeImportDialog_ = &dialog;
    dialog.setPreviewRequestCallback(
        [this, &dialog](std::uint64_t revision) {
            if (activeImportPreviewTaskId_.has_value()) {
                static_cast<void>(scheduler_.cancel(
                    *activeImportPreviewTaskId_));
                importPreviewRevisions_.erase(
                    *activeImportPreviewTaskId_);
                activeImportPreviewTaskId_.reset();
            }
            auto submitted = backgroundFileService_.previewWav(
                dialog.sourcePath());
            if (!submitted) {
                static_cast<void>(dialog.setPreviewError(
                    revision,
                    QString::fromStdString(
                        submitted.error().message)));
                return;
            }
            activeImportPreviewTaskId_ = submitted.value();
            importPreviewRevisions_.insert_or_assign(
                submitted.value(), revision);
            refreshTaskList();
            statusBar()->showMessage(
                QStringLiteral("WAV 预览任务 #%1 已排队")
                    .arg(static_cast<qulonglong>(submitted.value())),
                4000);
        });
    QTimer::singleShot(0, &dialog, [&dialog]() {
        static_cast<void>(dialog.requestPreview());
    });

    const int result = dialog.exec();
    if (activeImportPreviewTaskId_.has_value()) {
        static_cast<void>(scheduler_.cancel(*activeImportPreviewTaskId_));
        importPreviewRevisions_.erase(*activeImportPreviewTaskId_);
        activeImportPreviewTaskId_.reset();
    }
    activeImportDialog_ = nullptr;
    if (result != QDialog::Accepted || !dialog.isPreviewCurrent()) {
        return;
    }

    const auto acceptedOptions = dialog.wavOptions();
    if (!acceptedOptions.has_value()) {
        return;
    }
    auto submitted = backgroundFileService_.importWav(
        dialog.sourcePath(), *acceptedOptions);
    if (!submitted) {
        const QString reason =
            QString::fromStdString(submitted.error().message);
        showFailure(
            QStringLiteral("WAV 导入无法排队"),
            reason,
            actionableSuggestion(reason));
        return;
    }
    refreshTaskList();
    statusBar()->showMessage(
        QStringLiteral("WAV 导入任务 #%1 已排队")
            .arg(static_cast<qulonglong>(submitted.value())),
        4000);
}

void MainWindow::analyzeSelectedSignal()
{
    const auto* entry = selectedSignal();
    if (entry == nullptr) {
        statusBar()->showMessage(
            QStringLiteral("请先选择信号并修正分析参数。"), 4000);
        return;
    }

    const std::int64_t recordId = entry->recordId;
    if (isSignalDeletionPending(recordId)) {
        statusBar()->showMessage(
            QStringLiteral("该信号正在删除，不能再提交分析任务。"),
            5000);
        updateAnalysisValidation();
        return;
    }
    if (!analyzeButton_->isEnabled()) {
        statusBar()->showMessage(
            QStringLiteral("请先选择信号并修正分析参数。"), 4000);
        return;
    }

    const signallab::app::AnalysisOptions options = analysisOptions();
    auto submitted = backgroundAnalysisService_.submit(
        recordId, options);
    if (!submitted) {
        const QString reason =
            QString::fromStdString(submitted.error().message);
        showFailure(
            QStringLiteral("分析无法排队"),
            reason,
            actionableSuggestion(reason));
        return;
    }
    signalTaskRecords_.insert_or_assign(submitted.value(), recordId);
    updateSignalDeleteButton();
    refreshTaskList();
    statusBar()->showMessage(
        QStringLiteral("分析任务 #%1 已排队，可在任务中心取消")
            .arg(static_cast<qulonglong>(submitted.value())),
        5000);
}

void MainWindow::filterSelectedSignal()
{
    const auto* entry = selectedSignal();
    if (entry == nullptr) {
        statusBar()->showMessage(
            QStringLiteral("请先选择信号并修正 FIR 参数。"), 4000);
        return;
    }

    const std::int64_t recordId = entry->recordId;
    if (isSignalDeletionPending(recordId)) {
        statusBar()->showMessage(
            QStringLiteral("该信号正在删除，不能再提交 FIR 任务。"),
            5000);
        updateFirValidation();
        return;
    }
    if (!applyFirButton_->isEnabled()) {
        statusBar()->showMessage(
            QStringLiteral("请先选择信号并修正 FIR 参数。"), 4000);
        return;
    }

    auto submitted = backgroundFileService_.filterFir(
        *entry, firFilterOptions());
    if (!submitted) {
        const QString reason =
            QString::fromStdString(submitted.error().message);
        showFailure(
            QStringLiteral("FIR 滤波无法排队"),
            reason,
            actionableSuggestion(reason));
        return;
    }
    signalTaskRecords_.insert_or_assign(submitted.value(), recordId);
    updateSignalDeleteButton();
    refreshTaskList();
    statusBar()->showMessage(
        QStringLiteral("FIR 滤波任务 #%1 已进入计算队列")
            .arg(static_cast<qulonglong>(submitted.value())),
        5000);
}

void MainWindow::showAnalysisResult(
    std::shared_ptr<const signallab::app::AnalysisResult> result,
    const signallab::app::AnalysisOptions& options,
    bool focusSpectrum)
{
    if (!result) {
        return;
    }
    displayedAnalysis_ = std::move(result);
    displayedAnalysisOptions_ = options;
    const auto* entry = selectedSignal();
    const QString unit = entry == nullptr
        ? QStringLiteral("幅值")
        : QString::fromStdString(entry->signal.metadata().unit);
    resultModel_->setResult(
        *displayedAnalysis_,
        unit,
        options,
        QString::fromUtf8(signallab::app::kAnalysisServiceVersion));
    resultVersionLabel_->setText(
        QStringLiteral("算法版本：%1")
            .arg(resultModel_->algorithmVersion()));
    resultParametersLabel_->setText(
        QStringLiteral("实际参数：%1")
            .arg(resultModel_->parameterSummary()));
    resultView_->resizeColumnsToContents();
    renderSpectrum(true);

    if (focusSpectrum) {
        workspaceTabs_->setCurrentWidget(spectrumChartView_->parentWidget());
        statusBar()->showMessage(
            QStringLiteral("分析完成：FFT %1 点，检测到 %2 个峰值")
                .arg(static_cast<qulonglong>(
                    displayedAnalysis_->spectrum.fftSize))
                .arg(static_cast<qulonglong>(
                    displayedAnalysis_->peaks.size())),
            5000);
    }
}

void MainWindow::processBackgroundAnalysisEvents()
{
    auto events = backgroundAnalysisService_.takeEvents();
    bool historyChanged = false;
    for (auto& event : events) {
        if (!signallab::task::isTerminal(event.task.status)) {
            continue;
        }
        historyChanged = true;
        if (event.task.status == signallab::task::TaskStatus::Succeeded) {
            if (!event.result) {
                showFailure(
                    QStringLiteral("后台分析失败"),
                    QStringLiteral("任务报告成功，但没有返回分析结果。"),
                    QStringLiteral("请重新运行分析；若问题持续出现，请保留任务记录。"));
                continue;
            }
            const auto* entry = selectedSignal();
            if (entry != nullptr
                && entry->recordId == event.signalRecordId) {
                showAnalysisResult(
                    std::move(event.result), event.options, true);
            } else {
                statusBar()->showMessage(
                    QStringLiteral("信号 #%1 的后台分析已完成")
                        .arg(event.signalRecordId),
                    5000);
            }
        } else if (
            event.task.status == signallab::task::TaskStatus::Failed) {
            const QString reason =
                QString::fromStdString(event.task.errorMessage);
            showFailure(
                QStringLiteral("后台分析失败"),
                reason,
                actionableSuggestion(reason));
        } else {
            statusBar()->showMessage(
                QStringLiteral("分析任务 #%1 已取消")
                    .arg(static_cast<qulonglong>(event.task.id)),
                4000);
        }
    }
    refreshTaskList();
    if (historyChanged) {
        refreshAnalysisHistory();
    }
    updateSignalDeleteButton();
}

void MainWindow::processBackgroundFileEvents()
{
    auto events = backgroundFileService_.takeEvents();
    for (auto& event : events) {
        if (!signallab::task::isTerminal(event.task.status)) {
            continue;
        }

        const bool isPreview = event.operation
                == signallab::app::BackgroundFileOperation::PreviewCsv
            || event.operation
                == signallab::app::BackgroundFileOperation::PreviewWav;
        if (isPreview) {
            const auto request = importPreviewRevisions_.find(
                event.task.id);
            if (request == importPreviewRevisions_.end()) {
                continue;
            }
            const std::uint64_t revision = request->second;
            importPreviewRevisions_.erase(request);
            if (activeImportPreviewTaskId_ == event.task.id) {
                activeImportPreviewTaskId_.reset();
            }
            if (!activeImportDialog_) {
                continue;
            }

            if (event.task.status
                == signallab::task::TaskStatus::Cancelled) {
                static_cast<void>(activeImportDialog_->setPreviewError(
                    revision, QStringLiteral("文件预览已取消。")));
            } else if (event.task.status
                       == signallab::task::TaskStatus::Failed) {
                static_cast<void>(activeImportDialog_->setPreviewError(
                    revision,
                    QString::fromStdString(event.task.errorMessage)));
            } else if (!event.preview) {
                static_cast<void>(activeImportDialog_->setPreviewError(
                    revision,
                    QStringLiteral("预览任务完成，但没有返回文件信息。")));
            } else {
                static_cast<void>(activeImportDialog_->setPreview(
                    revision, *event.preview));
                statusBar()->showMessage(
                    QStringLiteral("文件预览已更新，可在当前窗口确认导入。"),
                    4000);
            }
            continue;
        }

        if (event.operation
            == signallab::app::BackgroundFileOperation::DeleteSignal) {
            PendingSignalDeletion request{event.sourceRecordId, std::nullopt};
            const auto pending = pendingSignalDeletions_.find(event.task.id);
            if (pending != pendingSignalDeletions_.end()) {
                request = pending->second;
                pendingSignalDeletions_.erase(pending);
            }

            if (event.task.status
                == signallab::task::TaskStatus::Cancelled) {
                statusBar()->showMessage(
                    QStringLiteral("信号删除任务 #%1 已取消")
                        .arg(static_cast<qulonglong>(event.task.id)),
                    4000);
                updateSignalDeleteButton();
                continue;
            }
            if (event.task.status
                == signallab::task::TaskStatus::Failed) {
                const QString reason = QString::fromStdString(
                    event.task.errorMessage);
                showFailure(
                    QStringLiteral("项目信号无法删除"),
                    reason,
                    actionableSuggestion(reason));
                updateSignalDeleteButton();
                continue;
            }
            if (!event.signalRemoval) {
                showFailure(
                    QStringLiteral("项目信号删除状态未知"),
                    QStringLiteral(
                        "任务报告成功，但没有返回持久化删除结果。"),
                    QStringLiteral(
                        "请重新启动项目以数据库状态为准；若问题持续出现，请保留任务记录。"));
                updateSignalDeleteButton();
                continue;
            }

            const auto selectedBefore = signalModel_->recordIdAt(
                signalListView_->currentIndex());
            const std::optional<std::int64_t> selectAfter =
                selectedBefore.has_value()
                    && *selectedBefore == request.recordId
                ? request.adjacentRecordId
                : selectedBefore;
            static_cast<void>(
                importService_.forgetSignal(request.recordId));
            refreshSignalList(selectAfter);
            refreshAnalysisHistory();

            const auto& outcome = *event.signalRemoval;
            if (!outcome.cleanupWarning.empty()) {
                showFailure(
                    QStringLiteral("信号已删除，项目存档清理未完成"),
                    QString::fromStdString(outcome.cleanupWarning),
                    QStringLiteral(
                        "数据库删除已经提交；可稍后手动检查 data/imports，保留的孤儿存档不会恢复该信号。"));
            } else if (!outcome.removed) {
                statusBar()->showMessage(
                    QStringLiteral(
                        "信号 #%1 已不在数据库中；项目列表已同步。")
                        .arg(request.recordId),
                    5000);
            } else if (outcome.storedFileRemoved) {
                statusBar()->showMessage(
                    QStringLiteral(
                        "已永久删除信号 #%1，并清理最后一个未共享项目存档。")
                        .arg(request.recordId),
                    5000);
            } else {
                statusBar()->showMessage(
                    QStringLiteral(
                        "已永久删除信号 #%1；历史记录和其他信号未受影响。")
                        .arg(request.recordId),
                    5000);
            }
            updateSignalDeleteButton();
            continue;
        }

        if (event.task.status == signallab::task::TaskStatus::Cancelled) {
            statusBar()->showMessage(
                QStringLiteral("后台信号任务 #%1 已取消")
                    .arg(static_cast<qulonglong>(event.task.id)),
                4000);
            continue;
        }
        if (event.task.status == signallab::task::TaskStatus::Failed) {
            const QString reason =
                QString::fromStdString(event.task.errorMessage);
            showFailure(
                event.operation
                        == signallab::app::BackgroundFileOperation::FilterFir
                    ? QStringLiteral("后台 FIR 滤波失败")
                    : QStringLiteral("后台文件任务失败"),
                reason,
                actionableSuggestion(reason));
            continue;
        }

        if (!event.importedSignal) {
            showFailure(
                QStringLiteral("后台信号任务失败"),
                QStringLiteral("任务报告成功，但没有返回信号。"),
                QStringLiteral("请重试任务；若问题持续出现，请保留任务记录。"));
            continue;
        }
        const std::int64_t recordId = event.importedSignal->recordId;
        const auto accepted = importService_.acceptImportedSignal(
            std::move(*event.importedSignal));
        if (!accepted.has_value()) {
            refreshSignalList();
            statusBar()->showMessage(
                QStringLiteral(
                    "已忽略信号 #%1 的迟到结果：该记录已经删除。")
                    .arg(recordId),
                5000);
            continue;
        }
        refreshSignalList(recordId);
        QString completionMessage;
        switch (event.operation) {
        case signallab::app::BackgroundFileOperation::ImportCsv:
            completionMessage = QStringLiteral("CSV 导入成功");
            break;
        case signallab::app::BackgroundFileOperation::ImportWav:
            completionMessage = QStringLiteral("WAV 导入成功");
            break;
        case signallab::app::BackgroundFileOperation::FilterFir:
            completionMessage = QStringLiteral(
                "FIR 滤波完成，派生信号已加入项目并持久化");
            break;
        default:
            completionMessage = QStringLiteral("后台信号任务完成");
            break;
        }
        statusBar()->showMessage(completionMessage, 5000);
    }
    refreshTaskList();
    updateAnalysisValidation();
    updateFirValidation();
    updateSignalDeleteButton();
}

void MainWindow::deleteSelectedSignal()
{
    const QModelIndex current = signalListView_->currentIndex();
    const auto selectedIndex = selectedSignalIndex();
    if (!selectedIndex.has_value()) {
        return;
    }

    const auto& selected = importService_.entries()[*selectedIndex];
    const std::int64_t recordId = selected.recordId;
    const QString displayName = QString::fromStdString(
        selected.displayName.empty()
            ? selected.signal.metadata().name
            : selected.displayName);
    const bool imported = selected.sourceType
        != signallab::repository::SignalSourceType::Generated;

    const auto activeTasks = scheduler_.snapshots();
    const bool hasActiveRelatedTask = std::any_of(
        activeTasks.begin(),
        activeTasks.end(),
        [this, recordId](const signallab::task::TaskSnapshot& task) {
            const auto related = signalTaskRecords_.find(task.id);
            return !signallab::task::isTerminal(task.status)
                && related != signalTaskRecords_.end()
                && related->second == recordId;
        });
    if (hasActiveRelatedTask) {
        statusBar()->showMessage(
            QStringLiteral(
                "该信号仍有排队或运行中的分析/FIR 任务；请先取消并等待任务结束。"),
            6000);
        updateSignalDeleteButton();
        return;
    }

    if (isSignalDeletionPending(recordId)) {
        statusBar()->showMessage(
            QStringLiteral("该信号的删除任务已经在队列中。"), 4000);
        updateSignalDeleteButton();
        return;
    }

    std::optional<std::int64_t> adjacentId;
    if (current.row() + 1 < signalModel_->rowCount()) {
        adjacentId = signalModel_->recordIdAt(current.row() + 1);
    } else if (current.row() > 0) {
        adjacentId = signalModel_->recordIdAt(current.row() - 1);
    }

    const QString archiveBoundary = imported
        ? QStringLiteral(
              "项目外的原始文件不会删除；data/imports 存档仅在没有其他逻辑信号共享时清理。")
        : QStringLiteral("该生成信号不关联导入存档。");
    if (QMessageBox::question(
            this,
            QStringLiteral("永久删除项目信号"),
            QStringLiteral(
                "确定永久删除信号 #%1（%2）吗？\n\n"
                "信号记录及来源会从 SQLite 删除。分析历史、特征结果、任务中心、"
                "脚本历史和已有 FIR 派生信号会保留，但历史中的实时信号关联会清空。\n"
                "%3\n\n此操作不可撤销。")
                .arg(recordId)
                .arg(displayName)
                .arg(archiveBoundary),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    auto submitted = backgroundFileService_.deleteSignal(
        recordId,
        displayName.toStdString());
    if (!submitted) {
        const QString reason = QString::fromStdString(
            submitted.error().message);
        showFailure(
            QStringLiteral("信号删除无法排队"),
            reason,
            actionableSuggestion(reason));
        return;
    }

    pendingSignalDeletions_.insert_or_assign(
        submitted.value(),
        PendingSignalDeletion{recordId, adjacentId});
    updateAnalysisValidation();
    updateFirValidation();
    updateSignalDeleteButton();
    refreshTaskList(submitted.value());
    statusBar()->showMessage(
        QStringLiteral("信号 #%1 的删除任务 #%2 已进入 I/O 队列")
            .arg(recordId)
            .arg(static_cast<qulonglong>(submitted.value())),
        5000);
}

void MainWindow::cancelSelectedTask()
{
    const auto id = taskModel_->taskIdAt(
        taskListView_->currentIndex());
    if (!id.has_value()) {
        return;
    }
    if (!scheduler_.cancel(*id)) {
        statusBar()->showMessage(
            QStringLiteral("所选任务已经结束，无法取消"), 3000);
    } else {
        statusBar()->showMessage(
            QStringLiteral("已请求取消任务 #%1")
                .arg(static_cast<qulonglong>(*id)),
            3000);
    }
    refreshTaskList();
}

void MainWindow::deleteSelectedTask()
{
    const QModelIndex current = taskListView_->currentIndex();
    const auto id = taskModel_->taskIdAt(current);
    if (!id.has_value()) {
        return;
    }

    const auto snapshot = scheduler_.snapshot(*id);
    if (!snapshot.has_value()
        || !signallab::task::isTerminal(snapshot->status)) {
        statusBar()->showMessage(
            QStringLiteral("只能删除已经结束的任务；请先取消并等待任务结束。"),
            5000);
        refreshTaskList();
        return;
    }

    std::optional<signallab::task::TaskId> adjacentId;
    if (current.row() + 1 < taskModel_->rowCount()) {
        adjacentId = taskModel_->taskIdAt(current.row() + 1);
    } else if (current.row() > 0) {
        adjacentId = taskModel_->taskIdAt(current.row() - 1);
    }

    const QString taskName = QString::fromStdString(snapshot->name);
    if (QMessageBox::question(
            this,
            QStringLiteral("删除任务中心记录"),
            QStringLiteral(
                "确定从任务中心删除任务 #%1（%2）吗？\n\n"
                "这只会移除本次进程内的任务记录，不会删除分析历史、"
                "脚本历史、导入信号或输出文件。")
                .arg(static_cast<qulonglong>(*id))
                .arg(taskName),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    if (!scheduler_.remove(*id)) {
        showFailure(
            QStringLiteral("任务记录无法删除"),
            QStringLiteral("任务可能已被删除，或仍未进入终态。"),
            QStringLiteral("刷新任务中心；运行中的任务请先取消并等待结束。"));
        refreshTaskList();
        return;
    }

    refreshTaskList(adjacentId);
    statusBar()->showMessage(
        QStringLiteral("已从任务中心删除任务 #%1；持久化历史未受影响。")
            .arg(static_cast<qulonglong>(*id)),
        5000);
}

void MainWindow::retrySelectedTask()
{
    const auto originalId = taskModel_->taskIdAt(
        taskListView_->currentIndex());
    if (!originalId.has_value()) {
        return;
    }
    const auto originalSnapshot = scheduler_.snapshot(*originalId);
    const bool isScriptExecution = originalSnapshot.has_value()
        && originalSnapshot->name == "Run SignalScript";
    const bool isImportPreview = originalSnapshot.has_value()
        && (originalSnapshot->name.starts_with("Preview CSV:")
            || originalSnapshot->name.starts_with("Preview WAV:"));
    const bool isSignalDeletion = originalSnapshot.has_value()
        && originalSnapshot->name.starts_with("Delete signal:");
    if (isImportPreview) {
        statusBar()->showMessage(
            QStringLiteral(
                "文件预览绑定导入设置窗口；请重新点击 CSV 或 WAV 导入。"),
            5000);
        return;
    }
    if (isSignalDeletion) {
        statusBar()->showMessage(
            QStringLiteral(
                "信号删除不能从任务中心重试；请回到项目列表重新确认。"),
            5000);
        return;
    }
    if (isScriptExecution && activeScriptTaskId_.has_value()) {
        statusBar()->showMessage(
            QStringLiteral(
                "已有脚本正在运行，请等待完成或先取消。"),
            4000);
        return;
    }

    std::optional<std::int64_t> relatedRecordId;
    const auto relatedSignal = signalTaskRecords_.find(*originalId);
    if (relatedSignal != signalTaskRecords_.end()) {
        relatedRecordId = relatedSignal->second;
        const auto& entries = importService_.entries();
        const bool recordStillPresent = std::any_of(
            entries.begin(),
            entries.end(),
            [recordId = *relatedRecordId](
                const signallab::app::SignalEntry& entry) {
                return entry.recordId == recordId;
            });
        if (!recordStillPresent
            || isSignalDeletionPending(*relatedRecordId)) {
            statusBar()->showMessage(
                recordStillPresent
                    ? QStringLiteral(
                          "该信号正在删除，不能重试相关任务。")
                    : QStringLiteral(
                          "源信号已经删除，不能重试相关任务。"),
                5000);
            return;
        }
    }

    auto retried = scheduler_.retry(*originalId);
    if (!retried) {
        const QString reason =
            QString::fromStdString(retried.error().message);
        showFailure(
            QStringLiteral("任务无法重试"),
            reason,
            actionableSuggestion(reason));
        return;
    }

    if (relatedRecordId.has_value()) {
        signalTaskRecords_.insert_or_assign(
            retried.value(), *relatedRecordId);
    }

    if (isScriptExecution) {
        activeScriptTaskId_ = retried.value();
        scriptStatusLabel_->setText(
            QStringLiteral("脚本重试任务 #%1 已排队")
                .arg(static_cast<qulonglong>(retried.value())));
        updateScriptButtons();
    }
    refreshTaskList();
    updateSignalDeleteButton();
    statusBar()->showMessage(
        QStringLiteral("重试任务 #%1 已排队（来源 #%2）")
            .arg(static_cast<qulonglong>(retried.value()))
            .arg(static_cast<qulonglong>(*originalId)),
        4000);
}

void MainWindow::clearAnalysis()
{
    displayedAnalysis_.reset();
    resultModel_->clear();
    resultVersionLabel_->setText(QStringLiteral("算法版本：—"));
    resultParametersLabel_->setText(QStringLiteral("参数：—"));
    spectrumSeries_->clear();
    spectrumChartView_->chart()->setTitle(
        QStringLiteral("完成分析后显示单边幅度谱"));
    {
        const QSignalBlocker xBlocker(spectrumXAxis_);
        const QSignalBlocker yBlocker(spectrumYAxis_);
        spectrumXAxis_->setRange(0.0, 1.0);
        spectrumYAxis_->setRange(0.0, 1.0);
    }
    spectrumChartView_->captureHomeRanges();
}

void MainWindow::refreshTaskList(
    std::optional<signallab::task::TaskId> selectTaskId)
{
    const auto selectedId = selectTaskId.has_value()
        ? selectTaskId
        : taskModel_->taskIdAt(taskListView_->currentIndex());
    const auto snapshots = scheduler_.snapshots();
    taskModel_->setSnapshots(snapshots);

    int rowToSelect = -1;
    if (selectedId.has_value()) {
        for (std::size_t row = 0; row < snapshots.size(); ++row) {
            if (snapshots[row].id == *selectedId) {
                rowToSelect = static_cast<int>(row);
                break;
            }
        }
    }
    if (rowToSelect < 0 && !snapshots.empty()) {
        rowToSelect = static_cast<int>(snapshots.size() - 1U);
    }
    if (rowToSelect >= 0) {
        taskListView_->setCurrentIndex(
            taskModel_->index(rowToSelect, 0));
    } else {
        taskDetails_->clear();
        cancelTaskButton_->setEnabled(false);
        retryTaskButton_->setEnabled(false);
        deleteTaskButton_->setEnabled(false);
    }
}

void MainWindow::showTaskDetails(const QModelIndex& index)
{
    const auto id = taskModel_->taskIdAt(index);
    if (!id.has_value()) {
        taskDetails_->clear();
        cancelTaskButton_->setEnabled(false);
        retryTaskButton_->setEnabled(false);
        deleteTaskButton_->setEnabled(false);
        return;
    }
    const auto snapshot = scheduler_.snapshot(*id);
    if (!snapshot.has_value()) {
        taskDetails_->setPlainText(
            QStringLiteral("任务快照已不可用。"));
        cancelTaskButton_->setEnabled(false);
        retryTaskButton_->setEnabled(false);
        deleteTaskButton_->setEnabled(false);
        return;
    }

    QStringList lines;
    lines << QStringLiteral("任务 #%1：%2")
                 .arg(static_cast<qulonglong>(snapshot->id))
                 .arg(QString::fromStdString(snapshot->name));
    lines << QStringLiteral("状态：%1")
                 .arg(index.data(
                     signallab::gui::TaskListModel::StatusRole)
                     .toString());
    lines << QStringLiteral("进度：%1%  %2")
                 .arg(snapshot->progressPercent)
                 .arg(QString::fromStdString(
                     snapshot->progressMessage));
    if (snapshot->retryOf != 0) {
        lines << QStringLiteral("重试来源：#%1")
                     .arg(static_cast<qulonglong>(
                         snapshot->retryOf));
    }
    const QString error =
        QString::fromStdString(snapshot->errorMessage);
    lines << QStringLiteral("错误原因：%1")
                 .arg(error.isEmpty()
                     ? QStringLiteral("—") : error);
    lines << QStringLiteral("建议：%1")
                 .arg(error.isEmpty()
                     ? QStringLiteral(
                         "运行中的任务可取消；终态任务可重试。")
                     : actionableSuggestion(error));
    taskDetails_->setPlainText(lines.join(QLatin1Char('\n')));
    cancelTaskButton_->setEnabled(
        !signallab::task::isTerminal(snapshot->status));
    retryTaskButton_->setEnabled(
        signallab::task::isTerminal(snapshot->status));
    deleteTaskButton_->setEnabled(
        signallab::task::isTerminal(snapshot->status));
}

void MainWindow::refreshComparisonSignalChoices()
{
    const qlonglong preservedRecordId = comparisonSignalCombo_->currentData()
        .toLongLong();
    const auto* primary = selectedSignal();
    const QSignalBlocker blocker(comparisonSignalCombo_);
    comparisonSignalCombo_->clear();
    comparisonSignalCombo_->addItem(
        QStringLiteral("不对比"), QVariant::fromValue<qlonglong>(0));

    int restoredIndex = 0;
    for (const auto& entry : importService_.entries()) {
        if (primary != nullptr && entry.recordId == primary->recordId) {
            continue;
        }
        const QString unit = QString::fromStdString(
            entry.signal.metadata().unit);
        const QString label = unit.isEmpty()
            ? QStringLiteral("%1 (#%2)")
                  .arg(signalName(entry))
                  .arg(entry.recordId)
            : QStringLiteral("%1 [%2] (#%3)")
                  .arg(signalName(entry), unit)
                  .arg(entry.recordId);
        comparisonSignalCombo_->addItem(
            label, QVariant::fromValue<qlonglong>(entry.recordId));
        if (entry.recordId == preservedRecordId) {
            restoredIndex = comparisonSignalCombo_->count() - 1;
        }
    }
    comparisonSignalCombo_->setCurrentIndex(restoredIndex);
    comparisonSignalCombo_->setEnabled(
        primary != nullptr && comparisonSignalCombo_->count() > 1);
}

void MainWindow::refreshSignalList(
    std::optional<std::int64_t> selectRecordId)
{
    if (!selectRecordId.has_value()) {
        selectRecordId = signalModel_->recordIdAt(
            signalListView_->currentIndex());
    }
    const auto& entries = importService_.entries();
    signalModel_->setEntries(entries);

    int targetRow = -1;
    if (selectRecordId.has_value()) {
        for (std::size_t row = 0; row < entries.size(); ++row) {
            if (entries[row].recordId == *selectRecordId) {
                targetRow = static_cast<int>(row);
                break;
            }
        }
    }
    if (targetRow < 0 && !entries.empty()) {
        targetRow = 0;
    }
    if (targetRow >= 0) {
        signalListView_->setCurrentIndex(
            signalModel_->index(targetRow, 0));
    } else {
        signalListView_->clearSelection();
        signalListView_->setCurrentIndex(QModelIndex());
        showSignal(QModelIndex());
    }
    refreshComparisonSignalChoices();
    updateSignalDeleteButton();
}

void MainWindow::showSignal(const QModelIndex& index)
{
    refreshComparisonSignalChoices();
    if (!index.isValid() || index.row() < 0
        || static_cast<std::size_t>(index.row())
            >= importService_.entries().size()) {
        nameValue_->setText(QStringLiteral("—"));
        sampleRateValue_->setText(QStringLiteral("—"));
        sampleCountValue_->setText(QStringLiteral("—"));
        durationValue_->setText(QStringLiteral("—"));
        timeSeries_->clear();
        comparisonTimeSeries_->clear();
        timeChartView_->clearSelection();
        timeChartView_->chart()->legend()->hide();
        timeChartView_->chart()->setTitle(
            QStringLiteral("请选择一个信号"));
        timeCursorLabel_->setText(
            QStringLiteral("游标：移入图表查看精确样本"));
        timeSelectionLabel_->setText(
            QStringLiteral("局部选择：按住 Shift 拖动"));
        timeYAxis_->setTitleText(QStringLiteral("幅值"));
        {
            const QSignalBlocker xBlocker(timeXAxis_);
            const QSignalBlocker yBlocker(timeYAxis_);
            timeXAxis_->setRange(0.0, 1.0);
            timeYAxis_->setRange(0.0, 1.0);
        }
        timeChartView_->captureHomeRanges();
        updateTimeAxisScrollBar();
        clearAnalysis();
        updateAnalysisValidation();
        updateFirValidation();
        updateSignalDeleteButton();
        return;
    }
    const auto& signal = importService_.entries()[
        static_cast<std::size_t>(index.row())].signal;
    nameValue_->setText(
        QString::fromStdString(signal.metadata().name));
    sampleRateValue_->setText(
        QStringLiteral("%1 Hz").arg(
            signal.sampleRate(), 0, 'f', 3));
    sampleCountValue_->setText(QString::number(
        static_cast<qulonglong>(signal.sampleCount())));
    durationValue_->setText(
        QStringLiteral("%1 s").arg(
            signal.durationSeconds(), 0, 'f', 6));
    timeYAxis_->setTitleText(
        QString::fromStdString(signal.metadata().unit));
    clearAnalysis();
    renderTimeSignal(true);
    workspaceTabs_->setCurrentWidget(timeChartView_->parentWidget());
    updateAnalysisValidation();
    updateFirValidation();
    updateSignalDeleteButton();
}

void MainWindow::renderTimeSignal(bool resetAxes)
{
    if (resetAxes) {
        timeChartView_->clearSelection();
        timeCursorLabel_->setText(
            QStringLiteral("游标：移入图表查看精确样本"));
        timeSelectionLabel_->setText(
            QStringLiteral("局部选择：按住 Shift 拖动"));
    }
    const auto* entry = selectedSignal();
    if (entry == nullptr) {
        timeSeries_->clear();
        comparisonTimeSeries_->clear();
        timeChartView_->chart()->legend()->hide();
        updateTimeAxisScrollBar();
        return;
    }
    const auto& signal = entry->signal;
    const std::size_t sampleCount = signal.sampleCount();
    if (sampleCount == 0U) {
        timeSeries_->clear();
        comparisonTimeSeries_->clear();
        timeChartView_->chart()->legend()->hide();
        updateTimeAxisScrollBar();
        return;
    }

    const auto* comparison = comparisonSignal();
    if (!hasDisplayableTimeDomain(signal)
        || (comparison != nullptr
            && !hasDisplayableTimeDomain(comparison->signal))) {
        timeSeries_->clear();
        comparisonTimeSeries_->clear();
        timeChartView_->chart()->legend()->hide();
        timeChartView_->chart()->setTitle(
            QStringLiteral("无法显示：信号时间范围无效"));
        updateTimeAxisScrollBar();
        statusBar()->showMessage(
            QStringLiteral("信号采样率与样本数无法形成有限的时间轴。"),
            5000);
        return;
    }
    const std::size_t pixelWidth = static_cast<std::size_t>(
        std::max(timeChartView_->viewport()->width(), 1));
    const auto makePoints = [this, resetAxes, pixelWidth](
                                const signallab::core::Signal& item) {
        const auto range = resetAxes
            ? std::pair<std::size_t, std::size_t>{0U, item.sampleCount()}
            : visibleSampleRange(
                  item, timeXAxis_->min(), timeXAxis_->max());
        return toTimePoints(signallab::app::makeDisplayPoints(
            item, pixelWidth, range.first, range.second));
    };
    const QList<QPointF> points = makePoints(signal);
    const QList<QPointF> comparisonPoints = comparison == nullptr
        ? QList<QPointF>{}
        : makePoints(comparison->signal);
    timeSeries_->setName(
        QStringLiteral("%1（主信号）").arg(signalLegendLabel(*entry)));
    timeSeries_->replace(points);
    if (comparison == nullptr) {
        comparisonTimeSeries_->clear();
        timeChartView_->chart()->legend()->hide();
        timeChartView_->chart()->setTitle(signalName(*entry));
    } else {
        comparisonTimeSeries_->setName(
            QStringLiteral("%1（对比）")
                .arg(signalLegendLabel(*comparison)));
        comparisonTimeSeries_->replace(comparisonPoints);
        timeChartView_->chart()->legend()->show();
        timeChartView_->chart()->setTitle(
            QStringLiteral("%1 与 %2（相对 t=0）")
                .arg(signalName(*entry), signalName(*comparison)));
    }

    const QString primaryUnit = QString::fromStdString(
        signal.metadata().unit);
    const QString comparisonUnit = comparison == nullptr
        ? primaryUnit
        : QString::fromStdString(comparison->signal.metadata().unit);
    if (comparison != nullptr && primaryUnit != comparisonUnit) {
        timeYAxis_->setTitleText(
            QStringLiteral("幅值（%1 / %2）")
                .arg(
                    primaryUnit.isEmpty()
                        ? QStringLiteral("未标注")
                        : primaryUnit,
                    comparisonUnit.isEmpty()
                        ? QStringLiteral("未标注")
                        : comparisonUnit));
    } else {
        timeYAxis_->setTitleText(
            primaryUnit.isEmpty() ? QStringLiteral("幅值") : primaryUnit);
    }

    if (resetAxes) {
        auto [minimum, maximum] = signal.sampleRange();
        if (comparison != nullptr) {
            const auto [comparisonMinimum, comparisonMaximum] =
                comparison->signal.sampleRange();
            minimum = std::min(minimum, comparisonMinimum);
            maximum = std::max(maximum, comparisonMaximum);
        }
        const auto [paddedMinimum, paddedMaximum] =
            paddedRange(minimum, maximum);
        {
            const QSignalBlocker xBlocker(timeXAxis_);
            const QSignalBlocker yBlocker(timeYAxis_);
            timeXAxis_->setRange(
                0.0,
                std::max(
                    timeDomainMaximum(), 1.0 / signal.sampleRate()));
            timeYAxis_->setRange(paddedMinimum, paddedMaximum);
        }
        timeChartView_->captureHomeRanges();
    }
    updateTimeAxisScrollBar();
    if (comparison == nullptr) {
        statusBar()->showMessage(
            QStringLiteral("原始样本 %1 点；当前向 Qt Charts 提交 %2 点")
                .arg(static_cast<qulonglong>(sampleCount))
                .arg(static_cast<qulonglong>(points.size())),
            3000);
    } else {
        statusBar()->showMessage(
            QStringLiteral(
                "双信号相对 t=0 对齐；主信号 %1/%2 点，对比信号 %3/%4 点")
                .arg(static_cast<qulonglong>(points.size()))
                .arg(static_cast<qulonglong>(sampleCount))
                .arg(static_cast<qulonglong>(comparisonPoints.size()))
                .arg(static_cast<qulonglong>(
                    comparison->signal.sampleCount())),
            3000);
    }
}

void MainWindow::updateTimeAxisScrollBar()
{
    const double fullDuration = timeDomainMaximum();
    if (!std::isfinite(fullDuration) || fullDuration <= 0.0) {
        const QSignalBlocker blocker(timeAxisScrollBar_);
        timeAxisScrollBar_->setRange(0, 0);
        timeAxisScrollBar_->setPageStep(kTimeScrollResolution);
        timeAxisScrollBar_->setValue(0);
        timeAxisScrollBar_->setEnabled(false);
        timeAxisRangeLabel_->setText(QStringLiteral("无可用波形"));
        return;
    }

    const double visibleSpan = std::clamp(
        timeXAxis_->max() - timeXAxis_->min(), 0.0, fullDuration);
    const double movableDuration = std::max(
        fullDuration - visibleSpan, 0.0);
    const bool canScroll = movableDuration > 0.0;
    const int pageStep = canScroll
        ? std::clamp(
              static_cast<int>(std::llround(
                  visibleSpan / fullDuration * kTimeScrollResolution)),
              1,
              kTimeScrollResolution - 1)
        : kTimeScrollResolution;
    const int maximum = kTimeScrollResolution - pageStep;
    const double first = canScroll
        ? std::clamp(timeXAxis_->min(), 0.0, movableDuration)
        : 0.0;
    const double last = canScroll ? first + visibleSpan : fullDuration;
    const int value = !canScroll || maximum <= 0
        ? 0
        : first <= 0.0
            ? 0
            : first >= movableDuration
                ? maximum
                : static_cast<int>(std::llround(
                    first / movableDuration * maximum));

    {
        const QSignalBlocker blocker(timeAxisScrollBar_);
        timeAxisScrollBar_->setRange(0, maximum);
        timeAxisScrollBar_->setPageStep(pageStep);
        timeAxisScrollBar_->setSingleStep(std::max(pageStep / 10, 1));
        timeAxisScrollBar_->setValue(value);
        timeAxisScrollBar_->setEnabled(canScroll);
    }
    timeAxisRangeLabel_->setText(
        canScroll
            ? QStringLiteral("可见 %1–%2 s")
                  .arg(first, 0, 'g', 7)
                  .arg(last, 0, 'g', 7)
            : QStringLiteral("可见 0–%1 s（完整）")
                  .arg(fullDuration, 0, 'g', 7));
}

void MainWindow::renderSpectrum(bool resetAxes)
{
    if (!displayedAnalysis_
        || displayedAnalysis_->spectrum.frequenciesHz.empty()
        || displayedAnalysis_->spectrum.frequenciesHz.size()
            != displayedAnalysis_->spectrum.amplitudes.size()) {
        spectrumSeries_->clear();
        return;
    }
    const auto& spectrum = displayedAnalysis_->spectrum;
    const auto& frequencies = spectrum.frequenciesHz;
    std::size_t firstIndex = 0;
    std::size_t pastLastIndex = frequencies.size();
    if (!resetAxes) {
        const auto first = std::lower_bound(
            frequencies.begin(),
            frequencies.end(),
            spectrumXAxis_->min());
        const auto pastLast = std::upper_bound(
            frequencies.begin(),
            frequencies.end(),
            spectrumXAxis_->max());
        firstIndex = static_cast<std::size_t>(
            std::distance(frequencies.begin(), first));
        pastLastIndex = static_cast<std::size_t>(
            std::distance(frequencies.begin(), pastLast));
        if (firstIndex >= frequencies.size()) {
            firstIndex = frequencies.size() - 1U;
        }
        if (pastLastIndex <= firstIndex) {
            pastLastIndex = std::min(
                firstIndex + 1U, frequencies.size());
        }
    }

    const auto displayPoints =
        signallab::app::makeSpectrumDisplayPoints(
            spectrum,
            static_cast<std::size_t>(
                std::max(
                    spectrumChartView_->viewport()->width(), 1)),
            firstIndex,
            pastLastIndex);
    const QList<QPointF> points = toSpectrumPoints(displayPoints);
    spectrumSeries_->replace(points);
    spectrumChartView_->chart()->setTitle(
        QStringLiteral("单边幅度谱"));
    if (resetAxes) {
        auto [minimum, maximum] = yRange(points);
        minimum = std::min(0.0, minimum);
        const QSignalBlocker xBlocker(spectrumXAxis_);
        const QSignalBlocker yBlocker(spectrumYAxis_);
        spectrumXAxis_->setRange(
            0.0, std::max(frequencies.back(), 1.0));
        spectrumYAxis_->setRange(minimum, maximum);
        spectrumChartView_->captureHomeRanges();
    }
}

void MainWindow::updateSignalDeleteButton()
{
    const auto* entry = selectedSignal();
    if (entry == nullptr) {
        deleteSignalButton_->setEnabled(false);
        deleteSignalButton_->setToolTip(
            QStringLiteral("请先选择一个项目信号。"));
        return;
    }

    const std::int64_t recordId = entry->recordId;
    if (isSignalDeletionPending(recordId)) {
        deleteSignalButton_->setEnabled(false);
        deleteSignalButton_->setToolTip(
            QStringLiteral("该信号的删除任务正在处理。"));
        return;
    }

    const auto tasks = scheduler_.snapshots();
    const bool activeRelatedTask = std::any_of(
        tasks.begin(),
        tasks.end(),
        [this, recordId](const signallab::task::TaskSnapshot& task) {
            const auto related = signalTaskRecords_.find(task.id);
            return !signallab::task::isTerminal(task.status)
                && related != signalTaskRecords_.end()
                && related->second == recordId;
        });
    deleteSignalButton_->setEnabled(!activeRelatedTask);
    deleteSignalButton_->setToolTip(
        activeRelatedTask
            ? QStringLiteral(
                  "请先取消并等待该信号的分析/FIR 任务结束。")
            : QStringLiteral(
                  "永久删除所选信号；操作前会显示影响范围。"));
}

void MainWindow::updateAnalysisValidation()
{
    const auto* entry = selectedSignal();
    QString error;
    if (entry == nullptr) {
        error = QStringLiteral("请先选择信号。");
    } else if (isSignalDeletionPending(entry->recordId)) {
        error = QStringLiteral("该信号的删除任务正在处理。");
    } else {
        const auto options = analysisOptions();
        std::size_t fftSize = 1U;
        while (fftSize < entry->signal.sampleCount()
               && fftSize
                   <= std::numeric_limits<std::size_t>::max() / 2U) {
            fftSize *= 2U;
        }
        const std::size_t spectrumBinCount = fftSize / 2U + 1U;
        if (!std::isfinite(options.minimumPeakAmplitudeRatio)
            || options.minimumPeakAmplitudeRatio < 0.0
            || options.minimumPeakAmplitudeRatio > 1.0) {
            error = QStringLiteral("最小峰值比必须位于 0 到 1。");
        } else if (!std::isfinite(options.minimumPeakDistanceHz)
                   || options.minimumPeakDistanceHz < 0.0) {
            error = QStringLiteral("最小峰间距必须是非负有限数。");
        } else if (options.maximumPeakCount == 0U) {
            error = QStringLiteral("最大峰数必须至少为 1。");
        } else if (options.thdHarmonicCount < 2U) {
            error = QStringLiteral("THD 谐波数必须至少为 2。");
        } else if (options.metricGuardBins >= spectrumBinCount) {
            error = QStringLiteral(
                "保护频点必须小于预计频谱的 %1 个频点。")
                .arg(static_cast<qulonglong>(spectrumBinCount));
        }
    }

    const bool valid = error.isEmpty();
    analyzeButton_->setEnabled(valid);
    if (valid) {
        validationLabel_->setText(QStringLiteral(
            "✓ 参数有效；计算将在后台执行。"));
        validationLabel_->setAccessibleName(
            QStringLiteral("分析参数有效"));
    } else {
        validationLabel_->setText(
            QStringLiteral("⚠ %1").arg(error));
        validationLabel_->setAccessibleName(
            QStringLiteral("分析参数无效：%1").arg(error));
    }
}

void MainWindow::updateFirValidation()
{
    const auto* entry = selectedSignal();
    QString error;
    if (entry == nullptr) {
        error = QStringLiteral("请先选择信号。");
    } else if (isSignalDeletionPending(entry->recordId)) {
        error = QStringLiteral("该信号的删除任务正在处理。");
    } else {
        const auto validation = signallab::app::validateFirFilterOptions(
            entry->signal.sampleRate(), firFilterOptions());
        if (!validation) {
            const auto& options = firFilterOptions();
            if (options.tapCount < 3U || options.tapCount % 2U == 0U
                || options.tapCount
                    > signallab::app::kMaximumFirTapCount) {
                error = QStringLiteral("抽头数必须是 3—65535 的奇数。");
            } else {
                error = QStringLiteral(
                    "截止频率必须大于 0 且小于奈奎斯特频率 %1 Hz。")
                            .arg(
                                entry->signal.sampleRate() / 2.0,
                                0,
                                'g',
                                10);
            }
        }
    }

    const bool valid = error.isEmpty();
    applyFirButton_->setEnabled(valid);
    if (valid) {
        firValidationLabel_->setText(QStringLiteral(
            "✓ 参数有效；滤波与持久化将在后台执行。"));
        firValidationLabel_->setAccessibleName(
            QStringLiteral("FIR 参数有效"));
    } else {
        firValidationLabel_->setText(
            QStringLiteral("⚠ %1").arg(error));
        firValidationLabel_->setAccessibleName(
            QStringLiteral("FIR 参数无效：%1").arg(error));
    }
}

void MainWindow::deleteSelectedAnalysisHistory()
{
    const QModelIndex current = analysisHistoryView_->currentIndex();
    const auto* selected = analysisHistoryModel_->recordAt(current);
    if (selected == nullptr) {
        return;
    }

    const std::int64_t taskId = selected->id;
    const auto status = selected->status;
    const QString signalName = textOrDash(selected->signalName);
    if (!isTerminalAnalysisStatus(status)) {
        statusBar()->showMessage(
            QStringLiteral("等待中或运行中的分析记录不能删除。"),
            5000);
        refreshAnalysisHistory();
        return;
    }

    std::optional<std::int64_t> adjacentId;
    if (const auto* next = analysisHistoryModel_->recordAt(
            current.row() + 1)) {
        adjacentId = next->id;
    } else if (const auto* previous = analysisHistoryModel_->recordAt(
                   current.row() - 1)) {
        adjacentId = previous->id;
    }

    if (QMessageBox::question(
            this,
            QStringLiteral("永久删除分析历史"),
            QStringLiteral(
                "确定永久删除分析记录 #%1（%2）吗？\n\n"
                "该记录及其特征结果会从 SQLite 中删除。"
                "原始信号和任务中心记录不会被删除，此操作不可撤销。")
                .arg(taskId)
                .arg(signalName),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    auto removed = analysisService_.removeHistoryEntry(taskId);
    if (!removed) {
        const QString reason =
            QString::fromStdString(removed.error().message);
        showFailure(
            QStringLiteral("分析历史无法删除"),
            reason,
            reason.contains(QStringLiteral("terminal"), Qt::CaseInsensitive)
                ? QStringLiteral("等待分析结束后再删除。")
                : QStringLiteral("刷新分析历史并重试；若仍失败，请检查数据库状态。"));
        refreshAnalysisHistory();
        return;
    }
    if (!removed.value()) {
        statusBar()->showMessage(
            QStringLiteral("分析记录 #%1 已不存在，列表已刷新。")
                .arg(taskId),
            5000);
        refreshAnalysisHistory(adjacentId);
        return;
    }

    refreshAnalysisHistory(adjacentId);
    statusBar()->showMessage(
        QStringLiteral("已永久删除分析记录 #%1 及其特征结果。")
            .arg(taskId),
        5000);
}

void MainWindow::refreshAnalysisHistory(
    std::optional<std::int64_t> selectRecordId)
{
    const auto selectedRecord =
        analysisHistoryModel_->recordAt(
            analysisHistoryView_->currentIndex());
    const std::int64_t selectedId = selectRecordId.has_value()
        ? *selectRecordId
        : selectedRecord == nullptr ? 0 : selectedRecord->id;
    auto history = analysisService_.history();
    if (!history) {
        deleteAnalysisHistoryButton_->setEnabled(false);
        analysisHistoryDetails_->setPlainText(
            QStringLiteral("无法读取分析历史。"));
        const QString reason =
            QString::fromStdString(history.error().message);
        statusBar()->showMessage(
            QStringLiteral("分析历史读取失败：%1").arg(reason),
            5000);
        return;
    }
    analysisHistoryModel_->setRecords(history.value());
    int targetRow = -1;
    for (std::size_t row = 0; row < history.value().size(); ++row) {
        if (history.value()[row].id == selectedId) {
            targetRow = static_cast<int>(row);
            break;
        }
    }
    if (targetRow < 0 && !history.value().empty()) {
        targetRow = 0;
    }
    if (targetRow >= 0) {
        analysisHistoryView_->setCurrentIndex(
            analysisHistoryModel_->index(targetRow, 0));
    } else {
        deleteAnalysisHistoryButton_->setEnabled(false);
        analysisHistoryDetails_->setPlainText(QStringLiteral(
            "尚无分析记录。完成一次分析后，记录会显示在这里。"));
    }
}

void MainWindow::showAnalysisHistory(const QModelIndex& index)
{
    const auto* task = analysisHistoryModel_->recordAt(index);
    if (task == nullptr) {
        analysisHistoryDetails_->clear();
        deleteAnalysisHistoryButton_->setEnabled(false);
        return;
    }
    deleteAnalysisHistoryButton_->setEnabled(
        isTerminalAnalysisStatus(task->status));
    auto features = analysisService_.featureResults(task->id);
    if (!features) {
        analysisHistoryDetails_->setPlainText(
            QStringLiteral("无法读取此任务的特征结果。"));
        return;
    }

    QStringList lines;
    lines << QStringLiteral("任务 ID：%1").arg(task->id);
    lines << QStringLiteral("状态：%1")
                 .arg(analysisStatusText(task->status));
    lines << QStringLiteral("信号：%1").arg(textOrDash(task->signalName));
    lines << QStringLiteral("算法：%1")
                 .arg(textOrDash(task->algorithmName));
    lines << QStringLiteral("算法版本：%1")
                 .arg(textOrDash(task->algorithmVersion));
    lines << QStringLiteral("参数 JSON：%1")
                 .arg(textOrDash(task->parametersJson));
    lines << QStringLiteral("输入哈希：%1")
                 .arg(textOrDash(task->inputHash));
    lines << QStringLiteral("输出哈希：%1")
                 .arg(textOrDash(task->outputHash));
    lines << QStringLiteral("创建时间 (UTC)：%1")
                 .arg(textOrDash(task->createdUtc));
    lines << QStringLiteral("开始时间 (UTC)：%1")
                 .arg(textOrDash(task->startedUtc));
    lines << QStringLiteral("完成时间 (UTC)：%1")
                 .arg(textOrDash(task->finishedUtc));
    lines << QStringLiteral("错误原因：%1")
                 .arg(textOrDash(task->errorMessage));
    if (!task->errorMessage.empty()) {
        lines << QStringLiteral("建议：%1")
                     .arg(actionableSuggestion(
                         QString::fromStdString(task->errorMessage)));
    }
    lines << QString() << QStringLiteral("特征结果：");
    if (features.value().empty()) {
        lines << QStringLiteral("  （无）");
    } else {
        for (const auto& feature : features.value()) {
            const QString value = feature.numericValue.has_value()
                ? QString::number(*feature.numericValue, 'g', 17)
                : textOrDash(feature.valueText);
            const QString unit = feature.unit.empty()
                ? QString()
                : QStringLiteral(" ")
                    + QString::fromStdString(feature.unit);
            lines << QStringLiteral("  %1 = %2%3")
                         .arg(
                             QString::fromStdString(feature.name),
                             value,
                             unit);
        }
    }
    analysisHistoryDetails_->setPlainText(
        lines.join(QLatin1Char('\n')));
}

void MainWindow::exportChart(
    InteractiveChartView* chart,
    const QString& baseName)
{
    QString fileName = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出图表图片"),
        baseName + QStringLiteral(".png"),
        QStringLiteral("PNG 图片 (*.png)"));
    if (fileName.isEmpty()) {
        return;
    }
    if (!fileName.endsWith(
            QStringLiteral(".png"), Qt::CaseInsensitive)) {
        fileName += QStringLiteral(".png");
    }
    QString error;
    if (!chart->saveImage(fileName, &error)) {
        showFailure(
            QStringLiteral("图片导出失败"),
            error,
            QStringLiteral("选择可写目录并使用 .png 扩展名后重试。"));
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("图表已导出到 %1").arg(fileName), 5000);
}

void MainWindow::showFailure(
    const QString& title,
    const QString& reason,
    const QString& suggestion)
{
    taskDetails_->setPlainText(
        QStringLiteral("错误原因：%1\n建议：%2")
            .arg(reason, suggestion));
    QMessageBox::warning(
        this,
        title,
        QStringLiteral("错误原因：%1\n\n建议：%2")
            .arg(reason, suggestion));
}

void MainWindow::runScript()
{
    if (backgroundScriptService_ == nullptr
        || activeScriptTaskId_.has_value()) {
        return;
    }
    const QByteArray utf8 = scriptEditor_->toPlainText().toUtf8();
    if (utf8.trimmed().isEmpty()) {
        scriptStatusLabel_->setText(
            QStringLiteral("脚本为空，请先输入源码。"));
        return;
    }
    clearScriptDiagnostics();
    displayedScriptSource_.reset();
    auto submitted = backgroundScriptService_->submit(
        std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    if (!submitted) {
        const QString reason =
            QString::fromStdString(submitted.error().message);
        showFailure(
            QStringLiteral("脚本无法排队"),
            reason,
            actionableSuggestion(reason));
        return;
    }
    activeScriptTaskId_ = submitted.value();
    scriptStatusLabel_->setText(
        QStringLiteral("脚本任务 #%1 已排队")
            .arg(static_cast<qulonglong>(submitted.value())));
    updateScriptButtons();
    refreshTaskList();
}

void MainWindow::cancelActiveScript()
{
    if (!activeScriptTaskId_.has_value()) {
        return;
    }
    if (scheduler_.cancel(*activeScriptTaskId_)) {
        scriptStatusLabel_->setText(
            QStringLiteral("已请求取消脚本任务 #%1")
                .arg(static_cast<qulonglong>(
                    *activeScriptTaskId_)));
    } else {
        scriptStatusLabel_->setText(
            QStringLiteral("脚本任务已结束，无法取消。"));
    }
}

void MainWindow::updateScriptButtons()
{
    const bool available = backgroundScriptService_ != nullptr;
    const bool running = activeScriptTaskId_.has_value();
    runScriptButton_->setEnabled(available && !running);
    cancelScriptButton_->setEnabled(available && running);
    loadScriptHistoryButton_->setEnabled(available && !running);
    scriptEditor_->setReadOnly(running);
}

void MainWindow::clearScriptDiagnostics()
{
    scriptDiagnosticModel_->clear();
}

void MainWindow::refreshScriptHistory()
{
    if (backgroundScriptService_ == nullptr) {
        scriptHistoryModel_->setRecords({});
        scriptHistoryDetails_->setPlainText(
            QStringLiteral("脚本服务未装配。"));
        return;
    }
    auto submitted = backgroundScriptService_->requestHistory();
    if (!submitted) {
        scriptStatusLabel_->setText(
            QStringLiteral("脚本历史无法排队：%1")
                .arg(QString::fromStdString(
                    submitted.error().message)));
    }
}

void MainWindow::processBackgroundScriptEvents()
{
    if (backgroundScriptService_ == nullptr) {
        return;
    }
    auto events = backgroundScriptService_->takeEvents();
    bool requestHistoryAfterExecution = false;
    for (auto& event : events) {
        if (event.operation
            == signallab::app::BackgroundScriptOperation::History) {
            if (!signallab::task::isTerminal(event.task.status)) {
                continue;
            }
            if (event.task.status
                    == signallab::task::TaskStatus::Succeeded
                && event.history) {
                const auto* selected = scriptHistoryModel_->recordAt(
                    scriptHistoryView_->currentIndex());
                const std::int64_t selectedId =
                    selected == nullptr ? 0 : selected->id;
                int rowToSelect = -1;
                for (std::size_t row = 0;
                     row < event.history->size();
                     ++row) {
                    if ((*event.history)[row].id == selectedId) {
                        rowToSelect = static_cast<int>(row);
                        break;
                    }
                }
                const bool hasHistory = !event.history->empty();
                scriptHistoryModel_->setRecords(
                    std::move(*event.history));
                if (rowToSelect < 0 && hasHistory) {
                    rowToSelect = 0;
                }
                if (rowToSelect >= 0) {
                    scriptHistoryView_->setCurrentIndex(
                        scriptHistoryModel_->index(rowToSelect, 0));
                } else {
                    scriptHistoryDetails_->setPlainText(
                        QStringLiteral("尚无脚本执行历史。"));
                }
            } else if (
                event.task.status
                == signallab::task::TaskStatus::Failed) {
                scriptStatusLabel_->setText(
                    QStringLiteral("脚本历史读取失败：%1")
                        .arg(QString::fromStdString(
                            event.task.errorMessage)));
            }
            continue;
        }

        if (!signallab::task::isTerminal(event.task.status)) {
            if (activeScriptTaskId_.has_value()
                && *activeScriptTaskId_ == event.task.id) {
                scriptStatusLabel_->setText(
                    QStringLiteral("脚本任务 #%1：%2% %3")
                        .arg(static_cast<qulonglong>(event.task.id))
                        .arg(event.task.progressPercent)
                        .arg(QString::fromStdString(
                            event.task.progressMessage)));
            }
            continue;
        }

        if (event.result) {
            showScriptResult(event);
        } else if (
            event.task.status
            == signallab::task::TaskStatus::Cancelled) {
            scriptOutput_->setPlainText(QStringLiteral(
                "脚本在开始执行前已取消，因此没有产生诊断、变量或绘图结果。"));
            scriptStatusLabel_->setText(
                QStringLiteral("脚本任务 #%1 已取消")
                    .arg(static_cast<qulonglong>(event.task.id)));
        } else {
            const QString reason =
                QString::fromStdString(event.task.errorMessage);
            scriptOutput_->setPlainText(
                QStringLiteral(
                    "脚本基础设施任务未返回结构化结果。\n"
                    "错误原因：%1\n建议：%2")
                    .arg(reason, actionableSuggestion(reason)));
            scriptStatusLabel_->setText(
                QStringLiteral("脚本任务 #%1 失败")
                    .arg(static_cast<qulonglong>(event.task.id)));
        }
        if (activeScriptTaskId_.has_value()
            && *activeScriptTaskId_ == event.task.id) {
            activeScriptTaskId_.reset();
        }
        requestHistoryAfterExecution = true;
    }
    updateScriptButtons();
    refreshTaskList();
    if (requestHistoryAfterExecution) {
        refreshScriptHistory();
    }
}

void MainWindow::showScriptResult(
    const signallab::app::BackgroundScriptEvent& event)
{
    if (!event.result) {
        return;
    }
    auto& result = *event.result;
    displayedScriptSource_ = event.source;
    clearScriptDiagnostics();

    const auto addDiagnostic = [this](
                                   const QString& severity,
                                   const QString& code,
                                   std::size_t line,
                                   std::size_t column,
                                   const QString& message) {
        auto* item = new QStandardItem(
            QStringLiteral("%1  %2  %3:%4  %5")
                .arg(severity, code)
                .arg(static_cast<qulonglong>(line))
                .arg(static_cast<qulonglong>(column))
                .arg(message));
        item->setData(
            QVariant::fromValue<qulonglong>(line),
            kDiagnosticLineRole);
        item->setData(
            QVariant::fromValue<qulonglong>(column),
            kDiagnosticColumnRole);
        item->setToolTip(
            QStringLiteral("%1 %2，位置 %3:%4\n%5\n建议：%6")
                .arg(severity, code)
                .arg(static_cast<qulonglong>(line))
                .arg(static_cast<qulonglong>(column))
                .arg(message, actionableSuggestion(code)));
        scriptDiagnosticModel_->appendRow(item);
    };
    for (const auto& diagnostic : result.diagnostics) {
        addDiagnostic(
            diagnostic.severity
                    == signallab::parser::DiagnosticSeverity::Error
                ? QStringLiteral("错误")
                : QStringLiteral("警告"),
            QString::fromStdString(diagnostic.code),
            diagnostic.span.begin.line,
            diagnostic.span.begin.column,
            QString::fromStdString(diagnostic.message));
    }
    if (result.runtimeIssue.has_value()) {
        addDiagnostic(
            QStringLiteral("错误"),
            QString::fromStdString(result.runtimeIssue->code),
            result.runtimeIssue->span.begin.line,
            result.runtimeIssue->span.begin.column,
            QString::fromStdString(result.runtimeIssue->message));
    }

    std::size_t acceptedSignalCount = 0U;
    std::optional<std::int64_t> firstOpenedRecordId;
    for (auto& entry : result.output.openedSignals) {
        const std::int64_t recordId = entry.recordId;
        const auto accepted = importService_.acceptImportedSignal(
            std::move(entry));
        if (!accepted.has_value()) {
            continue;
        }
        ++acceptedSignalCount;
        if (!firstOpenedRecordId.has_value()) {
            firstOpenedRecordId = recordId;
        }
    }
    result.output.openedSignals.clear();
    if (firstOpenedRecordId.has_value()) {
        QWidget* const currentWorkspace = workspaceTabs_->currentWidget();
        refreshSignalList(firstOpenedRecordId);
        if (currentWorkspace != nullptr) {
            workspaceTabs_->setCurrentWidget(currentWorkspace);
        }
    }

    QStringList lines;
    lines << QStringLiteral("任务 ID：%1")
                 .arg(static_cast<qulonglong>(event.task.id));
    lines << QStringLiteral("历史 ID：%1").arg(result.historyId);
    lines << QStringLiteral("状态：%1")
                 .arg(scriptStatusText(result.status));
    if (result.status
            != signallab::app::ScriptExecutionStatus::Succeeded
        && (!result.output.plots.empty()
            || !result.output.outputs.empty())) {
        lines << QStringLiteral(
            "注意：以下为失败或取消前已经提交的部分结果，不会自动回滚。");
    }
    lines << QStringLiteral("载入并接纳信号：%1 个")
                 .arg(static_cast<qulonglong>(acceptedSignalCount));
    lines << QStringLiteral("变量：");
    if (result.output.variables.empty()) {
        lines << QStringLiteral("  （无）");
    } else {
        for (const auto& [name, value] : result.output.variables) {
            lines << QStringLiteral("  %1 = %2")
                         .arg(
                             QString::fromStdString(name),
                             scriptValueText(value));
        }
    }
    lines << QStringLiteral("输出文件：");
    if (result.output.outputs.empty()) {
        lines << QStringLiteral("  （无）");
    } else {
        for (const auto& output : result.output.outputs) {
            lines << QStringLiteral("  %1")
                         .arg(QString::fromStdWString(
                             output.wstring()));
        }
    }
    lines << QStringLiteral("绘图请求：%1 个")
                 .arg(static_cast<qulonglong>(
                     result.output.plots.size()));
    if (!result.astText.empty()) {
        lines << QString() << QStringLiteral("AST：")
              << QString::fromStdString(result.astText);
    }
    scriptOutput_->setPlainText(lines.join(QLatin1Char('\n')));
    scriptStatusLabel_->setText(
        QStringLiteral("脚本任务 #%1：%2")
            .arg(static_cast<qulonglong>(event.task.id))
            .arg(scriptStatusText(result.status)));

    while (scriptPlotTabs_->count() > 0) {
        QWidget* page = scriptPlotTabs_->widget(0);
        scriptPlotTabs_->removeTab(0);
        delete page;
    }
    for (std::size_t plotIndex = 0;
         plotIndex < result.output.plots.size();
         ++plotIndex) {
        const auto& request = result.output.plots[plotIndex];
        auto* page = new QWidget(scriptPlotTabs_);
        auto* layout = new QVBoxLayout(page);
        auto* tools = new QHBoxLayout;
        auto* resetButton = new QPushButton(
            QStringLiteral("复位视图"), page);
        auto* exportButton = new QPushButton(
            QStringLiteral("导出 PNG"), page);
        auto* cursorLabel = new QLabel(
            QStringLiteral("游标：移入图表查看坐标"), page);
        tools->addWidget(resetButton);
        tools->addWidget(exportButton);
        tools->addWidget(cursorLabel, 1);
        layout->addLayout(tools);

        auto* chart = new QChart;
        auto* series = new QLineSeries(chart);
        auto* xAxis = new QValueAxis(chart);
        auto* yAxis = new QValueAxis(chart);
        chart->addSeries(series);
        chart->addAxis(xAxis, Qt::AlignBottom);
        chart->addAxis(yAxis, Qt::AlignLeft);
        series->attachAxis(xAxis);
        series->attachAxis(yAxis);
        chart->legend()->hide();
        chart->setTitle(QString::fromStdString(request.title));
        auto* view = new InteractiveChartView(chart, page);
        view->setObjectName(
            QStringLiteral("scriptPlotView%1")
                .arg(static_cast<qulonglong>(plotIndex)));
        layout->addWidget(view, 1);

        if (const auto* signalValue =
                std::get_if<
                    signallab::app::script_runtime::SignalValue>(
                    &request.data);
            signalValue != nullptr && *signalValue) {
            const auto signal = *signalValue;
            xAxis->setTitleText(QStringLiteral("时间 (s)"));
            yAxis->setTitleText(QString::fromStdString(
                signal->metadata().unit));
            xAxis->setRange(
                0.0,
                std::max(
                    signal->durationSeconds(),
                    1.0 / signal->sampleRate()));
            const auto redraw = [view, series, signal, xAxis]() {
                const std::size_t sampleCount = signal->sampleCount();
                const double firstTime = std::clamp(
                    xAxis->min(), 0.0, signal->durationSeconds());
                const double lastTime = std::clamp(
                    xAxis->max(), 0.0, signal->durationSeconds());
                std::size_t firstIndex = std::min(
                    static_cast<std::size_t>(std::floor(
                        firstTime * signal->sampleRate())),
                    sampleCount - 1U);
                std::size_t pastLastIndex = std::min(
                    static_cast<std::size_t>(std::ceil(
                        lastTime * signal->sampleRate())) + 1U,
                    sampleCount);
                if (pastLastIndex <= firstIndex) {
                    pastLastIndex = std::min(
                        firstIndex + 1U, sampleCount);
                }
                const auto display =
                    signallab::app::makeDisplayPoints(
                        *signal,
                        static_cast<std::size_t>(
                            std::max(
                                view->viewport()->width(), 1)),
                        firstIndex,
                        pastLastIndex);
                series->replace(toTimePoints(display));
            };
            redraw();
            const auto range = yRange(series->points());
            yAxis->setRange(range.first, range.second);
            view->captureHomeRanges();
            auto* redrawTimer = new QTimer(view);
            redrawTimer->setSingleShot(true);
            redrawTimer->setInterval(80);
            connect(
                redrawTimer,
                &QTimer::timeout,
                view,
                redraw);
            connect(
                xAxis,
                &QValueAxis::rangeChanged,
                view,
                [redrawTimer](double, double) {
                    redrawTimer->start();
                });
            view->setResizeSettledCallback([redrawTimer]() {
                redrawTimer->start();
            });
            view->setCursorCallback(
                [cursorLabel, signal](QPointF point, bool visible) {
                    if (!visible) {
                        cursorLabel->setText(
                            QStringLiteral("游标：移入图表查看坐标"));
                        return;
                    }
                    const auto samples = signal->samples();
                    const auto index = static_cast<std::size_t>(
                        std::clamp(
                            std::llround(
                                point.x() * signal->sampleRate()),
                            0LL,
                            static_cast<long long>(
                                samples.size() - 1U)));
                    cursorLabel->setText(
                        QStringLiteral("t=%1 s，值=%2")
                            .arg(
                                signal->sampleTimeSeconds(index),
                                0,
                                'g',
                                9)
                            .arg(samples[index], 0, 'g', 9));
                });
        } else if (
            const auto* spectrumValue =
                std::get_if<
                    signallab::app::script_runtime::SpectrumValuePtr>(
                    &request.data);
            spectrumValue != nullptr && *spectrumValue
            && (*spectrumValue)->analysis) {
            const auto spectrum = *spectrumValue;
            xAxis->setTitleText(QStringLiteral("频率 (Hz)"));
            yAxis->setTitleText(QStringLiteral("幅值"));
            const auto& frequencies =
                spectrum->analysis->spectrum.frequenciesHz;
            xAxis->setRange(
                0.0,
                frequencies.empty()
                    ? 1.0
                    : std::max(frequencies.back(), 1.0));
            const auto redraw = [
                                    view,
                                    series,
                                    spectrum,
                                    xAxis]() {
                const auto& fullSpectrum =
                    spectrum->analysis->spectrum;
                const auto& frequencies =
                    fullSpectrum.frequenciesHz;
                if (frequencies.empty()) {
                    series->clear();
                    return;
                }
                const auto first = std::lower_bound(
                    frequencies.begin(),
                    frequencies.end(),
                    xAxis->min());
                const auto pastLast = std::upper_bound(
                    frequencies.begin(),
                    frequencies.end(),
                    xAxis->max());
                std::size_t firstIndex = static_cast<std::size_t>(
                    std::distance(frequencies.begin(), first));
                std::size_t pastLastIndex = static_cast<std::size_t>(
                    std::distance(frequencies.begin(), pastLast));
                if (firstIndex >= frequencies.size()) {
                    firstIndex = frequencies.size() - 1U;
                }
                if (pastLastIndex <= firstIndex) {
                    pastLastIndex = std::min(
                        firstIndex + 1U, frequencies.size());
                }
                const auto display =
                    signallab::app::makeSpectrumDisplayPoints(
                        fullSpectrum,
                        static_cast<std::size_t>(
                            std::max(
                                view->viewport()->width(), 1)),
                        firstIndex,
                        pastLastIndex);
                series->replace(toSpectrumPoints(display));
            };
            redraw();
            const auto range = yRange(series->points());
            yAxis->setRange(std::min(0.0, range.first), range.second);
            view->captureHomeRanges();
            auto* redrawTimer = new QTimer(view);
            redrawTimer->setSingleShot(true);
            redrawTimer->setInterval(80);
            connect(
                redrawTimer,
                &QTimer::timeout,
                view,
                redraw);
            connect(
                xAxis,
                &QValueAxis::rangeChanged,
                view,
                [redrawTimer](double, double) {
                    redrawTimer->start();
                });
            view->setResizeSettledCallback([redrawTimer]() {
                redrawTimer->start();
            });
            view->setCursorCallback(
                [cursorLabel, spectrum](
                    QPointF point,
                    bool visible) {
                    if (!visible) {
                        cursorLabel->setText(
                            QStringLiteral("游标：移入图表查看坐标"));
                        return;
                    }
                    const auto& frequencies =
                        spectrum->analysis->spectrum.frequenciesHz;
                    const auto& amplitudes =
                        spectrum->analysis->spectrum.amplitudes;
                    if (frequencies.empty()) {
                        return;
                    }
                    auto found = std::lower_bound(
                        frequencies.begin(),
                        frequencies.end(),
                        point.x());
                    const std::size_t index =
                        found == frequencies.end()
                        ? frequencies.size() - 1U
                        : static_cast<std::size_t>(
                            std::distance(
                                frequencies.begin(), found));
                    cursorLabel->setText(
                        QStringLiteral("f=%1 Hz，幅值=%2")
                            .arg(frequencies[index], 0, 'g', 9)
                            .arg(amplitudes[index], 0, 'g', 9));
                });
        }
        connect(resetButton, &QPushButton::clicked, view, [view]() {
            view->resetView();
        });
        connect(
            exportButton,
            &QPushButton::clicked,
            this,
            [this, view, plotIndex]() {
                exportChart(
                    view,
                    QStringLiteral("script-plot-%1")
                        .arg(static_cast<qulonglong>(
                            plotIndex + 1U)));
            });
        scriptPlotTabs_->addTab(
            page,
            QString::fromStdString(request.title).isEmpty()
                ? QStringLiteral("绘图 %1")
                    .arg(static_cast<qulonglong>(
                        plotIndex + 1U))
                : QString::fromStdString(request.title));
    }
}

void MainWindow::jumpToScriptDiagnostic(const QModelIndex& index)
{
    if (!index.isValid()) {
        return;
    }
    const std::size_t line = static_cast<std::size_t>(
        index.data(kDiagnosticLineRole).toULongLong());
    const std::size_t column = static_cast<std::size_t>(
        index.data(kDiagnosticColumnRole).toULongLong());
    if (line == 0U || column == 0U) {
        return;
    }

    if (displayedScriptSource_) {
        const QByteArray current = scriptEditor_->toPlainText().toUtf8();
        const std::string currentSource(
            current.constData(),
            static_cast<std::size_t>(current.size()));
        if (currentSource != *displayedScriptSource_) {
            const QSignalBlocker blocker(scriptEditor_);
            scriptEditor_->setPlainText(QString::fromUtf8(
                displayedScriptSource_->data(),
                static_cast<qsizetype>(
                    displayedScriptSource_->size())));
        }
    }

    const QTextBlock block = scriptEditor_->document()->findBlockByNumber(
        static_cast<int>(line - 1U));
    if (!block.isValid()) {
        scriptStatusLabel_->setText(
            QStringLiteral("诊断位置超出当前源码范围。"));
        return;
    }
    QTextCursor cursor(block);
    cursor.setPosition(
        block.position()
        + utf16OffsetForCodePointColumn(block.text(), column));
    scriptEditor_->setTextCursor(cursor);
    scriptEditor_->centerCursor();
    scriptEditor_->setFocus();
    scriptStatusLabel_->setText(
        QStringLiteral("已跳转到 %1:%2")
            .arg(static_cast<qulonglong>(line))
            .arg(static_cast<qulonglong>(column)));
}

void MainWindow::showScriptHistory(const QModelIndex& index)
{
    const auto* record = scriptHistoryModel_->recordAt(index);
    if (record == nullptr) {
        scriptHistoryDetails_->clear();
        return;
    }
    QString location = QStringLiteral("—");
    if (record->errorLocation.has_value()) {
        location = QStringLiteral("%1:%2")
            .arg(record->errorLocation->line)
            .arg(record->errorLocation->column);
    }
    QStringList lines;
    lines << QStringLiteral("历史 ID：%1").arg(record->id);
    lines << QStringLiteral("状态：%1")
                 .arg(index.data(
                     signallab::gui::ScriptHistoryModel::StatusRole)
                     .toString());
    lines << QStringLiteral("源码 SHA-256：%1")
                 .arg(textOrDash(record->sourceHash));
    lines << QStringLiteral("创建时间 (UTC)：%1")
                 .arg(textOrDash(record->createdUtc));
    lines << QStringLiteral("完成时间 (UTC)：%1")
                 .arg(textOrDash(record->finishedUtc));
    lines << QStringLiteral("错误位置：%1").arg(location);
    lines << QStringLiteral("错误原因：%1")
                 .arg(textOrDash(record->errorMessage));
    if (!record->errorMessage.empty()) {
        lines << QStringLiteral("建议：%1")
                     .arg(actionableSuggestion(
                         QString::fromStdString(
                             record->errorMessage)));
    }
    lines << QString() << QStringLiteral("源码：")
          << QString::fromStdString(record->scriptText);
    scriptHistoryDetails_->setPlainText(
        lines.join(QLatin1Char('\n')));
    loadScriptHistoryButton_->setEnabled(
        !activeScriptTaskId_.has_value());
}

void MainWindow::loadSelectedScriptHistory()
{
    const auto* record = scriptHistoryModel_->recordAt(
        scriptHistoryView_->currentIndex());
    if (record == nullptr || activeScriptTaskId_.has_value()) {
        return;
    }
    const QSignalBlocker blocker(scriptEditor_);
    scriptEditor_->setPlainText(
        QString::fromStdString(record->scriptText));
    displayedScriptSource_ =
        std::make_shared<const std::string>(record->scriptText);
    clearScriptDiagnostics();
    scriptStatusLabel_->setText(
        QStringLiteral("已载入历史 #%1 的源码；可编辑后重新运行。")
            .arg(record->id));
}
