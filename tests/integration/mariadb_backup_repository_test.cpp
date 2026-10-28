#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include <gtest/gtest.h>
#include <mysql.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "signallab/database/mariadb_backup_repository.hpp"

namespace signallab::database {
namespace {

struct SqlParameter {
    std::string bytes;
    enum_field_types type{MYSQL_TYPE_STRING};
};

class TestConnection {
public:
    explicit TestConnection(const repository::BackupConnectionOptions& options)
    {
        connection_ = mysql_init(nullptr);
        if (connection_ == nullptr) {
            return;
        }
        const unsigned int connectTimeout = 5;
        const unsigned int ioTimeout = 10;
        const unsigned int localInfile = 0;
        const my_bool reconnect = 0;
        if (mysql_options(connection_, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout) != 0
            || mysql_options(connection_, MYSQL_OPT_READ_TIMEOUT, &ioTimeout) != 0
            || mysql_options(connection_, MYSQL_OPT_WRITE_TIMEOUT, &ioTimeout) != 0
            || mysql_options(connection_, MYSQL_OPT_RECONNECT, &reconnect) != 0
            || mysql_options(connection_, MYSQL_OPT_LOCAL_INFILE, &localInfile) != 0
            || mysql_options(connection_, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
            return;
        }
        connected_ = mysql_real_connect(connection_, options.host.c_str(), options.username.c_str(),
            options.password.c_str(), nullptr, options.port, nullptr, 0) != nullptr;
    }

    ~TestConnection()
    {
        if (connection_ != nullptr) {
            mysql_close(connection_);
        }
    }

    TestConnection(const TestConnection&) = delete;
    TestConnection& operator=(const TestConnection&) = delete;

    bool isOpen() const { return connected_; }

    bool selectDatabase(const std::string& databaseName)
    {
        return connected_ && mysql_select_db(connection_, databaseName.c_str()) == 0;
    }

    bool execute(const std::string& sql)
    {
        if (!connected_ || mysql_real_query(connection_, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
            return false;
        }
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(
            mysql_store_result(connection_), &mysql_free_result);
        return result != nullptr || mysql_field_count(connection_) == 0;
    }

    bool executePrepared(const std::string& sql, const std::vector<SqlParameter>& parameters)
    {
        if (!connected_) {
            return false;
        }
        std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)> statement(
            mysql_stmt_init(connection_), &mysql_stmt_close);
        return statement != nullptr && executeStatement(statement.get(), sql, parameters);
    }

    std::optional<std::uint64_t> scalar(
        const std::string& sql, const std::vector<SqlParameter>& parameters = {})
    {
        if (!connected_) {
            return {};
        }
        std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)> statement(
            mysql_stmt_init(connection_), &mysql_stmt_close);
        if (!statement || !executeStatement(statement.get(), sql, parameters)
            || mysql_stmt_field_count(statement.get()) != 1
            || mysql_stmt_store_result(statement.get()) != 0) {
            return {};
        }
        std::uint64_t value = 0;
        my_bool isNull = 0;
        my_bool truncated = 0;
        MYSQL_BIND result{};
        result.buffer_type = MYSQL_TYPE_LONGLONG;
        result.buffer = &value;
        result.buffer_length = sizeof(value);
        result.is_unsigned = 1;
        result.is_null = &isNull;
        result.error = &truncated;
        if (mysql_stmt_bind_result(statement.get(), &result) != 0
            || mysql_stmt_fetch(statement.get()) != 0 || isNull || truncated
            || mysql_stmt_fetch(statement.get()) != MYSQL_NO_DATA) {
            return {};
        }
        return value;
    }

private:
    static bool executeStatement(
        MYSQL_STMT* statement, const std::string& sql, const std::vector<SqlParameter>& parameters)
    {
        if (mysql_stmt_prepare(statement, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
            return false;
        }
        std::vector<MYSQL_BIND> bindings(parameters.size());
        std::vector<unsigned long> lengths(parameters.size());
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            lengths[index] = static_cast<unsigned long>(parameters[index].bytes.size());
            bindings[index].buffer_type = parameters[index].type;
            bindings[index].buffer = const_cast<char*>(parameters[index].bytes.data());
            bindings[index].buffer_length = lengths[index];
            bindings[index].length = &lengths[index];
        }
        return (bindings.empty() || mysql_stmt_bind_param(statement, bindings.data()) == 0)
            && mysql_stmt_execute(statement) == 0;
    }

