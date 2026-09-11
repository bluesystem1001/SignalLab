#include "signallab/database/sqlite_signal_repository.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QMetaType>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <bit>
#include <cstddef>
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

constexpr const char* selectStoredSignalSql =
    "SELECT sr.id, ss.id, ss.source_type, ss.source_key, ss.source_path, "
    "COALESCE(fv.stored_relative_path, ''), "
    "COALESCE(fv.content_hash, ''), sr.sample_hash, ss.parameters_json, "
    "sr.sample_rate, sr.sample_count, sr.sample_blob, sr.signal_name, "
    "sr.signal_unit, sr.signal_source_label "
    "FROM signal_record sr "
    "JOIN signal_source ss ON ss.id = sr.source_id "
    "LEFT JOIN file_version fv ON fv.id = ss.file_version_id ";

QString sourceTypeText(repository::SignalSourceType type)
{
    switch (type) {
    case repository::SignalSourceType::Generated:
        return "GENERATED";
    case repository::SignalSourceType::Csv:
        return "CSV";
    case repository::SignalSourceType::Wav:
        return "WAV";
    }
    return {};
}

std::optional<repository::SignalSourceType> sourceTypeFromText(
    const QString& text)
{
    if (text == "GENERATED") {
        return repository::SignalSourceType::Generated;
    }
    if (text == "CSV") {
        return repository::SignalSourceType::Csv;
    }
    if (text == "WAV") {
        return repository::SignalSourceType::Wav;
    }
    return std::nullopt;
}

QString pathText(const std::filesystem::path& path)
{
    return QString::fromStdWString(path.generic_wstring());
}

std::filesystem::path pathFromText(const QString& text)
{
    return std::filesystem::path(text.toStdWString());
}

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

core::Result<QByteArray> encodeSamples(std::span<const double> samples)
{
    constexpr std::size_t bytesPerSample = sizeof(double);
    const auto largestByteArray =
        static_cast<std::size_t>((std::numeric_limits<qsizetype>::max)());
    if (samples.size() > largestByteArray / bytesPerSample) {
        return core::Result<QByteArray>::failure(
            core::ErrorCode::InvalidParameter,
            "The signal is too large for one SQLite BLOB.");
    }

    QByteArray bytes;
    bytes.reserve(static_cast<qsizetype>(samples.size() * bytesPerSample));
    for (double sample : samples) {
        const std::uint64_t bits = std::bit_cast<std::uint64_t>(sample);
        for (unsigned int shift = 0; shift < 64; shift += 8) {
            bytes.append(static_cast<char>((bits >> shift) & 0xffU));
        }
    }
    return core::Result<QByteArray>::success(std::move(bytes));
}

std::string hashSignalInput(
    double sampleRate,
    const QByteArray& sampleBytes)
{
    QByteArray sampleRateBytes;
    sampleRateBytes.reserve(static_cast<qsizetype>(sizeof(double)));
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(sampleRate);
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        sampleRateBytes.append(
            static_cast<char>((bits >> shift) & 0xffU));
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(sampleRateBytes);
    hash.addData(sampleBytes);
    return hash.result().toHex().toStdString();
}

core::Result<std::vector<double>> decodeSamples(
    const QByteArray& bytes,
    std::int64_t sampleCount)
{
    if (sampleCount <= 0) {
        return core::Result<std::vector<double>>::failure(
            core::ErrorCode::DatabaseQueryFailed,
            "A stored signal has an invalid sample count.");
    }

    constexpr std::int64_t bytesPerSample =
        static_cast<std::int64_t>(sizeof(double));
    if (sampleCount
        > (std::numeric_limits<std::int64_t>::max)() / bytesPerSample
        || sampleCount * bytesPerSample != bytes.size()) {
        return core::Result<std::vector<double>>::failure(
            core::ErrorCode::DatabaseQueryFailed,
            "A stored signal BLOB size does not match its sample count.");
    }

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(sampleCount));
    for (std::int64_t index = 0; index < sampleCount; ++index) {
        std::uint64_t bits = 0;
        for (unsigned int byteIndex = 0; byteIndex < 8; ++byteIndex) {
            const auto offset = static_cast<qsizetype>(
                index * bytesPerSample + byteIndex);
            const auto byte = static_cast<unsigned char>(bytes.at(offset));
            bits |= static_cast<std::uint64_t>(byte) << (byteIndex * 8U);
        }
        samples.push_back(std::bit_cast<double>(bits));
    }
    return core::Result<std::vector<double>>::success(std::move(samples));
}

