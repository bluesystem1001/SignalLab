#include "signallab/app/import_service.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string_view>
#include <utility>

#include "signallab/core/error.hpp"

namespace signallab::app {
namespace {

std::string chooseDisplayName(const repository::StoredSignal& stored)
{
    if (!stored.signal.metadata().name.empty()) {
        return stored.signal.metadata().name;
    }
    if (!stored.sourcePath.empty()) {
        return stored.sourcePath.filename().string();
    }
    return "Generated signal";
}

SignalEntry makeEntry(repository::StoredSignal stored)
{
    const std::string displayName = chooseDisplayName(stored);

    return SignalEntry{
        stored.recordId,
        stored.sourceId,
        stored.sourceType,
        std::move(stored.sourceKey),
        displayName,
        std::move(stored.fileHash),
        std::move(stored.sampleHash),
        std::move(stored.sourcePath),
        std::move(stored.storedRelativePath),
        std::move(stored.parametersJson),
        std::move(stored.signal)
    };
}

std::string escapeJson(std::string_view value)
{
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());

    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (character < 0x20U) {
                escaped += "\\u00";
                escaped += hexadecimal[(character >> 4U) & 0x0fU];
                escaped += hexadecimal[character & 0x0fU];
            } else {
                escaped += static_cast<char>(character);
            }
            break;
        }
    }

    return escaped;
}

std::string jsonString(std::string_view value)
{
    return "\"" + escapeJson(value) + "\"";
}

std::string jsonNumber(double value)
{
    if (!std::isfinite(value)) {
        return "null";
    }
    if (value == 0.0) {
        return "0";
    }

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << value;
    return stream.str();
}

std::string metadataJson(const core::SignalMetadata& metadata)
{
    // Keys are deliberately kept in alphabetical order.
    return "{\"name\":" + jsonString(metadata.name)
        + ",\"source\":" + jsonString(metadata.source)
        + ",\"unit\":" + jsonString(metadata.unit) + "}";
}

std::string csvDelimiterName(repository::CsvDelimiter delimiter)
{
    if (delimiter == repository::CsvDelimiter::Tab) {
        return "tab";
    }
    return "comma";
}

std::string stereoChannelName(repository::StereoChannel channel)
{
    switch (channel) {
    case repository::StereoChannel::Left:
        return "left";
    case repository::StereoChannel::Right:
        return "right";
    case repository::StereoChannel::Average:
        return "average";
    }
    return "unknown";
}

std::string csvParametersJson(const repository::CsvImportOptions& options)
{
    const std::string timeColumn = options.timeColumn.has_value()
        ? std::to_string(*options.timeColumn)
        : "null";

    // Object keys and nested metadata keys are in alphabetical order.
    return "{\"delimiter\":" + jsonString(csvDelimiterName(options.delimiter))
        + ",\"has_header\":"
        + (options.hasHeader ? "true" : "false")
        + ",\"metadata\":" + metadataJson(options.metadata)
        + ",\"sample_rate_if_no_time_column\":"
        + jsonNumber(options.sampleRateIfNoTimeColumn)
        + ",\"time_column\":" + timeColumn
        + ",\"value_column\":" + std::to_string(options.valueColumn)
        + "}";
}

std::string csvLogicalOptionsJson(
    const repository::CsvImportOptions& options)
{
    const std::string timeColumn = options.timeColumn.has_value()
        ? std::to_string(*options.timeColumn)
        : "null";

    return "{\"delimiter\":" + jsonString(csvDelimiterName(options.delimiter))
        + ",\"has_header\":"
        + (options.hasHeader ? "true" : "false")
        + ",\"sample_rate_if_no_time_column\":"
        + jsonNumber(options.sampleRateIfNoTimeColumn)
        + ",\"time_column\":" + timeColumn
        + ",\"value_column\":" + std::to_string(options.valueColumn)
        + "}";
}

std::string wavParametersJson(const repository::WavImportOptions& options)
{
    return "{\"channel\":" + jsonString(stereoChannelName(options.channel))
        + ",\"metadata\":" + metadataJson(options.metadata) + "}";
}

