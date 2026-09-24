#pragma once

#include <QAbstractListModel>
#include <QAbstractTableModel>
#include <QHash>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "signallab/app/analysis_service.hpp"
#include "signallab/app/import_service.hpp"
#include "signallab/repository/script_repository.hpp"
#include "signallab/repository/task_repository.hpp"
#include "signallab/task/task.hpp"

namespace signallab::gui {

struct ResultRow {
    QString metric;
    QString value;
    QString unit;
};

class AnalysisResultModel final : public QAbstractTableModel
{
public:
    enum Column {
        MetricColumn = 0,
        ValueColumn,
        UnitColumn,
        ColumnCount
    };

    enum Role {
        MetricRole = Qt::UserRole + 1,
        ValueRole,
        UnitRole
    };

    explicit AnalysisResultModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] int columnCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(
        const QModelIndex& index,
        int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(
        int section,
        Qt::Orientation orientation,
        int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setResult(
        const app::AnalysisResult& result,
        const QString& signalUnit,
        const app::AnalysisOptions& options,
        const QString& algorithmVersion);
    void clear();

    [[nodiscard]] const ResultRow* rowAt(int row) const noexcept;
    [[nodiscard]] QString parameterSummary() const;
    [[nodiscard]] QString algorithmVersion() const;

private:
    std::vector<ResultRow> rows_;
    QString parameterSummary_;
    QString algorithmVersion_;
};

class SignalListModel final : public QAbstractListModel
{
public:
    enum Role {
        RecordIdRole = Qt::UserRole + 1,
        SourceIdRole,
        DisplayNameRole,
        SampleRateRole,
        SampleCountRole
    };

    explicit SignalListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(
        const QModelIndex& index,
        int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // Copies only lightweight presentation metadata. Signal samples remain
    // owned by ImportService and are never duplicated in the GUI model.
    void setEntries(const std::vector<app::SignalEntry>& entries);

    [[nodiscard]] std::optional<std::int64_t> recordIdAt(int row) const noexcept;
    [[nodiscard]] std::optional<std::int64_t> recordIdAt(
        const QModelIndex& index) const noexcept;

private:
    struct Item {
        std::int64_t recordId;
        std::int64_t sourceId;
        QString displayName;
        QString unit;
        QString source;
        double sampleRate;
        std::size_t sampleCount;
        double durationSeconds;
    };

    std::vector<Item> items_;
};

class TaskListModel final : public QAbstractListModel
{
public:
    enum Role {
        TaskIdRole = Qt::UserRole + 1,
        StatusRole,
        ProgressRole,
        LaneRole,
        RetryOfRole
    };

    explicit TaskListModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(
        const QModelIndex& index,
        int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setSnapshots(const std::vector<task::TaskSnapshot>& snapshots);

    [[nodiscard]] std::optional<task::TaskId> taskIdAt(int row) const noexcept;
    [[nodiscard]] std::optional<task::TaskId> taskIdAt(
        const QModelIndex& index) const noexcept;

private:
    std::vector<task::TaskSnapshot> snapshots_;
};

class AnalysisHistoryModel final : public QAbstractListModel
{
public:
    enum Role {
        RecordIdRole = Qt::UserRole + 1,
        StatusRole,
        AlgorithmRole,
        CreatedUtcRole
    };

    explicit AnalysisHistoryModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(
        const QModelIndex& index,
        int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setRecords(
        const std::vector<repository::AnalysisTaskRecord>& records);

    [[nodiscard]] const repository::AnalysisTaskRecord* recordAt(
        int row) const noexcept;
    [[nodiscard]] const repository::AnalysisTaskRecord* recordAt(
        const QModelIndex& index) const noexcept;

private:
    std::vector<repository::AnalysisTaskRecord> records_;
};

class ScriptHistoryModel final : public QAbstractListModel
{
public:
    enum Role {
        RecordIdRole = Qt::UserRole + 1,
        StatusRole,
        CreatedUtcRole,
        SourceHashRole
    };

    explicit ScriptHistoryModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(
        const QModelIndex& index,
        int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setRecords(
        std::vector<repository::ScriptHistoryRecord> records);

    [[nodiscard]] const repository::ScriptHistoryRecord* recordAt(
        int row) const noexcept;
    [[nodiscard]] const repository::ScriptHistoryRecord* recordAt(
        const QModelIndex& index) const noexcept;

private:
    std::vector<repository::ScriptHistoryRecord> records_;
};

}  // namespace signallab::gui