core::Result<repository::StoredSignal> storedSignalFromRow(
    const QSqlQuery& query)
{
    const auto sourceType = sourceTypeFromText(query.value(2).toString());
    if (!sourceType.has_value()) {
        return core::Result<repository::StoredSignal>::failure(
            core::ErrorCode::DatabaseQueryFailed,
            "A stored signal has an unknown source type.");
    }

    const QByteArray blob = query.value(11).toByteArray();
    const std::string storedHash = query.value(7).toString().toStdString();
    const double sampleRate = query.value(9).toDouble();
    if (hashSignalInput(sampleRate, blob) != storedHash) {
        return core::Result<repository::StoredSignal>::failure(
            core::ErrorCode::DatabaseQueryFailed,
            "A stored signal failed its SHA-256 integrity check.");
    }

    const auto samples = decodeSamples(blob, query.value(10).toLongLong());
    if (!samples) {
        return core::Result<repository::StoredSignal>::failure(
            samples.error());
    }

    core::SignalMetadata metadata{
        query.value(12).toString().toStdString(),
        query.value(13).toString().toStdString(),
        query.value(14).toString().toStdString()};
    auto signal = core::Signal::create(
        samples.value(), sampleRate, std::move(metadata));
    if (!signal) {
        return core::Result<repository::StoredSignal>::failure(
            core::ErrorCode::DatabaseQueryFailed,
            "A stored signal contains invalid signal data: "
                + signal.error().message);
    }

    repository::StoredSignal stored{
        query.value(0).toLongLong(),
        query.value(1).toLongLong(),
        *sourceType,
        query.value(3).toString().toStdString(),
        pathFromText(query.value(4).toString()),
        pathFromText(query.value(5).toString()),
        query.value(6).toString().toStdString(),
        storedHash,
        query.value(8).toString().toStdString(),
        std::move(signal).value()};
    return core::Result<repository::StoredSignal>::success(
        std::move(stored));
}

core::Result<bool> rollbackFailure(
    QSqlDatabase& database,
    const core::Error& error)
{
    database.rollback();
    return core::Result<bool>::failure(error);
}

}  // namespace

SqliteSignalRepository::SqliteSignalRepository(
    DatabaseConnection& connection)
    : connection_(connection)
{
}

