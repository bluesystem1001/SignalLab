#include "signallab/database/sqlite_task_repository.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QMetaType>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "database_helpers.hpp"
#include "signallab/core/error.hpp"

namespace signallab::database {
namespace {

bool isSha256(const std::string& text)
{
    if (text.size() != 64) {
        return false;
    }
    for (char character : text) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        if (!digit && !lower) {
            return false;
        }
    }
    return true;
}

std::optional<repository::AnalysisTaskStatus> statusFromText(
    const QString& text)
{
    if (text == "PENDING") {
        return repository::AnalysisTaskStatus::Pending;
    }
    if (text == "RUNNING") {
        return repository::AnalysisTaskStatus::Running;
    }
    if (text == "SUCCEEDED") {
        return repository::AnalysisTaskStatus::Succeeded;
    }
    if (text == "FAILED") {
        return repository::AnalysisTaskStatus::Failed;
    }
    if (text == "INTERRUPTED") {
        return repository::AnalysisTaskStatus::Interrupted;
    }
    return std::nullopt;
}

std::string canonicalNumber(double value)
{
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value > 0.0 ? "inf" : "-inf";
    }

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10)
           << std::defaultfloat << value;
    return stream.str();
}

std::vector<repository::FeatureValue> sortedFeatures(
    const std::vector<repository::FeatureValue>& features)
{
    std::vector<repository::FeatureValue> sorted = features;
    std::stable_sort(
        sorted.begin(),
        sorted.end(),
        [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
    return sorted;
}

std::string canonicalFeatureBytes(
    const std::vector<repository::FeatureValue>& features)
{
    std::string canonical;
    for (const auto& feature : features) {
        const std::string number = canonicalNumber(feature.value);
        canonical += std::to_string(feature.name.size());
        canonical += ':';
        canonical += feature.name;
        canonical += '|';
        canonical += std::to_string(feature.unit.size());
        canonical += ':';
        canonical += feature.unit;
        canonical += '|';
        canonical += number;
        canonical += '\n';
    }
    return canonical;
}

std::string sha256(const std::string& bytes)
{
    const QByteArray input(bytes.data(), static_cast<qsizetype>(bytes.size()));
    return QCryptographicHash::hash(input, QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

core::Result<bool> rollbackFailure(
    QSqlDatabase& database,
    const core::Error& error)
{
    database.rollback();
    return core::Result<bool>::failure(error);
}

// Completion failure is deliberately recorded in a new transaction. This
// keeps partial feature rows out of the database while preserving the failure.
core::Result<bool> markCompletionFailed(
    QSqlDatabase& database,
    std::int64_t taskId,
    const std::string& message)
{
    if (!database.transaction()) {
        return detail::databaseFailure(
            "Cannot begin the completion failure transaction",
            database);
    }

    QSqlQuery update(database);
    if (!update.prepare(
            "UPDATE analysis_task SET status = 'FAILED', "
            "finished_utc = ?, error_message = ?, output_hash = '' "
            "WHERE id = ? AND status = 'RUNNING'")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the completion failure update",
            update);
        return rollbackFailure(database, failure.error());
    }
    update.addBindValue(detail::utcNow());
    update.addBindValue(QString::fromStdString(message));
    update.addBindValue(taskId);
    if (!update.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot mark the incomplete task FAILED",
            update);
        return rollbackFailure(database, failure.error());
    }
    if (update.numRowsAffected() != 1) {
        database.rollback();
        return core::Result<bool>::failure(
            core::ErrorCode::DatabaseQueryFailed,
            "The incomplete task could not be marked FAILED.");
    }

    const auto log = detail::writeOperationLog(
        database,
        "FAIL_TASK",
        "analysis_task",
        taskId,
        QString::fromStdString(message));
    if (!log) {
        return rollbackFailure(database, log.error());
    }
    if (!database.commit()) {
        const auto failure = detail::databaseFailure(
            "Cannot commit the completion failure transaction",
            database);
        database.rollback();
        return failure;
    }
    return core::Result<bool>::success(true);
}

core::Result<std::string> completionFailure(
    QSqlDatabase& database,
    std::int64_t taskId,
    core::Error error)
{
    const auto failedStatus = markCompletionFailed(
        database,
        taskId,
        error.message);
    if (!failedStatus) {
        error.message += " The task also could not be marked FAILED: "
            + failedStatus.error().message;
    }
    return core::Result<std::string>::failure(std::move(error));
}

}  // namespace

SqliteTaskRepository::SqliteTaskRepository(DatabaseConnection& connection)
    : connection_(connection)
{
}

core::Result<std::int64_t> SqliteTaskRepository::createTask(
    const repository::AnalysisTaskStart& task)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<std::int64_t>::failure(threadCheck.error());
    }
    if (task.signalName.empty() || task.algorithmName.empty()
        || task.algorithmVersion.empty() || task.parametersJson.empty()
        || !isSha256(task.inputHash)) {
        return core::Result<std::int64_t>::failure(
            core::ErrorCode::InvalidParameter,
            "The task name, algorithm, parameters and input SHA-256 "
            "must be valid.");
    }
    if (task.signalRecordId.has_value() && *task.signalRecordId <= 0) {
        return core::Result<std::int64_t>::failure(
            core::ErrorCode::InvalidParameter,
            "The optional signal record id must be positive.");
    }

    QSqlDatabase database = connection_.database();
    if (!database.transaction()) {
        const auto failure = detail::databaseFailure(
            "Cannot begin the task creation transaction", database);
        return core::Result<std::int64_t>::failure(failure.error());
    }

    if (task.signalRecordId.has_value()) {
        QSqlQuery signalLookup(database);
        if (!signalLookup.prepare(
                "SELECT sample_hash FROM signal_record WHERE id = ?")) {
            const auto failure = detail::queryFailure(
                "Cannot prepare the task input lookup", signalLookup);
            rollbackFailure(database, failure.error());
            return core::Result<std::int64_t>::failure(failure.error());
        }
        signalLookup.addBindValue(*task.signalRecordId);
        if (!signalLookup.exec()) {
            const auto failure = detail::queryFailure(
                "Cannot load the task input signal", signalLookup);
            rollbackFailure(database, failure.error());
            return core::Result<std::int64_t>::failure(failure.error());
        }
        if (!signalLookup.next()) {
            database.rollback();
            return core::Result<std::int64_t>::failure(
                core::ErrorCode::InvalidParameter,
                "The task input signal record does not exist.");
        }
        if (signalLookup.value(0).toString().toStdString()
            != task.inputHash) {
            database.rollback();
            return core::Result<std::int64_t>::failure(
                core::ErrorCode::InvalidParameter,
                "The task input hash does not match its signal record.");
        }
        signalLookup.finish();
    }

    const QString now = detail::utcNow();
    QSqlQuery insert(database);
    if (!insert.prepare(
            "INSERT INTO analysis_task("
            "signal_record_id, signal_name, status, algorithm_name, "
            "algorithm_version, parameters_json, input_hash, output_hash, "
            "created_utc, started_utc, finished_utc, error_message"
            ") VALUES (?, ?, 'RUNNING', ?, ?, ?, ?, '', ?, ?, '', '')")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the task insert", insert);
        rollbackFailure(database, failure.error());
        return core::Result<std::int64_t>::failure(failure.error());
    }
    if (task.signalRecordId.has_value()) {
        insert.addBindValue(*task.signalRecordId);
    } else {
        insert.addBindValue(QVariant(QMetaType::fromType<qint64>()));
    }
    insert.addBindValue(QString::fromStdString(task.signalName));
    insert.addBindValue(QString::fromStdString(task.algorithmName));
    insert.addBindValue(QString::fromStdString(task.algorithmVersion));
    insert.addBindValue(QString::fromStdString(task.parametersJson));
    insert.addBindValue(QString::fromStdString(task.inputHash));
    insert.addBindValue(now);
    insert.addBindValue(now);
    if (!insert.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot create the analysis task", insert);
        rollbackFailure(database, failure.error());
        return core::Result<std::int64_t>::failure(failure.error());
    }
    const std::int64_t taskId = insert.lastInsertId().toLongLong();

    const auto log = detail::writeOperationLog(
        database,
        "CREATE_TASK",
        "analysis_task",
        taskId,
        QString::fromStdString(task.algorithmName));
    if (!log) {
        rollbackFailure(database, log.error());
        return core::Result<std::int64_t>::failure(log.error());
    }

    if (!database.commit()) {
        const auto failure = detail::databaseFailure(
            "Cannot commit the task creation transaction", database);
        database.rollback();
        return core::Result<std::int64_t>::failure(failure.error());
    }
    return core::Result<std::int64_t>::success(taskId);
}

