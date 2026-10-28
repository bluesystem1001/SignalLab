#include "signallab/database/project_backup_source.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace signallab::database {
namespace {

constexpr std::uint64_t maximumBackupBytes = 8ULL * 1024 * 1024 * 1024;
constexpr std::size_t maximumBackupFiles = 10'000;
constexpr qint64 blockSize = 256 * 1024;
constexpr const char* databaseRelativePath = "data/signallab.sqlite3";

template<typename Value>
core::Result<Value> failure(const std::string& message)
{
    return core::Result<Value>::failure(
        core::ErrorCode::InvalidParameter, message);
}

template<typename Value>
core::Result<Value> cancelled()
{
    return core::Result<Value>::failure(
        core::ErrorCode::Cancelled, "Project backup was cancelled.");
}

bool isSha256(const std::string& value)
{
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](char character) {
            return (character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'f');
        });
}

bool isImportPath(const std::string& value)
{
    constexpr std::size_t prefixLength = 13;
    if (!value.starts_with("data/imports/")
        || value.size() <= prefixLength + 64
        || !isSha256(value.substr(prefixLength, 64))) {
        return false;
    }
    const auto extension = value.substr(prefixLength + 64);
    return extension == ".source" || extension == ".csv" || extension == ".wav";
}

bool isLink(const std::filesystem::path& path, std::error_code& error)
{
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status)) {
        return true;
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = std::error_code(
            static_cast<int>(GetLastError()), std::system_category());
        return true;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

core::Result<std::filesystem::path> containedFile(
    const std::filesystem::path& directory,
    const std::string& relativePath)
{
    if (relativePath != databaseRelativePath && !isImportPath(relativePath)) {
        return failure<std::filesystem::path>(
            "The backup contains an unsupported file path.");
    }
    std::error_code error;
    const auto root = std::filesystem::canonical(directory, error);
    if (error || !std::filesystem::is_directory(root, error)) {
        return failure<std::filesystem::path>(
            "The project backup directory does not exist.");
    }
    auto candidate = root;
    for (const auto& component : std::filesystem::path(relativePath)) {
        candidate /= component;
        if (isLink(candidate, error)) {
            return failure<std::filesystem::path>(
                "A backup file is missing or passes through a symbolic link: "
                + relativePath);
        }
    }
    if (!std::filesystem::is_regular_file(candidate, error) || error) {
        return failure<std::filesystem::path>(
            "A required backup file is not a regular file: " + relativePath);
    }
    const auto canonical = std::filesystem::canonical(candidate, error);
    if (error || canonical.lexically_relative(root).generic_string()
            != relativePath) {
        return failure<std::filesystem::path>(
            "A backup file escapes the project directory.");
    }
    return core::Result<std::filesystem::path>::success(canonical);
}

class ReadOnlyDatabase {
public:
    ReadOnlyDatabase()
        : name_("signallab-backup-"
            + QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
    }

    ~ReadOnlyDatabase()
    {
        database_.close();
        database_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(name_);
    }

    core::Result<bool> open(const std::filesystem::path& path)
    {
        database_ = QSqlDatabase::addDatabase("QSQLITE", name_);
        database_.setConnectOptions(
            "QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000");
        database_.setDatabaseName(QString::fromStdWString(path.wstring()));
        if (!database_.open()) {
            return core::Result<bool>::failure(
                core::ErrorCode::DatabaseOpenFailed,
                "Cannot open the project backup database: "
                    + database_.lastError().text().toStdString());
        }
        return core::Result<bool>::success(true);
    }

    QSqlDatabase database() const
    {
        return database_;
    }

private:
    QString name_;
    QSqlDatabase database_;
};

core::Result<std::vector<repository::BackupFile>> databaseImports(
    const std::filesystem::path& directory,
    std::stop_token stopToken)
{
    using Files = std::vector<repository::BackupFile>;
    if (stopToken.stop_requested()) {
        return cancelled<Files>();
    }
    const auto databasePath = containedFile(directory, databaseRelativePath);
    if (!databasePath) {
        return core::Result<Files>::failure(databasePath.error());
    }
    ReadOnlyDatabase connection;
    const auto opened = connection.open(databasePath.value());
    if (!opened) {
        return core::Result<Files>::failure(opened.error());
    }
    QSqlQuery integrity(connection.database());
    if (!integrity.exec("PRAGMA integrity_check")
        || !integrity.next() || integrity.value(0).toString() != "ok"
        || integrity.next() || integrity.lastError().isValid()) {
        return failure<Files>("The project database failed its integrity check.");
    }
    integrity.finish();
    if (stopToken.stop_requested()) {
        return cancelled<Files>();
    }
    QSqlQuery foreignKeys(connection.database());
    if (!foreignKeys.exec("PRAGMA foreign_key_check") || foreignKeys.next()
        || foreignKeys.lastError().isValid()) {
        return failure<Files>("The project database has invalid foreign keys.");
    }
    foreignKeys.finish();

    const std::map<QString, QString> requiredTables{
        {"schema_migration", "version, applied_utc"},
        {"file_version", "id, content_hash, stored_relative_path, original_name, byte_size, created_utc"},
        {"signal_source", "id, source_type, source_key, source_path, parameters_json, file_version_id, created_utc"},
        {"signal_record", "id, source_id, sample_rate, sample_count, sample_blob, sample_hash, signal_name, signal_unit, signal_source_label, created_utc"},
        {"analysis_task", "id, signal_record_id, signal_name, status, algorithm_name, algorithm_version, parameters_json, input_hash, output_hash, created_utc, started_utc, finished_utc, error_message"},
        {"feature_result", "id, task_id, feature_name, numeric_value, value_text, unit"},
        {"script_history", "id, script_text, status, created_utc, finished_utc, error_message, source_hash, error_line, error_column"},
        {"operation_log", "id, operation, entity_type, entity_id, detail, created_utc"}
    };
    for (const auto& [table, columns] : requiredTables) {
        if (stopToken.stop_requested()) {
            return cancelled<Files>();
        }
        QSqlQuery schema(connection.database());
        if (!schema.prepare(
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?")) {
            return failure<Files>("Cannot inspect the project database schema.");
        }
        schema.addBindValue(table);
        if (!schema.exec() || !schema.next()) {
            return failure<Files>("The backup is not a SignalLab project database.");
        }
        schema.finish();
        if (!schema.exec("SELECT " + columns + " FROM " + table + " LIMIT 0")) {
            return failure<Files>("The project database schema is incompatible.");
        }
    }
    QSqlQuery migrations(connection.database());
    if (!migrations.exec("SELECT version FROM schema_migration ORDER BY version")) {
        return failure<Files>("Cannot read project database migrations.");
    }
    std::vector<std::string> versions;
    while (migrations.next()) {
        versions.push_back(migrations.value(0).toString().toStdString());
        if (versions.size() > 3) {
            return failure<Files>("The backup uses an unsupported schema version.");
        }
    }
    if (migrations.lastError().isValid()
        || versions != std::vector<std::string>{
            "001_init.sql", "002_task_scheduler.sql", "003_signal_script.sql"}) {
        return failure<Files>("The backup uses an unsupported schema version.");
    }
    migrations.finish();

    QSqlQuery imports(connection.database());
    if (!imports.exec(
            "SELECT stored_relative_path, content_hash, byte_size "
            "FROM file_version ORDER BY stored_relative_path")) {
        return failure<Files>("Cannot read the project's imported-file manifest.");
    }
    Files files;
    std::uint64_t totalBytes = 0;
    std::set<std::string> paths;
    while (imports.next()) {
        if (stopToken.stop_requested()) {
            return cancelled<Files>();
        }
        const auto relativePath = imports.value(0).toString().toStdString();
        const auto hash = imports.value(1).toString().toStdString();
        bool validSize = false;
        const auto size = imports.value(2).toLongLong(&validSize);
        if (!isImportPath(relativePath) || !isSha256(hash)
            || relativePath.substr(13, 64) != hash
            || !validSize || size < 0
            || !paths.insert(relativePath).second) {
            return failure<Files>("The project's imported-file manifest is invalid.");
        }
        const auto unsignedSize = static_cast<std::uint64_t>(size);
        if (unsignedSize > maximumBackupBytes - totalBytes
            || files.size() >= maximumBackupFiles - 1) {
            return failure<Files>("The backup exceeds the 8 GiB or 10,000-file limit.");
        }
        totalBytes += unsignedSize;
        files.push_back({relativePath, {}, unsignedSize, hash});
    }
    if (imports.lastError().isValid()) {
        return failure<Files>("Cannot finish reading the imported-file manifest.");
    }
    return core::Result<Files>::success(std::move(files));
}

core::Result<repository::BackupFile> inspectFile(
    const std::filesystem::path& directory,
    const std::string& relativePath,
    const std::filesystem::path& destination,
    std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return cancelled<repository::BackupFile>();
    }
    const auto resolved = containedFile(directory, relativePath);
    if (!resolved) {
        return core::Result<repository::BackupFile>::failure(resolved.error());
    }
    std::error_code error;
    const auto initialSize = std::filesystem::file_size(resolved.value(), error);
    if (error || initialSize > maximumBackupBytes) {
        return failure<repository::BackupFile>("A backup file exceeds its size limit.");
    }
    const auto initialWrite = std::filesystem::last_write_time(resolved.value(), error);
    if (error) {
        return failure<repository::BackupFile>("Cannot inspect a backup file.");
    }
    QFile input(QString::fromStdWString(resolved.value().wstring()));
    if (!input.open(QIODevice::ReadOnly)) {
        return core::Result<repository::BackupFile>::failure(
            core::ErrorCode::FileOpenFailed, "Cannot read backup file: " + relativePath);
    }
    QFile output;
    if (!destination.empty()) {
        output.setFileName(QString::fromStdWString(destination.wstring()));
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            return core::Result<repository::BackupFile>::failure(
                core::ErrorCode::FileWriteFailed,
                "Cannot create staged backup file: " + relativePath);
        }
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::uint64_t bytesRead = 0;
    while (!input.atEnd()) {
        if (stopToken.stop_requested()) {
            return cancelled<repository::BackupFile>();
        }
        const QByteArray block = input.read(blockSize);
        if ((block.isEmpty() && !input.atEnd())
            || input.error() != QFileDevice::NoError) {
            return failure<repository::BackupFile>("Cannot finish reading a backup file.");
        }
        bytesRead += static_cast<std::uint64_t>(block.size());
        if (bytesRead > initialSize) {
            return failure<repository::BackupFile>("A file changed during backup.");
        }
        hash.addData(block);
        if (output.isOpen() && output.write(block) != block.size()) {
            return core::Result<repository::BackupFile>::failure(
                core::ErrorCode::FileWriteFailed, "Cannot write a staged backup file.");
        }
    }
    if (output.isOpen() && !output.flush()) {
        return core::Result<repository::BackupFile>::failure(
            core::ErrorCode::FileWriteFailed, "Cannot flush a staged backup file.");
    }
    output.close();
    input.close();
    if (stopToken.stop_requested()) {
        return cancelled<repository::BackupFile>();
    }
    const auto finalPath = containedFile(directory, relativePath);
    const auto finalSize = std::filesystem::file_size(resolved.value(), error);
    if (!finalPath || finalPath.value() != resolved.value() || error
        || bytesRead != initialSize || finalSize != initialSize) {
        return failure<repository::BackupFile>("A file disappeared or changed during backup.");
    }
    const auto finalWrite = std::filesystem::last_write_time(resolved.value(), error);
    if (error || finalWrite != initialWrite) {
        return failure<repository::BackupFile>("A file changed during backup.");
    }
    return core::Result<repository::BackupFile>::success({
        relativePath,
        destination.empty() ? resolved.value() : destination,
        bytesRead,
        hash.result().toHex().toStdString()});
}

core::Result<bool> checkDirectoryContents(
    const std::filesystem::path& directory,
    const std::map<std::string, const repository::BackupFile*>& manifest,
    std::stop_token stopToken)
{
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(directory, error);
    const std::filesystem::recursive_directory_iterator end;
    std::size_t fileCount = 0;
    while (!error && iterator != end) {
        if (stopToken.stop_requested()) {
            return cancelled<bool>();
        }
        const auto& path = iterator->path();
        const auto relative = path.lexically_relative(directory).generic_string();
        if (isLink(path, error)) {
            return failure<bool>("The restored project contains a symbolic link.");
        }
        if (iterator->is_directory(error)) {
            if (relative != "data" && relative != "data/imports") {
                return failure<bool>("The restored project contains an unexpected directory.");
            }
        } else if (!iterator->is_regular_file(error)
            || !manifest.contains(relative)) {
            return failure<bool>("The restored project contains an unexpected file.");
        } else {
            ++fileCount;
        }
        iterator.increment(error);
    }
    if (error || fileCount != manifest.size()) {
        return failure<bool>("The restored project has missing or unreadable files.");
    }
    return core::Result<bool>::success(true);
}

}