    MYSQL* connection_{nullptr};
    bool connected_{false};
};

class MariaDbBackupRepositoryTest : public testing::Test {
protected:
    void SetUp() override
    {
        const auto password = qEnvironmentVariable("SIGNALLAB_MARIADB_TEST_PASSWORD");
        if (password.isEmpty()) {
            GTEST_SKIP() << "Set SIGNALLAB_MARIADB_TEST_PASSWORD to run isolated MariaDB integration tests.";
        }
        options_.password = password.toStdString();
        options_.host = qEnvironmentVariable("SIGNALLAB_MARIADB_TEST_HOST", "127.0.0.1").toStdString();
        options_.username = qEnvironmentVariable("SIGNALLAB_MARIADB_TEST_USER", "root").toStdString();
        options_.port = qEnvironmentVariableIntValue("SIGNALLAB_MARIADB_TEST_PORT");
        if (options_.port == 0) {
            options_.port = 3306;
        }
        options_.databaseName = "signallab_backup_test_"
            + QUuid::createUuid().toString(QUuid::Id128).toStdString();
        ASSERT_TRUE(staging_.isValid());
        const auto connected = repository_.testConnection(options_);
        ASSERT_TRUE(connected) << connected.error().message;
        TestConnection connection(options_);
        ASSERT_TRUE(connection.isOpen());
        const auto existing = connection.scalar(
            "SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = ?",
            {{options_.databaseName}});
        ASSERT_TRUE(existing.has_value());
        ASSERT_EQ(*existing, 0U);
        cleanupRequired_ = true;
    }

    void TearDown() override
    {
        if (!cleanupRequired_) {
            return;
        }
        ASSERT_TRUE(options_.databaseName.starts_with("signallab_backup_test_"));
        ASSERT_TRUE(std::all_of(options_.databaseName.begin(), options_.databaseName.end(),
            [](unsigned char character) {
                return (character >= 'a' && character <= 'z')
                    || (character >= '0' && character <= '9') || character == '_';
            }));
        TestConnection connection(options_);
        ASSERT_TRUE(connection.isOpen());
        ASSERT_TRUE(connection.execute("DROP DATABASE IF EXISTS `" + options_.databaseName + "`"));
    }

    repository::BackupFile makeFile(const std::string& relativePath, const QByteArray& payload)
    {
        const auto path = std::filesystem::path(staging_.path().toStdWString()) / relativePath;
        std::filesystem::create_directories(path.parent_path());
        QFile file(QString::fromStdWString(path.wstring()));
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        EXPECT_EQ(file.write(payload), payload.size());
        file.close();
        return {relativePath, path, static_cast<std::uint64_t>(payload.size()),
            QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex().toStdString()};
    }

    repository::BackupRecord makeRecord(const std::vector<repository::BackupFile>& files)
    {
        repository::BackupRecord record;
        record.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        record.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
        record.label = "麦克风录音备份 🎙";
        record.sourceName = "SignalLab";
        record.fileCount = files.size();
        for (const auto& file : files) {
            record.totalBytes += file.byteSize;
        }
        return record;
    }

    bool mutate(const std::string& sql)
    {
        TestConnection connection(options_);
        return connection.selectDatabase(options_.databaseName) && connection.execute(sql);
    }