std::string wavLogicalOptionsJson(
    const repository::WavImportOptions& options)
{
    return "{\"channel\":"
        + jsonString(stereoChannelName(options.channel)) + "}";
}

std::string importedSourceKey(
    const repository::FileImport& fileImport,
    std::string_view logicalOptionsJson)
{
    // The same archived file may produce more than one logical signal when
    // the user selects different columns or channels.
    return "file-sha256:" + fileImport.contentHash
        + "|options-json:" + std::string(logicalOptionsJson);
}

core::Error duplicateFileError(
    const std::filesystem::path& sourcePath)
{
    return core::Error{
        core::ErrorCode::DuplicateFile,
        "This file has already been imported: "
            + sourcePath.generic_string()};
}

}  // namespace

ImportService::ImportService(
    repository::FileRepository& fileRepository,
    repository::SignalRepository& signalRepository)
    : fileRepository_(fileRepository),
      signalRepository_(signalRepository)
{
}

core::Result<std::size_t> ImportService::restoreSignals()
{
    auto storedSignalsResult = signalRepository_.listSignals();
    if (!storedSignalsResult) {
        return core::Result<std::size_t>::failure(
            storedSignalsResult.error());
    }

    std::vector<repository::StoredSignal> storedSignals =
        std::move(storedSignalsResult).value();
    std::vector<SignalEntry> restoredEntries;
    restoredEntries.reserve(storedSignals.size());

    for (repository::StoredSignal& stored : storedSignals) {
        if (stored.sourceType != repository::SignalSourceType::Generated) {
            auto validationResult = fileRepository_.validateStoredFile(
                stored.storedRelativePath,
                stored.fileHash);
            if (!validationResult) {
                return core::Result<std::size_t>::failure(
                    validationResult.error());
            }
            if (!validationResult.value()) {
                return core::Result<std::size_t>::failure(
                    core::ErrorCode::FileWriteFailed,
                    "Stored import is missing: "
                        + stored.storedRelativePath.generic_string());
            }
        }

        restoredEntries.push_back(makeEntry(std::move(stored)));
    }

    entries_ = std::move(restoredEntries);
    return core::Result<std::size_t>::success(entries_.size());
}

core::Result<std::size_t> ImportService::addGeneratedSignal(
    core::Signal signal,
    const std::string& stableSourceKey)
{
    auto persistedResult = persistGeneratedSignal(
        std::move(signal), stableSourceKey);
    if (!persistedResult) {
        return core::Result<std::size_t>::failure(
            persistedResult.error());
    }

    const auto accepted = acceptImportedSignal(
        std::move(persistedResult).value());
    if (!accepted.has_value()) {
        return core::Result<std::size_t>::failure(
            core::ErrorCode::InvalidParameter,
            "The generated signal was deleted before it could be accepted.");
    }
    return core::Result<std::size_t>::success(*accepted);
}

core::Result<SignalEntry> ImportService::persistGeneratedSignal(
    core::Signal signal,
    const std::string& stableSourceKey,
    std::string parametersJson,
    std::stop_token stopToken)
{
    if (stableSourceKey.empty()) {
        return core::Result<SignalEntry>::failure(
            core::ErrorCode::InvalidParameter,
            "A generated signal requires a non-empty stable source key.");
    }
    if (parametersJson.empty()) {
        return core::Result<SignalEntry>::failure(
            core::ErrorCode::InvalidParameter,
            "Generated signal parameters must not be empty.");
    }
    if (stopToken.stop_requested()) {
        return core::Result<SignalEntry>::failure(
            core::ErrorCode::Cancelled,
            "Generated signal persistence was cancelled.");
    }

    // The lookup and possible insert form one logical idempotent operation.
    // DatabaseExecutor serializes individual SQL calls, while this lock also
    // prevents two workers from both observing a missing source key between
    // those calls.
    std::lock_guard registrationLock(importMutex_);

    auto existingResult = signalRepository_.findBySourceKey(stableSourceKey);
    if (!existingResult) {
        return core::Result<SignalEntry>::failure(existingResult.error());
    }

    if (existingResult.value().has_value()) {
        if (stopToken.stop_requested()) {
            return core::Result<SignalEntry>::failure(
                core::ErrorCode::Cancelled,
                "Generated signal persistence was cancelled.");
        }
        return core::Result<SignalEntry>::success(
            makeEntry(*existingResult.value()));
    }

    if (stopToken.stop_requested()) {
        return core::Result<SignalEntry>::failure(
            core::ErrorCode::Cancelled,
            "Generated signal persistence was cancelled.");
    }

    repository::SignalRegistration registration{
        repository::SignalSourceType::Generated,
        stableSourceKey,
        {},
        std::move(parametersJson),
        std::nullopt,
        std::move(signal)
    };

    auto savedResult = signalRepository_.saveSignal(std::move(registration));
    if (!savedResult) {
        return core::Result<SignalEntry>::failure(savedResult.error());
    }

    // The database commit is the point of no return. A cancellation arriving
    // after saveSignal() succeeds must not relabel the committed result.
    return core::Result<SignalEntry>::success(
        makeEntry(std::move(savedResult).value()));
}