core::Result<repository::StoredSignal> SqliteSignalRepository::saveSignal(
    repository::SignalRegistration registration)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<repository::StoredSignal>::failure(
            threadCheck.error());
    }
    if (registration.sourceKey.empty()) {
        return core::Result<repository::StoredSignal>::failure(
            core::ErrorCode::InvalidParameter,
            "The source key must not be empty.");
    }
    if (registration.parametersJson.empty()) {
        return core::Result<repository::StoredSignal>::failure(
            core::ErrorCode::InvalidParameter,
            "The parameter JSON must not be empty.");
    }
    const QString sourceType = sourceTypeText(registration.sourceType);
    if (sourceType.isEmpty()) {
        return core::Result<repository::StoredSignal>::failure(
            core::ErrorCode::InvalidParameter,
            "The signal source type is invalid.");
    }

    const bool generatedSource =
        registration.sourceType == repository::SignalSourceType::Generated;
    if (generatedSource && registration.fileVersion.has_value()) {
        return core::Result<repository::StoredSignal>::failure(
            core::ErrorCode::InvalidParameter,
            "A generated signal must not have an imported file version.");
    }
    if (!generatedSource
        && (!registration.fileVersion.has_value()
            || registration.sourcePath.empty())) {
        return core::Result<repository::StoredSignal>::failure(
            core::ErrorCode::InvalidParameter,
            "A CSV or WAV signal requires its source path and file version.");
    }

    if (registration.fileVersion.has_value()) {
        const auto& file = *registration.fileVersion;
        if (!isSha256(file.contentHash)
            || file.storedRelativePath.empty()
            || file.byteSize
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int64_t>::max)())) {
            return core::Result<repository::StoredSignal>::failure(
                core::ErrorCode::InvalidParameter,
                "The file version registration is invalid.");
        }
    }

    const auto encoded = encodeSamples(registration.signal.samples());
    if (!encoded) {
        return core::Result<repository::StoredSignal>::failure(
            encoded.error());
    }
    const QByteArray sampleBlob = encoded.value();
    const std::string sampleHash = hashSignalInput(
        registration.signal.sampleRate(),
        sampleBlob);

    QSqlDatabase database = connection_.database();
    if (!database.transaction()) {
        return core::Result<repository::StoredSignal>::failure(
            core::ErrorCode::DatabaseQueryFailed,
            "Cannot begin the signal save transaction: "
                + database.lastError().text().toStdString());
    }

    std::optional<std::int64_t> fileVersionId;
    std::filesystem::path storedRelativePath;
    std::string fileHash;
    if (registration.fileVersion.has_value()) {
        const auto& file = *registration.fileVersion;
        QSqlQuery insertFile(database);
        if (!insertFile.prepare(
                "INSERT INTO file_version("
                "content_hash, stored_relative_path, original_name, "
                "byte_size, created_utc) VALUES (?, ?, ?, ?, ?) "
                "ON CONFLICT(content_hash) DO NOTHING")) {
            const auto failure = detail::queryFailure(
                "Cannot prepare the file version insert", insertFile);
            rollbackFailure(database, failure.error());
            return core::Result<repository::StoredSignal>::failure(
                failure.error());
        }
        insertFile.addBindValue(QString::fromStdString(file.contentHash));
        insertFile.addBindValue(pathText(file.storedRelativePath));
        insertFile.addBindValue(QString::fromStdString(file.originalName));
        insertFile.addBindValue(static_cast<qint64>(file.byteSize));
        insertFile.addBindValue(detail::utcNow());
        if (!insertFile.exec()) {
            const auto failure = detail::queryFailure(
                "Cannot insert the file version", insertFile);
            rollbackFailure(database, failure.error());
            return core::Result<repository::StoredSignal>::failure(
                failure.error());
        }

        QSqlQuery findFile(database);
        if (!findFile.prepare(
                "SELECT id, stored_relative_path, content_hash "
                "FROM file_version WHERE content_hash = ?")) {
            const auto failure = detail::queryFailure(
                "Cannot prepare the file version lookup", findFile);
            rollbackFailure(database, failure.error());
            return core::Result<repository::StoredSignal>::failure(
                failure.error());
        }
        findFile.addBindValue(QString::fromStdString(file.contentHash));
        if (!findFile.exec() || !findFile.next()) {
            const auto failure = detail::queryFailure(
                "Cannot load the file version after insertion", findFile);
            rollbackFailure(database, failure.error());
            return core::Result<repository::StoredSignal>::failure(
                failure.error());
        }
        fileVersionId = findFile.value(0).toLongLong();
        storedRelativePath = pathFromText(findFile.value(1).toString());
        fileHash = findFile.value(2).toString().toStdString();
    }

    QSqlQuery insertSource(database);
    if (!insertSource.prepare(
            "INSERT INTO signal_source("
            "source_type, source_key, source_path, parameters_json, "
            "file_version_id, created_utc) VALUES (?, ?, ?, ?, ?, ?)")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the signal source insert", insertSource);
        rollbackFailure(database, failure.error());
        return core::Result<repository::StoredSignal>::failure(
            failure.error());
    }
    insertSource.addBindValue(sourceType);
    insertSource.addBindValue(QString::fromStdString(registration.sourceKey));
    insertSource.addBindValue(pathText(registration.sourcePath));
    insertSource.addBindValue(
        QString::fromStdString(registration.parametersJson));
    if (fileVersionId.has_value()) {
        insertSource.addBindValue(*fileVersionId);
    } else {
        insertSource.addBindValue(QVariant(QMetaType::fromType<qint64>()));
    }
    insertSource.addBindValue(detail::utcNow());
    if (!insertSource.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot insert the signal source", insertSource);
        rollbackFailure(database, failure.error());
        return core::Result<repository::StoredSignal>::failure(
            failure.error());
    }
    const std::int64_t sourceId =
        insertSource.lastInsertId().toLongLong();

    const auto& metadata = registration.signal.metadata();
    QSqlQuery insertRecord(database);
    if (!insertRecord.prepare(
            "INSERT INTO signal_record("
            "source_id, sample_rate, sample_count, sample_blob, sample_hash, "
            "signal_name, signal_unit, signal_source_label, created_utc"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the signal record insert", insertRecord);
        rollbackFailure(database, failure.error());
        return core::Result<repository::StoredSignal>::failure(
            failure.error());
    }
    insertRecord.addBindValue(sourceId);
    insertRecord.addBindValue(registration.signal.sampleRate());
    insertRecord.addBindValue(
        static_cast<qint64>(registration.signal.sampleCount()));
    insertRecord.addBindValue(sampleBlob);
    insertRecord.addBindValue(QString::fromStdString(sampleHash));
    insertRecord.addBindValue(QString::fromStdString(metadata.name));
    insertRecord.addBindValue(QString::fromStdString(metadata.unit));
    insertRecord.addBindValue(QString::fromStdString(metadata.source));
    insertRecord.addBindValue(detail::utcNow());
    if (!insertRecord.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot insert the signal record", insertRecord);
        rollbackFailure(database, failure.error());
        return core::Result<repository::StoredSignal>::failure(
            failure.error());
    }
    const std::int64_t recordId =
        insertRecord.lastInsertId().toLongLong();

    const auto log = detail::writeOperationLog(
        database,
        "SAVE_SIGNAL",
        "signal_record",
        recordId,
        QString::fromStdString(registration.sourceKey));
    if (!log) {
        rollbackFailure(database, log.error());
        return core::Result<repository::StoredSignal>::failure(log.error());
    }

    if (!database.commit()) {
        const core::Error error{
            core::ErrorCode::DatabaseQueryFailed,
            "Cannot commit the signal save transaction: "
                + database.lastError().text().toStdString()};
        rollbackFailure(database, error);
        return core::Result<repository::StoredSignal>::failure(error);
    }

    repository::StoredSignal stored{
        recordId,
        sourceId,
        registration.sourceType,
        std::move(registration.sourceKey),
        std::move(registration.sourcePath),
        std::move(storedRelativePath),
        std::move(fileHash),
        sampleHash,
        std::move(registration.parametersJson),
        std::move(registration.signal)};
    return core::Result<repository::StoredSignal>::success(
        std::move(stored));
}

