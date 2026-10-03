#include "file_import_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace signallab::gui {
namespace {

constexpr std::size_t maximumDisplayedColumns = 64U;

QString pathText(const std::filesystem::path& path)
{
    return QString::fromStdWString(path.wstring());
}

QString previewSummary(const repository::ImportPreview& preview)
{
    QString summary = QStringLiteral("样本数：%1    列数：%2    通道数：%3")
                          .arg(static_cast<qulonglong>(preview.sampleCount))
                          .arg(static_cast<qulonglong>(preview.columnCount))
                          .arg(static_cast<qulonglong>(preview.channelCount));
    if (preview.sampleRate.has_value()) {
        summary += QStringLiteral("    采样率：%1 Hz")
                       .arg(*preview.sampleRate, 0, 'f', 3);
    }
    return summary;
}

}  // namespace

FileImportDialog::FileImportDialog(
    std::filesystem::path sourcePath,
    repository::CsvImportOptions options,
    QWidget* parent)
    : QDialog(parent),
      mode_(Mode::Csv),
      sourcePath_(std::move(sourcePath)),
      initialCsvOptions_(std::move(options))
{
    buildUi();
}

FileImportDialog::FileImportDialog(
    std::filesystem::path sourcePath,
    repository::WavImportOptions options,
    QWidget* parent)
    : QDialog(parent),
      mode_(Mode::Wav),
      sourcePath_(std::move(sourcePath)),
      initialWavOptions_(std::move(options))
{
    buildUi();
}

FileImportDialog::Mode FileImportDialog::mode() const noexcept
{
    return mode_;
}

const std::filesystem::path& FileImportDialog::sourcePath() const noexcept
{
    return sourcePath_;
}

std::uint64_t FileImportDialog::revision() const noexcept
{
    return revision_;
}

bool FileImportDialog::isPreviewCurrent() const noexcept
{
    return acceptedPreviewRevision_.has_value()
        && *acceptedPreviewRevision_ == revision_;
}

bool FileImportDialog::isPreviewPending() const noexcept
{
    return pendingPreviewRevision_.has_value()
        && *pendingPreviewRevision_ == revision_;
}

std::optional<repository::CsvImportOptions>
FileImportDialog::csvOptions() const
{
    if (mode_ != Mode::Csv || !initialCsvOptions_.has_value()) {
        return std::nullopt;
    }

    repository::CsvImportOptions options = *initialCsvOptions_;
    options.delimiter = static_cast<repository::CsvDelimiter>(
        delimiterCombo_->currentData().toInt());
    options.hasHeader = hasHeaderCheck_->isChecked();
    options.valueColumn = static_cast<std::size_t>(
        valueColumnSpin_->value() - 1);
    if (timeSourceCombo_->currentIndex() == 0) {
        options.timeColumn = static_cast<std::size_t>(
            timeColumnSpin_->value() - 1);
    } else {
        options.timeColumn.reset();
        options.sampleRateIfNoTimeColumn = sampleRateSpin_->value();
    }
    options.metadata.name = nameEdit_->text().toStdString();
    options.metadata.unit = unitEdit_->text().toStdString();
    return options;
}

std::optional<repository::WavImportOptions>
FileImportDialog::wavOptions() const
{
    if (mode_ != Mode::Wav || !initialWavOptions_.has_value()) {
        return std::nullopt;
    }

    repository::WavImportOptions options = *initialWavOptions_;
    options.channel = static_cast<repository::StereoChannel>(
        wavChannelCombo_->currentData().toInt());
    options.metadata.name = nameEdit_->text().toStdString();
    options.metadata.unit = unitEdit_->text().toStdString();
    return options;
}

void FileImportDialog::setPreviewRequestCallback(
    PreviewRequestCallback callback)
{
    previewRequestCallback_ = std::move(callback);
    updateButtons();
}