core::Result<std::string> SqliteTaskRepository::completeTask(
    std::int64_t taskId,
    const std::vector<repository::FeatureValue>& features)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<std::string>::failure(threadCheck.error());
    }
    if (taskId <= 0) {
        return core::Result<std::string>::failure(
            core::ErrorCode::InvalidParameter,
            "The task id must be positive.");
    }

    const auto ordered = sortedFeatures(features);
    for (const auto& feature : ordered) {
        if (feature.name.empty()) {
            QSqlDatabase database = connection_.database();
            return completionFailure(
                database,
                taskId,
                core::Error{
                    core::ErrorCode::InvalidParameter,
                    "A feature name must not be empty."});
        }
        if (std::isnan(feature.value)) {
            QSqlDatabase database = connection_.database();
            return completionFailure(
                database,
                taskId,
                core::Error{
                    core::ErrorCode::InvalidParameter,
                    "A feature value must not be NaN."});
        }
    }

    const std::string outputHash = sha256(canonicalFeatureBytes(ordered));
    QSqlDatabase database = connection_.database();
    if (!database.transaction()) {
        const auto failure = detail::databaseFailure(
            "Cannot begin the task completion transaction", database);
        return completionFailure(database, taskId, failure.error());
    }

    QSqlQuery taskLookup(database);
    if (!taskLookup.prepare(
            "SELECT status FROM analysis_task WHERE id = ?")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the task status lookup", taskLookup);
        rollbackFailure(database, failure.error());
        return completionFailure(database, taskId, failure.error());
    }
    taskLookup.addBindValue(taskId);
    if (!taskLookup.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot load the task before completion", taskLookup);
        rollbackFailure(database, failure.error());
        return completionFailure(database, taskId, failure.error());
    }
    if (!taskLookup.next()) {
        database.rollback();
        return core::Result<std::string>::failure(
            core::ErrorCode::DatabaseQueryFailed,
            "The analysis task does not exist.");
    }
    if (taskLookup.value(0).toString() != "RUNNING") {
        database.rollback();
        return core::Result<std::string>::failure(
            core::ErrorCode::InvalidParameter,
            "Only a RUNNING task can be completed.");
    }
    taskLookup.finish();

    QSqlQuery insertFeature(database);
    if (!insertFeature.prepare(
            "INSERT INTO feature_result("
            "task_id, feature_name, numeric_value, value_text, unit"
            ") VALUES (?, ?, ?, ?, ?)")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the feature insert", insertFeature);
        rollbackFailure(database, failure.error());
        return completionFailure(database, taskId, failure.error());
    }

    for (const auto& feature : ordered) {
        insertFeature.bindValue(0, taskId);
        insertFeature.bindValue(1, QString::fromStdString(feature.name));
        if (std::isinf(feature.value)) {
            insertFeature.bindValue(
                2, QVariant(QMetaType::fromType<double>()));
            insertFeature.bindValue(
                3, QString::fromStdString(canonicalNumber(feature.value)));
        } else {
            insertFeature.bindValue(2, feature.value);
            insertFeature.bindValue(3, QString::fromLatin1(""));
        }
        insertFeature.bindValue(4, QString::fromStdString(feature.unit));
        if (!insertFeature.exec()) {
            const auto failure = detail::queryFailure(
                "Cannot insert a feature result", insertFeature);
            rollbackFailure(database, failure.error());
            return completionFailure(database, taskId, failure.error());
        }
        insertFeature.finish();
    }

    QSqlQuery update(database);
    if (!update.prepare(
            "UPDATE analysis_task SET status = 'SUCCEEDED', "
            "output_hash = ?, finished_utc = ?, error_message = '' "
            "WHERE id = ? AND status = 'RUNNING'")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the task completion update", update);
        rollbackFailure(database, failure.error());
        return completionFailure(database, taskId, failure.error());
    }
    update.addBindValue(QString::fromStdString(outputHash));
    update.addBindValue(detail::utcNow());
    update.addBindValue(taskId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        const auto failure = detail::queryFailure(
            "Cannot mark the task SUCCEEDED", update);
        rollbackFailure(database, failure.error());
        return completionFailure(database, taskId, failure.error());
    }

    const auto log = detail::writeOperationLog(
        database,
        "COMPLETE_TASK",
        "analysis_task",
        taskId,
        QString::fromStdString(outputHash));
    if (!log) {
        rollbackFailure(database, log.error());
        return completionFailure(database, taskId, log.error());
    }

    if (!database.commit()) {
        const auto failure = detail::databaseFailure(
            "Cannot commit the task completion transaction", database);
        database.rollback();
        return completionFailure(database, taskId, failure.error());
    }
    return core::Result<std::string>::success(outputHash);
}