core::Result<repository::ImportPreview> ImportService::previewCsv(
    const std::filesystem::path& sourcePath,
    const repository::CsvImportOptions& options,
    std::stop_token stopToken) const
{
    return fileRepository_.previewCsv(sourcePath, options, stopToken);
}

core::Result<repository::ImportPreview> ImportService::previewWav(
    const std::filesystem::path& sourcePath,
    std::stop_token stopToken) const
{
    return fileRepository_.previewWav(sourcePath, stopToken);
}

core::Result<std::size_t> ImportService::importCsv(
    const std::filesystem::path& sourcePath,
    const repository::CsvImportOptions& options,
    std::stop_token stopToken)
{
    auto loadResult = loadCsv(sourcePath, options, stopToken);
    if (!loadResult) {
        return core::Result<std::size_t>::failure(loadResult.error());
    }

    const auto accepted = acceptImportedSignal(std::move(loadResult).value());
    if (!accepted.has_value()) {
        return core::Result<std::size_t>::failure(
            core::ErrorCode::InvalidParameter,
            "The imported signal was deleted before it could be accepted.");
    }
    return core::Result<std::size_t>::success(*accepted);
}

core::Result<std::size_t> ImportService::importWav(
    const std::filesystem::path& sourcePath,
    const repository::WavImportOptions& options,
    std::stop_token stopToken)
{
    auto loadResult = loadWav(sourcePath, options, stopToken);
    if (!loadResult) {
        return core::Result<std::size_t>::failure(loadResult.error());
    }

    const auto accepted = acceptImportedSignal(std::move(loadResult).value());
    if (!accepted.has_value()) {
        return core::Result<std::size_t>::failure(
            core::ErrorCode::InvalidParameter,
            "The imported signal was deleted before it could be accepted.");
    }
    return core::Result<std::size_t>::success(*accepted);
}

core::Result<SignalEntry> ImportService::loadCsv(
    const std::filesystem::path& sourcePath,
    const repository::CsvImportOptions& options,
    std::stop_token stopToken)
{
    return loadCsvImpl(sourcePath, options, stopToken, false);
}

core::Result<SignalEntry> ImportService::openCsv(
    const std::filesystem::path& sourcePath,
    const repository::CsvImportOptions& options,
    std::stop_token stopToken)
{
    return loadCsvImpl(sourcePath, options, stopToken, true);
}

core::Result<SignalEntry> ImportService::loadCsvImpl(
    const std::filesystem::path& sourcePath,
    const repository::CsvImportOptions& options,
    std::stop_token stopToken,
    bool reuseExisting)
{
    std::lock_guard importLock(importMutex_);
    auto fileImportResult = fileRepository_.importCsv(
        sourcePath,
        options,
        stopToken);
    if (!fileImportResult) {
        return core::Result<SignalEntry>::failure(fileImportResult.error());
    }

    return registerImportedSignal(
        std::move(fileImportResult).value(),
        csvParametersJson(options),
        csvLogicalOptionsJson(options),
        stopToken,
        reuseExisting);
}