core::Result<std::optional<repository::StoredSignal>>
SqliteSignalRepository::findBySourceKey(const std::string& sourceKey)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<std::optional<repository::StoredSignal>>::failure(
            threadCheck.error());
    }

    QSqlQuery query(connection_.database());
    const QString sql = QString::fromLatin1(selectStoredSignalSql)
        + "WHERE ss.source_key = ? LIMIT 1";
    if (!query.prepare(sql)) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the signal source lookup", query);
        return core::Result<std::optional<repository::StoredSignal>>::failure(
            failure.error());
    }
    query.addBindValue(QString::fromStdString(sourceKey));
    if (!query.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot find a signal by source key", query);
        return core::Result<std::optional<repository::StoredSignal>>::failure(
            failure.error());
    }
    if (!query.next()) {
        return core::Result<std::optional<repository::StoredSignal>>::success(
            std::nullopt);
    }

    auto stored = storedSignalFromRow(query);
    if (!stored) {
        return core::Result<std::optional<repository::StoredSignal>>::failure(
            stored.error());
    }
    return core::Result<std::optional<repository::StoredSignal>>::success(
        std::move(stored).value());
}

core::Result<std::optional<repository::StoredSignal>>
SqliteSignalRepository::findByRecordId(std::int64_t recordId)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<std::optional<repository::StoredSignal>>::failure(
            threadCheck.error());
    }
    if (recordId <= 0) {
        return core::Result<std::optional<repository::StoredSignal>>::failure(
            core::ErrorCode::InvalidParameter,
            "The signal record id must be positive.");
    }

    QSqlQuery query(connection_.database());
    const QString sql = QString::fromLatin1(selectStoredSignalSql)
        + "WHERE sr.id = ? LIMIT 1";
    if (!query.prepare(sql)) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the signal record lookup", query);
        return core::Result<std::optional<repository::StoredSignal>>::failure(
            failure.error());
    }
    query.addBindValue(recordId);
    if (!query.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot find a signal by record id", query);
        return core::Result<std::optional<repository::StoredSignal>>::failure(
            failure.error());
    }
    if (!query.next()) {
        return core::Result<std::optional<repository::StoredSignal>>::success(
            std::nullopt);
    }

    auto stored = storedSignalFromRow(query);
    if (!stored) {
        return core::Result<std::optional<repository::StoredSignal>>::failure(
            stored.error());
    }
    return core::Result<std::optional<repository::StoredSignal>>::success(
        std::move(stored).value());
}

