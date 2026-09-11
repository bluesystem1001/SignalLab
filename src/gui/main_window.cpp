#include "main_window.hpp"

#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>

#include "signallab/app/analysis_service.hpp"
#include "signallab/app/display_downsampler.hpp"
#include "signallab/core/error.hpp"

namespace {

std::filesystem::path toPath(const QString& text)
{
    return std::filesystem::path(text.toStdWString());
}

QString previewDescription(
    const signallab::repository::ImportPreview& preview)
{
    QString description = QString("样本数：%1\n通道数：%2")
        .arg(static_cast<qulonglong>(preview.sampleCount))
        .arg(static_cast<qulonglong>(preview.channelCount));

    if (preview.sampleRate.has_value()) {
        description += QString("\n采样率：%1 Hz")
            .arg(*preview.sampleRate, 0, 'f', 3);
    }

    if (!preview.columnNames.empty()) {
        QStringList columnNames;
        for (const auto& name : preview.columnNames) {
            columnNames.push_back(QString::fromStdString(name));
        }
        description += "\n列：" + columnNames.join(" | ");
    }

    if (!preview.rows.empty()) {
        description += "\n\n前几行：";
        for (const auto& row : preview.rows) {
            QStringList cells;
            for (const auto& cell : row) {
                cells.push_back(QString::fromStdString(cell));
            }
            description += "\n" + cells.join(" | ");
        }
    }

    return description;
}

QString taskStatusText(signallab::repository::AnalysisTaskStatus status)
{
    using signallab::repository::AnalysisTaskStatus;

    switch (status) {
    case AnalysisTaskStatus::Pending:
        return "等待中";
    case AnalysisTaskStatus::Running:
        return "运行中";
    case AnalysisTaskStatus::Succeeded:
        return "成功";
    case AnalysisTaskStatus::Failed:
        return "失败";
    case AnalysisTaskStatus::Interrupted:
        return "已中断";
    }

    return "未知";
}

QString textOrDash(const std::string& text)
{
    return text.empty() ? QString("—") : QString::fromStdString(text);
}

}  // namespace

