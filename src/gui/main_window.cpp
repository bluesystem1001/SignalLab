#include "main_window.hpp"

#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
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

#include "signallab/app/display_downsampler.hpp"
#include "signallab/core/error.hpp"
#include "signallab/io/import_types.hpp"

namespace {

std::filesystem::path toPath(const QString& text)
{
    return std::filesystem::path(text.toStdWString());
}

QString previewDescription(const signallab::io::ImportPreview& preview)
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

}  // namespace

MainWindow::MainWindow(
    signallab::app::ImportService& importService,
    QWidget* parent)
    : QMainWindow(parent),
      importService_(importService),
      signalList_(nullptr),
      nameValue_(nullptr),
      sampleRateValue_(nullptr),
      sampleCountValue_(nullptr),
      durationValue_(nullptr),
      chartView_(nullptr)
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

    chartView_ = new QChartView(new QChart, centralWidget);
    chartView_->setRenderHint(QPainter::Antialiasing);
    signalLayout->addWidget(chartView_);
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

    refreshSignalList();
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

    signallab::io::CsvImportOptions options;
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
        ? signallab::io::CsvDelimiter::Tab
        : signallab::io::CsvDelimiter::Comma;

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

    signallab::io::WavImportOptions options;
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
            options.channel = signallab::io::StereoChannel::Left;
        } else if (channel == "右声道") {
            options.channel = signallab::io::StereoChannel::Right;
        } else {
            options.channel = signallab::io::StereoChannel::Average;
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

    auto* series = new QLineSeries;
    const int availableWidth = std::max(chartView_->viewport()->width(), 1);
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

    QChart* oldChart = chartView_->chart();
    chartView_->setChart(chart);
    delete oldChart;

    statusBar()->showMessage(
        QString("正在显示 %1 个原始样本，绘图点 %2 个")
            .arg(static_cast<qulonglong>(signal.sampleCount()))
            .arg(static_cast<qulonglong>(points.size())));
}