ProjectBackupSource::ProjectBackupSource(std::filesystem::path projectRoot)
    : projectRoot_(std::move(projectRoot))
{
}

core::Result<std::vector<repository::BackupFile>> ProjectBackupSource::snapshot(
    const std::filesystem::path& stagingDirectory,
    std::stop_token stopToken)
{
    using Files = std::vector<repository::BackupFile>;
    if (stopToken.stop_requested()) {
        return cancelled<Files>();
    }
    const auto sourcePath = containedFile(projectRoot_, databaseRelativePath);
    if (!sourcePath) {
        return core::Result<Files>::failure(sourcePath.error());
    }
    std::error_code error;
    if (!std::filesystem::is_directory(stagingDirectory, error)
        || error || !std::filesystem::is_empty(stagingDirectory, error) || error
        || isLink(stagingDirectory, error)) {
        return failure<Files>("Backup staging requires an empty directory without symbolic links.");
    }
    const auto staging = std::filesystem::canonical(stagingDirectory, error);
    if (error || std::filesystem::file_size(sourcePath.value(), error)
            > maximumBackupBytes || error) {
        return failure<Files>("Cannot stage the project database within the 8 GiB limit.");
    }
    std::filesystem::create_directories(staging / "data" / "imports", error);
    if (error) {
        return core::Result<Files>::failure(
            core::ErrorCode::FileWriteFailed, "Cannot create the backup staging directories.");
    }
    {
        ReadOnlyDatabase connection;
        const auto opened = connection.open(sourcePath.value());
        if (!opened) {
            return core::Result<Files>::failure(opened.error());
        }
        QSqlQuery query(connection.database());
        if (!query.prepare("VACUUM main INTO ?")) {
            return core::Result<Files>::failure(
                core::ErrorCode::DatabaseQueryFailed, "Cannot prepare the SQLite backup snapshot.");
        }
        query.addBindValue(QString::fromStdWString(
            (staging / databaseRelativePath).wstring()));
        if (!query.exec()) {
            return core::Result<Files>::failure(
                core::ErrorCode::DatabaseQueryFailed,
                "Cannot create the SQLite backup snapshot: "
                    + query.lastError().text().toStdString());
        }
    }
    if (stopToken.stop_requested()) {
        return cancelled<Files>();
    }
    const auto imports = databaseImports(staging, stopToken);
    if (!imports) {
        return core::Result<Files>::failure(imports.error());
    }
    const auto databaseFile = inspectFile(staging, databaseRelativePath, {}, stopToken);
    if (!databaseFile) {
        return core::Result<Files>::failure(databaseFile.error());
    }
    Files files{databaseFile.value()};
    auto totalBytes = databaseFile.value().byteSize;
    for (const auto& imported : imports.value()) {
        if (imported.byteSize > maximumBackupBytes - totalBytes) {
            return failure<Files>("The backup exceeds the 8 GiB limit.");
        }
        const auto copied = inspectFile(
            projectRoot_, imported.relativePath, staging / imported.relativePath, stopToken);
        if (!copied) {
            return core::Result<Files>::failure(copied.error());
        }
        if (copied.value().byteSize != imported.byteSize
            || copied.value().sha256 != imported.sha256) {
            return failure<Files>("An imported source file does not match the database: "
                + imported.relativePath);
        }
        totalBytes += imported.byteSize;
        files.push_back(copied.value());
    }
    if (stopToken.stop_requested()) {
        return cancelled<Files>();
    }
    return core::Result<Files>::success(std::move(files));
}

