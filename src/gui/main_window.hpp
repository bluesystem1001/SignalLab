#pragma once

#include <QMainWindow>

#include <cstddef>

#include "signallab/app/import_service.hpp"

class QLabel;
class QListWidget;
class QPushButton;
class QChartView;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(
        signallab::app::ImportService& importService,
        QWidget* parent = nullptr);

private:
    void importCsv();
    void importWav();
    void refreshSignalList();
    void showSignal(std::size_t index);

    signallab::app::ImportService& importService_;
    QListWidget* signalList_;
    QLabel* nameValue_;
    QLabel* sampleRateValue_;
    QLabel* sampleCountValue_;
    QLabel* durationValue_;
    QChartView* chartView_;
};
