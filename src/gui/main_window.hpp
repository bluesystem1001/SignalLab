#pragma once

#include <QMainWindow>

#include <cstddef>

#include "signallab/algorithm/spectrum.hpp"
#include "signallab/app/import_service.hpp"

class QLabel;
class QListWidget;
class QPushButton;
class QChartView;
class QTabWidget;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(
        signallab::app::ImportService& importService,
        QWidget* parent = nullptr);

private:
    void importCsv();
    void importWav();
    void analyzeSelectedSignal();
    void clearAnalysis();
    void refreshSignalList();
    void showSignal(std::size_t index);
    void showSpectrum(const signallab::algorithm::Spectrum& spectrum);

    signallab::app::ImportService& importService_;
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
};