core::Result<bool> SqliteTaskRepository::failTask(
    std::int64_t taskId,
    const std::string& errorMessage)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return threadCheck;
    }
    if (taskId <= 0 || errorMessage.empty()) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "The task id and failure message must be valid.");
    }

    QSqlDatabase database = connection_.database();
    if (!database.transaction()) {
        return detail::databaseFailure(
            "Cannot begin the task failure transaction", database);
    }

    QSqlQuery update(database);
    if (!update.prepare(
            "UPDATE analysis_task SET status = 'FAILED', "
            "finished_utc = ?, error_message = ?, output_hash = '' "
            "WHERE id = ? AND status IN ('PENDING', 'RUNNING')")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the task failure update", update);
        return rollbackFailure(database, failure.error());
    }
    update.addBindValue(detail::utcNow());
    update.addBindValue(QString::fromStdString(errorMessage));
    update.addBindValue(taskId);
    if (!update.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot mark the task FAILED", update);
        return rollbackFailure(database, failure.error());
    }
    if (update.numRowsAffected() == 0) {
        database.rollback();
        return core::Result<bool>::success(false);
    }

    const auto log = detail::writeOperationLog(
        database,
        "FAIL_TASK",
        "analysis_task",
        taskId,
        QString::fromStdString(errorMessage));
    if (!log) {
        return rollbackFailure(database, log.error());
    }
    if (!database.commit()) {
        const auto failure = detail::databaseFailure(
            "Cannot commit the task failure transaction", database);
        database.rollback();
        return failure;
    }
    return core::Result<bool>::success(true);
}

