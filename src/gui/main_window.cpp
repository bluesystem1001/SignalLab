#include "main_window.hpp"

#include "interactive_chart_view.hpp"
#include "stage7_models.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QMessageBox>
#include <QMetaObject>
#include <QModelIndex>
#include <QPlainTextEdit>
#include <QPointer>
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

namespace {

constexpr int kDiagnosticLineRole = Qt::UserRole + 1;
constexpr int kDiagnosticColumnRole = Qt::UserRole + 2;

std::filesystem::path toPath(const QString& text)
{
    return std::filesystem::path(text.toStdWString());
}

QString previewDescription(
    const signallab::repository::ImportPreview& preview)
{
    QString description = QStringLiteral("样本数：%1\n通道数：%2")
        .arg(static_cast<qulonglong>(preview.sampleCount))
        .arg(static_cast<qulonglong>(preview.channelCount));
    if (preview.sampleRate.has_value()) {
        description += QStringLiteral("\n采样率：%1 Hz")
            .arg(*preview.sampleRate, 0, 'f', 3);
    }
    if (!preview.columnNames.empty()) {
        QStringList names;
        for (const auto& name : preview.columnNames) {
            names.push_back(QString::fromStdString(name));
        }
        description += QStringLiteral("\n列：") + names.join(QStringLiteral(" | "));
    }
    if (!preview.rows.empty()) {
        description += QStringLiteral("\n\n前几行：");
        for (const auto& row : preview.rows) {
            QStringList cells;
            for (const auto& cell : row) {
                cells.push_back(QString::fromStdString(cell));
            }
            description += QStringLiteral("\n")
                + cells.join(QStringLiteral(" | "));
        }
    }
    return description;
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
      nameValue_(nullptr),
      sampleRateValue_(nullptr),
      sampleCountValue_(nullptr),
      durationValue_(nullptr),
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
      timeXAxis_(nullptr),
      timeYAxis_(nullptr),
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
      taskListView_(nullptr),
      taskDetails_(nullptr),
      cancelTaskButton_(nullptr),
      retryTaskButton_(nullptr),
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
    auto* importCsvButton = new QPushButton(
        QStringLiteral("导入 CSV"), projectPanel);
    importCsvButton->setObjectName(QStringLiteral("importCsvButton"));
    auto* importWavButton = new QPushButton(
        QStringLiteral("导入 WAV"), projectPanel);
    importWavButton->setObjectName(QStringLiteral("importWavButton"));
    projectLayout->addWidget(importCsvButton);
    projectLayout->addWidget(importWavButton);
    projectPanel->setMinimumWidth(210);
    mainSplitter->addWidget(projectPanel);

    workspaceTabs_ = new QTabWidget(mainSplitter);
    workspaceTabs_->setObjectName(QStringLiteral("workspaceTabs"));

    auto* timePage = new QWidget(workspaceTabs_);
    auto* timeLayout = new QVBoxLayout(timePage);
    auto* timeTools = new QHBoxLayout;
    auto* resetTimeButton = new QPushButton(
        QStringLiteral("复位视图"), timePage);
    auto* exportTimeButton = new QPushButton(
        QStringLiteral("导出 PNG"), timePage);
    timeCursorLabel_ = new QLabel(
        QStringLiteral("游标：移入图表查看精确样本"), timePage);
    timeCursorLabel_->setObjectName(QStringLiteral("timeCursorLabel"));
    timeSelectionLabel_ = new QLabel(
        QStringLiteral("局部选择：按住 Shift 拖动"), timePage);
    timeTools->addWidget(resetTimeButton);
    timeTools->addWidget(exportTimeButton);
    timeTools->addWidget(timeCursorLabel_, 1);
    timeTools->addWidget(timeSelectionLabel_);
    timeLayout->addLayout(timeTools);
    auto* timeChart = new QChart;
    timeSeries_ = new QLineSeries(timeChart);
    timeXAxis_ = new QValueAxis(timeChart);
    timeYAxis_ = new QValueAxis(timeChart);
    timeXAxis_->setTitleText(QStringLiteral("时间 (s)"));
    timeYAxis_->setTitleText(QStringLiteral("幅值"));
    timeChart->addSeries(timeSeries_);
    timeChart->addAxis(timeXAxis_, Qt::AlignBottom);
    timeChart->addAxis(timeYAxis_, Qt::AlignLeft);
    timeSeries_->attachAxis(timeXAxis_);
    timeSeries_->attachAxis(timeYAxis_);
    timeChart->legend()->hide();
    timeChart->setTitle(QStringLiteral("请选择一个信号"));
    timeChartView_ = new InteractiveChartView(timeChart, timePage);
    timeChartView_->setObjectName(QStringLiteral("timeChartView"));
    timeChartView_->setAccessibleName(QStringLiteral("时域波形图"));
    timeLayout->addWidget(timeChartView_, 1);
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
    cancelTaskButton_->setObjectName(QStringLiteral("cancelTaskButton"));
    retryTaskButton_->setObjectName(QStringLiteral("retryTaskButton"));
    cancelTaskButton_->setEnabled(false);
    retryTaskButton_->setEnabled(false);
    taskButtons->addWidget(cancelTaskButton_);
    taskButtons->addWidget(retryTaskButton_);
    taskButtons->addStretch();
    taskLayout->addLayout(taskButtons);
    workspaceTabs_->addTab(taskPage, QStringLiteral("任务中心"));

    auto* historyPage = new QWidget(workspaceTabs_);
    auto* historyLayout = new QHBoxLayout(historyPage);
    analysisHistoryView_ = new QListView(historyPage);
    analysisHistoryView_->setObjectName(
        QStringLiteral("analysisHistoryView"));
    analysisHistoryView_->setModel(analysisHistoryModel_);
    analysisHistoryView_->setMinimumWidth(300);
    analysisHistoryDetails_ = new QTextEdit(historyPage);
    analysisHistoryDetails_->setObjectName(
        QStringLiteral("analysisHistoryDetails"));
    analysisHistoryDetails_->setReadOnly(true);
    analysisHistoryDetails_->setPlaceholderText(
        QStringLiteral("选择分析记录查看参数、版本、哈希和特征结果。"));
    historyLayout->addWidget(analysisHistoryView_);
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
    connect(cancelTaskButton_, &QPushButton::clicked, this, [this]() {
        cancelSelectedTask();
    });
    connect(retryTaskButton_, &QPushButton::clicked, this, [this]() {
        retrySelectedTask();
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
    connect(resetTimeButton, &QPushButton::clicked, this, [this]() {
        timeChartView_->resetView();
        renderTimeSignal(false);
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
        [this](double, double) { timeRedrawTimer_->start(); });
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
        const auto samples = entry->signal.samples();
        const double rawIndex = point.x() * entry->signal.sampleRate();
        const auto index = static_cast<std::size_t>(std::clamp(
            std::llround(rawIndex),
            0LL,
            static_cast<long long>(samples.size() - 1U)));
        timeCursorLabel_->setText(
            QStringLiteral("游标：t=%1 s，样本 #%2，值=%3")
                .arg(entry->signal.sampleTimeSeconds(index), 0, 'g', 9)
                .arg(static_cast<qulonglong>(index))
                .arg(samples[index], 0, 'g', 9));
    });
    timeChartView_->setSelectionCallback(
        [this](double first, double last) {
            const auto* entry = selectedSignal();
            if (entry == nullptr) {
                return;
            }
            const double duration = std::max(last - first, 0.0);
            const auto count = static_cast<qulonglong>(std::max(
                std::llround(duration * entry->signal.sampleRate()),
                0LL));
            timeSelectionLabel_->setText(
                QStringLiteral("选择：%1–%2 s（约 %3 点）")
                    .arg(first, 0, 'g', 7)
                    .arg(last, 0, 'g', 7)
                    .arg(count));
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
    updateScriptButtons();
    if (signalModel_->rowCount() > 0) {
        QTimer::singleShot(0, this, [this]() {
            signalListView_->setCurrentIndex(signalModel_->index(0, 0));
        });
    }
    statusBar()->showMessage(QStringLiteral(
        "文件读取、FFT/FIR、脚本执行和 SQL 写入均在后台运行；"
        "图表支持缩放、游标、局部选择和 PNG 导出。"));
}

MainWindow::~MainWindow()
{
    if (backgroundScriptService_ != nullptr) {
        backgroundScriptService_->clearEventNotifier();
    }
    backgroundAnalysisService_.clearEventNotifier();
    backgroundFileService_.clearEventNotifier();
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

void MainWindow::importCsv()
{
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
    bool accepted = false;
    const int defaultDelimiter =
        QFileInfo(fileName).suffix().compare(
            QStringLiteral("tsv"), Qt::CaseInsensitive) == 0
        ? 1
        : 0;
    const QString delimiter = QInputDialog::getItem(
        this,
        QStringLiteral("CSV 分隔符"),
        QStringLiteral("选择列分隔符："),
        {QStringLiteral("逗号 (,)"), QStringLiteral("制表符 (Tab)")},
        defaultDelimiter,
        false,
        &accepted);
    if (!accepted) {
        return;
    }
    options.delimiter = delimiter.startsWith(QStringLiteral("制表符"))
        ? signallab::repository::CsvDelimiter::Tab
        : signallab::repository::CsvDelimiter::Comma;

    const auto headerAnswer = QMessageBox::question(
        this,
        QStringLiteral("CSV 表头"),
        QStringLiteral("第一行是否为列名？"),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
    if (headerAnswer == QMessageBox::Cancel) {
        return;
    }
    options.hasHeader = headerAnswer == QMessageBox::Yes;

    const int valueColumn = QInputDialog::getInt(
        this,
        QStringLiteral("数值列"),
        QStringLiteral("请输入数值列编号（从 1 开始）："),
        2,
        1,
        1024,
        1,
        &accepted);
    if (!accepted) {
        return;
    }
    options.valueColumn = static_cast<std::size_t>(valueColumn - 1);

    const auto timeAnswer = QMessageBox::question(
        this,
        QStringLiteral("时间列"),
        QStringLiteral("文件中是否有以秒为单位的等间隔时间列？"),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
    if (timeAnswer == QMessageBox::Cancel) {
        return;
    }
    if (timeAnswer == QMessageBox::Yes) {
        const int timeColumn = QInputDialog::getInt(
            this,
            QStringLiteral("时间列"),
            QStringLiteral("请输入时间列编号（从 1 开始）："),
            1,
            1,
            1024,
            1,
            &accepted);
        if (!accepted) {
            return;
        }
        options.timeColumn = static_cast<std::size_t>(timeColumn - 1);
    } else {
        options.timeColumn.reset();
        options.sampleRateIfNoTimeColumn = QInputDialog::getDouble(
            this,
            QStringLiteral("采样率"),
            QStringLiteral("请输入采样率 (Hz)："),
            48'000.0,
            0.001,
            1'000'000'000.0,
            3,
            &accepted);
        if (!accepted) {
            return;
        }
    }

    const QFileInfo information(fileName);
    options.metadata.name =
        information.completeBaseName().toStdString();
    options.metadata.unit = "幅值";
    options.metadata.source = fileName.toStdString();

    auto submitted = backgroundFileService_.previewCsv(
        toPath(fileName), options);
    if (!submitted) {
        showFailure(
            QStringLiteral("CSV 预览无法排队"),
            QString::fromStdString(submitted.error().message),
            QStringLiteral("确认任务调度器仍在运行后重试。"));
        return;
    }
    csvPreviewOptions_.insert_or_assign(submitted.value(), options);
    refreshTaskList();
    statusBar()->showMessage(
        QStringLiteral("CSV 预览任务 #%1 已排队")
            .arg(static_cast<qulonglong>(submitted.value())),
        4000);
}

void MainWindow::importWav()
{
    const QString fileName = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择 WAV 文件"),
        {},
        QStringLiteral("WAV 文件 (*.wav);;所有文件 (*.*)"));
    if (fileName.isEmpty()) {
        return;
    }

    auto submitted = backgroundFileService_.previewWav(toPath(fileName));
    if (!submitted) {
        showFailure(
            QStringLiteral("WAV 预览无法排队"),
            QString::fromStdString(submitted.error().message),
            QStringLiteral("确认任务调度器仍在运行后重试。"));
        return;
    }
    refreshTaskList();
    statusBar()->showMessage(
        QStringLiteral("WAV 预览任务 #%1 已排队")
            .arg(static_cast<qulonglong>(submitted.value())),
        4000);
}

void MainWindow::analyzeSelectedSignal()
{
    const auto* entry = selectedSignal();
    if (entry == nullptr || !analyzeButton_->isEnabled()) {
        statusBar()->showMessage(
            QStringLiteral("请先选择信号并修正分析参数。"), 4000);
        return;
    }

    const signallab::app::AnalysisOptions options = analysisOptions();
    auto submitted = backgroundAnalysisService_.submit(
        entry->recordId, options);
    if (!submitted) {
        const QString reason =
            QString::fromStdString(submitted.error().message);
        showFailure(
            QStringLiteral("分析无法排队"),
            reason,
            actionableSuggestion(reason));
        return;
    }
    refreshTaskList();
    statusBar()->showMessage(
        QStringLiteral("分析任务 #%1 已排队，可在任务中心取消")
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
}

void MainWindow::processBackgroundFileEvents()
{
    auto events = backgroundFileService_.takeEvents();
    for (auto& event : events) {
        if (!signallab::task::isTerminal(event.task.status)) {
            continue;
        }
        if (event.task.status == signallab::task::TaskStatus::Cancelled) {
            statusBar()->showMessage(
                QStringLiteral("文件任务 #%1 已取消")
                    .arg(static_cast<qulonglong>(event.task.id)),
                4000);
            continue;
        }
        if (event.task.status == signallab::task::TaskStatus::Failed) {
            const QString reason =
                QString::fromStdString(event.task.errorMessage);
            showFailure(
                QStringLiteral("后台文件任务失败"),
                reason,
                actionableSuggestion(reason));
            continue;
        }

        if (event.operation
            == signallab::app::BackgroundFileOperation::PreviewCsv) {
            const auto options = csvPreviewOptions_.find(event.task.id);
            if (!event.preview || options == csvPreviewOptions_.end()) {
                showFailure(
                    QStringLiteral("CSV 预览失败"),
                    QStringLiteral("预览结果缺少文件信息或导入参数。"),
                    QStringLiteral("请重新选择文件并再次预览。"));
                continue;
            }
            if (QMessageBox::question(
                    this,
                    QStringLiteral("确认导入 CSV"),
                    previewDescription(*event.preview)
                        + QStringLiteral("\n\n确认导入吗？"))
                != QMessageBox::Yes) {
                continue;
            }
            auto submitted = backgroundFileService_.importCsv(
                event.sourcePath, options->second);
            if (!submitted) {
                const QString reason =
                    QString::fromStdString(submitted.error().message);
                showFailure(
                    QStringLiteral("CSV 导入无法排队"),
                    reason,
                    actionableSuggestion(reason));
            } else {
                statusBar()->showMessage(
                    QStringLiteral("CSV 导入任务 #%1 已排队")
                        .arg(static_cast<qulonglong>(
                            submitted.value())),
                    4000);
            }
            continue;
        }

        if (event.operation
            == signallab::app::BackgroundFileOperation::PreviewWav) {
            if (!event.preview) {
                showFailure(
                    QStringLiteral("WAV 预览失败"),
                    QStringLiteral("预览任务没有返回文件信息。"),
                    QStringLiteral("请重新选择 WAV 文件后重试。"));
                continue;
            }

            signallab::repository::WavImportOptions options;
            const QString sourceName =
                QString::fromStdWString(event.sourcePath.wstring());
            const QFileInfo information(sourceName);
            options.metadata.name =
                information.completeBaseName().toStdString();
            options.metadata.unit = "幅值";
            options.metadata.source = sourceName.toStdString();

            QString selectedChannel = QStringLiteral("单声道");
            if (event.preview->channelCount == 2) {
                bool accepted = false;
                const QString channel = QInputDialog::getItem(
                    this,
                    QStringLiteral("立体声通道"),
                    QStringLiteral("选择要导入的通道："),
                    {QStringLiteral("左右平均"),
                     QStringLiteral("左声道"),
                     QStringLiteral("右声道")},
                    0,
                    false,
                    &accepted);
                if (!accepted) {
                    continue;
                }
                if (channel == QStringLiteral("左声道")) {
                    options.channel =
                        signallab::repository::StereoChannel::Left;
                } else if (channel == QStringLiteral("右声道")) {
                    options.channel =
                        signallab::repository::StereoChannel::Right;
                } else {
                    options.channel =
                        signallab::repository::StereoChannel::Average;
                }
                selectedChannel = channel;
            }
            if (QMessageBox::question(
                    this,
                    QStringLiteral("确认导入 WAV"),
                    previewDescription(*event.preview)
                        + QStringLiteral("\n导入通道：")
                        + selectedChannel
                        + QStringLiteral("\n\n确认导入吗？"))
                != QMessageBox::Yes) {
                continue;
            }
            auto submitted = backgroundFileService_.importWav(
                event.sourcePath, options);
            if (!submitted) {
                const QString reason =
                    QString::fromStdString(submitted.error().message);
                showFailure(
                    QStringLiteral("WAV 导入无法排队"),
                    reason,
                    actionableSuggestion(reason));
            } else {
                statusBar()->showMessage(
                    QStringLiteral("WAV 导入任务 #%1 已排队")
                        .arg(static_cast<qulonglong>(
                            submitted.value())),
                    4000);
            }
            continue;
        }

        if (!event.importedSignal) {
            showFailure(
                QStringLiteral("后台导入失败"),
                QStringLiteral("导入任务报告成功，但没有返回信号。"),
                QStringLiteral("请重新导入；若问题持续出现，请保留任务记录。"));
            continue;
        }
        const std::int64_t recordId = event.importedSignal->recordId;
        static_cast<void>(importService_.acceptImportedSignal(
            std::move(*event.importedSignal)));
        refreshSignalList(recordId);
        statusBar()->showMessage(
            event.operation
                    == signallab::app::BackgroundFileOperation::ImportCsv
                ? QStringLiteral("CSV 导入成功")
                : QStringLiteral("WAV 导入成功"),
            4000);
    }
    refreshTaskList();
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
    if (isScriptExecution && activeScriptTaskId_.has_value()) {
        statusBar()->showMessage(
            QStringLiteral(
                "已有脚本正在运行，请等待完成或先取消。"),
            4000);
        return;
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

    const auto csvOptions = csvPreviewOptions_.find(*originalId);
    if (csvOptions != csvPreviewOptions_.end()) {
        csvPreviewOptions_.insert_or_assign(
            retried.value(), csvOptions->second);
    }
    if (isScriptExecution) {
        activeScriptTaskId_ = retried.value();
        scriptStatusLabel_->setText(
            QStringLiteral("脚本重试任务 #%1 已排队")
                .arg(static_cast<qulonglong>(retried.value())));
        updateScriptButtons();
    }
    refreshTaskList();
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

void MainWindow::refreshTaskList()
{
    const auto selectedId = taskModel_->taskIdAt(
        taskListView_->currentIndex());
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
    }
}

void MainWindow::showTaskDetails(const QModelIndex& index)
{
    const auto id = taskModel_->taskIdAt(index);
    if (!id.has_value()) {
        taskDetails_->clear();
        cancelTaskButton_->setEnabled(false);
        retryTaskButton_->setEnabled(false);
        return;
    }
    const auto snapshot = scheduler_.snapshot(*id);
    if (!snapshot.has_value()) {
        taskDetails_->setPlainText(
            QStringLiteral("任务快照已不可用。"));
        cancelTaskButton_->setEnabled(false);
        retryTaskButton_->setEnabled(false);
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
    }
}

void MainWindow::showSignal(const QModelIndex& index)
{
    if (!index.isValid() || index.row() < 0
        || static_cast<std::size_t>(index.row())
            >= importService_.entries().size()) {
        nameValue_->setText(QStringLiteral("—"));
        sampleRateValue_->setText(QStringLiteral("—"));
        sampleCountValue_->setText(QStringLiteral("—"));
        durationValue_->setText(QStringLiteral("—"));
        timeSeries_->clear();
        clearAnalysis();
        updateAnalysisValidation();
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
}

void MainWindow::renderTimeSignal(bool resetAxes)
{
    const auto* entry = selectedSignal();
    if (entry == nullptr) {
        timeSeries_->clear();
        return;
    }
    const auto& signal = entry->signal;
    const std::size_t sampleCount = signal.sampleCount();
    if (sampleCount == 0U) {
        timeSeries_->clear();
        return;
    }

    std::size_t firstIndex = 0;
    std::size_t pastLastIndex = sampleCount;
    if (!resetAxes) {
        const double sampleRate = signal.sampleRate();
        const double firstTime = std::clamp(
            timeXAxis_->min(), 0.0, signal.durationSeconds());
        const double lastTime = std::clamp(
            timeXAxis_->max(), 0.0, signal.durationSeconds());
        firstIndex = std::min(
            static_cast<std::size_t>(
                std::floor(firstTime * sampleRate)),
            sampleCount - 1U);
        pastLastIndex = std::min(
            static_cast<std::size_t>(
                std::ceil(lastTime * sampleRate)) + 1U,
            sampleCount);
        if (pastLastIndex <= firstIndex) {
            pastLastIndex = std::min(firstIndex + 1U, sampleCount);
        }
    }

    const auto displayPoints = signallab::app::makeDisplayPoints(
        signal,
        static_cast<std::size_t>(
            std::max(timeChartView_->viewport()->width(), 1)),
        firstIndex,
        pastLastIndex);
    const QList<QPointF> points = toTimePoints(displayPoints);
    timeSeries_->replace(points);
    timeChartView_->chart()->setTitle(
        QString::fromStdString(signal.metadata().name));

    if (resetAxes) {
        const auto [minimum, maximum] = yRange(points);
        const QSignalBlocker xBlocker(timeXAxis_);
        const QSignalBlocker yBlocker(timeYAxis_);
        timeXAxis_->setRange(
            0.0, std::max(signal.durationSeconds(), 1.0 / signal.sampleRate()));
        timeYAxis_->setRange(minimum, maximum);
        timeChartView_->captureHomeRanges();
    }
    statusBar()->showMessage(
        QStringLiteral("原始样本 %1 点；当前向 Qt Charts 提交 %2 点")
            .arg(static_cast<qulonglong>(sampleCount))
            .arg(static_cast<qulonglong>(points.size())),
        3000);
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

void MainWindow::updateAnalysisValidation()
{
    const auto* entry = selectedSignal();
    QString error;
    if (entry == nullptr) {
        error = QStringLiteral("请先选择信号。");
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

void MainWindow::refreshAnalysisHistory()
{
    const auto selectedRecord =
        analysisHistoryModel_->recordAt(
            analysisHistoryView_->currentIndex());
    const std::int64_t selectedId =
        selectedRecord == nullptr ? 0 : selectedRecord->id;
    auto history = analysisService_.history();
    if (!history) {
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
        analysisHistoryDetails_->setPlainText(QStringLiteral(
            "尚无分析记录。完成一次分析后，记录会显示在这里。"));
    }
}

void MainWindow::showAnalysisHistory(const QModelIndex& index)
{
    const auto* task = analysisHistoryModel_->recordAt(index);
    if (task == nullptr) {
        analysisHistoryDetails_->clear();
        return;
    }
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

    const std::size_t openedSignalCount =
        result.output.openedSignals.size();
    std::optional<std::int64_t> firstOpenedRecordId;
    for (auto& entry : result.output.openedSignals) {
        if (!firstOpenedRecordId.has_value()) {
            firstOpenedRecordId = entry.recordId;
        }
        static_cast<void>(importService_.acceptImportedSignal(
            std::move(entry)));
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
                 .arg(static_cast<qulonglong>(openedSignalCount));
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