core::Result<SignalEntry> ImportService::loadWav(
    const std::filesystem::path& sourcePath,
    const repository::WavImportOptions& options,
    std::stop_token stopToken)
{
    return loadWavImpl(sourcePath, options, stopToken, false);
}

core::Result<SignalEntry> ImportService::openWav(
    const std::filesystem::path& sourcePath,
    const repository::WavImportOptions& options,
    std::stop_token stopToken)
{
    return loadWavImpl(sourcePath, options, stopToken, true);
}

core::Result<SignalEntry> ImportService::loadWavImpl(
    const std::filesystem::path& sourcePath,
    const repository::WavImportOptions& options,
    std::stop_token stopToken,
    bool reuseExisting)
{
    std::lock_guard importLock(importMutex_);
    auto fileImportResult = fileRepository_.importWav(
        sourcePath,
        options,
        stopToken);
    if (!fileImportResult) {
        return core::Result<SignalEntry>::failure(fileImportResult.error());
    }

    return registerImportedSignal(
        std::move(fileImportResult).value(),
        wavParametersJson(options),
        wavLogicalOptionsJson(options),
        stopToken,
        reuseExisting);
}

std::optional<std::size_t> ImportService::acceptImportedSignal(
    SignalEntry entry)
{
    std::lock_guard registrationLock(importMutex_);
    if (deletedRecordIds_.contains(entry.recordId)) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].recordId == entry.recordId) {
            return index;
        }
    }

    entries_.push_back(std::move(entry));
    return entries_.size() - 1U;
}

core::Result<SignalRemovalOutcome> ImportService::removePersistedSignal(
    std::int64_t recordId)
{
    if (recordId <= 0) {
        return core::Result<SignalRemovalOutcome>::failure(
            core::ErrorCode::InvalidParameter,
            "The signal record id must be positive.");
    }

    // This lock makes the database last-reference decision and physical
    // archive cleanup indivisible with respect to imports and generated
    // signal registration performed through this service instance.
    std::lock_guard registrationLock(importMutex_);
    auto removal = signalRepository_.removeSignal(recordId);
    if (!removal) {
        return core::Result<SignalRemovalOutcome>::failure(removal.error());
    }

    deletedRecordIds_.insert(recordId);
    SignalRemovalOutcome outcome;
    outcome.removed = removal.value().removed;
    if (!removal.value().unreferencedStoredRelativePath.has_value()) {
        return core::Result<SignalRemovalOutcome>::success(
            std::move(outcome));
    }

    outcome.unreferencedStoredRelativePath =
        *removal.value().unreferencedStoredRelativePath;
    // The SQLite commit is already durable. Cleanup deliberately ignores any
    // task cancellation arriving after that point and reports a warning while
    // leaving a safe orphan archive for later reuse.
    auto cleanup = fileRepository_.removeStoredFile(
        outcome.unreferencedStoredRelativePath);
    if (!cleanup) {
        outcome.cleanupWarning =
            "The signal was deleted, but its unreferenced project archive "
            "could not be removed: " + cleanup.error().message;
    } else if (cleanup.value()) {
        outcome.storedFileRemoved = true;
    } else {
        outcome.storedFileAlreadyMissing = true;
    }
    return core::Result<SignalRemovalOutcome>::success(std::move(outcome));
}

bool ImportService::forgetSignal(std::int64_t recordId)
{
    if (recordId <= 0) {
        return false;
    }

    std::lock_guard registrationLock(importMutex_);
    deletedRecordIds_.insert(recordId);
    const auto found = std::find_if(
        entries_.begin(),
        entries_.end(),
        [recordId](const SignalEntry& entry) {
            return entry.recordId == recordId;
        });
    if (found == entries_.end()) {
        return false;
    }
    entries_.erase(found);
    return true;
}

const std::vector<SignalEntry>& ImportService::entries() const noexcept
{
    return entries_;
}