core::Result<std::size_t> SqliteTaskRepository::interruptRunningTasks()
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<std::size_t>::failure(threadCheck.error());
    }

    QSqlDatabase database = connection_.database();
    if (!database.transaction()) {
        const auto failure = detail::databaseFailure(
            "Cannot begin the task interruption transaction", database);
        return core::Result<std::size_t>::failure(failure.error());
    }

    QSqlQuery update(database);
    if (!update.prepare(
            "UPDATE analysis_task SET status = 'INTERRUPTED', "
            "finished_utc = ?, error_message = ? WHERE status = 'RUNNING'")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the running task interruption", update);
        rollbackFailure(database, failure.error());
        return core::Result<std::size_t>::failure(failure.error());
    }
    update.addBindValue(detail::utcNow());
    update.addBindValue(
        QString("The application stopped before this task completed."));
    if (!update.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot interrupt running tasks", update);
        rollbackFailure(database, failure.error());
        return core::Result<std::size_t>::failure(failure.error());
    }
    const int affectedRows = update.numRowsAffected();
    const auto affected = static_cast<std::size_t>(
        affectedRows < 0 ? 0 : affectedRows);

    if (affected > 0) {
        const auto log = detail::writeOperationLog(
            database,
            "INTERRUPT_TASKS",
            "analysis_task",
            QVariant(QMetaType::fromType<qint64>()),
            QString::number(static_cast<qulonglong>(affected)));
        if (!log) {
            rollbackFailure(database, log.error());
            return core::Result<std::size_t>::failure(log.error());
        }
    }
    if (!database.commit()) {
        const auto failure = detail::databaseFailure(
            "Cannot commit the task interruption transaction", database);
        database.rollback();
        return core::Result<std::size_t>::failure(failure.error());
    }
    return core::Result<std::size_t>::success(affected);
}