bool FileImportDialog::requestPreview()
{
    QString validationMessage;
    if (!validateOptions(&validationMessage)) {
        return applyPreviewError(validationMessage);
    }
    if (!previewRequestCallback_) {
        return applyPreviewError(
            QStringLiteral("预览服务尚未连接，请关闭窗口后重试。"));
    }

    acceptedPreviewRevision_.reset();
    pendingPreviewRevision_ = revision_;
    previewErrorLabel_->clear();
    previewSummaryLabel_->setText(QStringLiteral("正在生成预览…"));
    previewTable_->setEnabled(false);
    updateButtons();

    const PreviewRequestCallback callback = previewRequestCallback_;
    callback(revision_);
    return true;
}

bool FileImportDialog::setPreview(
    std::uint64_t revision,
    const repository::ImportPreview& preview)
{
    if (pendingPreviewRevision_ == revision) {
        pendingPreviewRevision_.reset();
    }
    if (revision != revision_) {
        updateButtons();
        return false;
    }

    const repository::ImportFormat expectedFormat = mode_ == Mode::Csv
        ? repository::ImportFormat::Csv
        : repository::ImportFormat::Wav;
    if (preview.format != expectedFormat) {
        return applyPreviewError(
            QStringLiteral("预览格式与当前导入类型不一致。"));
    }
    if (mode_ == Mode::Wav
        && preview.channelCount != 1U
        && preview.channelCount != 2U) {
        return applyPreviewError(
            QStringLiteral("WAV 只支持单声道或双声道。"));
    }

    if (mode_ == Mode::Wav) {
        if (preview.channelCount == 1U) {
            const QSignalBlocker blocker(wavChannelCombo_);
            wavChannelCombo_->setCurrentIndex(0);
            wavChannelCombo_->setEnabled(false);
        } else {
            wavChannelCombo_->setEnabled(true);
        }
    }

    acceptedPreviewRevision_ = revision;
    previewErrorLabel_->clear();
    renderPreview(preview);
    previewTable_->setEnabled(true);
    updateButtons();
    return true;
}

bool FileImportDialog::setPreviewError(
    std::uint64_t revision,
    const QString& message)
{
    if (pendingPreviewRevision_ == revision) {
        pendingPreviewRevision_.reset();
    }
    if (revision != revision_) {
        updateButtons();
        return false;
    }
    static_cast<void>(applyPreviewError(message));
    return true;
}