core::Result<SignalEntry> ImportService::registerImportedSignal(
    repository::FileImport fileImport,
    std::string parametersJson,
    std::string logicalOptionsJson,
    std::stop_token stopToken,
    bool reuseExisting)
{
    auto cleanNewArchive = [this, &fileImport](core::Error& error) {
        if (!fileImport.createdStoredFile) {
            return;
        }

        auto referenceResult = signalRepository_.findByFileHash(
            fileImport.contentHash);
        const bool referencedAtThisPath = referenceResult
            && referenceResult.value().has_value()
            && referenceResult.value()->storedRelativePath
                == fileImport.storedRelativePath;

        if (!referenceResult) {
            error.message +=
                " The new archive was kept because its database "
                "references could not be checked.";
            return;
        }
        if (referencedAtThisPath) {
            return;
        }

        // Cleanup must not reuse the cancelled token. Otherwise a cancelled
        // import could deliberately leave a half-owned archive behind.
        auto cleanupResult = fileRepository_.removeStoredFile(
            fileImport.storedRelativePath);
        if (!cleanupResult) {
            error.message += " Cleanup of the new archive also failed: "
                + cleanupResult.error().message;
        } else if (!cleanupResult.value()) {
            error.message += " The new archive could not be removed.";
        }
    };

    if (stopToken.stop_requested()) {
        core::Error cancelledError{
            core::ErrorCode::Cancelled,
            "The file import was cancelled before it was saved."};
        cleanNewArchive(cancelledError);
        return core::Result<SignalEntry>::failure(
            std::move(cancelledError));
    }

    const std::string sourceKey = importedSourceKey(
        fileImport,
        logicalOptionsJson);
    auto existingResult = signalRepository_.findBySourceKey(sourceKey);
    if (!existingResult) {
        core::Error databaseError = existingResult.error();
        cleanNewArchive(databaseError);
        return core::Result<SignalEntry>::failure(
            std::move(databaseError));
    }

    if (existingResult.value().has_value()) {
        const auto& existing = *existingResult.value();
        if (fileImport.createdStoredFile
            && existing.storedRelativePath
                != fileImport.storedRelativePath) {
            auto cleanupResult = fileRepository_.removeStoredFile(
                fileImport.storedRelativePath);
            if (!cleanupResult || !cleanupResult.value()) {
                core::Error duplicateError = duplicateFileError(
                    fileImport.sourcePath);
                duplicateError.message += !cleanupResult
                    ? " Cleanup of the replacement archive also failed: "
                        + cleanupResult.error().message
                    : " The replacement archive could not be removed.";
                return core::Result<SignalEntry>::failure(
                    std::move(duplicateError));
            }
        }
        if (reuseExisting) {
            return core::Result<SignalEntry>::success(makeEntry(existing));
        }
        return core::Result<SignalEntry>::failure(
            duplicateFileError(fileImport.sourcePath));
    }

    if (stopToken.stop_requested()) {
        core::Error cancelledError{
            core::ErrorCode::Cancelled,
            "The file import was cancelled before it was saved."};
        cleanNewArchive(cancelledError);
        return core::Result<SignalEntry>::failure(
            std::move(cancelledError));
    }

    repository::FileVersionRegistration fileVersion{
        fileImport.contentHash,
        fileImport.storedRelativePath,
        fileImport.sourcePath.filename().string(),
        fileImport.byteSize
    };

    repository::SignalRegistration registration{
        fileImport.sourceType,
        sourceKey,
        fileImport.sourcePath,
        std::move(parametersJson),
        std::move(fileVersion),
        std::move(fileImport.signal)
    };

    auto savedResult = signalRepository_.saveSignal(std::move(registration));
    if (!savedResult) {
        core::Error databaseError = savedResult.error();

        // Another importer may have committed the same file between our
        // lookup and save. The helper checks that before deleting anything.
        cleanNewArchive(databaseError);
        return core::Result<SignalEntry>::failure(
            std::move(databaseError));
    }

    // The database commit is the point of no return. If cancellation arrives
    // after this line, the committed success wins and the UI receives it.
    return core::Result<SignalEntry>::success(
        makeEntry(std::move(savedResult).value()));
}

}  // namespace signallab::app
