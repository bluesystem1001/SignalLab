#include "signallab/database/mariadb_backup_repository.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <mysql.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace signallab::database {
namespace {

constexpr qint64 chunkSize = 256 * 1024;
constexpr std::uint64_t maximumBytes = 8ULL * 1024 * 1024 * 1024;
constexpr std::uint64_t maximumFiles = 10'000;
constexpr qsizetype maximumManifestBytes = 4 * 1024 * 1024;

QString text(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString pathText(const std::filesystem::path& path)
{
    return QString::fromStdWString(path.wstring());
}

core::Error invalid(std::string message)
{
    return {core::ErrorCode::InvalidParameter, std::move(message)};
}

core::Error cancelled()
{
    return {core::ErrorCode::Cancelled, "Backup operation was cancelled."};
}

core::Error sqlError(const std::string& operation, unsigned int code)
{
    std::string message = operation + ".";
    if (code == 1045 || code == 1698) {
        message += " Authentication failed; check the username and password.";
    } else if (code == 2002 || code == 2003) {
        message += " Cannot reach MariaDB; check the service, host and port.";
    } else if (code == 1044 || code == 1142) {
        message += " The account does not have the required backup database permissions.";
    }
    if (code != 0) {
        message += " SQL error " + std::to_string(code) + ".";
    }
    return {core::ErrorCode::DatabaseQueryFailed, std::move(message)};
}

bool validId(const std::string& value)
{
    static const QRegularExpression pattern(
        "\\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\z");
    return pattern.match(text(value)).hasMatch();
}

bool validHash(const std::string& value)
{
    static const QRegularExpression pattern("\\A[0-9a-f]{64}\\z");
    return pattern.match(text(value)).hasMatch();
}

bool validRelativePath(const std::string& value)
{
    static const QRegularExpression pattern("\\Adata/imports/[0-9a-f]{64}\\.(?:source|csv|wav)\\z");
    return value == "data/signallab.sqlite3" || pattern.match(text(value)).hasMatch();
}

core::Result<bool> validateOptions(const repository::BackupConnectionOptions& options)
{
    static const QRegularExpression databasePattern("\\A[A-Za-z][A-Za-z0-9_]{0,63}\\z");
    const auto databaseName = text(options.databaseName);
    if (!databasePattern.match(databaseName).hasMatch()) {
        return core::Result<bool>::failure(invalid(
            "Backup database name must start with a letter and contain only letters, digits or underscores (64 characters maximum)."));
    }
    const auto loweredName = databaseName.toLower();
    if (loweredName == "mysql" || loweredName == "information_schema"
        || loweredName == "performance_schema" || loweredName == "sys") {
        return core::Result<bool>::failure(invalid("Choose a dedicated backup database instead of a MariaDB system schema."));
    }
    if (options.host.empty() || options.host.size() > 255 || options.username.empty()
        || options.username.size() > 128 || options.port == 0 || options.port > 65535
        || options.host.find('\0') != std::string::npos
        || options.username.find('\0') != std::string::npos
        || options.password.find('\0') != std::string::npos) {
        return core::Result<bool>::failure(invalid("Invalid MariaDB connection settings."));
    }
    return core::Result<bool>::success(true);
}

class ClientLibrary {
public:
    ClientLibrary() : initialized_(mysql_library_init(0, nullptr, nullptr) == 0) {}
    ~ClientLibrary()
    {
        if (initialized_) {
            mysql_library_end();
        }
    }
    bool initialized() const { return initialized_; }

private:
    bool initialized_;
};

class ClientThread {
public:
    ClientThread() : initialized_(mysql_thread_init() == 0) {}
    ~ClientThread()
    {
        if (initialized_) {
            mysql_thread_end();
        }
    }
    bool initialized() const { return initialized_; }

private:
    bool initialized_;
};

class Connection {
public:
    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection()
    {
        if (database_) {
            mysql_close(database_);
        }
    }

    core::Result<bool> open(const repository::BackupConnectionOptions& options, bool selectDatabase)
    {
        const auto valid = validateOptions(options);
        if (!valid) {
            return valid;
        }
        static const ClientLibrary library;
        if (!library.initialized()) {
            return core::Result<bool>::failure(core::ErrorCode::DatabaseOpenFailed,
                "Cannot initialize the MariaDB client library.");
        }
        thread_local const ClientThread thread;
        if (!thread.initialized()) {
            return core::Result<bool>::failure(core::ErrorCode::DatabaseOpenFailed,
                "Cannot initialize the MariaDB client thread.");
        }
        database_ = mysql_init(nullptr);
        if (!database_) {
            return core::Result<bool>::failure(core::ErrorCode::DatabaseOpenFailed,
                "Cannot allocate a MariaDB connection.");
        }
        const unsigned int connectTimeout = 5;
        const unsigned int transferTimeout = 10;
        const unsigned int localInfile = 0;
        const my_bool reconnect = 0;
        const my_bool reportTruncation = 1;
        if (mysql_options(database_, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout) != 0
            || mysql_options(database_, MYSQL_OPT_READ_TIMEOUT, &transferTimeout) != 0
            || mysql_options(database_, MYSQL_OPT_WRITE_TIMEOUT, &transferTimeout) != 0
            || mysql_options(database_, MYSQL_OPT_RECONNECT, &reconnect) != 0
            || mysql_options(database_, MYSQL_OPT_LOCAL_INFILE, &localInfile) != 0
            || mysql_options(database_, MYSQL_REPORT_DATA_TRUNCATION, &reportTruncation) != 0
            || mysql_options(database_, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
            return core::Result<bool>::failure(sqlError("Cannot configure MariaDB client options", lastError()));
        }
        if (!mysql_real_connect(database_, options.host.c_str(), options.username.c_str(),
                options.password.c_str(), selectDatabase ? options.databaseName.c_str() : nullptr,
                options.port, nullptr, 0)) {
            auto error = sqlError("Cannot open MariaDB backup connection", lastError());
            error.code = core::ErrorCode::DatabaseOpenFailed;
            return core::Result<bool>::failure(std::move(error));
        }
        if (mysql_set_character_set(database_, "utf8mb4") != 0
            || !execute("SET SESSION innodb_lock_wait_timeout = 10")) {
            return core::Result<bool>::failure(sqlError("Cannot configure backup connection", lastError()));
        }
        return core::Result<bool>::success(true);
    }

    MYSQL* handle() const { return database_; }
    unsigned int lastError() const { return database_ ? mysql_errno(database_) : 0; }
    bool execute(std::string_view sql)
    {
        return mysql_real_query(database_, sql.data(), static_cast<unsigned long>(sql.size())) == 0;
    }

private:
    MYSQL* database_{nullptr};
};

struct Parameter {
    Parameter(const std::string& value) : type(MYSQL_TYPE_STRING), stringValue(value) {}
    Parameter(const QByteArray& value) : type(MYSQL_TYPE_BLOB), bytesValue(value) {}
    Parameter(std::uint64_t value) : type(MYSQL_TYPE_LONGLONG), integerValue(value) {}

    enum_field_types type;
    std::string stringValue;
    QByteArray bytesValue;
    std::uint64_t integerValue{0};
};

struct ResultColumn {
    QByteArray bytes;
    std::uint64_t integer{0};
    unsigned long length{0};
    my_bool isNull{0};
    my_bool truncated{0};
};

class Statement {
public:
    explicit Statement(Connection& connection) : connection_(connection), statement_(mysql_stmt_init(connection.handle())) {}
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement()
    {
        if (statement_) {
            mysql_stmt_free_result(statement_);
            mysql_stmt_close(statement_);
        }
    }

    bool prepare(std::string_view sql, bool cursor = false)
    {
        if (!statement_ || mysql_stmt_prepare(statement_, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
            return false;
        }
        if (cursor) {
            const unsigned long cursorType = CURSOR_TYPE_READ_ONLY;
            const unsigned long prefetchRows = 1;
            if (mysql_stmt_attr_set(statement_, STMT_ATTR_CURSOR_TYPE, &cursorType) != 0
                || mysql_stmt_attr_set(statement_, STMT_ATTR_PREFETCH_ROWS, &prefetchRows) != 0) {
                return false;
            }
        }
        return true;
    }

    bool execute(std::initializer_list<Parameter> parameters = {})
    {
        if (!statement_ || mysql_stmt_param_count(statement_) != parameters.size()) {
            return false;
        }
        std::vector<MYSQL_BIND> bindings(parameters.size());
        std::vector<unsigned long> lengths(parameters.size());
        std::size_t index = 0;
        for (const auto& parameter : parameters) {
            auto& binding = bindings[index];
            binding.buffer_type = parameter.type;
            if (parameter.type == MYSQL_TYPE_LONGLONG) {
                binding.buffer = const_cast<std::uint64_t*>(&parameter.integerValue);
                binding.buffer_length = sizeof(parameter.integerValue);
                binding.is_unsigned = 1;
            } else if (parameter.type == MYSQL_TYPE_BLOB) {
                binding.buffer = const_cast<char*>(parameter.bytesValue.constData());
                binding.buffer_length = static_cast<unsigned long>(parameter.bytesValue.size());
            } else {
                binding.buffer = const_cast<char*>(parameter.stringValue.data());
                binding.buffer_length = static_cast<unsigned long>(parameter.stringValue.size());
            }
            lengths[index] = binding.buffer_length;
            binding.length = &lengths[index];
            ++index;
        }
        if (!bindings.empty() && mysql_stmt_bind_param(statement_, bindings.data()) != 0) {
            return false;
        }
        return mysql_stmt_execute(statement_) == 0;
    }

    bool bindResults(std::initializer_list<qsizetype> capacities)
    {
        if (!statement_ || mysql_stmt_field_count(statement_) != capacities.size()) {
            return false;
        }
        columns_.clear();
        columns_.resize(capacities.size());
        std::vector<MYSQL_BIND> bindings(capacities.size());
        std::size_t index = 0;
        for (const auto capacity : capacities) {
            auto& column = columns_[index];
            auto& binding = bindings[index];
            if (capacity == 0) {
                binding.buffer_type = MYSQL_TYPE_LONGLONG;
                binding.buffer = &column.integer;
                binding.buffer_length = sizeof(column.integer);
                binding.is_unsigned = 1;
            } else {
                column.bytes.resize(capacity);
                binding.buffer_type = MYSQL_TYPE_BLOB;
                binding.buffer = column.bytes.data();
                binding.buffer_length = static_cast<unsigned long>(capacity);
            }
            binding.length = &column.length;
            binding.is_null = &column.isNull;
            binding.error = &column.truncated;
            ++index;
        }
        return mysql_stmt_bind_result(statement_, bindings.data()) == 0;
    }

    core::Result<bool> next(const std::string& operation)
    {
        const int status = mysql_stmt_fetch(statement_);
        if (status == MYSQL_NO_DATA) {
            return core::Result<bool>::success(false);
        }
        if (status == MYSQL_DATA_TRUNCATED) {
            return core::Result<bool>::failure(invalid("Backup query returned a value outside its supported size or numeric range."));
        }
        if (status != 0) {
            return core::Result<bool>::failure(sqlError(operation, lastError()));
        }
        for (const auto& column : columns_) {
            if (column.isNull || column.truncated
                || (!column.bytes.isEmpty() && column.length > static_cast<unsigned long>(column.bytes.size()))) {
                return core::Result<bool>::failure(invalid("Backup query returned a NULL or truncated value."));
            }
        }
        return core::Result<bool>::success(true);
    }

    QByteArray bytes(std::size_t index) const
    {
        return columns_[index].bytes.first(static_cast<qsizetype>(columns_[index].length));
    }
    std::string string(std::size_t index) const { return bytes(index).toStdString(); }
    std::uint64_t integer(std::size_t index) const { return columns_[index].integer; }
    unsigned int lastError() const { return statement_ ? mysql_stmt_errno(statement_) : connection_.lastError(); }
    bool finish() { return mysql_stmt_free_result(statement_) == 0; }

private:
    Connection& connection_;
    MYSQL_STMT* statement_;
    std::vector<ResultColumn> columns_;
};

class Transaction {
public:
    explicit Transaction(Connection& database) : database_(database) {}
    ~Transaction() { if (active_) { mysql_rollback(database_.handle()); } }
    bool begin() { active_ = database_.execute("START TRANSACTION"); return active_; }
    bool commit()
    {
        if (mysql_commit(database_.handle()) != 0) {
            return false;
        }
        active_ = false;
        return true;
    }

private:
    Connection& database_;
    bool active_{false};
};

core::Result<bool> ensureSchema(Connection& database, const std::string& databaseName)
{
    const std::array<std::string, 5> statements{
        "CREATE DATABASE IF NOT EXISTS `" + databaseName + "` CHARACTER SET utf8mb4 COLLATE utf8mb4_bin",
        "USE `" + databaseName + "`",
        "CREATE TABLE IF NOT EXISTS signallab_backups ("
        "id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY, "
        "created_utc VARCHAR(40) NOT NULL, label VARCHAR(255) NOT NULL, source_name VARCHAR(255) NOT NULL, "
        "total_bytes BIGINT UNSIGNED NOT NULL, file_count INT UNSIGNED NOT NULL, "
        "manifest_json MEDIUMTEXT NOT NULL, manifest_sha256 CHAR(64) CHARACTER SET ascii NOT NULL, "
        "INDEX signallab_backup_created (created_utc)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
        "CREATE TABLE IF NOT EXISTS signallab_backup_files ("
        "backup_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, file_index INT UNSIGNED NOT NULL, "
        "relative_path VARCHAR(100) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, "
        "byte_size BIGINT UNSIGNED NOT NULL, sha256 CHAR(64) CHARACTER SET ascii NOT NULL, "
        "chunk_count INT UNSIGNED NOT NULL, PRIMARY KEY (backup_id, file_index), "
        "UNIQUE KEY signallab_backup_path (backup_id, relative_path), "
        "FOREIGN KEY (backup_id) REFERENCES signallab_backups(id)) ENGINE=InnoDB",
        "CREATE TABLE IF NOT EXISTS signallab_backup_chunks ("
        "backup_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, "
        "file_index INT UNSIGNED NOT NULL, chunk_index INT UNSIGNED NOT NULL, payload MEDIUMBLOB NOT NULL, "
        "PRIMARY KEY (backup_id, file_index, chunk_index), "
        "FOREIGN KEY (backup_id, file_index) REFERENCES signallab_backup_files(backup_id, file_index)) ENGINE=InnoDB"
    };
    for (const auto& statement : statements) {
        if (!database.execute(statement)) {
            return core::Result<bool>::failure(sqlError("Cannot initialize backup database", database.lastError()));
        }
    }
    Statement query(database);
    if (!query.prepare("SELECT ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA = ? "
                       "AND TABLE_NAME IN ('signallab_backups', 'signallab_backup_files', 'signallab_backup_chunks')")) {
        return core::Result<bool>::failure(sqlError("Cannot inspect backup tables", query.lastError()));
    }
    if (!query.execute({databaseName}) || !query.bindResults({64})) {
        return core::Result<bool>::failure(sqlError("Cannot inspect backup tables", query.lastError()));
    }
    int count = 0;
    for (;;) {
        const auto fetched = query.next("Cannot inspect backup tables");
        if (!fetched) {
            return fetched;
        }
        if (!fetched.value()) {
            break;
        }
        ++count;
        if (text(query.string(0)).compare("InnoDB", Qt::CaseInsensitive) != 0) {
            return core::Result<bool>::failure(invalid("Backup tables must use the transactional InnoDB engine."));
        }
    }
    if (count != 3) {
        return core::Result<bool>::failure(invalid("The backup table schema is incomplete."));
    }
    return core::Result<bool>::success(true);
}

core::Result<bool> validateFiles(const std::vector<repository::BackupFile>& files, std::uint64_t totalBytes)
{
    if (files.empty() || files.size() > maximumFiles || totalBytes > maximumBytes) {
        return core::Result<bool>::failure(invalid("Backup exceeds the supported limit of 10,000 files or 8 GiB."));
    }
    bool hasDatabase = false;
    std::uint64_t countedBytes = 0;
    std::set<std::string> paths;
    for (const auto& file : files) {
        if (!validRelativePath(file.relativePath) || !validHash(file.sha256)
            || file.byteSize > maximumBytes - countedBytes
            || !paths.insert(file.relativePath).second) {
            return core::Result<bool>::failure(invalid("Backup contains an invalid file path, hash, size or duplicate entry."));
        }
        hasDatabase = hasDatabase || file.relativePath == "data/signallab.sqlite3";
        countedBytes += file.byteSize;
    }
    if (!hasDatabase || countedBytes != totalBytes) {
        return core::Result<bool>::failure(invalid("Backup database file or total size is inconsistent."));
    }
    return core::Result<bool>::success(true);
}

QByteArray makeManifest(const repository::BackupRecord& record, const std::vector<repository::BackupFile>& files)
{
    QJsonArray entries;
    for (const auto& file : files) {
        entries.append(QJsonObject{
            {"path", text(file.relativePath)},
            {"bytes", QString::number(file.byteSize)},
            {"sha256", text(file.sha256)}
        });
    }
    return QJsonDocument(QJsonObject{
        {"version", 1}, {"id", text(record.id)}, {"files", entries}
    }).toJson(QJsonDocument::Compact);
}

bool plainPath(const std::filesystem::path& path)
{
    std::error_code error;
    const auto absolutePath = std::filesystem::absolute(path, error);
    if (error) {
        return false;
    }
    auto componentPath = absolutePath.root_path();
    for (const auto& component : absolutePath.relative_path()) {
        componentPath /= component;
        const QFileInfo info(pathText(componentPath));
        if (info.isSymLink() || info.isJunction()) {
            return false;
        }
    }
    return true;
}

core::Result<std::vector<repository::BackupFile>> parseManifest(
    const QByteArray& manifest, const std::string& backupId, std::uint64_t totalBytes, std::uint64_t fileCount)
{
    if (manifest.size() > maximumManifestBytes) {
        return core::Result<std::vector<repository::BackupFile>>::failure(invalid("Backup manifest is too large."));
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(manifest, &parseError);
    const auto object = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || object.value("version").toInt() != 1 || object.value("id").toString() != text(backupId)
        || !object.value("files").isArray()) {
        return core::Result<std::vector<repository::BackupFile>>::failure(invalid("Backup manifest is invalid or unsupported."));
    }
    const auto entries = object.value("files").toArray();
    if (entries.size() != static_cast<qsizetype>(fileCount) || fileCount > maximumFiles) {
        return core::Result<std::vector<repository::BackupFile>>::failure(invalid("Backup manifest file count does not match."));
    }
    std::vector<repository::BackupFile> files;
    files.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto& entry : entries) {
        const auto file = entry.toObject();
        bool sizeValid = false;
        const auto bytes = file.value("bytes").toString().toULongLong(&sizeValid);
        if (!entry.isObject() || !sizeValid) {
            return core::Result<std::vector<repository::BackupFile>>::failure(invalid("Backup manifest file entry is invalid."));
        }
        files.push_back({file.value("path").toString().toStdString(), {}, bytes,
            file.value("sha256").toString().toStdString()});
    }
    const auto valid = validateFiles(files, totalBytes);
    if (!valid) {
        return core::Result<std::vector<repository::BackupFile>>::failure(valid.error());
    }
    return core::Result<std::vector<repository::BackupFile>>::success(std::move(files));
}

}

core::Result<std::string> MariaDbBackupRepository::testConnection(
    const repository::BackupConnectionOptions& options)
{
    Connection connection;
    const auto opened = connection.open(options, false);
    if (!opened) {
        return core::Result<std::string>::failure(opened.error());
    }
    Statement query(connection);
    if (!query.prepare("SELECT VERSION()") || !query.execute() || !query.bindResults({512})) {
        return core::Result<std::string>::failure(sqlError("Cannot read MariaDB version", query.lastError()));
    }
    const auto fetched = query.next("Cannot read MariaDB version");
    if (!fetched) {
        return core::Result<std::string>::failure(fetched.error());
    }
    if (!fetched.value()) {
        return core::Result<std::string>::failure(invalid("MariaDB did not return a server version."));
    }
    return core::Result<std::string>::success(query.string(0));
}

core::Result<repository::BackupRecord> MariaDbBackupRepository::upload(
    const repository::BackupConnectionOptions& options,
    repository::BackupRecord record,
    const std::vector<repository::BackupFile>& files,
    std::stop_token stopToken,
    repository::BackupProgress progress)
{
    if (stopToken.stop_requested()) {
        return core::Result<repository::BackupRecord>::failure(cancelled());
    }
    const auto valid = validateFiles(files, record.totalBytes);
    const auto timestamp = QDateTime::fromString(text(record.createdUtc), Qt::ISODateWithMs);
    if (!valid) {
        return core::Result<repository::BackupRecord>::failure(valid.error());
    }
    if (!validId(record.id) || !timestamp.isValid() || record.createdUtc.size() > 40
        || text(record.label).size() > 255 || text(record.sourceName).size() > 255
        || record.fileCount != files.size()) {
        return core::Result<repository::BackupRecord>::failure(invalid("Backup metadata is invalid."));
    }
    const auto manifest = makeManifest(record, files);
    if (manifest.size() > maximumManifestBytes) {
        return core::Result<repository::BackupRecord>::failure(invalid("Backup manifest is too large."));
    }
    Connection connection;
    const auto opened = connection.open(options, false);
    if (!opened) {
        return core::Result<repository::BackupRecord>::failure(opened.error());
    }
    auto& database = connection;
    const auto schema = ensureSchema(database, options.databaseName);
    if (!schema) {
        return core::Result<repository::BackupRecord>::failure(schema.error());
    }
    Transaction transaction(database);
    if (!transaction.begin()) {
        return core::Result<repository::BackupRecord>::failure(sqlError("Cannot start backup transaction", database.lastError()));
    }
    Statement insertRecord(database);
    if (!insertRecord.prepare("INSERT INTO signallab_backups "
            "(id, created_utc, label, source_name, total_bytes, file_count, manifest_json, manifest_sha256) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)")) {
        return core::Result<repository::BackupRecord>::failure(sqlError("Cannot prepare backup metadata", insertRecord.lastError()));
    }
    if (!insertRecord.execute({record.id, record.createdUtc, record.label, record.sourceName,
            record.totalBytes, record.fileCount, manifest,
            QCryptographicHash::hash(manifest, QCryptographicHash::Sha256).toHex()})) {
        return core::Result<repository::BackupRecord>::failure(sqlError("Cannot store backup metadata", insertRecord.lastError()));
    }
    std::uint64_t completed = 0;
    if (progress) {
        progress(completed, record.totalBytes);
    }
    for (std::size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
        if (stopToken.stop_requested()) {
            return core::Result<repository::BackupRecord>::failure(cancelled());
        }
        const auto& file = files[fileIndex];
        const QFileInfo info(pathText(file.localPath));
        if (!plainPath(file.localPath) || !info.isFile() || info.size() < 0
            || static_cast<std::uint64_t>(info.size()) != file.byteSize) {
            return core::Result<repository::BackupRecord>::failure(invalid("Backup source file is missing, unsafe or has changed size."));
        }
        QFile input(pathText(file.localPath));
        if (!input.open(QIODevice::ReadOnly)) {
            return core::Result<repository::BackupRecord>::failure(core::ErrorCode::FileOpenFailed, "Cannot open staged backup file.");
        }
        Statement insertFile(database);
        if (!insertFile.prepare("INSERT INTO signallab_backup_files "
                "(backup_id, file_index, relative_path, byte_size, sha256, chunk_count) VALUES (?, ?, ?, ?, ?, ?)")) {
            return core::Result<repository::BackupRecord>::failure(sqlError("Cannot prepare backup file", insertFile.lastError()));
        }
        if (!insertFile.execute({record.id, static_cast<std::uint64_t>(fileIndex), file.relativePath,
                file.byteSize, file.sha256, (file.byteSize + chunkSize - 1) / chunkSize})) {
            return core::Result<repository::BackupRecord>::failure(sqlError("Cannot store backup file metadata", insertFile.lastError()));
        }
        Statement insertChunk(database);
        if (!insertChunk.prepare("INSERT INTO signallab_backup_chunks (backup_id, file_index, chunk_index, payload) VALUES (?, ?, ?, ?)")) {
            return core::Result<repository::BackupRecord>::failure(sqlError("Cannot prepare backup chunk", insertChunk.lastError()));
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        std::uint64_t bytesRead = 0;
        std::uint64_t chunkIndex = 0;
        while (bytesRead < file.byteSize) {
            if (stopToken.stop_requested()) {
                return core::Result<repository::BackupRecord>::failure(cancelled());
            }
            const auto expectedBytes = static_cast<qint64>(std::min<std::uint64_t>(chunkSize, file.byteSize - bytesRead));
            const auto payload = input.read(expectedBytes);
            if (payload.size() != expectedBytes) {
                return core::Result<repository::BackupRecord>::failure(core::ErrorCode::FileOpenFailed, "Cannot read staged backup file completely.");
            }
            hash.addData(payload);
            if (!insertChunk.execute({record.id, static_cast<std::uint64_t>(fileIndex), chunkIndex, payload})) {
                return core::Result<repository::BackupRecord>::failure(sqlError("Cannot store backup chunk", insertChunk.lastError()));
            }
            bytesRead += static_cast<std::uint64_t>(payload.size());
            completed += static_cast<std::uint64_t>(payload.size());
            ++chunkIndex;
            if (progress) {
                progress(completed, record.totalBytes);
            }
        }
        if (input.error() != QFileDevice::NoError || !input.atEnd() || hash.result().toHex() != QByteArray::fromStdString(file.sha256)) {
            return core::Result<repository::BackupRecord>::failure(invalid("Staged backup file failed SHA-256 or length verification."));
        }
    }
    if (stopToken.stop_requested()) {
        return core::Result<repository::BackupRecord>::failure(cancelled());
    }
    if (!transaction.commit()) {
        return core::Result<repository::BackupRecord>::failure(sqlError("Cannot commit backup transaction", database.lastError()));
    }
    return core::Result<repository::BackupRecord>::success(std::move(record));
}

core::Result<std::vector<repository::BackupRecord>> MariaDbBackupRepository::list(
    const repository::BackupConnectionOptions& options)
{
    Connection connection;
    const auto opened = connection.open(options, true);
    if (!opened) {
        if (connection.lastError() == 1049) {
            return core::Result<std::vector<repository::BackupRecord>>::success({});
        }
        return core::Result<std::vector<repository::BackupRecord>>::failure(opened.error());
    }
    Statement query(connection);
    if (!query.prepare("SELECT id, created_utc, label, source_name, total_bytes, file_count "
                    "FROM signallab_backups ORDER BY created_utc DESC, id DESC")
        || !query.execute() || !query.bindResults({36, 40, 1020, 1020, 0, 0})) {
        if (query.lastError() == 1146) {
            return core::Result<std::vector<repository::BackupRecord>>::success({});
        }
        return core::Result<std::vector<repository::BackupRecord>>::failure(sqlError("Cannot list backups", query.lastError()));
    }
    std::vector<repository::BackupRecord> records;
    for (;;) {
        const auto fetched = query.next("Cannot finish reading backup list");
        if (!fetched) {
            return core::Result<std::vector<repository::BackupRecord>>::failure(fetched.error());
        }
        if (!fetched.value()) {
            break;
        }
        records.push_back({query.string(0), query.string(1), query.string(2), query.string(3),
            query.integer(4), query.integer(5)});
    }
    return core::Result<std::vector<repository::BackupRecord>>::success(std::move(records));
}

core::Result<std::vector<repository::BackupFile>> MariaDbBackupRepository::download(
    const repository::BackupConnectionOptions& options,
    const std::string& backupId,
    const std::filesystem::path& stagingDirectory,
    std::stop_token stopToken,
    repository::BackupProgress progress)
{
    using DownloadResult = core::Result<std::vector<repository::BackupFile>>;
    if (stopToken.stop_requested()) {
        return DownloadResult::failure(cancelled());
    }
    std::error_code filesystemError;
    if (!validId(backupId) || !plainPath(stagingDirectory)
        || !std::filesystem::is_directory(stagingDirectory, filesystemError) || filesystemError
        || !std::filesystem::is_empty(stagingDirectory, filesystemError) || filesystemError) {
        return DownloadResult::failure(invalid("Backup download requires a valid identifier and an empty, ordinary staging directory."));
    }
    Connection connection;
    const auto opened = connection.open(options, true);
    if (!opened) {
        return DownloadResult::failure(opened.error());
    }
    auto& database = connection;
    Transaction transaction(database);
    if (!transaction.begin()) {
        return DownloadResult::failure(sqlError("Cannot start backup read transaction", database.lastError()));
    }
    Statement metadata(database);
    if (!metadata.prepare("SELECT total_bytes, file_count, manifest_json, manifest_sha256 FROM signallab_backups WHERE id = ?")) {
        return DownloadResult::failure(sqlError("Cannot prepare backup lookup", metadata.lastError()));
    }
    if (!metadata.execute({backupId}) || !metadata.bindResults({0, 0, maximumManifestBytes, 64})) {
        return DownloadResult::failure(sqlError("Cannot load backup metadata", metadata.lastError()));
    }
    const auto metadataFetched = metadata.next("Cannot load backup metadata");
    if (!metadataFetched) {
        return DownloadResult::failure(metadataFetched.error());
    }
    if (!metadataFetched.value()) {
        return DownloadResult::failure(core::ErrorCode::FileNotFound, "The selected backup does not exist.");
    }
    const auto totalBytes = metadata.integer(0);
    const auto fileCount = metadata.integer(1);
    const auto manifest = metadata.bytes(2);
    if (QCryptographicHash::hash(manifest, QCryptographicHash::Sha256).toHex() != metadata.bytes(3)) {
        return DownloadResult::failure(invalid("Backup manifest failed SHA-256 verification."));
    }
    auto parsed = parseManifest(manifest, backupId, totalBytes, fileCount);
    if (!parsed) {
        return parsed;
    }
    auto files = std::move(parsed).value();
    if (!metadata.finish()) {
        return DownloadResult::failure(sqlError("Cannot finish reading backup metadata", metadata.lastError()));
    }
    Statement fileQuery(database);
    if (!fileQuery.prepare("SELECT file_index, relative_path, byte_size, sha256, chunk_count "
                           "FROM signallab_backup_files WHERE backup_id = ? ORDER BY file_index")) {
        return DownloadResult::failure(sqlError("Cannot prepare backup files lookup", fileQuery.lastError()));
    }
    if (!fileQuery.execute({backupId}) || !fileQuery.bindResults({0, 100, 0, 64, 0})) {
        return DownloadResult::failure(sqlError("Cannot load backup file metadata", fileQuery.lastError()));
    }
    std::vector<std::uint64_t> chunkCounts;
    for (;;) {
        const auto fetched = fileQuery.next("Cannot load backup file metadata");
        if (!fetched) {
            return DownloadResult::failure(fetched.error());
        }
        if (!fetched.value()) {
            break;
        }
        const auto index = chunkCounts.size();
        if (index >= files.size() || fileQuery.integer(0) != index
            || fileQuery.string(1) != files[index].relativePath
            || fileQuery.integer(2) != files[index].byteSize
            || fileQuery.string(3) != files[index].sha256
            || fileQuery.integer(4) != (files[index].byteSize + chunkSize - 1) / chunkSize) {
            return DownloadResult::failure(invalid("Backup file metadata does not match its manifest."));
        }
        chunkCounts.push_back(fileQuery.integer(4));
    }
    if (chunkCounts.size() != files.size()) {
        return DownloadResult::failure(invalid("Backup file metadata is incomplete."));
    }
    if (!fileQuery.finish()) {
        return DownloadResult::failure(sqlError("Cannot finish reading backup file metadata", fileQuery.lastError()));
    }
    std::uint64_t completed = 0;
    if (progress) {
        progress(completed, totalBytes);
    }
    for (std::size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
        if (stopToken.stop_requested()) {
            return DownloadResult::failure(cancelled());
        }
        auto& file = files[fileIndex];
        file.localPath = stagingDirectory / std::filesystem::path(file.relativePath);
        if (!plainPath(file.localPath)) {
            return DownloadResult::failure(invalid("Unsafe staging file path."));
        }
        std::filesystem::create_directories(file.localPath.parent_path(), filesystemError);
        if (filesystemError) {
            return DownloadResult::failure(core::ErrorCode::FileWriteFailed, "Cannot create backup staging directory.");
        }
        QFile output(pathText(file.localPath));
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            return DownloadResult::failure(core::ErrorCode::FileWriteFailed, "Cannot create staged backup file.");
        }
        Statement chunks(database);
        if (!chunks.prepare("SELECT chunk_index, payload FROM signallab_backup_chunks "
                            "WHERE backup_id = ? AND file_index = ? ORDER BY chunk_index", true)) {
            return DownloadResult::failure(sqlError("Cannot prepare backup chunk lookup", chunks.lastError()));
        }
        if (!chunks.execute({backupId, static_cast<std::uint64_t>(fileIndex)}) || !chunks.bindResults({0, chunkSize})) {
            return DownloadResult::failure(sqlError("Cannot read backup chunks", chunks.lastError()));
        }
        std::uint64_t chunkIndex = 0;
        std::uint64_t bytesWritten = 0;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (;;) {
            if (stopToken.stop_requested()) {
                return DownloadResult::failure(cancelled());
            }
            const auto fetched = chunks.next("Cannot read backup chunks");
            if (!fetched) {
                return DownloadResult::failure(fetched.error());
            }
            if (!fetched.value()) {
                break;
            }
            const auto payload = chunks.bytes(1);
            const auto expectedBytes = std::min<std::uint64_t>(chunkSize, file.byteSize - bytesWritten);
            if (chunks.integer(0) != chunkIndex || chunkIndex >= chunkCounts[fileIndex]
                || payload.size() != static_cast<qsizetype>(expectedBytes)) {
                return DownloadResult::failure(invalid("Backup chunk ordering or size is invalid."));
            }
            if (output.write(payload) != payload.size()) {
                return DownloadResult::failure(core::ErrorCode::FileWriteFailed, "Cannot write staged backup data.");
            }
            hash.addData(payload);
            bytesWritten += static_cast<std::uint64_t>(payload.size());
            completed += static_cast<std::uint64_t>(payload.size());
            ++chunkIndex;
            if (progress) {
                progress(completed, totalBytes);
            }
        }
        if (bytesWritten != file.byteSize || chunkIndex != chunkCounts[fileIndex]
            || hash.result().toHex() != QByteArray::fromStdString(file.sha256)) {
            return DownloadResult::failure(invalid("Downloaded backup failed SHA-256 or length verification."));
        }
        if (!output.flush()) {
            return DownloadResult::failure(core::ErrorCode::FileWriteFailed, "Cannot flush staged backup file.");
        }
    }
    if (stopToken.stop_requested()) {
        return DownloadResult::failure(cancelled());
    }
    if (!transaction.commit()) {
        return DownloadResult::failure(sqlError("Cannot finish backup read transaction", database.lastError()));
    }
    return DownloadResult::success(std::move(files));
}

}