core::Result<std::vector<repository::AnalysisTaskRecord>>
SqliteTaskRepository::listTasks(std::size_t maximumCount)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<
            std::vector<repository::AnalysisTaskRecord>>::failure(
            threadCheck.error());
    }

    QSqlQuery query(connection_.database());
    if (!query.prepare(
            "SELECT id, signal_record_id, signal_name, status, "
            "algorithm_name, algorithm_version, parameters_json, input_hash, "
            "output_hash, created_utc, started_utc, finished_utc, "
            "error_message FROM analysis_task "
            "ORDER BY id DESC LIMIT ?")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the task history query", query);
        return core::Result<
            std::vector<repository::AnalysisTaskRecord>>::failure(
            failure.error());
    }
    query.addBindValue(detail::checkedLimit(maximumCount));
    if (!query.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot load task history", query);
        return core::Result<
            std::vector<repository::AnalysisTaskRecord>>::failure(
            failure.error());
    }

    std::vector<repository::AnalysisTaskRecord> tasks;
    while (query.next()) {
        const auto status = statusFromText(query.value(3).toString());
        if (!status.has_value()) {
            return core::Result<
                std::vector<repository::AnalysisTaskRecord>>::failure(
                core::ErrorCode::DatabaseQueryFailed,
                "A stored task has an unknown status.");
        }
        tasks.push_back(repository::AnalysisTaskRecord{
            query.value(0).toLongLong(),
            query.value(1).isNull()
                ? std::nullopt
                : std::optional<std::int64_t>(
                    query.value(1).toLongLong()),
            query.value(2).toString().toStdString(),
            *status,
            query.value(4).toString().toStdString(),
            query.value(5).toString().toStdString(),
            query.value(6).toString().toStdString(),
            query.value(7).toString().toStdString(),
            query.value(8).toString().toStdString(),
            query.value(9).toString().toStdString(),
            query.value(10).toString().toStdString(),
            query.value(11).toString().toStdString(),
            query.value(12).toString().toStdString()});
    }
    return core::Result<
        std::vector<repository::AnalysisTaskRecord>>::success(
        std::move(tasks));
}

core::Result<std::vector<repository::FeatureResultRecord>>
SqliteTaskRepository::listFeatureResults(std::int64_t taskId)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<
            std::vector<repository::FeatureResultRecord>>::failure(
            threadCheck.error());
    }
    if (taskId <= 0) {
        return core::Result<
            std::vector<repository::FeatureResultRecord>>::failure(
            core::ErrorCode::InvalidParameter,
            "The task id must be positive.");
    }

    QSqlQuery query(connection_.database());
    if (!query.prepare(
            "SELECT id, task_id, feature_name, numeric_value, value_text, unit "
            "FROM feature_result WHERE task_id = ? ORDER BY feature_name")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the feature history query", query);
        return core::Result<
            std::vector<repository::FeatureResultRecord>>::failure(
            failure.error());
    }
    query.addBindValue(taskId);
    if (!query.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot load feature history", query);
        return core::Result<
            std::vector<repository::FeatureResultRecord>>::failure(
            failure.error());
    }

    std::vector<repository::FeatureResultRecord> features;
    while (query.next()) {
        features.push_back(repository::FeatureResultRecord{
            query.value(0).toLongLong(),
            query.value(1).toLongLong(),
            query.value(2).toString().toStdString(),
            query.value(3).isNull()
                ? std::nullopt
                : std::optional<double>(query.value(3).toDouble()),
            query.value(4).toString().toStdString(),
            query.value(5).toString().toStdString()});
    }
    return core::Result<
        std::vector<repository::FeatureResultRecord>>::success(
        std::move(features));
}

}  // namespace signallab::database