core::Result<std::optional<repository::StoredSignal>>
SqliteSignalRepository::findByFileHash(const std::string& fileHash)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<std::optional<repository::StoredSignal>>::failure(
            threadCheck.error());
    }

    QSqlQuery query(connection_.database());
    const QString sql = QString::fromLatin1(selectStoredSignalSql)
        + "WHERE fv.content_hash = ? ORDER BY sr.id DESC LIMIT 1";
    if (!query.prepare(sql)) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the file hash lookup", query);
        return core::Result<std::optional<repository::StoredSignal>>::failure(
            failure.error());
    }
    query.addBindValue(QString::fromStdString(fileHash));
    if (!query.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot find a signal by file hash", query);
        return core::Result<std::optional<repository::StoredSignal>>::failure(
            failure.error());
    }
    if (!query.next()) {
        return core::Result<std::optional<repository::StoredSignal>>::success(
            std::nullopt);
    }

    auto stored = storedSignalFromRow(query);
    if (!stored) {
        return core::Result<std::optional<repository::StoredSignal>>::failure(
            stored.error());
    }
    return core::Result<std::optional<repository::StoredSignal>>::success(
        std::move(stored).value());
}

core::Result<std::vector<repository::StoredSignal>>
SqliteSignalRepository::listSignals()
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return core::Result<std::vector<repository::StoredSignal>>::failure(
            threadCheck.error());
    }

    QSqlQuery query(connection_.database());
    const QString sql = QString::fromLatin1(selectStoredSignalSql)
        + "ORDER BY sr.id ASC";
    if (!query.prepare(sql) || !query.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot list stored signals", query);
        return core::Result<std::vector<repository::StoredSignal>>::failure(
            failure.error());
    }

    std::vector<repository::StoredSignal> storedSignals;
    while (query.next()) {
        auto stored = storedSignalFromRow(query);
        if (!stored) {
            return core::Result<std::vector<repository::StoredSignal>>::failure(
                stored.error());
        }
        storedSignals.push_back(std::move(stored).value());
    }
    return core::Result<std::vector<repository::StoredSignal>>::success(
        std::move(storedSignals));
}

