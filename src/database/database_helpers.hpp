#pragma once

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <cstdint>
#include <limits>
#include <string>

#include "signallab/core/error.hpp"
#include "signallab/core/result.hpp"

namespace signallab::database::detail {

inline QString utcNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

inline core::Result<bool> queryFailure(
    const std::string& action,
    const QSqlQuery& query)
{
    return core::Result<bool>::failure(
        core::ErrorCode::DatabaseQueryFailed,
        action + ": " + query.lastError().text().toStdString());
}

inline core::Result<bool> databaseFailure(
    const std::string& action,
    const QSqlDatabase& database)
{
    return core::Result<bool>::failure(
        core::ErrorCode::DatabaseQueryFailed,
        action + ": " + database.lastError().text().toStdString());
}

inline core::Result<bool> writeOperationLog(
    const QSqlDatabase& database,
    const QString& operation,
    const QString& entityType,
    const QVariant& entityId,
    const QString& detail)
{
    QSqlQuery query(database);
    if (!query.prepare(
            "INSERT INTO operation_log("
            "operation, entity_type, entity_id, detail, created_utc"
            ") VALUES (?, ?, ?, ?, ?)") ) {
        return queryFailure("Cannot prepare the operation log", query);
    }

    query.addBindValue(operation);
    query.addBindValue(entityType);
    query.addBindValue(entityId);
    query.addBindValue(detail);
    query.addBindValue(utcNow());

    if (!query.exec()) {
        return queryFailure("Cannot write the operation log", query);
    }

    return core::Result<bool>::success(true);
}

inline std::int64_t checkedLimit(std::size_t maximumCount)
{
    constexpr auto largest =
        static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)());
    if (maximumCount > largest) {
        return (std::numeric_limits<std::int64_t>::max)();
    }
    return static_cast<std::int64_t>(maximumCount);
}

}  // namespace signallab::database::detail
