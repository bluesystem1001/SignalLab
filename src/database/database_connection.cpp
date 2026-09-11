#include "signallab/database/database_connection.hpp"

#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>
#include <QVariant>

#include <system_error>
#include <utility>

#include "signallab/core/error.hpp"

namespace signallab::database {
namespace {

core::Result<bool> applyPragma(
    const QSqlDatabase& database,
    const QString& pragmaName,
    const QString& settingSql,
    const QString& querySql,
    const QString& expectedValue)
{
    QSqlQuery query(database);
    if (!query.prepare(settingSql) || !query.exec()) {
        return core::Result<bool>::failure(
            core::ErrorCode::DatabaseOpenFailed,
            "Cannot apply SQLite setting " + pragmaName.toStdString()
                + ": " + query.lastError().text().toStdString());
    }
    query.finish();

    QSqlQuery verification(database);
    if (!verification.prepare(querySql)
        || !verification.exec()
        || !verification.next()) {
        return core::Result<bool>::failure(
            core::ErrorCode::DatabaseOpenFailed,
            "Cannot verify SQLite setting " + pragmaName.toStdString()
                + ": " + verification.lastError().text().toStdString());
    }

    const QString actualValue = verification.value(0).toString();
    if (actualValue.compare(expectedValue, Qt::CaseInsensitive) != 0) {
        return core::Result<bool>::failure(
            core::ErrorCode::DatabaseOpenFailed,
            "SQLite setting " + pragmaName.toStdString()
                + " expected " + expectedValue.toStdString()
                + " but reported " + actualValue.toStdString() + ".");
    }

    return core::Result<bool>::success(true);
}

}  // namespace

DatabaseConnection::DatabaseConnection(std::filesystem::path databasePath)
    : databasePath_(std::move(databasePath)),
      connectionName_(
          "signallab-"
          + QUuid::createUuid().toString(QUuid::WithoutBraces)),
      ownerThread_(QThread::currentThread())
{
}

DatabaseConnection::~DatabaseConnection()
{
    closeWithoutResult();
}

core::Result<bool> DatabaseConnection::open()
{
    if (QThread::currentThread() != ownerThread_) {
        return core::Result<bool>::failure(
            core::ErrorCode::DatabaseOpenFailed,
            "The SQLite connection must be opened in its creating thread.");
    }

    if (database_.isOpen()) {
        return core::Result<bool>::success(true);
    }

    if (databasePath_.empty()) {
        return core::Result<bool>::failure(
            core::ErrorCode::DatabaseOpenFailed,
            "The database path must not be empty.");
    }

    if (databasePath_ != std::filesystem::path(":memory:")) {
        const std::filesystem::path parent = databasePath_.parent_path();
        if (!parent.empty()) {
            std::error_code directoryError;
            std::filesystem::create_directories(parent, directoryError);
            if (directoryError) {
                return core::Result<bool>::failure(
                    core::ErrorCode::DatabaseOpenFailed,
                    "Cannot create the database directory: "
                        + parent.generic_string() + " ("
                        + directoryError.message() + ")");
            }
        }
    }

    database_ = QSqlDatabase::addDatabase("QSQLITE", connectionName_);
    database_.setDatabaseName(
        QString::fromStdWString(databasePath_.wstring()));

    if (!database_.open()) {
        const std::string message =
            database_.lastError().text().toStdString();
        closeWithoutResult();
        return core::Result<bool>::failure(
            core::ErrorCode::DatabaseOpenFailed,
            "Cannot open the SQLite database: " + message);
    }

    const auto foreignKeys = applyPragma(
        database_,
        "foreign_keys",
        "PRAGMA foreign_keys = ON",
        "PRAGMA foreign_keys",
        "1");
    if (!foreignKeys) {
        closeWithoutResult();
        return foreignKeys;
    }

    const auto journalMode = applyPragma(
        database_,
        "journal_mode",
        "PRAGMA journal_mode = WAL",
        "PRAGMA journal_mode",
        "wal");
    if (!journalMode) {
        closeWithoutResult();
        return journalMode;
    }

    const auto busyTimeout = applyPragma(
        database_,
        "busy_timeout",
        "PRAGMA busy_timeout = 5000",
        "PRAGMA busy_timeout",
        "5000");
    if (!busyTimeout) {
        closeWithoutResult();
        return busyTimeout;
    }

    return core::Result<bool>::success(true);
}

core::Result<bool> DatabaseConnection::close()
{
    const auto threadCheck = checkThread();
    if (!threadCheck) {
        return threadCheck;
    }

    closeWithoutResult();
    return core::Result<bool>::success(true);
}

core::Result<bool> DatabaseConnection::checkThread() const
{
    if (QThread::currentThread() != ownerThread_) {
        return core::Result<bool>::failure(
            core::ErrorCode::DatabaseQueryFailed,
            "The SQLite connection was accessed from a different thread.");
    }

    if (!database_.isValid() || !database_.isOpen()) {
        return core::Result<bool>::failure(
            core::ErrorCode::DatabaseOpenFailed,
            "The SQLite connection is not open.");
    }

    return core::Result<bool>::success(true);
}

bool DatabaseConnection::isOpen() const noexcept
{
    return database_.isOpen();
}

QSqlDatabase DatabaseConnection::database() const
{
    return database_;
}

const std::filesystem::path& DatabaseConnection::databasePath() const noexcept
{
    return databasePath_;
}

QString DatabaseConnection::connectionName() const
{
    return connectionName_;
}

void DatabaseConnection::closeWithoutResult() noexcept
{
    if (!database_.isValid()) {
        return;
    }

    database_.close();

    // QSqlDatabase is a reference-counted handle. Drop our handle before
    // removeDatabase(), as required by Qt.
    database_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName_);
}

}  // namespace signallab::database