core::Result<bool> ProjectBackupSource::verify(
    const std::filesystem::path& directory,
    const std::vector<repository::BackupFile>& files,
    std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return cancelled<bool>();
    }
    if (files.empty() || files.size() > maximumBackupFiles) {
        return failure<bool>("The backup manifest contains an invalid number of files.");
    }
    std::error_code error;
    if (isLink(directory, error)) {
        return failure<bool>("The backup directory is a symbolic link or is unavailable.");
    }
    const auto root = std::filesystem::canonical(directory, error);
    if (error) {
        return failure<bool>("The backup directory is unavailable.");
    }
    std::map<std::string, const repository::BackupFile*> manifest;
    std::uint64_t totalBytes = 0;
    for (const auto& file : files) {
        if ((file.relativePath != databaseRelativePath && !isImportPath(file.relativePath))
            || !isSha256(file.sha256)
            || file.byteSize > maximumBackupBytes - totalBytes
            || !manifest.emplace(file.relativePath, &file).second) {
            return failure<bool>("The backup manifest is invalid or exceeds the 8 GiB limit.");
        }
        totalBytes += file.byteSize;
    }
    if (!manifest.contains(databaseRelativePath)) {
        return failure<bool>("The backup does not contain the project database.");
    }
    const auto contents = checkDirectoryContents(root, manifest, stopToken);
    if (!contents) {
        return contents;
    }
    for (const auto& file : files) {
        const auto inspected = inspectFile(root, file.relativePath, {}, stopToken);
        if (!inspected) {
            return core::Result<bool>::failure(inspected.error());
        }
        const auto providedPath = std::filesystem::canonical(file.localPath, error);
        if (error || providedPath != inspected.value().localPath
            || inspected.value().byteSize != file.byteSize
            || inspected.value().sha256 != file.sha256) {
            return failure<bool>("A restored backup file does not match its manifest.");
        }
    }
    const auto imports = databaseImports(root, stopToken);
    if (!imports) {
        return core::Result<bool>::failure(imports.error());
    }
    if (imports.value().size() + 1 != manifest.size()) {
        return failure<bool>("The database and backup manifest contain different files.");
    }
    for (const auto& imported : imports.value()) {
        const auto found = manifest.find(imported.relativePath);
        if (found == manifest.end()
            || found->second->byteSize != imported.byteSize
            || found->second->sha256 != imported.sha256) {
            return failure<bool>("An imported source does not match the restored database.");
        }
    }
    return checkDirectoryContents(root, manifest, stopToken);
}

}
