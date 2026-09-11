#include "signallab/database/migration_runner.hpp"

#include <QDateTime>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

#include "signallab/core/error.hpp"

namespace signallab::database {
namespace {

std::vector<QString> splitSqlStatements(const QString& script)
{
    std::vector<QString> statements;
    QString current;
    QChar quote;
    bool lineComment = false;
    bool blockComment = false;

    for (qsizetype index = 0; index < script.size(); ++index) {
        const QChar character = script.at(index);
        const QChar next = index + 1 < script.size()
            ? script.at(index + 1)
            : QChar();

        if (lineComment) {
            if (character == '\n') {
                lineComment = false;
                current.append(character);
            }
            continue;
        }

        if (blockComment) {
            if (character == '*' && next == '/') {
                blockComment = false;
                ++index;
            }
            continue;
        }

        if (quote.isNull()) {
            if (character == '-' && next == '-') {
                lineComment = true;
                ++index;
                continue;
            }
            if (character == '/' && next == '*') {
                blockComment = true;
                ++index;
                continue;
            }
            if (character == '\'' || character == '"'
                || character == '`') {
                quote = character;
                current.append(character);
                continue;
            }
            if (character == ';') {
                const QString statement = current.trimmed();
                if (!statement.isEmpty()) {
                    statements.push_back(statement);
                }
                current.clear();
                continue;
            }
        } else if (character == quote) {
            if (next == quote) {
                current.append(character);
                current.append(next);
                ++index;
                continue;
            }
            quote = QChar();
        }

        current.append(character);
    }

    const QString lastStatement = current.trimmed();
    if (!lastStatement.isEmpty()) {
        statements.push_back(lastStatement);
    }
    return statements;
}

core::Result<bool> migrationFailure(
    const std::string& action,
    const QSqlQuery& query)
{
    return core::Result<bool>::failure(
        core::ErrorCode::MigrationFailed,
        action + ": " + query.lastError().text().toStdString());
}

}  // namespace

MigrationRunner::MigrationRunner(DatabaseConnection& connection)
    : connection_(connection)
{
}

core::Result<std::size_t> MigrationRunner::run(
    const std::filesystem::path& migrationsDirectory)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<std::size_t>::failure(threadCheck.error());
    }

    std::error_code fileError;
    if (!std::filesystem::is_directory(migrationsDirectory, fileError)) {
        return core::Result<std::size_t>::failure(
            core::ErrorCode::MigrationFailed,
            "Migration directory not found: "
                + migrationsDirectory.generic_string());
    }

    std::vector<std::filesystem::path> migrationFiles;
    std::filesystem::directory_iterator iterator(
        migrationsDirectory, fileError);
    const std::filesystem::directory_iterator end;
    while (!fileError && iterator != end) {
        if (iterator->is_regular_file(fileError)
            && iterator->path().extension() == ".sql") {
            migrationFiles.push_back(iterator->path());
        }
        iterator.increment(fileError);
    }
    if (fileError) {
        return core::Result<std::size_t>::failure(
            core::ErrorCode::MigrationFailed,
            "Cannot scan migration directory: " + fileError.message());
    }

    std::sort(
        migrationFiles.begin(),
        migrationFiles.end(),
        [](const auto& left, const auto& right) {
            return left.filename().generic_string()
                < right.filename().generic_string();
        });

    QSqlDatabase database = connection_.database();
    {
        QSqlQuery createTable(database);
        if (!createTable.prepare(
                "CREATE TABLE IF NOT EXISTS schema_migration ("
                "version TEXT PRIMARY KEY, "
                "applied_utc TEXT NOT NULL"
                ")")
            || !createTable.exec()) {
            const auto failure = migrationFailure(
                "Cannot create schema_migration", createTable);
            return core::Result<std::size_t>::failure(failure.error());
        }
    }

    std::size_t appliedCount = 0;
    for (const auto& migrationPath : migrationFiles) {
        const QString version = QString::fromStdWString(
            migrationPath.filename().wstring());

        QSqlQuery exists(database);
        if (!exists.prepare(
                "SELECT 1 FROM schema_migration WHERE version = ?")) {
            const auto failure = migrationFailure(
                "Cannot prepare migration lookup", exists);
            return core::Result<std::size_t>::failure(failure.error());
        }
        exists.addBindValue(version);
        if (!exists.exec()) {
            const auto failure = migrationFailure(
                "Cannot query migration history", exists);
            return core::Result<std::size_t>::failure(failure.error());
        }
        if (exists.next()) {
            continue;
        }
        exists.finish();

        QFile file(QString::fromStdWString(migrationPath.wstring()));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return core::Result<std::size_t>::failure(
                core::ErrorCode::MigrationFailed,
                "Cannot read migration "
                    + migrationPath.filename().generic_string() + ": "
                    + file.errorString().toStdString());
        }
        const QString script = QString::fromUtf8(file.readAll());
        file.close();

        if (!database.transaction()) {
            return core::Result<std::size_t>::failure(
                core::ErrorCode::MigrationFailed,
                "Cannot begin migration transaction: "
                    + database.lastError().text().toStdString());
        }

        bool migrationSucceeded = true;
        std::string migrationError;
        for (const QString& statement : splitSqlStatements(script)) {
            QSqlQuery query(database);
            if (!query.prepare(statement) || !query.exec()) {
                migrationSucceeded = false;
                migrationError = query.lastError().text().toStdString();
                break;
            }
        }

        if (migrationSucceeded) {
            QSqlQuery record(database);
            if (!record.prepare(
                    "INSERT INTO schema_migration(version, applied_utc) "
                    "VALUES (?, ?)") ) {
                migrationSucceeded = false;
                migrationError = record.lastError().text().toStdString();
            } else {
                record.addBindValue(version);
                record.addBindValue(
                    QDateTime::currentDateTimeUtc().toString(
                        Qt::ISODateWithMs));
                if (!record.exec()) {
                    migrationSucceeded = false;
                    migrationError = record.lastError().text().toStdString();
                }
            }
        }

        if (!migrationSucceeded) {
            database.rollback();
            return core::Result<std::size_t>::failure(
                core::ErrorCode::MigrationFailed,
                "Migration " + version.toStdString()
                    + " failed and was rolled back: " + migrationError);
        }

        if (!database.commit()) {
            database.rollback();
            return core::Result<std::size_t>::failure(
                core::ErrorCode::MigrationFailed,
                "Cannot commit migration " + version.toStdString() + ": "
                    + database.lastError().text().toStdString());
        }
        ++appliedCount;
    }

    return core::Result<std::size_t>::success(appliedCount);
}

}  // namespace signallab::database