    repository::BackupConnectionOptions options_;
    MariaDbBackupRepository repository_;
    QTemporaryDir staging_;
    bool cleanupRequired_{false};
};

TEST_F(MariaDbBackupRepositoryTest, TestConnectionDoesNotCreateDatabaseAndListIsEmpty)
{
    const auto records = repository_.list(options_);
    ASSERT_TRUE(records) << records.error().message;
    EXPECT_TRUE(records.value().empty());
    TestConnection connection(options_);
    ASSERT_TRUE(connection.isOpen());
    const auto count = connection.scalar(
        "SELECT COUNT(*) FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = ?",
        {{options_.databaseName}});
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(*count, 0U);
}

TEST_F(MariaDbBackupRepositoryTest, RoundTripsMultipleChunksUnicodeAndEmptyFiles)
{
    QByteArray largePayload(700'001, Qt::Uninitialized);
    for (qsizetype index = 0; index < largePayload.size(); ++index) {
        largePayload[index] = static_cast<char>(index % 251);
    }
    const std::vector<repository::BackupFile> files{
        makeFile("data/signallab.sqlite3", largePayload),
        makeFile("data/imports/" + std::string(64, 'a') + ".source", {}),
        makeFile("data/imports/" + std::string(64, 'b') + ".csv", "time,value\n0,1\n"),
        makeFile("data/imports/" + std::string(64, 'c') + ".wav", "legacy-wav-backup-content")
    };
    const auto record = makeRecord(files);
    std::uint64_t uploadedBytes = 0;
    const auto uploaded = repository_.upload(options_, record, files, {},
        [&](std::uint64_t completed, std::uint64_t total) {
            EXPECT_GE(completed, uploadedBytes);
            EXPECT_EQ(total, record.totalBytes);
            uploadedBytes = completed;
        });
    ASSERT_TRUE(uploaded) << uploaded.error().message;
    EXPECT_EQ(uploadedBytes, record.totalBytes);
    const auto records = repository_.list(options_);
    ASSERT_TRUE(records) << records.error().message;
    ASSERT_EQ(records.value().size(), 1U);
    EXPECT_EQ(records.value().front().id, record.id);
    EXPECT_EQ(records.value().front().label, record.label);
    EXPECT_EQ(records.value().front().totalBytes, record.totalBytes);
    QTemporaryDir destination;
    ASSERT_TRUE(destination.isValid());
    const auto downloaded = repository_.download(options_, record.id,
        std::filesystem::path(destination.path().toStdWString()));
    ASSERT_TRUE(downloaded) << downloaded.error().message;
    ASSERT_EQ(downloaded.value().size(), files.size());
    QFile restored(QString::fromStdWString(downloaded.value().front().localPath.wstring()));
    ASSERT_TRUE(restored.open(QIODevice::ReadOnly));
    EXPECT_EQ(restored.readAll(), largePayload);
    EXPECT_EQ(downloaded.value()[1].byteSize, 0U);
    const auto duplicate = repository_.upload(options_, record, files);
    EXPECT_FALSE(duplicate);
    const auto afterDuplicate = repository_.list(options_);
    ASSERT_TRUE(afterDuplicate);
    EXPECT_EQ(afterDuplicate.value().size(), 1U);
}

TEST_F(MariaDbBackupRepositoryTest, AuthenticationErrorDoesNotExposePassword)
{
    auto wrongOptions = options_;
    wrongOptions.password = "invalid-test-secret-" + QUuid::createUuid().toString(QUuid::Id128).toStdString();
    const auto result = repository_.testConnection(wrongOptions);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().message.find(wrongOptions.password), std::string::npos);
    EXPECT_EQ(result.error().message.find(options_.password), std::string::npos);
    EXPECT_NE(result.error().message.find("Authentication failed"), std::string::npos);
}

TEST_F(MariaDbBackupRepositoryTest, CancellationRollsBackAllBackupRows)
{
    const std::vector<repository::BackupFile> files{
        makeFile("data/signallab.sqlite3", QByteArray(900'000, 'c'))
    };
    std::stop_source stopSource;
    const auto result = repository_.upload(options_, makeRecord(files), files, stopSource.get_token(),
        [&](std::uint64_t completed, std::uint64_t) {
            if (completed > 0) {
                stopSource.request_stop();
            }
        });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::Cancelled);
    const auto records = repository_.list(options_);
    ASSERT_TRUE(records) << records.error().message;
    EXPECT_TRUE(records.value().empty());
    TestConnection connection(options_);
    ASSERT_TRUE(connection.isOpen());
    ASSERT_TRUE(connection.selectDatabase(options_.databaseName));
    const auto count = connection.scalar(
        "SELECT (SELECT COUNT(*) FROM signallab_backup_files) + (SELECT COUNT(*) FROM signallab_backup_chunks)");
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(*count, 0U);
}

TEST_F(MariaDbBackupRepositoryTest, CancelsNativeDownloadAndCanRestoreAgain)
{
    const std::vector<repository::BackupFile> files{
        makeFile("data/signallab.sqlite3", QByteArray(900'000, 'c'))
    };
    const auto record = makeRecord(files);
    ASSERT_TRUE(repository_.upload(options_, record, files));
    QTemporaryDir cancelledDestination;
    ASSERT_TRUE(cancelledDestination.isValid());
    std::stop_source stopSource;
    const auto cancelled = repository_.download(options_, record.id,
        std::filesystem::path(cancelledDestination.path().toStdWString()), stopSource.get_token(),
        [&](std::uint64_t completed, std::uint64_t) {
            if (completed > 0) {
                stopSource.request_stop();
            }
        });
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, core::ErrorCode::Cancelled);
    QTemporaryDir destination;
    ASSERT_TRUE(destination.isValid());
    const auto restored = repository_.download(options_, record.id,
        std::filesystem::path(destination.path().toStdWString()));
    ASSERT_TRUE(restored) << restored.error().message;
    ASSERT_EQ(restored.value().size(), files.size());
    EXPECT_EQ(restored.value().front().sha256, files.front().sha256);
    EXPECT_EQ(std::filesystem::file_size(restored.value().front().localPath), files.front().byteSize);
}

TEST_F(MariaDbBackupRepositoryTest, RejectsCorruptChunkAndChangedSourceHash)
{
    std::vector<repository::BackupFile> files{
        makeFile("data/signallab.sqlite3", QByteArray(400'000, 'd'))
    };
    const auto record = makeRecord(files);
    ASSERT_TRUE(repository_.upload(options_, record, files));
    ASSERT_TRUE(mutate("UPDATE signallab_backup_chunks SET payload = REPEAT('x', OCTET_LENGTH(payload)) WHERE chunk_index = 0"));
    QTemporaryDir destination;
    ASSERT_TRUE(destination.isValid());
    const auto downloaded = repository_.download(options_, record.id,
        std::filesystem::path(destination.path().toStdWString()));
    ASSERT_FALSE(downloaded);
    EXPECT_NE(downloaded.error().message.find("SHA-256"), std::string::npos);
    files.front().sha256 = std::string(64, '0');
    const auto mismatched = repository_.upload(options_, makeRecord(files), files);
    ASSERT_FALSE(mismatched);
    const auto records = repository_.list(options_);
    ASSERT_TRUE(records);
    EXPECT_EQ(records.value().size(), 1U);
}

TEST_F(MariaDbBackupRepositoryTest, RejectsManifestTamperingAndNonemptyDestination)
{
    const std::vector<repository::BackupFile> files{
        makeFile("data/signallab.sqlite3", QByteArray("database"))
    };
    const auto record = makeRecord(files);
    ASSERT_TRUE(repository_.upload(options_, record, files));
    const auto existingDestination = repository_.download(options_, record.id,
        std::filesystem::path(staging_.path().toStdWString()));
    ASSERT_FALSE(existingDestination);
    ASSERT_TRUE(mutate("UPDATE signallab_backups SET manifest_json = '{}'"));
    QTemporaryDir destination;
    ASSERT_TRUE(destination.isValid());
    const auto result = repository_.download(options_, record.id,
        std::filesystem::path(destination.path().toStdWString()));
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("manifest"), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_empty(std::filesystem::path(destination.path().toStdWString())));
}

TEST_F(MariaDbBackupRepositoryTest, BoundUnicodeLabelsAndBinaryChunksSurviveARejectedDuplicate)
{
    constexpr qsizetype chunkBytes = 256 * 1024;
    QByteArray payload(2 * chunkBytes + 17, '\0');
    for (qsizetype index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>(index % 256);
    }
    payload[0] = '\0';
    payload[chunkBytes - 1] = '\'';
    payload[chunkBytes] = '\0';
    payload[chunkBytes + 1] = '\\';
    payload[payload.size() - 1] = '\0';
    const std::vector<repository::BackupFile> files{
        makeFile("data/signallab.sqlite3", payload)
    };
    auto record = makeRecord(files);
    record.label = "O'Reilly \\ \"中文录音\" 🎙🙂 '); DROP TABLE signallab_backups; --";
    record.sourceName = "SignalLab\\原始数据's 🎼";
    const auto uploaded = repository_.upload(options_, record, files);
    ASSERT_TRUE(uploaded) << uploaded.error().message;

    const std::vector<repository::BackupFile> replacementFiles{
        makeFile("data/signallab.sqlite3", QByteArray("replacement must never be published"))
    };
    auto replacement = makeRecord(replacementFiles);
    replacement.id = record.id;
    replacement.label = "duplicate label must not replace the existing record";
    EXPECT_FALSE(repository_.upload(options_, replacement, replacementFiles));

    const auto records = repository_.list(options_);
    ASSERT_TRUE(records) << records.error().message;
    ASSERT_EQ(records.value().size(), 1U);
    EXPECT_EQ(records.value().front().label, record.label);
    EXPECT_EQ(records.value().front().sourceName, record.sourceName);
    EXPECT_EQ(records.value().front().totalBytes, record.totalBytes);
    QTemporaryDir destination;
    ASSERT_TRUE(destination.isValid());
    const auto downloaded = repository_.download(options_, record.id,
        std::filesystem::path(destination.path().toStdWString()));
    ASSERT_TRUE(downloaded) << downloaded.error().message;
    ASSERT_EQ(downloaded.value().size(), 1U);
    EXPECT_EQ(downloaded.value().front().sha256, files.front().sha256);
    QFile restored(QString::fromStdWString(downloaded.value().front().localPath.wstring()));
    ASSERT_TRUE(restored.open(QIODevice::ReadOnly));
    EXPECT_EQ(restored.readAll(), payload);
}

TEST_F(MariaDbBackupRepositoryTest, ReadsLegacySchemaRecordsAndPreservesThemOnNewUpload)
{
    TestConnection connection(options_);
    ASSERT_TRUE(connection.isOpen());
    ASSERT_TRUE(connection.execute("CREATE DATABASE `" + options_.databaseName
        + "` CHARACTER SET utf8mb4 COLLATE utf8mb4_bin"));
    ASSERT_TRUE(connection.selectDatabase(options_.databaseName));
    const std::array<std::string, 3> legacySchema{
        "CREATE TABLE signallab_backups ("
        "id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY, "
        "created_utc VARCHAR(40) NOT NULL, label VARCHAR(255) NOT NULL, source_name VARCHAR(255) NOT NULL, "
        "total_bytes BIGINT UNSIGNED NOT NULL, file_count INT UNSIGNED NOT NULL, "
        "manifest_json MEDIUMTEXT NOT NULL, manifest_sha256 CHAR(64) CHARACTER SET ascii NOT NULL, "
        "INDEX signallab_backup_created (created_utc)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin",
        "CREATE TABLE signallab_backup_files ("
        "backup_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, file_index INT UNSIGNED NOT NULL, "
        "relative_path VARCHAR(100) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, "
        "byte_size BIGINT UNSIGNED NOT NULL, sha256 CHAR(64) CHARACTER SET ascii NOT NULL, "
        "chunk_count INT UNSIGNED NOT NULL, PRIMARY KEY (backup_id, file_index), "
        "UNIQUE KEY signallab_backup_path (backup_id, relative_path), "
        "FOREIGN KEY (backup_id) REFERENCES signallab_backups(id)) ENGINE=InnoDB",
        "CREATE TABLE signallab_backup_chunks ("
        "backup_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, "
        "file_index INT UNSIGNED NOT NULL, chunk_index INT UNSIGNED NOT NULL, payload MEDIUMBLOB NOT NULL, "
        "PRIMARY KEY (backup_id, file_index, chunk_index), "
        "FOREIGN KEY (backup_id, file_index) REFERENCES signallab_backup_files(backup_id, file_index)) ENGINE=InnoDB"
    };
    for (const auto& statement : legacySchema) {
        ASSERT_TRUE(connection.execute(statement));
    }
    const QByteArray payload("legacy\0database", 15);
    const std::vector<repository::BackupFile> files{
        makeFile("data/signallab.sqlite3", payload)
    };
    auto record = makeRecord(files);
    record.label = "旧版 Qt SQL 备份 🎙";
    const QJsonArray entries{QJsonObject{
        {"path", QString::fromStdString(files.front().relativePath)},
        {"bytes", QString::number(files.front().byteSize)},
        {"sha256", QString::fromStdString(files.front().sha256)}
    }};
    const QByteArray manifest = QJsonDocument(QJsonObject{
        {"version", 1}, {"id", QString::fromStdString(record.id)}, {"files", entries}
    }).toJson(QJsonDocument::Compact);
    ASSERT_TRUE(connection.executePrepared(
        "INSERT INTO signallab_backups "
        "(id, created_utc, label, source_name, total_bytes, file_count, manifest_json, manifest_sha256) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        {{record.id}, {record.createdUtc}, {record.label}, {record.sourceName},
         {std::to_string(record.totalBytes)}, {"1"}, {manifest.toStdString()},
         {QCryptographicHash::hash(manifest, QCryptographicHash::Sha256).toHex().toStdString()}}));
    ASSERT_TRUE(connection.executePrepared(
        "INSERT INTO signallab_backup_files "
        "(backup_id, file_index, relative_path, byte_size, sha256, chunk_count) VALUES (?, ?, ?, ?, ?, ?)",
        {{record.id}, {"0"}, {files.front().relativePath}, {std::to_string(files.front().byteSize)},
         {files.front().sha256}, {"1"}}));
    ASSERT_TRUE(connection.executePrepared(
        "INSERT INTO signallab_backup_chunks (backup_id, file_index, chunk_index, payload) VALUES (?, ?, ?, ?)",
        {{record.id}, {"0"}, {"0"}, {payload.toStdString(), MYSQL_TYPE_BLOB}}));

    const auto records = repository_.list(options_);
    ASSERT_TRUE(records) << records.error().message;
    ASSERT_EQ(records.value().size(), 1U);
    EXPECT_EQ(records.value().front().id, record.id);
    EXPECT_EQ(records.value().front().label, record.label);
    QTemporaryDir destination;
    ASSERT_TRUE(destination.isValid());
    const auto downloaded = repository_.download(options_, record.id,
        std::filesystem::path(destination.path().toStdWString()));
    ASSERT_TRUE(downloaded) << downloaded.error().message;
    ASSERT_EQ(downloaded.value().size(), 1U);
    QFile restored(QString::fromStdWString(downloaded.value().front().localPath.wstring()));
    ASSERT_TRUE(restored.open(QIODevice::ReadOnly));
    EXPECT_EQ(restored.readAll(), payload);

    const auto nextRecord = makeRecord(files);
    const auto uploaded = repository_.upload(options_, nextRecord, files);
    ASSERT_TRUE(uploaded) << uploaded.error().message;
    const auto afterUpload = repository_.list(options_);
    ASSERT_TRUE(afterUpload) << afterUpload.error().message;
    ASSERT_EQ(afterUpload.value().size(), 2U);
    EXPECT_TRUE(std::any_of(afterUpload.value().begin(), afterUpload.value().end(),
        [&record](const auto& existing) { return existing.id == record.id && existing.label == record.label; }));
}

TEST_F(MariaDbBackupRepositoryTest, RepeatedNativeConnectionsWorkAcrossConcurrentWorkers)
{
    const QByteArray payload("parallel\0connection payload", 27);
    const std::vector<repository::BackupFile> files{
        makeFile("data/signallab.sqlite3", payload)
    };
    const auto initial = repository_.upload(options_, makeRecord(files), files);
    ASSERT_TRUE(initial) << initial.error().message;
    constexpr std::size_t workerCount = 3;
    constexpr std::size_t iterations = 2;
    std::barrier startGate(static_cast<std::ptrdiff_t>(workerCount));
    std::vector<std::future<core::Result<bool>>> workers;
    for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        std::vector<repository::BackupRecord> workerRecords;
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            auto record = makeRecord(files);
            record.label = "parallel worker " + std::to_string(workerIndex) + " iteration " + std::to_string(iteration);
            workerRecords.push_back(std::move(record));
        }
        workers.push_back(std::async(std::launch::async,
            [this, &startGate, files, payload, workerRecords = std::move(workerRecords)]() {
                startGate.arrive_and_wait();
                for (const auto& record : workerRecords) {
                    const auto connection = repository_.testConnection(options_);
                    if (!connection) {
                        return core::Result<bool>::failure(connection.error());
                    }
                    const auto uploaded = repository_.upload(options_, record, files);
                    if (!uploaded) {
                        return core::Result<bool>::failure(uploaded.error());
                    }
                    const auto listed = repository_.list(options_);
                    if (!listed) {
                        return core::Result<bool>::failure(listed.error());
                    }
                    if (std::none_of(listed.value().begin(), listed.value().end(),
                            [&record](const auto& existing) { return existing.id == record.id; })) {
                        return core::Result<bool>::failure(core::ErrorCode::DatabaseQueryFailed,
                            "A concurrent worker could not find its committed backup.");
                    }
                    QTemporaryDir destination;
                    if (!destination.isValid()) {
                        return core::Result<bool>::failure(core::ErrorCode::FileWriteFailed,
                            "Cannot create the concurrent test restore directory.");
                    }
                    const auto downloaded = repository_.download(options_, record.id,
                        std::filesystem::path(destination.path().toStdWString()));
                    if (!downloaded) {
                        return core::Result<bool>::failure(downloaded.error());
                    }
                    if (downloaded.value().size() != 1U) {
                        return core::Result<bool>::failure(core::ErrorCode::InvalidParameter,
                            "A concurrent backup returned an unexpected file count.");
                    }
                    QFile restored(QString::fromStdWString(downloaded.value().front().localPath.wstring()));
                    if (!restored.open(QIODevice::ReadOnly) || restored.readAll() != payload) {
                        return core::Result<bool>::failure(core::ErrorCode::InvalidParameter,
                            "The concurrent backup round-trip changed the binary payload.");
                    }
                }
                return core::Result<bool>::success(true);
            }));
    }
    for (auto& worker : workers) {
        const auto completed = worker.get();
        EXPECT_TRUE(completed) << completed.error().message;
    }
    const auto records = repository_.list(options_);
    ASSERT_TRUE(records) << records.error().message;
    EXPECT_EQ(records.value().size(), 1U + workerCount * iterations);
    std::set<std::string> identities;
    for (const auto& record : records.value()) {
        EXPECT_TRUE(identities.insert(record.id).second);
    }
}

TEST_F(MariaDbBackupRepositoryTest, RejectsAnOversizedNativeChunkBeforeWritingTruncatedData)
{
    const std::vector<repository::BackupFile> files{
        makeFile("data/signallab.sqlite3", QByteArray(400'000, 'd'))
    };
    const auto record = makeRecord(files);
    const auto uploaded = repository_.upload(options_, record, files);
    ASSERT_TRUE(uploaded) << uploaded.error().message;
    ASSERT_TRUE(mutate("UPDATE signallab_backup_chunks SET payload = REPEAT('x', 262145) WHERE chunk_index = 0"));
    QTemporaryDir destination;
    ASSERT_TRUE(destination.isValid());
    const auto destinationPath = std::filesystem::path(destination.path().toStdWString());
    const auto downloaded = repository_.download(options_, record.id, destinationPath);
    ASSERT_FALSE(downloaded);
    EXPECT_EQ(downloaded.error().code, core::ErrorCode::InvalidParameter);
    const auto stagedFile = destinationPath / "data" / "signallab.sqlite3";
    if (std::filesystem::exists(stagedFile)) {
        EXPECT_EQ(std::filesystem::file_size(stagedFile), 0U);
    }
}

TEST(MariaDbBackupValidationTest, RejectsUnsafeDatabaseIdentifiersWithoutConnecting)
{
    MariaDbBackupRepository repository;
    repository::BackupConnectionOptions options;
    for (const auto& databaseName : {"backup`; DROP DATABASE important; --", "backup\n", "mysql", "SYS"}) {
        options.databaseName = databaseName;
        const auto result = repository.testConnection(options);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
    }
}

TEST(MariaDbBackupValidationTest, RejectsUnsafeManifestPathBeforeConnecting)
{
    MariaDbBackupRepository repository;
    repository::BackupRecord record;
    record.totalBytes = 2;
    for (const auto& path : {std::string("data/../../private.txt"), "data/imports/" + std::string(64, 'b') + ".source\n"}) {
        const std::vector<repository::BackupFile> files{
            {"data/signallab.sqlite3", {}, 1, std::string(64, 'a')},
            {path, {}, 1, std::string(64, 'b')}
        };
        const auto result = repository.upload({}, record, files);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
    }
}

}
}
