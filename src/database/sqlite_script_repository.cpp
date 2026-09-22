#include "signallab/database/sqlite_script_repository.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QMetaType>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "database_helpers.hpp"
#include "signallab/core/error.hpp"

namespace signallab::database {
namespace {

core::Result<QByteArray> validatedUtf8(
    const std::string& text,
    const std::string& description)
{
    constexpr auto largestQtString =
        static_cast<std::size_t>((std::numeric_limits<qsizetype>::max)());
    if (text.size() > largestQtString) {
        return core::Result<QByteArray>::failure(
            core::ErrorCode::InvalidParameter,
            description + " is too large.");
    }

    const QByteArray bytes(
        text.data(), static_cast<qsizetype>(text.size()));
    if (QString::fromUtf8(bytes).toUtf8() != bytes) {
        return core::Result<QByteArray>::failure(
            core::ErrorCode::InvalidParameter,
            description + " must contain valid UTF-8.");
    }
    return core::Result<QByteArray>::success(bytes);
}

std::string sha256(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

QString statusToText(repository::ScriptHistoryStatus status)
{
    switch (status) {
    case repository::ScriptHistoryStatus::Running:
        return QStringLiteral("RUNNING");
    case repository::ScriptHistoryStatus::Succeeded:
        return QStringLiteral("SUCCEEDED");
    case repository::ScriptHistoryStatus::Failed:
        return QStringLiteral("FAILED");
    case repository::ScriptHistoryStatus::Cancelled:
        return QStringLiteral("CANCELLED");
    case repository::ScriptHistoryStatus::Interrupted:
        return QStringLiteral("INTERRUPTED");
    case repository::ScriptHistoryStatus::Legacy:
        return {};
    }
    return {};
}

std::optional<repository::ScriptHistoryStatus> statusFromText(
    const QString& text)
{
    if (text == QStringLiteral("RUNNING")) {
        return repository::ScriptHistoryStatus::Running;
    }
    if (text == QStringLiteral("SUCCEEDED")) {
        return repository::ScriptHistoryStatus::Succeeded;
    }
    if (text == QStringLiteral("FAILED")) {
        return repository::ScriptHistoryStatus::Failed;
    }
    if (text == QStringLiteral("CANCELLED")) {
        return repository::ScriptHistoryStatus::Cancelled;
    }
    if (text == QStringLiteral("INTERRUPTED")) {
        return repository::ScriptHistoryStatus::Interrupted;
    }
    return std::nullopt;
}

bool isTerminal(repository::ScriptHistoryStatus status)
{
    return status == repository::ScriptHistoryStatus::Succeeded
        || status == repository::ScriptHistoryStatus::Failed
        || status == repository::ScriptHistoryStatus::Cancelled
        || status == repository::ScriptHistoryStatus::Interrupted;
}

template <typename T>
core::Result<T> rollback(
    QSqlDatabase& database,
    const core::Error& error)
{
    database.rollback();
    return core::Result<T>::failure(error);
}

QVariant nullInteger()
{
    return QVariant(QMetaType::fromType<qint64>());
}

}  // namespace

SqliteScriptRepository::SqliteScriptRepository(
    DatabaseConnection& connection)
    : connection_(connection)
{
}

core::Result<std::int64_t> SqliteScriptRepository::createScriptHistory(
    std::string scriptText)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<std::int64_t>::failure(threadCheck.error());
    }

