#include "audio_capture_widget.hpp"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLegend>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <utility>

#include "signallab/io/wav_writer.hpp"

namespace signallab::gui {

AudioCaptureWidget::AudioCaptureWidget(
    std::unique_ptr<repository::AudioInput> input,
    QWidget* parent)
    : QWidget(parent), service_(std::move(input))
{
    setObjectName(QStringLiteral("audioCaptureWidget"));
    auto* layout = new QVBoxLayout(this);
    auto* settings = new QGroupBox(QStringLiteral("麦克风与实时 FIR 滤波"), this);
    auto* settingsLayout = new QGridLayout(settings);

    deviceCombo_ = new QComboBox(settings);
    deviceCombo_->setObjectName(QStringLiteral("captureDeviceCombo"));
    deviceCombo_->setMinimumContentsLength(15);
    deviceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    refreshButton_ = new QPushButton(QStringLiteral("刷新设备"), settings);
    refreshButton_->setObjectName(QStringLiteral("captureRefreshButton"));
    settingsLayout->addWidget(new QLabel(QStringLiteral("输入设备"), settings), 0, 0);
    settingsLayout->addWidget(deviceCombo_, 0, 1, 1, 3);
    settingsLayout->addWidget(refreshButton_, 0, 4);

    sampleRateCombo_ = new QComboBox(settings);
    sampleRateCombo_->setObjectName(QStringLiteral("captureSampleRateCombo"));
    for (const int sampleRate : {16000, 44100, 48000}) {
        sampleRateCombo_->addItem(QStringLiteral("%1 Hz").arg(sampleRate), sampleRate);
    }
    sampleRateCombo_->setCurrentIndex(2);
    filterCombo_ = new QComboBox(settings);
    filterCombo_->setObjectName(QStringLiteral("captureFilterCombo"));
    filterCombo_->addItem(QStringLiteral("不滤波"), static_cast<int>(app::AudioFilterType::None));
    filterCombo_->addItem(QStringLiteral("低通 FIR"), static_cast<int>(app::AudioFilterType::LowPass));
    filterCombo_->addItem(QStringLiteral("高通 FIR"), static_cast<int>(app::AudioFilterType::HighPass));
    filterCombo_->setCurrentIndex(1);
    settingsLayout->addWidget(new QLabel(QStringLiteral("采样率"), settings), 1, 0);
    settingsLayout->addWidget(sampleRateCombo_, 1, 1);
    settingsLayout->addWidget(new QLabel(QStringLiteral("滤波器"), settings), 1, 2);
    settingsLayout->addWidget(filterCombo_, 1, 3, 1, 2);

    cutoffSpin_ = new QDoubleSpinBox(settings);
    cutoffSpin_->setObjectName(QStringLiteral("captureCutoffSpin"));
    cutoffSpin_->setRange(1.0, 23999.0);
    cutoffSpin_->setDecimals(1);
    cutoffSpin_->setValue(3000.0);
    cutoffSpin_->setSuffix(QStringLiteral(" Hz"));
    tapCountSpin_ = new QSpinBox(settings);
    tapCountSpin_->setObjectName(QStringLiteral("captureTapCountSpin"));
    tapCountSpin_->setRange(3, 255);
    tapCountSpin_->setSingleStep(2);
    tapCountSpin_->setValue(63);
    settingsLayout->addWidget(new QLabel(QStringLiteral("截止频率"), settings), 2, 0);
    settingsLayout->addWidget(cutoffSpin_, 2, 1);
    settingsLayout->addWidget(new QLabel(QStringLiteral("抽头数（奇数）"), settings), 2, 2);
    settingsLayout->addWidget(tapCountSpin_, 2, 3, 1, 2);

    maximumDurationSpin_ = new QSpinBox(settings);
    maximumDurationSpin_->setObjectName(QStringLiteral("captureMaximumDurationSpin"));
    maximumDurationSpin_->setRange(30, 300);
    maximumDurationSpin_->setValue(120);
    maximumDurationSpin_->setSuffix(QStringLiteral(" 秒"));
    settingsLayout->addWidget(new QLabel(QStringLiteral("最长录制"), settings), 3, 0);
    settingsLayout->addWidget(maximumDurationSpin_, 3, 1);
    auto* durationHint = new QLabel(QStringLiteral("到时自动停止；停止后可保存原始或滤波音频。"), settings);
    durationHint->setWordWrap(true);
    settingsLayout->addWidget(durationHint, 3, 2, 1, 3);
    layout->addWidget(settings);

    validationLabel_ = new QLabel(this);
    validationLabel_->setObjectName(QStringLiteral("captureValidationLabel"));
    validationLabel_->setWordWrap(true);
    layout->addWidget(validationLabel_);
    auto* controls = new QHBoxLayout;
    startButton_ = new QPushButton(QStringLiteral("开始采集"), this);
    startButton_->setObjectName(QStringLiteral("captureStartButton"));
    stopButton_ = new QPushButton(QStringLiteral("停止采集"), this);
    stopButton_->setObjectName(QStringLiteral("captureStopButton"));
    saveVersionCombo_ = new QComboBox(this);
    saveVersionCombo_->setObjectName(QStringLiteral("captureSaveVersionCombo"));
    saveVersionCombo_->addItem(QStringLiteral("滤波后音频"), true);
    saveVersionCombo_->addItem(QStringLiteral("原始音频"), false);
    saveButton_ = new QPushButton(QStringLiteral("保存 WAV"), this);
    saveButton_->setObjectName(QStringLiteral("captureSaveButton"));
    saveImportButton_ = new QPushButton(QStringLiteral("保存并导入"), this);
    saveImportButton_->setObjectName(QStringLiteral("captureSaveImportButton"));
    controls->addWidget(startButton_);
    controls->addWidget(stopButton_);
    controls->addStretch();
    controls->addWidget(saveVersionCombo_);
    controls->addWidget(saveButton_);
    controls->addWidget(saveImportButton_);
    layout->addLayout(controls);

    auto* indicators = new QHBoxLayout;
    durationLabel_ = new QLabel(QStringLiteral("时长：0.0 秒"), this);
    durationLabel_->setObjectName(QStringLiteral("captureDurationLabel"));
    sampleCountLabel_ = new QLabel(QStringLiteral("采样点：0"), this);
    sampleCountLabel_->setObjectName(QStringLiteral("captureSampleCountLabel"));
    levelBar_ = new QProgressBar(this);
    levelBar_->setObjectName(QStringLiteral("captureLevelBar"));
    levelBar_->setRange(0, 100);
    levelBar_->setValue(0);
    levelBar_->setFormat(QStringLiteral("输入电平 %p%"));
    indicators->addWidget(durationLabel_);
    indicators->addWidget(sampleCountLabel_);
    indicators->addWidget(levelBar_, 1);
    layout->addLayout(indicators);

    auto* chart = new QChart;
    chart->setTitle(QStringLiteral("实时波形 · 最近 2 秒"));
    chart->setAnimationOptions(QChart::NoAnimation);
    rawSeries_ = new QLineSeries(chart);
    rawSeries_->setObjectName(QStringLiteral("captureRawSeries"));
    rawSeries_->setName(QStringLiteral("原始"));
    rawSeries_->setColor(QColor(96, 125, 139));
    filteredSeries_ = new QLineSeries(chart);
    filteredSeries_->setObjectName(QStringLiteral("captureFilteredSeries"));
    filteredSeries_->setName(QStringLiteral("滤波后"));
    filteredSeries_->setColor(QColor(0, 137, 123));
    chart->addSeries(rawSeries_);
    chart->addSeries(filteredSeries_);
    timeAxis_ = new QValueAxis(chart);
    timeAxis_->setTitleText(QStringLiteral("时间 (s)"));
    timeAxis_->setRange(0.0, 2.0);
    amplitudeAxis_ = new QValueAxis(chart);
    amplitudeAxis_->setTitleText(QStringLiteral("归一化幅值"));
    amplitudeAxis_->setRange(-1.0, 1.0);
    chart->addAxis(timeAxis_, Qt::AlignBottom);
    chart->addAxis(amplitudeAxis_, Qt::AlignLeft);
    rawSeries_->attachAxis(timeAxis_);
    rawSeries_->attachAxis(amplitudeAxis_);
    filteredSeries_->attachAxis(timeAxis_);
    filteredSeries_->attachAxis(amplitudeAxis_);
    chart->legend()->setAlignment(Qt::AlignBottom);
    auto* chartView = new QChartView(chart, this);
    chartView->setObjectName(QStringLiteral("captureChartView"));
    chartView->setMinimumHeight(180);
    layout->addWidget(chartView, 1);

    statusLabel_ = new QLabel(QStringLiteral("选择麦克风后开始采集。"), this);
    statusLabel_->setObjectName(QStringLiteral("captureStatusLabel"));
    statusLabel_->setWordWrap(true);
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(statusLabel_);
    auto* hint = new QLabel(QStringLiteral(
        "单声道 PCM 16 位 WAV；FIR 为因果实时滤波，会有固定延迟。"
        "开始新录音会替换原始与滤波缓存，请先保存需要的版本。"
        "如无法采集，请检查 Windows 麦克风隐私权限与设备连接。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    connect(refreshButton_, &QPushButton::clicked, this, [this]() { refreshDevices(); });
    connect(startButton_, &QPushButton::clicked, this, [this]() { startCapture(); });
    connect(stopButton_, &QPushButton::clicked, this, [this]() { stopCapture(); });
    connect(saveButton_, &QPushButton::clicked, this, [this]() { chooseSavePath(false); });
    connect(saveImportButton_, &QPushButton::clicked, this, [this]() { chooseSavePath(true); });
    connect(deviceCombo_, &QComboBox::currentIndexChanged, this, [this]() { updateControls(); });
    connect(sampleRateCombo_, &QComboBox::currentIndexChanged, this, [this]() { updateControls(); });
    connect(filterCombo_, &QComboBox::currentIndexChanged, this, [this]() { updateControls(); });
    connect(cutoffSpin_, &QDoubleSpinBox::valueChanged, this, [this]() { updateControls(); });
    connect(tapCountSpin_, &QSpinBox::valueChanged, this, [this]() { updateControls(); });
    timer_ = new QTimer(this);
    timer_->setObjectName(QStringLiteral("capturePollTimer"));
    timer_->setInterval(100);
    connect(timer_, &QTimer::timeout, this, [this]() { poll(); });
    timer_->start();
    refreshDevices();
}

AudioCaptureWidget::~AudioCaptureWidget()
{
    timer_->stop();
    service_.stop();
    if (saveFuture_.valid()) {
        saveFuture_.wait();
    }
}

void AudioCaptureWidget::setImportCallback(
    std::function<void(const std::filesystem::path&)> callback)
{
    importCallback_ = std::move(callback);
    updateControls();
}

app::AudioCaptureOptions AudioCaptureWidget::options() const
{
    app::AudioCaptureOptions result;
    result.deviceId = deviceCombo_->currentData().toString().toStdString();
    result.sampleRate = sampleRateCombo_->currentData().toUInt();
    result.filterType = static_cast<app::AudioFilterType>(filterCombo_->currentData().toInt());
    result.cutoffHz = cutoffSpin_->value();
    result.tapCount = static_cast<std::size_t>(tapCountSpin_->value());
    result.maximumDurationSeconds = static_cast<unsigned int>(maximumDurationSpin_->value());
    return result;
}

QString AudioCaptureWidget::validationMessage() const
{
    if (deviceCombo_->count() == 0) {
        return QStringLiteral("未发现麦克风；连接设备后点击“刷新设备”。");
    }
    const auto selected = options();
    if (selected.filterType != app::AudioFilterType::None) {
        if (selected.cutoffHz >= static_cast<double>(selected.sampleRate) / 2.0) {
            return QStringLiteral("截止频率必须小于采样率的一半（%1 Hz）。")
                .arg(selected.sampleRate / 2U);
        }
        if (selected.tapCount % 2U == 0U) {
            return QStringLiteral("实时 FIR 抽头数必须为 3 至 255 之间的奇数。");
        }
    }
    return {};
}

void AudioCaptureWidget::refreshDevices()
{
    if (capturing_ || saveFuture_.valid()) {
        return;
    }
    const QString previousId = deviceCombo_->currentData().toString();
    const auto devices = service_.devices();
    deviceCombo_->clear();
    if (!devices) {
        statusLabel_->setText(QStringLiteral("无法读取麦克风列表：%1")
            .arg(QString::fromStdString(devices.error().message)));
    } else {
        for (const auto& device : devices.value()) {
            deviceCombo_->addItem(QString::fromStdString(device.name), QString::fromStdString(device.id));
        }
        const int previousIndex = deviceCombo_->findData(previousId);
        if (previousIndex >= 0) {
            deviceCombo_->setCurrentIndex(previousIndex);
        }
        statusLabel_->setText(devices.value().empty()
            ? QStringLiteral("没有可用的麦克风。")
            : QStringLiteral("设备已就绪，点击“开始采集”。"));
    }
    updateControls();
}

void AudioCaptureWidget::startCapture()
{
    if (capturing_ || saveFuture_.valid() || !validationMessage().isEmpty()) {
        return;
    }
    if (unsaved_ && sampleCount_ > 0U
        && QMessageBox::question(this, QStringLiteral("替换未保存的录音"),
            QStringLiteral("开始新的采集会替换当前未保存的录音。是否继续？"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
            != QMessageBox::Yes) {
        return;
    }
    const auto result = service_.start(options());
    if (!result) {
        statusLabel_->setText(QStringLiteral("开始采集失败：%1")
            .arg(QString::fromStdString(result.error().message)));
        updateControls();
        return;
    }
    capturing_ = true;
    unsaved_ = false;
    sampleCount_ = 0U;
    rawSeries_->clear();
    filteredSeries_->clear();
    statusLabel_->setText(QStringLiteral("正在采集并实时滤波；停止后可保存音频。"));
    poll();
}

void AudioCaptureWidget::stopCapture()
{
    if (!capturing_) {
        return;
    }
    service_.stop();
    capturing_ = false;
    statusLabel_->setText(QStringLiteral("已停止采集；录音保留，可选择原始或滤波版本保存。"));
    poll();
}

void AudioCaptureWidget::chooseSavePath(bool importAfterSave)
{
    if (capturing_ || saveFuture_.valid() || sampleCount_ == 0U) {
        return;
    }
    const QString name = QStringLiteral("capture-%1-%2.wav")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")))
        .arg(saveVersionCombo_->currentData().toBool()
            ? QStringLiteral("filtered") : QStringLiteral("raw"));
    QFileDialog dialog(this, QStringLiteral("保存采集音频"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(QStringLiteral("WAV 音频 (*.wav)"));
    dialog.setDefaultSuffix(QStringLiteral("wav"));
    dialog.selectFile(name);
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        return;
    }
    const QString path = dialog.selectedFiles().first();
    static_cast<void>(saveRecording(std::filesystem::path(path.toStdWString()), importAfterSave));
}

bool AudioCaptureWidget::saveRecording(
    const std::filesystem::path& path,
    bool importAfterSave)
{
    if (capturing_ || saveFuture_.valid() || sampleCount_ == 0U || path.empty()) {
        return false;
    }
    const bool filtered = saveVersionCombo_->currentData().toBool();
    savingPath_ = path;
    importAfterSave_ = importAfterSave;
    try {
        saveFuture_ = std::async(std::launch::async, [this, path, filtered]() {
            const auto signal = service_.recordedSignal(filtered);
            if (!signal) {
                return core::Result<bool>::failure(signal.error());
            }
            return io::writePcm16Wav(path, signal.value());
        });
    } catch (const std::exception& error) {
        statusLabel_->setText(QStringLiteral("无法开始保存：%1")
            .arg(QString::fromUtf8(error.what())));
        return false;
    }
    statusLabel_->setText(QStringLiteral("正在后台保存 WAV…"));
    updateControls();
    return true;
}

void AudioCaptureWidget::poll()
{
    if (saveFuture_.valid()
        && saveFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        try {
            const auto result = saveFuture_.get();
            if (result) {
                unsaved_ = false;
                statusLabel_->setText(QStringLiteral("已保存：%1")
                    .arg(QString::fromStdWString(savingPath_.wstring())));
                if (importAfterSave_ && importCallback_) {
                    importCallback_(savingPath_);
                }
            } else {
                statusLabel_->setText(QStringLiteral("保存失败，录音仍保留：%1")
                    .arg(QString::fromStdString(result.error().message)));
            }
        } catch (const std::exception& error) {
            statusLabel_->setText(QStringLiteral("保存失败，录音仍保留：%1")
                .arg(QString::fromUtf8(error.what())));
        }
        importAfterSave_ = false;
    }

    if (saveFuture_.valid()) {
        return;
    }

    auto snapshot = service_.snapshot();
    if (capturing_ && (snapshot.limitReached || !snapshot.error.empty())) {
        service_.stop();
        capturing_ = false;
        const QString reason = !snapshot.error.empty()
            ? QStringLiteral("采集已中断：%1").arg(QString::fromStdString(snapshot.error))
            : QStringLiteral("已达到最长录制时长，自动停止。");
        statusLabel_->setText(reason + QStringLiteral(" 已采集的音频仍可保存。"));
        snapshot = service_.snapshot();
    }
    if (snapshot.sampleCount != sampleCount_) {
        sampleCount_ = snapshot.sampleCount;
        unsaved_ = sampleCount_ > 0U;
    }
    durationLabel_->setText(QStringLiteral("时长：%1 秒")
        .arg(static_cast<double>(sampleCount_) / snapshot.sampleRate, 0, 'f', 1));
    sampleCountLabel_->setText(QStringLiteral("采样点：%1").arg(static_cast<qulonglong>(sampleCount_)));
    levelBar_->setValue(capturing_
        ? static_cast<int>(std::clamp(snapshot.peakLevel, 0.0, 1.0) * 100.0) : 0);
    QList<QPointF> rawPoints;
    QList<QPointF> filteredPoints;
    rawPoints.reserve(static_cast<qsizetype>(snapshot.preview.size()));
    filteredPoints.reserve(static_cast<qsizetype>(snapshot.preview.size()));
    double maximumAmplitude = 1.0;
    for (const auto& point : snapshot.preview) {
        rawPoints.append(QPointF(point.timeSeconds, point.raw));
        filteredPoints.append(QPointF(point.timeSeconds, point.filtered));
        maximumAmplitude = std::max({maximumAmplitude, std::abs(point.raw), std::abs(point.filtered)});
    }
    rawSeries_->replace(rawPoints);
    filteredSeries_->replace(filteredPoints);
    const double endTime = std::max(2.0, static_cast<double>(sampleCount_) / snapshot.sampleRate);
    timeAxis_->setRange(endTime - 2.0, endTime);
    amplitudeAxis_->setRange(-maximumAmplitude, maximumAmplitude);
    updateControls();
}

void AudioCaptureWidget::updateControls()
{
    const bool busy = capturing_ || saveFuture_.valid();
    const bool filterEnabled = options().filterType != app::AudioFilterType::None;
    deviceCombo_->setEnabled(!busy);
    refreshButton_->setEnabled(!busy);
    sampleRateCombo_->setEnabled(!busy);
    filterCombo_->setEnabled(!busy);
    cutoffSpin_->setEnabled(!busy && filterEnabled);
    tapCountSpin_->setEnabled(!busy && filterEnabled);
    maximumDurationSpin_->setEnabled(!busy);
    const QString validation = validationMessage();
    validationLabel_->setText(validation);
    validationLabel_->setVisible(!validation.isEmpty());
    startButton_->setEnabled(!busy && validation.isEmpty());
    stopButton_->setEnabled(capturing_);
    saveVersionCombo_->setEnabled(!busy);
    saveButton_->setEnabled(!busy && sampleCount_ > 0U);
    saveImportButton_->setEnabled(!busy && sampleCount_ > 0U && static_cast<bool>(importCallback_));
}

bool AudioCaptureWidget::requestClose()
{
    if (saveFuture_.valid()) {
        QMessageBox::information(this, QStringLiteral("正在保存"),
            QStringLiteral("音频仍在保存，请稍后再关闭窗口。"));
        return false;
    }
    if (capturing_) {
        if (QMessageBox::question(this, QStringLiteral("停止采集"),
                QStringLiteral("正在采集麦克风音频。是否停止采集并继续关闭？"),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
                != QMessageBox::Yes) {
            return false;
        }
        stopCapture();
    }
    if (unsaved_ && sampleCount_ > 0U) {
        const auto choice = QMessageBox::warning(this, QStringLiteral("录音尚未保存"),
            QStringLiteral("是否保存当前录音？选择保存后，请等待保存完成再关闭。"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (choice == QMessageBox::Save) {
            chooseSavePath(false);
            return false;
        }
        return choice == QMessageBox::Discard;
    }
    return true;
}

void AudioCaptureWidget::closeEvent(QCloseEvent* event)
{
    if (requestClose()) {
        event->accept();
    } else {
        event->ignore();
    }
}

}