core::Result<bool> SqliteSignalRepository::removeSignal(
    std::int64_t recordId)
{
    const auto threadCheck = connection_.checkThread();
    if (!threadCheck) {
        return threadCheck;
    }
    if (recordId <= 0) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "The signal record id must be positive.");
    }

    QSqlDatabase database = connection_.database();
    if (!database.transaction()) {
        return detail::databaseFailure(
            "Cannot begin the signal delete transaction", database);
    }

    QSqlQuery lookup(database);
    if (!lookup.prepare(
            "SELECT ss.id, ss.file_version_id, "
            "COALESCE(fv.stored_relative_path, '') "
            "FROM signal_record sr "
            "JOIN signal_source ss ON ss.id = sr.source_id "
            "LEFT JOIN file_version fv ON fv.id = ss.file_version_id "
            "WHERE sr.id = ?")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the signal delete lookup", lookup);
        return rollbackFailure(database, failure.error());
    }
    lookup.addBindValue(recordId);
    if (!lookup.exec()) {
        const auto failure = detail::queryFailure(
            "Cannot load the signal before deletion", lookup);
        return rollbackFailure(database, failure.error());
    }
    if (!lookup.next()) {
        database.rollback();
        return core::Result<bool>::success(false);
    }
    const std::int64_t sourceId = lookup.value(0).toLongLong();
    const std::optional<std::int64_t> fileVersionId =
        lookup.value(1).isNull()
        ? std::nullopt
        : std::optional<std::int64_t>(lookup.value(1).toLongLong());
    const QString storedPath = lookup.value(2).toString();
    lookup.finish();

    QSqlQuery deleteRecord(database);
    if (!deleteRecord.prepare("DELETE FROM signal_record WHERE id = ?")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the signal record delete", deleteRecord);
        return rollbackFailure(database, failure.error());
    }
    deleteRecord.addBindValue(recordId);
    if (!deleteRecord.exec() || deleteRecord.numRowsAffected() != 1) {
        const auto failure = detail::queryFailure(
            "Cannot delete the signal record", deleteRecord);
        return rollbackFailure(database, failure.error());
    }

    QSqlQuery deleteSource(database);
    if (!deleteSource.prepare("DELETE FROM signal_source WHERE id = ?")) {
        const auto failure = detail::queryFailure(
            "Cannot prepare the signal source delete", deleteSource);
        return rollbackFailure(database, failure.error());
    }
    deleteSource.addBindValue(sourceId);
    if (!deleteSource.exec() || deleteSource.numRowsAffected() != 1) {
        const auto failure = detail::queryFailure(
            "Cannot delete the signal source", deleteSource);
        return rollbackFailure(database, failure.error());
    }

    if (fileVersionId.has_value()) {
        QSqlQuery deleteFile(database);
        if (!deleteFile.prepare(
                "DELETE FROM file_version WHERE id = ? AND NOT EXISTS ("
                "SELECT 1 FROM signal_source WHERE file_version_id = ?"
                ")")) {
            const auto failure = detail::queryFailure(
                "Cannot prepare the unused file version delete", deleteFile);
            return rollbackFailure(database, failure.error());
        }
        deleteFile.addBindValue(*fileVersionId);
        deleteFile.addBindValue(*fileVersionId);
        if (!deleteFile.exec()) {
            const auto failure = detail::queryFailure(
                "Cannot delete the unused file version", deleteFile);
            return rollbackFailure(database, failure.error());
        }
    }

    const QString detailText = storedPath.isEmpty()
        ? QString("database rows removed")
        : QString("database rows removed; archived path to compensate: ")
            + storedPath;
    const auto log = detail::writeOperationLog(
        database, "REMOVE_SIGNAL", "signal_record", recordId, detailText);
    if (!log) {
        return rollbackFailure(database, log.error());
    }

    if (!database.commit()) {
        const auto failure = detail::databaseFailure(
            "Cannot commit the signal delete transaction", database);
        database.rollback();
        return failure;
    }
    return core::Result<bool>::success(true);
}

}  // namespace signallab::database
