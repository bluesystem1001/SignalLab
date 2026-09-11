#include "signallab/database/sqlite_script_repository.hpp"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "database_helpers.hpp"
#include "signallab/core/error.hpp"

namespace signallab::database {

SqliteScriptRepository::SqliteScriptRepository(
    DatabaseConnection& connection)
    : connection_(connection)
{
}

core::Result<std::int64_t> SqliteScriptRepository::addScriptHistory(
    const std::string& scriptText,
    const std::string& status,
    const std::string& errorMessage)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<std::int64_t>::failure(threadCheck.error());
    }
    if (status.empty()) {
        return core::Result<std::int64_t>::failure(
            core::ErrorCode::InvalidParameter,
            "The script status must not be empty.");
    }

    QSqlDatabase database = connection_.database();
    if (!database.transaction()) {
        const auto failure = detail::databaseFailure(
            "Cannot begin the script history transaction", database);
        return core::Result<std::int64_t>::failure(failure.error());
    }

    const QString now = detail::utcNow();
    QSqlQuery insert(database);
    if (!insert.prepare(
            "INSERT INTO script_history("
            "script_text, status, created_utc, finished_utc, error_message"
            ") VALUES (?, ?, ?, ?, ?)")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the script history insert", insert);
        database.rollback();
        return core::Result<std::int64_t>::failure(failure.error());
    }
    insert.addBindValue(QString::fromStdString(scriptText));
    insert.addBindValue(QString::fromStdString(status));
    insert.addBindValue(now);
    insert.addBindValue(now);
    insert.addBindValue(QString::fromStdString(errorMessage));
    if (!insert.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot insert script history", insert);
        database.rollback();
        return core::Result<std::int64_t>::failure(failure.error());
    }
    const std::int64_t historyId = insert.lastInsertId().toLongLong();

    const auto log = detail::writeOperationLog(
        database,
        "ADD_SCRIPT_HISTORY",
        "script_history",
        historyId,
        QString::fromStdString(status));
    if (!log) {
        database.rollback();
        return core::Result<std::int64_t>::failure(log.error());
    }
    if (!database.commit()) {
        const auto failure = detail::databaseFailure(
            "Cannot commit the script history transaction", database);
        database.rollback();
        return core::Result<std::int64_t>::failure(failure.error());
    }
    return core::Result<std::int64_t>::success(historyId);
}

core::Result<std::vector<repository::ScriptHistoryRecord>>
SqliteScriptRepository::listScriptHistory(std::size_t maximumCount)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<
            std::vector<repository::ScriptHistoryRecord>>::failure(
            threadCheck.error());
    }

    QSqlQuery query(connection_.database());
    if (!query.prepare(
            "SELECT id, script_text, status, created_utc, finished_utc, "
            "error_message FROM script_history ORDER BY id DESC LIMIT ?")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the script history query", query);
        return core::Result<
            std::vector<repository::ScriptHistoryRecord>>::failure(
            failure.error());
    }
    query.addBindValue(detail::checkedLimit(maximumCount));
    if (!query.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot load script history", query);
        return core::Result<
            std::vector<repository::ScriptHistoryRecord>>::failure(
            failure.error());
    }

    std::vector<repository::ScriptHistoryRecord> records;
    while (query.next()) {
        records.push_back(repository::ScriptHistoryRecord{
            query.value(0).toLongLong(),
            query.value(1).toString().toStdString(),
            query.value(2).toString().toStdString(),
            query.value(3).toString().toStdString(),
            query.value(4).toString().toStdString(),
            query.value(5).toString().toStdString()});
    }
    return core::Result<
        std::vector<repository::ScriptHistoryRecord>>::success(
        std::move(records));
}

}  // namespace signallab::database
