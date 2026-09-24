#include "stage7_models.hpp"

#include <QApplication>
#include <QIcon>
#include <QStyle>
#include <QVariant>

#include <algorithm>
#include <limits>
#include <utility>

namespace signallab::gui {
namespace {

QString taskStatusText(task::TaskStatus status)
{
    switch (status) {
    case task::TaskStatus::Pending:
        return QStringLiteral("等待中");
    case task::TaskStatus::Running:
        return QStringLiteral("运行中");
    case task::TaskStatus::Succeeded:
        return QStringLiteral("已完成");
    case task::TaskStatus::Failed:
        return QStringLiteral("失败");
    case task::TaskStatus::Cancelled:
        return QStringLiteral("已取消");
    }
    return QStringLiteral("未知");
}

QString windowTypeText(algorithm::WindowType type)
{
    switch (type) {
    case algorithm::WindowType::Rectangular:
        return QStringLiteral("矩形窗");
    case algorithm::WindowType::Hann:
        return QStringLiteral("Hann 窗");
    case algorithm::WindowType::Hamming:
        return QStringLiteral("Hamming 窗");
    }
    return QStringLiteral("未知窗");
}

QString numberText(double value)
{
    return QString::number(value, 'g', 12);
}

QString taskLaneText(task::TaskLane lane)
{
    switch (lane) {
    case task::TaskLane::Compute:
        return QStringLiteral("计算");
    case task::TaskLane::Io:
        return QStringLiteral("I/O");
    }
    return QStringLiteral("未知");
}

QString analysisStatusText(repository::AnalysisTaskStatus status)
{
    switch (status) {
    case repository::AnalysisTaskStatus::Pending:
        return QStringLiteral("等待中");
    case repository::AnalysisTaskStatus::Running:
        return QStringLiteral("运行中");
    case repository::AnalysisTaskStatus::Succeeded:
        return QStringLiteral("已完成");
    case repository::AnalysisTaskStatus::Failed:
        return QStringLiteral("失败");
    case repository::AnalysisTaskStatus::Interrupted:
        return QStringLiteral("已中断");
    case repository::AnalysisTaskStatus::Cancelled:
        return QStringLiteral("已取消");
    }
    return QStringLiteral("未知");
}

QString scriptStatusText(
    repository::ScriptHistoryStatus status,
    const std::string& persistedText)
{
    switch (status) {
    case repository::ScriptHistoryStatus::Running:
        return QStringLiteral("运行中");
    case repository::ScriptHistoryStatus::Succeeded:
        return QStringLiteral("已完成");
    case repository::ScriptHistoryStatus::Failed:
        return QStringLiteral("失败");
    case repository::ScriptHistoryStatus::Cancelled:
        return QStringLiteral("已取消");
    case repository::ScriptHistoryStatus::Interrupted:
        return QStringLiteral("已中断");
    case repository::ScriptHistoryStatus::Legacy:
        return persistedText.empty()
            ? QStringLiteral("旧版记录")
            : QStringLiteral("旧版记录（%1）")
                  .arg(QString::fromStdString(persistedText));
    }
    return QStringLiteral("未知");
}

QIcon standardIcon(QStyle::StandardPixmap pixmap)
{
    QStyle* const style = QApplication::style();
    return style == nullptr ? QIcon{} : style->standardIcon(pixmap);
}

QIcon taskStatusIcon(task::TaskStatus status)
{
    switch (status) {
    case task::TaskStatus::Pending:
        return standardIcon(QStyle::SP_BrowserReload);
    case task::TaskStatus::Running:
        return standardIcon(QStyle::SP_MediaPlay);
    case task::TaskStatus::Succeeded:
        return standardIcon(QStyle::SP_DialogApplyButton);
    case task::TaskStatus::Failed:
        return standardIcon(QStyle::SP_MessageBoxCritical);
    case task::TaskStatus::Cancelled:
        return standardIcon(QStyle::SP_DialogCancelButton);
    }
    return {};
}

QIcon analysisStatusIcon(repository::AnalysisTaskStatus status)
{
    switch (status) {
    case repository::AnalysisTaskStatus::Pending:
        return standardIcon(QStyle::SP_BrowserReload);
    case repository::AnalysisTaskStatus::Running:
        return standardIcon(QStyle::SP_MediaPlay);
    case repository::AnalysisTaskStatus::Succeeded:
        return standardIcon(QStyle::SP_DialogApplyButton);
    case repository::AnalysisTaskStatus::Failed:
        return standardIcon(QStyle::SP_MessageBoxCritical);
    case repository::AnalysisTaskStatus::Interrupted:
        return standardIcon(QStyle::SP_MessageBoxWarning);
    case repository::AnalysisTaskStatus::Cancelled:
        return standardIcon(QStyle::SP_DialogCancelButton);
    }
    return {};
}

QIcon scriptStatusIcon(repository::ScriptHistoryStatus status)
{
    switch (status) {
    case repository::ScriptHistoryStatus::Running:
        return standardIcon(QStyle::SP_MediaPlay);
    case repository::ScriptHistoryStatus::Succeeded:
        return standardIcon(QStyle::SP_DialogApplyButton);
    case repository::ScriptHistoryStatus::Failed:
        return standardIcon(QStyle::SP_MessageBoxCritical);
    case repository::ScriptHistoryStatus::Cancelled:
        return standardIcon(QStyle::SP_DialogCancelButton);
    case repository::ScriptHistoryStatus::Interrupted:
        return standardIcon(QStyle::SP_MessageBoxWarning);
    case repository::ScriptHistoryStatus::Legacy:
        return standardIcon(QStyle::SP_FileDialogDetailedView);
    }
    return {};
}

QString taskSuggestion(task::TaskStatus status)
{
    switch (status) {
    case task::TaskStatus::Pending:
        return QStringLiteral("可在任务开始前取消。");
    case task::TaskStatus::Running:
        return QStringLiteral("可取消任务；取消会在安全检查点生效。");
    case task::TaskStatus::Succeeded:
        return QStringLiteral("可在结果区查看或导出结果。");
    case task::TaskStatus::Failed:
        return QStringLiteral("请检查输入文件、参数和详细错误，然后重试。");
    case task::TaskStatus::Cancelled:
        return QStringLiteral("如需继续，请选中该任务后重试。");
    }
    return {};
}

QString analysisSuggestion(repository::AnalysisTaskStatus status)
{
    switch (status) {
    case repository::AnalysisTaskStatus::Pending:
        return QStringLiteral("分析正在等待调度。");
    case repository::AnalysisTaskStatus::Running:
        return QStringLiteral("分析正在后台运行。");
    case repository::AnalysisTaskStatus::Succeeded:
        return QStringLiteral("可选中此记录查看特征和分析参数。");
    case repository::AnalysisTaskStatus::Failed:
        return QStringLiteral("请检查输入信号和分析参数后重试。");
    case repository::AnalysisTaskStatus::Interrupted:
        return QStringLiteral("程序上次退出时分析未完成，请重新运行。");
    case repository::AnalysisTaskStatus::Cancelled:
        return QStringLiteral("如需结果，请重新运行该分析。");
    }
    return {};
}

QString scriptSuggestion(repository::ScriptHistoryStatus status)
{
    switch (status) {
    case repository::ScriptHistoryStatus::Running:
        return QStringLiteral("脚本正在后台运行。");
    case repository::ScriptHistoryStatus::Succeeded:
        return QStringLiteral("可查看变量、绘图请求和导出结果。");
    case repository::ScriptHistoryStatus::Failed:
        return QStringLiteral("请跳转到错误位置，修正脚本后重新运行。");
    case repository::ScriptHistoryStatus::Cancelled:
        return QStringLiteral("如需继续，请重新运行脚本。");
    case repository::ScriptHistoryStatus::Interrupted:
        return QStringLiteral("程序上次退出时脚本未完成，请重新运行。");
    case repository::ScriptHistoryStatus::Legacy:
        return QStringLiteral("此记录来自旧版数据库，状态仅供参考。");
    }
    return {};
}

template <typename Container>
bool validRow(const QModelIndex& index, const Container& container)
{
    return index.isValid()
        && index.row() >= 0
        && static_cast<std::size_t>(index.row()) < container.size();
}

}  // namespace

AnalysisResultModel::AnalysisResultModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int AnalysisResultModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(std::min<std::size_t>(
        rows_.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

int AnalysisResultModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant AnalysisResultModel::data(const QModelIndex& index, int role) const
{
    if (!validRow(index, rows_)
        || index.model() != this
        || index.column() < MetricColumn
        || index.column() >= ColumnCount) {
        return {};
    }

    const ResultRow& row = rows_[static_cast<std::size_t>(index.row())];
    if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) {
        switch (index.column()) {
        case MetricColumn:
            return row.metric;
        case ValueColumn:
            return row.value;
        case UnitColumn:
            return row.unit;
        default:
            return {};
        }
    }
    if (role == Qt::ToolTipRole || role == Qt::AccessibleDescriptionRole) {
        return QStringLiteral("%1：%2 %3")
            .arg(row.metric, row.value, row.unit);
    }
    switch (role) {
    case MetricRole:
        return row.metric;
    case ValueRole:
        return row.value;
    case UnitRole:
        return row.unit;
    default:
        return {};
    }
}

QVariant AnalysisResultModel::headerData(
    int section,
    Qt::Orientation orientation,
    int role) const
{
    if (role != Qt::DisplayRole) {
        return {};
    }
    if (orientation == Qt::Vertical) {
        return section + 1;
    }
    switch (section) {
    case MetricColumn:
        return QStringLiteral("指标");
    case ValueColumn:
        return QStringLiteral("数值");
    case UnitColumn:
        return QStringLiteral("单位");
    default:
        return {};
    }
}

QHash<int, QByteArray> AnalysisResultModel::roleNames() const
{
    auto roles = QAbstractTableModel::roleNames();
    roles.insert(MetricRole, "metric");
    roles.insert(ValueRole, "value");
    roles.insert(UnitRole, "unit");
    return roles;
}

void AnalysisResultModel::setResult(
    const app::AnalysisResult& result,
    const QString& signalUnit,
    const app::AnalysisOptions& options,
    const QString& algorithmVersion)
{
    const QString normalizedUnit = signalUnit.trimmed().isEmpty()
        ? QStringLiteral("未指定")
        : signalUnit.trimmed();
    const QString squaredUnit = signalUnit.trimmed().isEmpty()
        ? QStringLiteral("未指定²")
        : QStringLiteral("%1²").arg(signalUnit.trimmed());
    const QString missing = QStringLiteral("—");

    std::vector<ResultRow> replacement;
    replacement.reserve(9);
    replacement.push_back(ResultRow{
        QStringLiteral("均值"),
        numberText(result.features.mean),
        normalizedUnit
    });
    replacement.push_back(ResultRow{
        QStringLiteral("方差"),
        numberText(result.features.variance),
        squaredUnit
    });
    replacement.push_back(ResultRow{
        QStringLiteral("均方根"),
        numberText(result.features.rms),
        normalizedUnit
    });
    replacement.push_back(ResultRow{
        QStringLiteral("峰峰值"),
        numberText(result.features.peakToPeak),
        normalizedUnit
    });
    replacement.push_back(ResultRow{
        QStringLiteral("主导频率"),
        result.dominantPeak.has_value()
            ? numberText(result.dominantPeak->frequencyHz)
            : missing,
        QStringLiteral("Hz")
    });
    replacement.push_back(ResultRow{
        QStringLiteral("主导幅值"),
        result.dominantPeak.has_value()
            ? numberText(result.dominantPeak->amplitude)
            : missing,
        normalizedUnit
    });
    replacement.push_back(ResultRow{
        QStringLiteral("信噪比"),
        result.snrDb.has_value() ? numberText(*result.snrDb) : missing,
        QStringLiteral("dB")
    });
    replacement.push_back(ResultRow{
        QStringLiteral("总谐波失真"),
        result.thdPercent.has_value()
            ? numberText(*result.thdPercent)
            : missing,
        QStringLiteral("%")
    });
    replacement.push_back(ResultRow{
        QStringLiteral("FFT 规模"),
        QString::number(static_cast<qulonglong>(result.spectrum.fftSize)),
        QStringLiteral("点")
    });

    const QString summary = QStringLiteral(
        "窗函数：%1；最小峰值比：%2；最小峰间距：%3 Hz；"
        "最大峰数：%4；保护频点：%5；THD 谐波数：%6")
        .arg(windowTypeText(options.windowType))
        .arg(options.minimumPeakAmplitudeRatio, 0, 'g', 12)
        .arg(options.minimumPeakDistanceHz, 0, 'g', 12)
        .arg(static_cast<qulonglong>(options.maximumPeakCount))
        .arg(static_cast<qulonglong>(options.metricGuardBins))
        .arg(static_cast<qulonglong>(options.thdHarmonicCount));

    beginResetModel();
    rows_ = std::move(replacement);
    parameterSummary_ = summary;
    algorithmVersion_ = algorithmVersion;
    endResetModel();
}

void AnalysisResultModel::clear()
{
    beginResetModel();
    rows_.clear();
    parameterSummary_.clear();
    algorithmVersion_.clear();
    endResetModel();
}

const ResultRow* AnalysisResultModel::rowAt(int row) const noexcept
{
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return nullptr;
    }
    return &rows_[static_cast<std::size_t>(row)];
}

QString AnalysisResultModel::parameterSummary() const
{
    return parameterSummary_;
}

QString AnalysisResultModel::algorithmVersion() const
{
    return algorithmVersion_;
}

SignalListModel::SignalListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int SignalListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(std::min<std::size_t>(
        items_.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

QVariant SignalListModel::data(const QModelIndex& index, int role) const
{
    if (!validRow(index, items_)
        || index.model() != this || index.column() != 0) {
        return {};
    }

    const Item& item = items_[static_cast<std::size_t>(index.row())];
    const QString description = QStringLiteral(
        "%1\n记录 ID：%2\n采样率：%3 Hz\n样本数：%4\n时长：%5 s\n单位：%6\n来源：%7")
        .arg(item.displayName)
        .arg(item.recordId)
        .arg(item.sampleRate, 0, 'g', 12)
        .arg(static_cast<qulonglong>(item.sampleCount))
        .arg(item.durationSeconds, 0, 'g', 12)
        .arg(item.unit.isEmpty() ? QStringLiteral("未指定") : item.unit)
        .arg(item.source.isEmpty() ? QStringLiteral("未指定") : item.source);

    switch (role) {
    case Qt::DisplayRole:
    case Qt::AccessibleTextRole:
        return item.displayName;
    case Qt::ToolTipRole:
    case Qt::AccessibleDescriptionRole:
        return description;
    case RecordIdRole:
        return QVariant::fromValue<qlonglong>(item.recordId);
    case SourceIdRole:
        return QVariant::fromValue<qlonglong>(item.sourceId);
    case DisplayNameRole:
        return item.displayName;
    case SampleRateRole:
        return item.sampleRate;
    case SampleCountRole:
        return QVariant::fromValue<qulonglong>(item.sampleCount);
    default:
        return {};
    }
}

QHash<int, QByteArray> SignalListModel::roleNames() const
{
    auto roles = QAbstractListModel::roleNames();
    roles.insert(RecordIdRole, "recordId");
    roles.insert(SourceIdRole, "sourceId");
    roles.insert(DisplayNameRole, "displayName");
    roles.insert(SampleRateRole, "sampleRate");
    roles.insert(SampleCountRole, "sampleCount");
    return roles;
}

void SignalListModel::setEntries(const std::vector<app::SignalEntry>& entries)
{
    std::vector<Item> replacement;
    replacement.reserve(entries.size());
    for (const auto& entry : entries) {
        replacement.push_back(Item{
            entry.recordId,
            entry.sourceId,
            QString::fromStdString(entry.displayName),
            QString::fromStdString(entry.signal.metadata().unit),
            QString::fromStdString(entry.signal.metadata().source),
            entry.signal.sampleRate(),
            entry.signal.sampleCount(),
            entry.signal.durationSeconds()
        });
    }

    beginResetModel();
    items_ = std::move(replacement);
    endResetModel();
}

std::optional<std::int64_t> SignalListModel::recordIdAt(int row) const noexcept
{
    if (row < 0 || static_cast<std::size_t>(row) >= items_.size()) {
        return std::nullopt;
    }
    return items_[static_cast<std::size_t>(row)].recordId;
}

std::optional<std::int64_t> SignalListModel::recordIdAt(
    const QModelIndex& index) const noexcept
{
    return recordIdAt(index.row());
}

TaskListModel::TaskListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int TaskListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(std::min<std::size_t>(
        snapshots_.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

QVariant TaskListModel::data(const QModelIndex& index, int role) const
{
    if (!validRow(index, snapshots_)
        || index.model() != this || index.column() != 0) {
        return {};
    }

    const auto& snapshot = snapshots_[static_cast<std::size_t>(index.row())];
    const QString name = QString::fromStdString(snapshot.name);
    const QString status = taskStatusText(snapshot.status);
    const QString progress = QString::fromStdString(snapshot.progressMessage);
    const QString error = QString::fromStdString(snapshot.errorMessage);
    const QString suggestion = taskSuggestion(snapshot.status);
    QString description = QStringLiteral(
        "%1\n状态：%2\n通道：%3\n进度：%4%")
        .arg(name, status, taskLaneText(snapshot.lane))
        .arg(snapshot.progressPercent);
    if (!progress.isEmpty()) {
        description += QStringLiteral("\n进度说明：%1").arg(progress);
    }
    if (!error.isEmpty()) {
        description += QStringLiteral("\n错误：%1").arg(error);
    }
    if (!suggestion.isEmpty()) {
        description += QStringLiteral("\n建议：%1").arg(suggestion);
    }

    switch (role) {
    case Qt::DisplayRole:
        return QStringLiteral("%1 · %2 · %3%")
            .arg(name, status)
            .arg(snapshot.progressPercent);
    case Qt::DecorationRole:
        return taskStatusIcon(snapshot.status);
    case Qt::ToolTipRole:
    case Qt::AccessibleDescriptionRole:
        return description;
    case Qt::AccessibleTextRole:
        return QStringLiteral("%1，%2，进度 %3%")
            .arg(name, status)
            .arg(snapshot.progressPercent);
    case TaskIdRole:
        return QVariant::fromValue<qulonglong>(snapshot.id);
    case StatusRole:
        return status;
    case ProgressRole:
        return snapshot.progressPercent;
    case LaneRole:
        return taskLaneText(snapshot.lane);
    case RetryOfRole:
        return QVariant::fromValue<qulonglong>(snapshot.retryOf);
    default:
        return {};
    }
}

QHash<int, QByteArray> TaskListModel::roleNames() const
{
    auto roles = QAbstractListModel::roleNames();
    roles.insert(TaskIdRole, "taskId");
    roles.insert(StatusRole, "status");
    roles.insert(ProgressRole, "progress");
    roles.insert(LaneRole, "lane");
    roles.insert(RetryOfRole, "retryOf");
    return roles;
}

void TaskListModel::setSnapshots(
    const std::vector<task::TaskSnapshot>& snapshots)
{
    beginResetModel();
    snapshots_ = snapshots;
    endResetModel();
}

std::optional<task::TaskId> TaskListModel::taskIdAt(int row) const noexcept
{
    if (row < 0 || static_cast<std::size_t>(row) >= snapshots_.size()) {
        return std::nullopt;
    }
    return snapshots_[static_cast<std::size_t>(row)].id;
}

std::optional<task::TaskId> TaskListModel::taskIdAt(
    const QModelIndex& index) const noexcept
{
    return taskIdAt(index.row());
}

AnalysisHistoryModel::AnalysisHistoryModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int AnalysisHistoryModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(std::min<std::size_t>(
        records_.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

QVariant AnalysisHistoryModel::data(const QModelIndex& index, int role) const
{
    if (!validRow(index, records_)
        || index.model() != this || index.column() != 0) {
        return {};
    }

    const auto& record = records_[static_cast<std::size_t>(index.row())];
    const QString signalName = QString::fromStdString(record.signalName);
    const QString status = analysisStatusText(record.status);
    const QString algorithm = QStringLiteral("%1 %2")
        .arg(
            QString::fromStdString(record.algorithmName),
            QString::fromStdString(record.algorithmVersion));
    QString description = QStringLiteral(
        "%1\n状态：%2\n算法：%3\n创建时间：%4\n参数：%5")
        .arg(
            signalName,
            status,
            algorithm,
            QString::fromStdString(record.createdUtc),
            QString::fromStdString(record.parametersJson));
    if (!record.errorMessage.empty()) {
        description += QStringLiteral("\n错误：%1")
            .arg(QString::fromStdString(record.errorMessage));
    }
    description += QStringLiteral("\n建议：%1")
        .arg(analysisSuggestion(record.status));

    switch (role) {
    case Qt::DisplayRole:
        return QStringLiteral("#%1 · %2 · %3")
            .arg(record.id)
            .arg(signalName, status);
    case Qt::DecorationRole:
        return analysisStatusIcon(record.status);
    case Qt::ToolTipRole:
    case Qt::AccessibleDescriptionRole:
        return description;
    case Qt::AccessibleTextRole:
        return QStringLiteral("分析记录 %1，%2，%3")
            .arg(record.id)
            .arg(signalName, status);
    case RecordIdRole:
        return QVariant::fromValue<qlonglong>(record.id);
    case StatusRole:
        return status;
    case AlgorithmRole:
        return algorithm;
    case CreatedUtcRole:
        return QString::fromStdString(record.createdUtc);
    default:
        return {};
    }
}

QHash<int, QByteArray> AnalysisHistoryModel::roleNames() const
{
    auto roles = QAbstractListModel::roleNames();
    roles.insert(RecordIdRole, "recordId");
    roles.insert(StatusRole, "status");
    roles.insert(AlgorithmRole, "algorithm");
    roles.insert(CreatedUtcRole, "createdUtc");
    return roles;
}

void AnalysisHistoryModel::setRecords(
    const std::vector<repository::AnalysisTaskRecord>& records)
{
    beginResetModel();
    records_ = records;
    endResetModel();
}

const repository::AnalysisTaskRecord* AnalysisHistoryModel::recordAt(
    int row) const noexcept
{
    if (row < 0 || static_cast<std::size_t>(row) >= records_.size()) {
        return nullptr;
    }
    return &records_[static_cast<std::size_t>(row)];
}

const repository::AnalysisTaskRecord* AnalysisHistoryModel::recordAt(
    const QModelIndex& index) const noexcept
{
    return recordAt(index.row());
}

ScriptHistoryModel::ScriptHistoryModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ScriptHistoryModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(std::min<std::size_t>(
        records_.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

QVariant ScriptHistoryModel::data(const QModelIndex& index, int role) const
{
    if (!validRow(index, records_)
        || index.model() != this || index.column() != 0) {
        return {};
    }

    const auto& record = records_[static_cast<std::size_t>(index.row())];
    const QString status = scriptStatusText(record.status, record.statusText);
    QString description = QStringLiteral(
        "脚本记录 #%1\n状态：%2\n创建时间：%3\n完成时间：%4")
        .arg(record.id)
        .arg(
            status,
            QString::fromStdString(record.createdUtc),
            QString::fromStdString(record.finishedUtc));
    if (record.errorLocation.has_value()) {
        description += QStringLiteral("\n位置：第 %1 行，第 %2 列")
            .arg(record.errorLocation->line)
            .arg(record.errorLocation->column);
    }
    if (!record.errorMessage.empty()) {
        description += QStringLiteral("\n错误：%1")
            .arg(QString::fromStdString(record.errorMessage));
    }
    description += QStringLiteral("\n建议：%1")
        .arg(scriptSuggestion(record.status));

    switch (role) {
    case Qt::DisplayRole:
        return QStringLiteral("#%1 · %2 · %3")
            .arg(record.id)
            .arg(status, QString::fromStdString(record.createdUtc));
    case Qt::DecorationRole:
        return scriptStatusIcon(record.status);
    case Qt::ToolTipRole:
    case Qt::AccessibleDescriptionRole:
        return description;
    case Qt::AccessibleTextRole:
        return QStringLiteral("脚本记录 %1，%2")
            .arg(record.id)
            .arg(status);
    case RecordIdRole:
        return QVariant::fromValue<qlonglong>(record.id);
    case StatusRole:
        return status;
    case CreatedUtcRole:
        return QString::fromStdString(record.createdUtc);
    case SourceHashRole:
        return QString::fromStdString(record.sourceHash);
    default:
        return {};
    }
}

QHash<int, QByteArray> ScriptHistoryModel::roleNames() const
{
    auto roles = QAbstractListModel::roleNames();
    roles.insert(RecordIdRole, "recordId");
    roles.insert(StatusRole, "status");
    roles.insert(CreatedUtcRole, "createdUtc");
    roles.insert(SourceHashRole, "sourceHash");
    return roles;
}

void ScriptHistoryModel::setRecords(
    std::vector<repository::ScriptHistoryRecord> records)
{
    beginResetModel();
    records_ = std::move(records);
    endResetModel();
}

const repository::ScriptHistoryRecord* ScriptHistoryModel::recordAt(
    int row) const noexcept
{
    if (row < 0 || static_cast<std::size_t>(row) >= records_.size()) {
        return nullptr;
    }
    return &records_[static_cast<std::size_t>(row)];
}

const repository::ScriptHistoryRecord* ScriptHistoryModel::recordAt(
    const QModelIndex& index) const noexcept
{
    return recordAt(index.row());
}

}  // namespace signallab::gui
