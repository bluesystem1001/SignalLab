#include "main_window.hpp"

#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QStatusBar>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <cstddef>

MainWindow::MainWindow(
    const signallab::core::Signal& signal,
    QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("SignalLab " SIGNALLAB_VERSION_STRING);
    resize(960, 600);

    auto* centralWidget = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(centralWidget);

    auto* informationLayout = new QFormLayout;
    informationLayout->addRow(
        "名称：",
        new QLabel(QString::fromStdString(signal.metadata().name), centralWidget));
    informationLayout->addRow(
        "采样率：",
        new QLabel(
            QString("%1 Hz").arg(signal.sampleRate(), 0, 'f', 0),
            centralWidget));
    informationLayout->addRow(
        "样本数：",
        new QLabel(
            QString::number(
                static_cast<qulonglong>(signal.sampleCount())),
            centralWidget));
    informationLayout->addRow(
        "持续时间：",
        new QLabel(
            QString("%1 s").arg(signal.durationSeconds(), 0, 'f', 6),
            centralWidget));
    mainLayout->addLayout(informationLayout);

    auto* series = new QLineSeries;
    const auto samples = signal.samples();
    for (std::size_t index = 0; index < samples.size(); ++index) {
        series->append(signal.sampleTimeSeconds(index), samples[index]);
    }

    auto* chart = new QChart;
    chart->addSeries(series);
    chart->setTitle(QString::fromStdString(signal.metadata().name));
    chart->legend()->hide();

    auto* timeAxis = new QValueAxis;
    timeAxis->setTitleText("时间 (s)");
    timeAxis->setRange(0.0, signal.durationSeconds());

    const auto [minimumSample, maximumSample] = signal.sampleRange();
    auto* sampleAxis = new QValueAxis;
    sampleAxis->setTitleText(QString::fromStdString(signal.metadata().unit));
    sampleAxis->setRange(minimumSample, maximumSample);

    chart->addAxis(timeAxis, Qt::AlignBottom);
    chart->addAxis(sampleAxis, Qt::AlignLeft);
    series->attachAxis(timeAxis);
    series->attachAxis(sampleAxis);

    auto* chartView = new QChartView(chart, centralWidget);
    chartView->setRenderHint(QPainter::Antialiasing);
    mainLayout->addWidget(chartView);

    setCentralWidget(centralWidget);

    statusBar()->showMessage("已生成 1 kHz 正弦信号");
}