    auto sourceBytes = validatedUtf8(scriptText, "The script source");
    if (!sourceBytes) {
        return core::Result<std::int64_t>::failure(sourceBytes.error());
    }
    const std::string sourceHash = sha256(sourceBytes.value());

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
            "script_text, source_hash, status, created_utc, finished_utc, "
            "error_line, error_column, error_message"
            ") VALUES (?, ?, 'RUNNING', ?, '', NULL, NULL, '')")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the script history insert", insert);
        return rollback<std::int64_t>(database, failure.error());
    }
    insert.addBindValue(QString::fromUtf8(sourceBytes.value()));
    insert.addBindValue(QString::fromStdString(sourceHash));
    insert.addBindValue(now);
    if (!insert.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot insert script history", insert);
        return rollback<std::int64_t>(database, failure.error());
    }
    const std::int64_t historyId = insert.lastInsertId().toLongLong();

    const auto log = detail::writeOperationLog(
        database,
        QStringLiteral("CREATE_SCRIPT_HISTORY"),
        QStringLiteral("script_history"),
        historyId,
        QString::fromStdString(sourceHash));
    if (!log) {
        return rollback<std::int64_t>(database, log.error());
    }
    if (!database.commit()) {
        const auto failure = detail::databaseFailure(
            "Cannot commit the script history transaction", database);
        database.rollback();
        return core::Result<std::int64_t>::failure(failure.error());
    }
    return core::Result<std::int64_t>::success(historyId);
}

core::Result<bool> SqliteScriptRepository::finishScriptHistory(
    std::int64_t historyId,
    repository::ScriptHistoryFinish finish)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<bool>::failure(threadCheck.error());
    }
    if (historyId <= 0) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "The script history id must be positive.");
    }
    if (!isTerminal(finish.status)) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "A script may only finish in a terminal state.");
    }
    if (finish.errorLocation
        && (finish.errorLocation->line <= 0
            || finish.errorLocation->column <= 0)) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "Script error line and column must be positive.");
    }
    if (finish.status == repository::ScriptHistoryStatus::Succeeded) {
        if (!finish.errorMessage.empty() || finish.errorLocation) {
            return core::Result<bool>::failure(
                core::ErrorCode::InvalidParameter,
                "A successful script must not have an error.");
        }
    } else if (finish.errorMessage.empty()) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "An unsuccessful script must have an error message.");
    }

    auto errorBytes = validatedUtf8(
        finish.errorMessage, "The script error message");
    if (!errorBytes) {
        return core::Result<bool>::failure(errorBytes.error());
    }
    const QString status = statusToText(finish.status);

    QSqlDatabase database = connection_.database();
    if (!database.transaction()) {
        const auto failure = detail::databaseFailure(
            "Cannot begin the script completion transaction", database);
        return core::Result<bool>::failure(failure.error());
    }

    QSqlQuery update(database);
    if (!update.prepare(
            "UPDATE script_history SET status = ?, finished_utc = ?, "
            "error_line = ?, error_column = ?, error_message = ? "
            "WHERE id = ? AND status = 'RUNNING' "
            "AND source_hash <> ''")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the script completion", update);
        return rollback<bool>(database, failure.error());
    }
    update.addBindValue(status);
    update.addBindValue(detail::utcNow());
    update.addBindValue(
        finish.errorLocation
            ? QVariant::fromValue<qint64>(finish.errorLocation->line)
            : nullInteger());
    update.addBindValue(
        finish.errorLocation
            ? QVariant::fromValue<qint64>(finish.errorLocation->column)
            : nullInteger());
    update.addBindValue(QString::fromUtf8(errorBytes.value()));
    update.addBindValue(historyId);
    if (!update.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot finish script history", update);
        return rollback<bool>(database, failure.error());
    }
    if (update.numRowsAffected() == 0) {
        database.rollback();
        return core::Result<bool>::success(false);
    }
    if (update.numRowsAffected() != 1) {
        return rollback<bool>(
            database,
            core::Error{
                core::ErrorCode::DatabaseQueryFailed,
                "A script completion changed more than one record."});
    }

    const auto log = detail::writeOperationLog(
        database,
        QStringLiteral("FINISH_SCRIPT_HISTORY"),
        QStringLiteral("script_history"),
        historyId,
        status);
    if (!log) {
        return rollback<bool>(database, log.error());
    }
    if (!database.commit()) {
        const auto failure = detail::databaseFailure(
            "Cannot commit the script completion transaction", database);
        database.rollback();
        return core::Result<bool>::failure(failure.error());
    }
    return core::Result<bool>::success(true);
}