void FileImportDialog::buildUi()
{
    setWindowTitle(mode_ == Mode::Csv
            ? QStringLiteral("CSV 导入设置")
            : QStringLiteral("WAV 导入设置"));
    setModal(true);
    resize(760, 600);

    auto* layout = new QVBoxLayout(this);

    auto* sourceTitle = new QLabel(QStringLiteral("源文件"), this);
    sourceTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
    layout->addWidget(sourceTitle);

    auto* sourceValue = new QLabel(pathText(sourcePath_), this);
    sourceValue->setObjectName(QStringLiteral("importSourcePathLabel"));
    sourceValue->setWordWrap(true);
    sourceValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(sourceValue);

    auto* metadataGroup = new QGroupBox(
        QStringLiteral("信号信息"), this);
    auto* metadataLayout = new QFormLayout(metadataGroup);
    nameEdit_ = new QLineEdit(metadataGroup);
    nameEdit_->setObjectName(QStringLiteral("importSignalNameEdit"));
    unitEdit_ = new QLineEdit(metadataGroup);
    unitEdit_->setObjectName(QStringLiteral("importSignalUnitEdit"));

    const core::SignalMetadata& metadata = mode_ == Mode::Csv
        ? initialCsvOptions_->metadata
        : initialWavOptions_->metadata;
    nameEdit_->setText(QString::fromStdString(metadata.name));
    unitEdit_->setText(QString::fromStdString(metadata.unit));
    metadataLayout->addRow(QStringLiteral("名称"), nameEdit_);
    metadataLayout->addRow(QStringLiteral("单位"), unitEdit_);
    layout->addWidget(metadataGroup);

    if (mode_ == Mode::Csv) {
        buildCsvControls();
    } else {
        buildWavControls();
    }

    auto* previewGroup = new QGroupBox(QStringLiteral("文件预览"), this);
    auto* previewLayout = new QVBoxLayout(previewGroup);
    previewSummaryLabel_ = new QLabel(
        QStringLiteral("请检查设置，然后生成预览。"), previewGroup);
    previewSummaryLabel_->setObjectName(
        QStringLiteral("importPreviewSummaryLabel"));
    previewSummaryLabel_->setWordWrap(true);
    previewLayout->addWidget(previewSummaryLabel_);

    previewErrorLabel_ = new QLabel(previewGroup);
    previewErrorLabel_->setObjectName(
        QStringLiteral("importPreviewErrorLabel"));
    previewErrorLabel_->setWordWrap(true);
    previewErrorLabel_->setStyleSheet(QStringLiteral("color: #a02020;"));
    previewLayout->addWidget(previewErrorLabel_);

    previewTable_ = new QTableWidget(previewGroup);
    previewTable_->setObjectName(QStringLiteral("importPreviewTable"));
    previewTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    previewTable_->setSelectionMode(QAbstractItemView::NoSelection);
    previewTable_->horizontalHeader()->setStretchLastSection(true);
    previewTable_->verticalHeader()->setVisible(false);
    previewTable_->setEnabled(false);
    previewLayout->addWidget(previewTable_, 1);
    layout->addWidget(previewGroup, 1);

    buttonBox_ = new QDialogButtonBox(this);
    buttonBox_->setObjectName(QStringLiteral("importDialogButtonBox"));
    previewButton_ = buttonBox_->addButton(
        QStringLiteral("更新预览"), QDialogButtonBox::ActionRole);
    previewButton_->setObjectName(QStringLiteral("importPreviewButton"));
    importButton_ = buttonBox_->addButton(
        QStringLiteral("导入"), QDialogButtonBox::AcceptRole);
    importButton_->setObjectName(QStringLiteral("importAcceptButton"));
    auto* cancelButton = buttonBox_->addButton(QDialogButtonBox::Cancel);
    cancelButton->setObjectName(QStringLiteral("importCancelButton"));
    importButton_->setDefault(true);
    layout->addWidget(buttonBox_);

    connect(previewButton_, &QPushButton::clicked, this, [this]() {
        static_cast<void>(requestPreview());
    });
    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connectOptionChanges();
    updateButtons();
}