MainWindow::MainWindow(
    signallab::app::ImportService& importService,
    signallab::app::AnalysisService& analysisService,
    QWidget* parent)
    : QMainWindow(parent),
      importService_(importService),
      analysisService_(analysisService),
      signalList_(nullptr),
      nameValue_(nullptr),
      sampleRateValue_(nullptr),
      sampleCountValue_(nullptr),
      durationValue_(nullptr),
      meanValue_(nullptr),
      varianceValue_(nullptr),
      rmsValue_(nullptr),
      peakToPeakValue_(nullptr),
      dominantFrequencyValue_(nullptr),
      snrValue_(nullptr),
      thdValue_(nullptr),
      chartTabs_(nullptr),
      timeChartView_(nullptr),
      spectrumChartView_(nullptr),
      historyList_(nullptr),
      historyDetails_(nullptr)
{
    setWindowTitle("SignalLab " SIGNALLAB_VERSION_STRING);
    resize(1080, 680);

    auto* centralWidget = new QWidget(this);
    auto* windowLayout = new QHBoxLayout(centralWidget);

    auto* browserLayout = new QVBoxLayout;
    browserLayout->addWidget(new QLabel("项目信号", centralWidget));

    signalList_ = new QListWidget(centralWidget);
    signalList_->setMinimumWidth(220);
    browserLayout->addWidget(signalList_);

    auto* importCsvButton = new QPushButton("导入 CSV", centralWidget);
    auto* importWavButton = new QPushButton("导入 WAV", centralWidget);
    browserLayout->addWidget(importCsvButton);
    browserLayout->addWidget(importWavButton);
    windowLayout->addLayout(browserLayout);

    auto* signalLayout = new QVBoxLayout;
    auto* informationLayout = new QFormLayout;
    nameValue_ = new QLabel(centralWidget);
    sampleRateValue_ = new QLabel(centralWidget);
    sampleCountValue_ = new QLabel(centralWidget);
    durationValue_ = new QLabel(centralWidget);
    informationLayout->addRow("名称：", nameValue_);
    informationLayout->addRow("采样率：", sampleRateValue_);
    informationLayout->addRow("样本数：", sampleCountValue_);
    informationLayout->addRow("持续时间：", durationValue_);
    signalLayout->addLayout(informationLayout);

    auto* analyzeButton = new QPushButton("分析所选信号", centralWidget);
    signalLayout->addWidget(analyzeButton);

    auto* featureGroup = new QGroupBox("同步分析结果", centralWidget);
    auto* featureLayout = new QFormLayout(featureGroup);
    meanValue_ = new QLabel("—", featureGroup);
    varianceValue_ = new QLabel("—", featureGroup);
    rmsValue_ = new QLabel("—", featureGroup);
    peakToPeakValue_ = new QLabel("—", featureGroup);
    dominantFrequencyValue_ = new QLabel("—", featureGroup);
    snrValue_ = new QLabel("—", featureGroup);
    thdValue_ = new QLabel("—", featureGroup);
    featureLayout->addRow("均值：", meanValue_);
    featureLayout->addRow("总体方差：", varianceValue_);
    featureLayout->addRow("RMS：", rmsValue_);
    featureLayout->addRow("峰峰值：", peakToPeakValue_);
    featureLayout->addRow("主频：", dominantFrequencyValue_);
    featureLayout->addRow("SNR：", snrValue_);
    featureLayout->addRow("THD：", thdValue_);
    signalLayout->addWidget(featureGroup);

    chartTabs_ = new QTabWidget(centralWidget);
    timeChartView_ = new QChartView(new QChart, chartTabs_);
    spectrumChartView_ = new QChartView(new QChart, chartTabs_);
    timeChartView_->setRenderHint(QPainter::Antialiasing);
    spectrumChartView_->setRenderHint(QPainter::Antialiasing);
    chartTabs_->addTab(timeChartView_, "时域波形");
    chartTabs_->addTab(spectrumChartView_, "单边频谱");

    auto* historyPage = new QWidget(chartTabs_);
    auto* historyLayout = new QHBoxLayout(historyPage);
    historyList_ = new QListWidget(historyPage);
    historyList_->setMinimumWidth(300);
    historyDetails_ = new QTextEdit(historyPage);
    historyDetails_->setReadOnly(true);
    historyDetails_->setPlaceholderText("选择左侧任务查看持久化详情");
    historyLayout->addWidget(historyList_);
    historyLayout->addWidget(historyDetails_, 1);
    chartTabs_->addTab(historyPage, "历史任务");

    signalLayout->addWidget(chartTabs_);
    windowLayout->addLayout(signalLayout, 1);

    setCentralWidget(centralWidget);

    connect(
        signalList_,
        &QListWidget::currentRowChanged,
        this,
        [this](int row) {
            if (row >= 0) {
                showSignal(static_cast<std::size_t>(row));
            }
        });
    connect(
        importCsvButton,
        &QPushButton::clicked,
        this,
        [this]() { importCsv(); });
    connect(
        importWavButton,
        &QPushButton::clicked,
        this,
        [this]() { importWav(); });
    connect(
        analyzeButton,
        &QPushButton::clicked,
        this,
        [this]() { analyzeSelectedSignal(); });
    connect(
        historyList_,
        &QListWidget::currentRowChanged,
        this,
        [this](int row) { showHistoryTask(row); });

    refreshSignalList();
    refreshHistory();
    if (!importService_.entries().empty()) {
        QTimer::singleShot(
            0,
            this,
            [this]() { signalList_->setCurrentRow(0); });
    }
    statusBar()->showMessage("可导入 CSV 或 PCM16 WAV 文件");
}

