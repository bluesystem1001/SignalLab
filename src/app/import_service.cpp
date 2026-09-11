#include "signallab/app/import_service.hpp"

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

core::Result<std::size_t> duplicateFileError(
    const std::filesystem::path& sourcePath)
{
    return core::Result<std::size_t>::failure(
        core::ErrorCode::DuplicateFile,
        "This file has already been imported: "
            + sourcePath.generic_string());
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
    if (stableSourceKey.empty()) {
        return core::Result<std::size_t>::failure(
            core::ErrorCode::InvalidParameter,
            "A generated signal requires a non-empty stable source key.");
    }

    auto existingResult = signalRepository_.findBySourceKey(stableSourceKey);
    if (!existingResult) {
        return core::Result<std::size_t>::failure(existingResult.error());
    }

    if (existingResult.value().has_value()) {
        const std::int64_t recordId = existingResult.value()->recordId;
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            if (entries_[index].recordId == recordId) {
                return core::Result<std::size_t>::success(index);
            }
        }

        entries_.push_back(makeEntry(*existingResult.value()));
        return core::Result<std::size_t>::success(entries_.size() - 1U);
    }

    repository::SignalRegistration registration{
        repository::SignalSourceType::Generated,
        stableSourceKey,
        {},
        "{}",
        std::nullopt,
        std::move(signal)
    };

    auto savedResult = signalRepository_.saveSignal(std::move(registration));
    if (!savedResult) {
        return core::Result<std::size_t>::failure(savedResult.error());
    }

    entries_.push_back(makeEntry(std::move(savedResult).value()));
    return core::Result<std::size_t>::success(entries_.size() - 1U);
}

core::Result<repository::ImportPreview> ImportService::previewCsv(
    const std::filesystem::path& sourcePath,
    const repository::CsvImportOptions& options) const
{
    return fileRepository_.previewCsv(sourcePath, options);
}

core::Result<repository::ImportPreview> ImportService::previewWav(
    const std::filesystem::path& sourcePath) const
{
    return fileRepository_.previewWav(sourcePath);
}

core::Result<std::size_t> ImportService::importCsv(
    const std::filesystem::path& sourcePath,
    const repository::CsvImportOptions& options)
{
    auto fileImportResult = fileRepository_.importCsv(sourcePath, options);
    if (!fileImportResult) {
        return core::Result<std::size_t>::failure(fileImportResult.error());
    }

    return registerImportedSignal(
        std::move(fileImportResult).value(),
        csvParametersJson(options),
        csvLogicalOptionsJson(options));
}

core::Result<std::size_t> ImportService::importWav(
    const std::filesystem::path& sourcePath,
    const repository::WavImportOptions& options)
{
    auto fileImportResult = fileRepository_.importWav(sourcePath, options);
    if (!fileImportResult) {
        return core::Result<std::size_t>::failure(fileImportResult.error());
    }

    return registerImportedSignal(
        std::move(fileImportResult).value(),
        wavParametersJson(options),
        wavLogicalOptionsJson(options));
}

const std::vector<SignalEntry>& ImportService::entries() const noexcept
{
    return entries_;
}

core::Result<std::size_t> ImportService::registerImportedSignal(
    repository::FileImport fileImport,
    std::string parametersJson,
    std::string logicalOptionsJson)
{
    const std::string sourceKey = importedSourceKey(
        fileImport,
        logicalOptionsJson);
    auto existingResult = signalRepository_.findBySourceKey(sourceKey);
    if (!existingResult) {
        core::Error databaseError = existingResult.error();
        if (fileImport.createdStoredFile) {
            auto referenceResult = signalRepository_.findByFileHash(
                fileImport.contentHash);
            const bool referencedAtThisPath = referenceResult
                && referenceResult.value().has_value()
                && referenceResult.value()->storedRelativePath
                    == fileImport.storedRelativePath;

            if (!referenceResult) {
                databaseError.message +=
                    " The new archive was kept because its database "
                    "references could not be checked.";
            } else if (!referencedAtThisPath) {
                auto cleanupResult = fileRepository_.removeStoredFile(
                    fileImport.storedRelativePath);
                if (!cleanupResult) {
                    databaseError.message +=
                        " Cleanup of the new archive also failed: "
                        + cleanupResult.error().message;
                } else if (!cleanupResult.value()) {
                    databaseError.message +=
                        " The new archive could not be removed.";
                }
            }
        }
        return core::Result<std::size_t>::failure(
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
                auto duplicateResult = duplicateFileError(
                    fileImport.sourcePath);
                core::Error duplicateError = duplicateResult.error();
                duplicateError.message += !cleanupResult
                    ? " Cleanup of the replacement archive also failed: "
                        + cleanupResult.error().message
                    : " The replacement archive could not be removed.";
                return core::Result<std::size_t>::failure(
                    std::move(duplicateError));
            }
        }
        return duplicateFileError(fileImport.sourcePath);
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

        if (fileImport.createdStoredFile) {
            // Another importer may have committed the same file between our
            // lookup and save. Never delete an archive already referenced by
            // the database.
            auto referenceResult = signalRepository_.findByFileHash(
                fileImport.contentHash);
            const bool referencedAtThisPath = referenceResult
                && referenceResult.value().has_value()
                && referenceResult.value()->storedRelativePath
                    == fileImport.storedRelativePath;

            if (!referenceResult) {
                databaseError.message +=
                    " The new archive was kept because its database "
                    "references could not be checked.";
            } else if (!referencedAtThisPath) {
                auto cleanupResult = fileRepository_.removeStoredFile(
                    fileImport.storedRelativePath);
                if (!cleanupResult) {
                    databaseError.message +=
                        " Cleanup of the newly stored file also failed: "
                        + cleanupResult.error().message;
                } else if (!cleanupResult.value()) {
                    databaseError.message +=
                        " The newly stored file could not be removed.";
                }
            }
        }

        return core::Result<std::size_t>::failure(
            std::move(databaseError));
    }

    entries_.push_back(makeEntry(std::move(savedResult).value()));
    return core::Result<std::size_t>::success(entries_.size() - 1U);
}

}  // namespace signallab::app