void FileImportDialog::buildCsvControls()
{
    auto* optionsGroup = new QGroupBox(QStringLiteral("CSV 参数"), this);
    auto* form = new QFormLayout(optionsGroup);

    delimiterCombo_ = new QComboBox(optionsGroup);
    delimiterCombo_->setObjectName(QStringLiteral("importDelimiterCombo"));
    delimiterCombo_->addItem(
        QStringLiteral("逗号 (, )"),
        static_cast<int>(repository::CsvDelimiter::Comma));
    delimiterCombo_->addItem(
        QStringLiteral("制表符 (Tab)"),
        static_cast<int>(repository::CsvDelimiter::Tab));
    delimiterCombo_->setCurrentIndex(delimiterCombo_->findData(
        static_cast<int>(initialCsvOptions_->delimiter)));

    hasHeaderCheck_ = new QCheckBox(
        QStringLiteral("第一行是列名"), optionsGroup);
    hasHeaderCheck_->setObjectName(QStringLiteral("importHasHeaderCheck"));
    hasHeaderCheck_->setChecked(initialCsvOptions_->hasHeader);

    valueColumnSpin_ = new QSpinBox(optionsGroup);
    valueColumnSpin_->setObjectName(
        QStringLiteral("importValueColumnSpin"));
    valueColumnSpin_->setRange(1, 1024);
    valueColumnSpin_->setValue(
        static_cast<int>(initialCsvOptions_->valueColumn + 1U));

    timeSourceCombo_ = new QComboBox(optionsGroup);
    timeSourceCombo_->setObjectName(
        QStringLiteral("importTimeSourceCombo"));
    timeSourceCombo_->addItem(QStringLiteral("使用时间列"));
    timeSourceCombo_->addItem(QStringLiteral("直接指定采样率"));
    timeSourceCombo_->setCurrentIndex(
        initialCsvOptions_->timeColumn.has_value() ? 0 : 1);

    timeColumnSpin_ = new QSpinBox(optionsGroup);
    timeColumnSpin_->setObjectName(
        QStringLiteral("importTimeColumnSpin"));
    timeColumnSpin_->setRange(1, 1024);
    timeColumnSpin_->setValue(static_cast<int>(
        initialCsvOptions_->timeColumn.value_or(0U) + 1U));

    sampleRateSpin_ = new QDoubleSpinBox(optionsGroup);
    sampleRateSpin_->setObjectName(
        QStringLiteral("importSampleRateSpin"));
    sampleRateSpin_->setRange(0.001, 1'000'000'000.0);
    sampleRateSpin_->setDecimals(3);
    sampleRateSpin_->setSuffix(QStringLiteral(" Hz"));
    sampleRateSpin_->setValue(
        initialCsvOptions_->sampleRateIfNoTimeColumn);

    form->addRow(QStringLiteral("分隔符"), delimiterCombo_);
    form->addRow(QString(), hasHeaderCheck_);
    form->addRow(QStringLiteral("数值列（从 1 开始）"), valueColumnSpin_);
    form->addRow(QStringLiteral("时间来源"), timeSourceCombo_);
    form->addRow(QStringLiteral("时间列（从 1 开始）"), timeColumnSpin_);
    form->addRow(QStringLiteral("采样率"), sampleRateSpin_);
    layout()->addWidget(optionsGroup);
    updateTimeSourceControls();
}

void FileImportDialog::buildWavControls()
{
    auto* optionsGroup = new QGroupBox(QStringLiteral("WAV 参数"), this);
    auto* form = new QFormLayout(optionsGroup);

    wavChannelCombo_ = new QComboBox(optionsGroup);
    wavChannelCombo_->setObjectName(
        QStringLiteral("importWavChannelCombo"));
    wavChannelCombo_->addItem(
        QStringLiteral("左右平均"),
        static_cast<int>(repository::StereoChannel::Average));
    wavChannelCombo_->addItem(
        QStringLiteral("左声道"),
        static_cast<int>(repository::StereoChannel::Left));
    wavChannelCombo_->addItem(
        QStringLiteral("右声道"),
        static_cast<int>(repository::StereoChannel::Right));
    const int initialIndex = wavChannelCombo_->findData(
        static_cast<int>(initialWavOptions_->channel));
    wavChannelCombo_->setCurrentIndex(initialIndex >= 0 ? initialIndex : 0);
    wavChannelCombo_->setEnabled(false);
    wavChannelCombo_->setToolTip(
        QStringLiteral("预览读取通道数后即可选择。"));

    form->addRow(QStringLiteral("导入通道"), wavChannelCombo_);
    layout()->addWidget(optionsGroup);
}

void FileImportDialog::connectOptionChanges()
{
    connect(nameEdit_, &QLineEdit::textChanged, this, [this]() {
        invalidatePreview();
    });
    connect(unitEdit_, &QLineEdit::textChanged, this, [this]() {
        invalidatePreview();
    });

    if (mode_ == Mode::Csv) {
        connect(
            delimiterCombo_,
            &QComboBox::currentIndexChanged,
            this,
            [this]() { invalidatePreview(); });
        connect(hasHeaderCheck_, &QCheckBox::toggled, this, [this]() {
            invalidatePreview();
        });
        connect(
            valueColumnSpin_,
            &QSpinBox::valueChanged,
            this,
            [this]() { invalidatePreview(); });
        connect(
            timeSourceCombo_,
            &QComboBox::currentIndexChanged,
            this,
            [this]() {
                updateTimeSourceControls();
                invalidatePreview();
            });
        connect(
            timeColumnSpin_,
            &QSpinBox::valueChanged,
            this,
            [this]() { invalidatePreview(); });
        connect(
            sampleRateSpin_,
            &QDoubleSpinBox::valueChanged,
            this,
            [this]() { invalidatePreview(); });
    } else {
        connect(
            wavChannelCombo_,
            &QComboBox::currentIndexChanged,
            this,
            [this]() { invalidatePreview(); });
    }
}

void FileImportDialog::invalidatePreview()
{
    ++revision_;
    acceptedPreviewRevision_.reset();
    previewErrorLabel_->clear();
    previewSummaryLabel_->setText(
        QStringLiteral("设置已更改，请更新预览。"));
    previewTable_->setEnabled(false);
    updateButtons();
}

void FileImportDialog::updateTimeSourceControls()
{
    if (mode_ != Mode::Csv) {
        return;
    }
    const bool useTimeColumn = timeSourceCombo_->currentIndex() == 0;
    timeColumnSpin_->setEnabled(useTimeColumn);
    sampleRateSpin_->setEnabled(!useTimeColumn);
}

void FileImportDialog::updateButtons()
{
    if (previewButton_ == nullptr || importButton_ == nullptr) {
        return;
    }
    previewButton_->setEnabled(!isPreviewPending());
    importButton_->setEnabled(isPreviewCurrent());
}

void FileImportDialog::clearPreviewTable()
{
    previewTable_->clear();
    previewTable_->setRowCount(0);
    previewTable_->setColumnCount(0);
    previewTable_->setEnabled(false);
}

void FileImportDialog::renderPreview(
    const repository::ImportPreview& preview)
{
    previewSummaryLabel_->setText(previewSummary(preview));

    std::size_t columnCount = std::max(
        preview.columnCount, preview.columnNames.size());
    for (const auto& row : preview.rows) {
        columnCount = std::max(columnCount, row.size());
    }
    columnCount = std::min(columnCount, maximumDisplayedColumns);
    const std::size_t rowCount = std::min<std::size_t>(
        preview.rows.size(), 5U);

    previewTable_->clear();
    previewTable_->setColumnCount(static_cast<int>(columnCount));
    previewTable_->setRowCount(static_cast<int>(rowCount));

    for (std::size_t column = 0; column < columnCount; ++column) {
        const QString title = column < preview.columnNames.size()
            ? QString::fromStdString(preview.columnNames[column])
            : QStringLiteral("第 %1 列").arg(
                  static_cast<qulonglong>(column + 1U));
        previewTable_->setHorizontalHeaderItem(
            static_cast<int>(column), new QTableWidgetItem(title));
    }

    for (std::size_t row = 0; row < rowCount; ++row) {
        const std::size_t cellCount = std::min(
            preview.rows[row].size(), columnCount);
        for (std::size_t column = 0; column < cellCount; ++column) {
            previewTable_->setItem(
                static_cast<int>(row),
                static_cast<int>(column),
                new QTableWidgetItem(QString::fromStdString(
                    preview.rows[row][column])));
        }
    }
    previewTable_->resizeColumnsToContents();
}

bool FileImportDialog::validateOptions(QString* message) const
{
    if (message != nullptr) {
        message->clear();
    }
    if (mode_ != Mode::Csv) {
        return true;
    }

    const auto options = csvOptions();
    if (!options.has_value()) {
        if (message != nullptr) {
            *message = QStringLiteral("CSV 参数不可用。");
        }
        return false;
    }
    if (options->timeColumn.has_value()
        && *options->timeColumn == options->valueColumn) {
        if (message != nullptr) {
            *message = QStringLiteral("时间列和数值列不能是同一列。");
        }
        return false;
    }
    return true;
}

bool FileImportDialog::applyPreviewError(const QString& message)
{
    pendingPreviewRevision_.reset();
    acceptedPreviewRevision_.reset();
    previewSummaryLabel_->setText(QStringLiteral("预览未通过。"));
    previewErrorLabel_->setText(message.isEmpty()
            ? QStringLiteral("文件预览失败，请检查设置后重试。")
            : message);
    clearPreviewTable();
    updateButtons();
    return false;
}

}  // namespace signallab::gui
