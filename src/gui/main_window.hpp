#pragma once

#include <QMainWindow>

#include "signallab/core/signal.hpp"

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(
        const signallab::core::Signal& signal,
        QWidget* parent = nullptr);
};