void MainWindow::importCsv()
{
    const QString fileName = QFileDialog::getOpenFileName(
        this,
        "选择 CSV 文件",
        {},
        "CSV 文件 (*.csv *.tsv *.txt);;所有文件 (*.*)");
    if (fileName.isEmpty()) {
        return;
    }

    signallab::repository::CsvImportOptions options;
    bool accepted = false;
    const int defaultDelimiter =
        QFileInfo(fileName).suffix().compare("tsv", Qt::CaseInsensitive) == 0
        ? 1
        : 0;
    const QString delimiter = QInputDialog::getItem(
        this,
        "CSV 分隔符",
        "选择列分隔符：",
        {"逗号 (,)", "制表符 (Tab)"},
        defaultDelimiter,
        false,
        &accepted);
    if (!accepted) {
        return;
    }
    options.delimiter = delimiter.startsWith("制表符")
        ? signallab::repository::CsvDelimiter::Tab
        : signallab::repository::CsvDelimiter::Comma;

    const auto headerAnswer = QMessageBox::question(
        this,
        "CSV 表头",
        "第一行是否为列名？",
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
    if (headerAnswer == QMessageBox::Cancel) {
        return;
    }
    options.hasHeader = headerAnswer == QMessageBox::Yes;

    const int valueColumn = QInputDialog::getInt(
        this,
        "数值列",
        "请输入数值列编号（从 1 开始）：",
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
        "时间列",
        "文件中是否有以秒为单位的等间隔时间列？",
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
    if (timeAnswer == QMessageBox::Cancel) {
        return;
    }

    if (timeAnswer == QMessageBox::Yes) {
        const int timeColumn = QInputDialog::getInt(
            this,
            "时间列",
            "请输入时间列编号（从 1 开始）：",
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
            "采样率",
            "请输入采样率 (Hz)：",
            48000.0,
            0.001,
            1000000000.0,
            3,
            &accepted);
        if (!accepted) {
            return;
        }
    }

    const QFileInfo fileInformation(fileName);
    options.metadata.name = fileInformation.completeBaseName().toStdString();
    options.metadata.unit = "幅值";
    options.metadata.source = fileName.toStdString();

    const std::filesystem::path sourcePath = toPath(fileName);
    auto previewResult = importService_.previewCsv(sourcePath, options);
    if (!previewResult) {
        QMessageBox::warning(
            this,
            "CSV 无法导入",
            QString::fromStdString(previewResult.error().message));
        return;
    }

    if (QMessageBox::question(
            this,
            "确认导入 CSV",
            previewDescription(previewResult.value()) + "\n\n确认导入吗？")
        != QMessageBox::Yes) {
        return;
    }

    auto importResult = importService_.importCsv(sourcePath, options);
    if (!importResult) {
        const auto icon = importResult.error().code
                == signallab::core::ErrorCode::DuplicateFile
            ? QMessageBox::Information
            : QMessageBox::Warning;
        QMessageBox messageBox(icon, "CSV 导入结果",
            QString::fromStdString(importResult.error().message),
            QMessageBox::Ok, this);
        messageBox.exec();
        return;
    }

    refreshSignalList();
    signalList_->setCurrentRow(static_cast<int>(importResult.value()));
    statusBar()->showMessage("CSV 导入成功", 4000);
}

void MainWindow::importWav()
{
    const QString fileName = QFileDialog::getOpenFileName(
        this,
        "选择 WAV 文件",
        {},
        "WAV 文件 (*.wav);;所有文件 (*.*)");
    if (fileName.isEmpty()) {
        return;
    }

    const std::filesystem::path sourcePath = toPath(fileName);
    auto previewResult = importService_.previewWav(sourcePath);
    if (!previewResult) {
        QMessageBox::warning(
            this,
            "WAV 无法导入",
            QString::fromStdString(previewResult.error().message));
        return;
    }

    signallab::repository::WavImportOptions options;
    const QFileInfo fileInformation(fileName);
    options.metadata.name = fileInformation.completeBaseName().toStdString();
    options.metadata.unit = "幅值";
    options.metadata.source = fileName.toStdString();

    QString selectedChannel = "单声道";

    if (previewResult.value().channelCount == 2) {
        bool accepted = false;
        const QString channel = QInputDialog::getItem(
            this,
            "立体声通道",
            "选择要导入的通道：",
            {"左右平均", "左声道", "右声道"},
            0,
            false,
            &accepted);
        if (!accepted) {
            return;
        }

        if (channel == "左声道") {
            options.channel = signallab::repository::StereoChannel::Left;
        } else if (channel == "右声道") {
            options.channel = signallab::repository::StereoChannel::Right;
        } else {
            options.channel = signallab::repository::StereoChannel::Average;
        }
        selectedChannel = channel;
    }

    if (QMessageBox::question(
            this,
            "确认导入 WAV",
            previewDescription(previewResult.value())
                + "\n导入通道：" + selectedChannel
                + "\n\n确认导入吗？")
        != QMessageBox::Yes) {
        return;
    }

    auto importResult = importService_.importWav(sourcePath, options);
    if (!importResult) {
        const auto icon = importResult.error().code
                == signallab::core::ErrorCode::DuplicateFile
            ? QMessageBox::Information
            : QMessageBox::Warning;
        QMessageBox messageBox(icon, "WAV 导入结果",
            QString::fromStdString(importResult.error().message),
            QMessageBox::Ok, this);
        messageBox.exec();
        return;
    }

    refreshSignalList();
    signalList_->setCurrentRow(static_cast<int>(importResult.value()));
    statusBar()->showMessage("WAV 导入成功", 4000);
}

void MainWindow::analyzeSelectedSignal()
{
    const int selectedRow = signalList_->currentRow();
    if (selectedRow < 0) {
        QMessageBox::information(
            this,
            "分析信号",
            "请先在左侧选择一个信号。");
        return;
    }

    const auto entryIndex = static_cast<std::size_t>(selectedRow);
    const auto& entries = importService_.entries();
    if (entryIndex >= entries.size()) {
        return;
    }

    const auto& entry = entries[entryIndex];
    signallab::app::AnalysisOptions options;
    auto analysisResult = analysisService_.analyze(
        entry.recordId,
        options);
    if (!analysisResult) {
        refreshHistory();
        QMessageBox::warning(
            this,
            "分析失败",
            QString::fromStdString(analysisResult.error().message));
        return;
    }

    const auto& result = analysisResult.value();
    meanValue_->setText(QString::number(result.features.mean, 'g', 8));
    varianceValue_->setText(
        QString::number(result.features.variance, 'g', 8));
    rmsValue_->setText(QString::number(result.features.rms, 'g', 8));
    peakToPeakValue_->setText(
        QString::number(result.features.peakToPeak, 'g', 8));

    if (result.dominantPeak.has_value()) {
        dominantFrequencyValue_->setText(
            QString("%1 Hz，幅值 %2")
                .arg(result.dominantPeak->frequencyHz, 0, 'f', 3)
                .arg(result.dominantPeak->amplitude, 0, 'g', 6));
    } else {
        dominantFrequencyValue_->setText("未检测到");
    }

    snrValue_->setText(result.snrDb.has_value()
        ? QString("%1 dB").arg(*result.snrDb, 0, 'f', 2)
        : QString("不可计算"));
    thdValue_->setText(result.thdPercent.has_value()
        ? QString("%1 %").arg(*result.thdPercent, 0, 'f', 3)
        : QString("不可计算"));

    showSpectrum(result.spectrum);
    refreshHistory();
    chartTabs_->setCurrentWidget(spectrumChartView_);
    statusBar()->showMessage(
        QString("同步分析完成：FFT %1 点，检测到 %2 个峰值")
            .arg(static_cast<qulonglong>(result.spectrum.fftSize))
            .arg(static_cast<qulonglong>(result.peaks.size())));
}

void MainWindow::clearAnalysis()
{
    meanValue_->setText("—");
    varianceValue_->setText("—");
    rmsValue_->setText("—");
    peakToPeakValue_->setText("—");
    dominantFrequencyValue_->setText("—");
    snrValue_->setText("—");
    thdValue_->setText("—");

    auto* chart = new QChart;
    chart->setTitle("选择信号后单击“分析所选信号”");
    QChart* oldChart = spectrumChartView_->chart();
    spectrumChartView_->setChart(chart);
    delete oldChart;
}

void MainWindow::refreshHistory()
{
    auto historyResult = analysisService_.history();

    historyList_->clear();
    historyTasks_.clear();

    if (!historyResult) {
        historyDetails_->setPlainText("无法读取历史任务。");
        QMessageBox::warning(
            this,
            "历史任务读取失败",
            QString::fromStdString(historyResult.error().message));
        return;
    }

    historyTasks_ = historyResult.value();
    for (const auto& task : historyTasks_) {
        historyList_->addItem(
            QString("#%1  [%2]  %3")
                .arg(task.id)
                .arg(taskStatusText(task.status))
                .arg(QString::fromStdString(task.signalName)));
    }

    if (historyTasks_.empty()) {
        historyDetails_->setPlainText("尚无分析任务。完成一次分析后，记录会显示在这里。");
        return;
    }

    historyList_->setCurrentRow(0);
}

void MainWindow::showHistoryTask(int row)
{
    if (row < 0 || static_cast<std::size_t>(row) >= historyTasks_.size()) {
        historyDetails_->clear();
        return;
    }

    const auto& task = historyTasks_[static_cast<std::size_t>(row)];
    auto featureResult = analysisService_.featureResults(task.id);
    if (!featureResult) {
        historyDetails_->setPlainText("无法读取此任务的特征结果。");
        QMessageBox::warning(
            this,
            "分析结果读取失败",
            QString::fromStdString(featureResult.error().message));
        return;
    }

    QStringList lines;
    lines << QString("任务 ID：%1").arg(task.id);
    lines << "状态：" + taskStatusText(task.status);
    lines << "信号：" + textOrDash(task.signalName);
    lines << "算法：" + textOrDash(task.algorithmName);
    lines << "算法版本：" + textOrDash(task.algorithmVersion);
    lines << "参数 JSON：" + textOrDash(task.parametersJson);
    lines << "输入哈希：" + textOrDash(task.inputHash);
    lines << "输出哈希：" + textOrDash(task.outputHash);
    lines << "创建时间 (UTC)：" + textOrDash(task.createdUtc);
    lines << "开始时间 (UTC)：" + textOrDash(task.startedUtc);
    lines << "完成时间 (UTC)：" + textOrDash(task.finishedUtc);
    lines << "错误：" + textOrDash(task.errorMessage);
    lines << "";
    lines << "特征结果：";

    if (featureResult.value().empty()) {
        lines << "  （无）";
    } else {
        for (const auto& feature : featureResult.value()) {
            QString value;
            if (feature.numericValue.has_value()) {
                value = QString::number(*feature.numericValue, 'g', 17);
            } else {
                value = textOrDash(feature.valueText);
            }

            const QString unit = feature.unit.empty()
                ? QString()
                : " " + QString::fromStdString(feature.unit);
            lines << QString("  %1 = %2%3")
                         .arg(QString::fromStdString(feature.name))
                         .arg(value)
                         .arg(unit);
        }
    }

    historyDetails_->setPlainText(lines.join('\n'));
}

void MainWindow::refreshSignalList()
{
    signalList_->clear();
    for (const auto& entry : importService_.entries()) {
        signalList_->addItem(QString::fromStdString(entry.displayName));
    }
}

void MainWindow::showSignal(std::size_t index)
{
    const auto& entries = importService_.entries();
    if (index >= entries.size()) {
        return;
    }

    const auto& signal = entries[index].signal;
    nameValue_->setText(QString::fromStdString(signal.metadata().name));
    sampleRateValue_->setText(
        QString("%1 Hz").arg(signal.sampleRate(), 0, 'f', 3));
    sampleCountValue_->setText(
        QString::number(static_cast<qulonglong>(signal.sampleCount())));
    durationValue_->setText(
        QString("%1 s").arg(signal.durationSeconds(), 0, 'f', 6));
    clearAnalysis();
    chartTabs_->setCurrentWidget(timeChartView_);

    auto* series = new QLineSeries;
    const int availableWidth =
        std::max(timeChartView_->viewport()->width(), 1);
    const auto points = signallab::app::makeDisplayPoints(
        signal,
        static_cast<std::size_t>(availableWidth));
    for (const auto& point : points) {
        series->append(point.timeSeconds, point.value);
    }

    auto* chart = new QChart;
    chart->addSeries(series);
    chart->setTitle(QString::fromStdString(signal.metadata().name));
    chart->legend()->hide();

    auto* timeAxis = new QValueAxis;
    timeAxis->setTitleText("时间 (s)");
    timeAxis->setRange(0.0, signal.durationSeconds());

    auto [minimumSample, maximumSample] = signal.sampleRange();
    if (minimumSample == maximumSample) {
        minimumSample -= 0.5;
        maximumSample += 0.5;
    }
    auto* sampleAxis = new QValueAxis;
    sampleAxis->setTitleText(QString::fromStdString(signal.metadata().unit));
    sampleAxis->setRange(minimumSample, maximumSample);

    chart->addAxis(timeAxis, Qt::AlignBottom);
    chart->addAxis(sampleAxis, Qt::AlignLeft);
    series->attachAxis(timeAxis);
    series->attachAxis(sampleAxis);

    QChart* oldChart = timeChartView_->chart();
    timeChartView_->setChart(chart);
    delete oldChart;

    statusBar()->showMessage(
        QString("正在显示 %1 个原始样本，绘图点 %2 个")
            .arg(static_cast<qulonglong>(signal.sampleCount()))
            .arg(static_cast<qulonglong>(points.size())));
}

void MainWindow::showSpectrum(
    const signallab::algorithm::Spectrum& spectrum)
{
    auto* series = new QLineSeries;
    const int availableWidth =
        std::max(spectrumChartView_->viewport()->width(), 1);
    const auto displayPoints =
        signallab::app::makeSpectrumDisplayPoints(
            spectrum,
            static_cast<std::size_t>(availableWidth));
    for (const auto& point : displayPoints) {
        series->append(point.frequencyHz, point.amplitude);
    }

    auto* chart = new QChart;
    chart->addSeries(series);
    chart->setTitle("单边幅度谱");
    chart->legend()->hide();

    auto* frequencyAxis = new QValueAxis;
    frequencyAxis->setTitleText("频率 (Hz)");
    const double maximumFrequency = spectrum.frequenciesHz.empty()
        ? 1.0
        : std::max(spectrum.frequenciesHz.back(), 1.0);
    frequencyAxis->setRange(0.0, maximumFrequency);

    const auto maximumAmplitude = std::max_element(
        spectrum.amplitudes.begin(),
        spectrum.amplitudes.end());
    double amplitudeLimit = 1.0;
    if (maximumAmplitude != spectrum.amplitudes.end()
        && *maximumAmplitude > 0.0) {
        amplitudeLimit = *maximumAmplitude * 1.05;
    }
    auto* amplitudeAxis = new QValueAxis;
    amplitudeAxis->setTitleText("幅值");
    amplitudeAxis->setRange(0.0, amplitudeLimit);

    chart->addAxis(frequencyAxis, Qt::AlignBottom);
    chart->addAxis(amplitudeAxis, Qt::AlignLeft);
    series->attachAxis(frequencyAxis);
    series->attachAxis(amplitudeAxis);

    QChart* oldChart = spectrumChartView_->chart();
    spectrumChartView_->setChart(chart);
    delete oldChart;
}