core::Result<std::size_t>
SqliteScriptRepository::interruptRunningScripts()
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<std::size_t>::failure(threadCheck.error());
    }

    QSqlDatabase database = connection_.database();
    if (!database.transaction()) {
        const auto failure = detail::databaseFailure(
            "Cannot begin the script interruption transaction", database);
        return core::Result<std::size_t>::failure(failure.error());
    }

    QSqlQuery update(database);
    if (!update.prepare(
            "UPDATE script_history SET status = 'INTERRUPTED', "
            "finished_utc = ?, error_line = NULL, error_column = NULL, "
            "error_message = ? WHERE status = 'RUNNING' "
            "AND source_hash <> ''")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare unfinished script interruption", update);
        return rollback<std::size_t>(database, failure.error());
    }
    update.addBindValue(detail::utcNow());
    update.addBindValue(QStringLiteral(
        "The application stopped before this script completed."));
    if (!update.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot interrupt unfinished scripts", update);
        return rollback<std::size_t>(database, failure.error());
    }
    const int affectedRows = update.numRowsAffected();
    const auto affected = static_cast<std::size_t>(
        affectedRows < 0 ? 0 : affectedRows);

    if (affected > 0) {
        const auto log = detail::writeOperationLog(
            database,
            QStringLiteral("INTERRUPT_SCRIPTS"),
            QStringLiteral("script_history"),
            nullInteger(),
            QString::number(static_cast<qulonglong>(affected)));
        if (!log) {
            return rollback<std::size_t>(database, log.error());
        }
    }
    if (!database.commit()) {
        const auto failure = detail::databaseFailure(
            "Cannot commit the script interruption transaction", database);
        database.rollback();
        return core::Result<std::size_t>::failure(failure.error());
    }
    return core::Result<std::size_t>::success(affected);
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
            "SELECT id, script_text, source_hash, status, created_utc, "
            "finished_utc, error_line, error_column, error_message "
            "FROM script_history ORDER BY id DESC LIMIT ?")) {
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
        const QByteArray sourceBytes = query.value(1).toString().toUtf8();
        const std::string scriptText(
            sourceBytes.constData(),
            static_cast<std::size_t>(sourceBytes.size()));
        const std::string sourceHash =
            query.value(2).toString().toStdString();
        const QString statusText = query.value(3).toString();
        auto status = statusFromText(statusText);
        if (!status) {
            if (!sourceHash.empty()) {
                return core::Result<
                    std::vector<repository::ScriptHistoryRecord>>::failure(
                    core::ErrorCode::DatabaseQueryFailed,
                    "A stage-6 script record has an unknown status.");
            }
            status = repository::ScriptHistoryStatus::Legacy;
        }
        if (!sourceHash.empty() && sha256(sourceBytes) != sourceHash) {
            return core::Result<
                std::vector<repository::ScriptHistoryRecord>>::failure(
                core::ErrorCode::DatabaseQueryFailed,
                "A stored script source does not match its SHA-256 hash.");
        }

        const bool hasLine = !query.value(6).isNull();
        const bool hasColumn = !query.value(7).isNull();
        if (hasLine != hasColumn) {
            return core::Result<
                std::vector<repository::ScriptHistoryRecord>>::failure(
                core::ErrorCode::DatabaseQueryFailed,
                "A stored script error has an incomplete source location.");
        }
        std::optional<repository::ScriptErrorLocation> errorLocation;
        if (hasLine) {
            const auto line = query.value(6).toLongLong();
            const auto column = query.value(7).toLongLong();
            if (line <= 0 || column <= 0) {
                return core::Result<
                    std::vector<repository::ScriptHistoryRecord>>::failure(
                    core::ErrorCode::DatabaseQueryFailed,
                    "A stored script error location is not positive.");
            }
            errorLocation = repository::ScriptErrorLocation{line, column};
        }

        records.push_back(repository::ScriptHistoryRecord{
            query.value(0).toLongLong(),
            scriptText,
            sourceHash,
            *status,
            statusText.toStdString(),
            query.value(4).toString().toStdString(),
            query.value(5).toString().toStdString(),
            errorLocation,
            query.value(8).toString().toStdString()});
    }
    return core::Result<
        std::vector<repository::ScriptHistoryRecord>>::success(
        std::move(records));
}

}  // namespace signallab::database
