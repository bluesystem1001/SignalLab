#include "signallab/app/import_service.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "signallab/core/error.hpp"
#include "signallab/io/csv_reader.hpp"
#include "signallab/io/hash_service.hpp"
#include "signallab/io/wav_reader.hpp"

namespace signallab::app {
namespace {

std::atomic<std::uint64_t> snapshotCounter{0};

class TemporarySnapshotGuard {
public:
    explicit TemporarySnapshotGuard(std::filesystem::path path)
        : path_(std::move(path))
    {
    }

    ~TemporarySnapshotGuard()
    {
        std::error_code ignoredError;
        std::filesystem::remove(path_, ignoredError);
    }

    TemporarySnapshotGuard(const TemporarySnapshotGuard&) = delete;
    TemporarySnapshotGuard& operator=(const TemporarySnapshotGuard&) = delete;

private:
    std::filesystem::path path_;
};

std::filesystem::path importsDirectory(
    const std::filesystem::path& projectRoot)
{
    return projectRoot / "data" / "imports";
}

std::filesystem::path makeStoredRelativePath(
    const std::filesystem::path& sourcePath,
    const std::string& hash,
    const std::string& fallbackExtension)
{
    std::string extension = sourcePath.extension().string();
    if (extension.empty()) {
        extension = fallbackExtension;
    }

    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });

    return std::filesystem::path("data")
        / "imports"
        / (hash + extension);
}

std::string chooseDisplayName(
    const core::Signal& signal,
    const std::filesystem::path& sourcePath)
{
    if (!signal.metadata().name.empty()) {
        return signal.metadata().name;
    }

    if (!sourcePath.empty()) {
        return sourcePath.filename().string();
    }

    return "Generated signal";
}

core::Result<std::size_t> duplicateFileError(
    const std::filesystem::path& sourcePath)
{
    return core::Result<std::size_t>::failure(
        core::ErrorCode::DuplicateFile,
        "This file has already been imported: "
            + sourcePath.generic_string());
}

core::Result<std::size_t> importStorageError(
    const std::filesystem::path& sourcePath,
    const std::string& details)
{
    return core::Result<std::size_t>::failure(
        core::ErrorCode::FileWriteFailed,
        "Cannot store imported file " + sourcePath.generic_string()
            + ": " + details);
}

void replaceAll(
    std::string& text,
    const std::string& oldText,
    const std::string& newText)
{
    if (oldText.empty() || oldText == newText) {
        return;
    }

    std::size_t position = 0;
    while ((position = text.find(oldText, position)) != std::string::npos) {
        text.replace(position, oldText.size(), newText);
        position += newText.size();
    }
}

core::Error sourceFacingError(
    core::Error error,
    const std::filesystem::path& snapshotPath,
    const std::filesystem::path& sourcePath)
{
    replaceAll(
        error.message,
        snapshotPath.string(),
        sourcePath.string());
    replaceAll(
        error.message,
        snapshotPath.generic_string(),
        sourcePath.generic_string());
    return error;
}

core::Result<std::filesystem::path> createSourceSnapshot(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath)
{
    std::error_code error;
    const bool sourceExists = std::filesystem::exists(sourcePath, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileOpenFailed,
            "Cannot inspect source file " + sourcePath.generic_string()
                + ": " + error.message());
    }
    if (!sourceExists) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileNotFound,
            "File not found: " + sourcePath.generic_string());
    }

    const bool sourceIsFile =
        std::filesystem::is_regular_file(sourcePath, error);
    if (error || !sourceIsFile) {
        const std::string details = error
            ? error.message()
            : "the source path is not a regular file";
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileOpenFailed,
            "Cannot read source file " + sourcePath.generic_string()
                + ": " + details);
    }

    const auto directory = importsDirectory(projectRoot);
    std::filesystem::create_directories(directory, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot create project import folder: " + error.message());
    }

    constexpr int maximumAttempts = 10;
    for (int attempt = 0; attempt < maximumAttempts; ++attempt) {
        const auto timeValue = std::chrono::high_resolution_clock::now()
            .time_since_epoch()
            .count();
        const auto counterValue = snapshotCounter.fetch_add(
            1,
            std::memory_order_relaxed);
        const std::string uniquePart = std::to_string(timeValue)
            + "-" + std::to_string(counterValue);
        const auto snapshotPath =
            directory / (".signallab-import-" + uniquePart + ".tmp");

        error.clear();
        const bool copied = std::filesystem::copy_file(
            sourcePath,
            snapshotPath,
            std::filesystem::copy_options::none,
            error);

        if (copied && !error) {
            return core::Result<std::filesystem::path>::success(snapshotPath);
        }

        if (error == std::make_error_code(std::errc::file_exists)
            || (!copied && !error)) {
            continue;
        }

        std::error_code cleanupError;
        std::filesystem::remove(snapshotPath, cleanupError);

        const std::string details = error
            ? error.message()
            : "the snapshot could not be created";
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot snapshot source file " + sourcePath.generic_string()
                + ": " + details);
    }

    return core::Result<std::filesystem::path>::failure(
        core::ErrorCode::FileWriteFailed,
        "Cannot create a unique temporary snapshot for "
            + sourcePath.generic_string());
}

core::Result<bool> validateStoredFile(
    const std::filesystem::path& storedPath,
    const std::string& expectedHash)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(storedPath, error);
    if (error) {
        return core::Result<bool>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot inspect stored import file "
                + storedPath.generic_string() + ": " + error.message());
    }
    if (!exists) {
        return core::Result<bool>::success(false);
    }

    const bool isFile = std::filesystem::is_regular_file(storedPath, error);
    if (error || !isFile) {
        const std::string details = error
            ? error.message()
            : "the stored path is not a regular file";
        return core::Result<bool>::failure(
            core::ErrorCode::FileWriteFailed,
            "Stored import is damaged: " + storedPath.generic_string()
                + " (" + details + ")");
    }

    auto storedHashResult = io::calculateSha256(storedPath);
    if (!storedHashResult) {
        return core::Result<bool>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot validate stored import file "
                + storedPath.generic_string() + ": "
                + storedHashResult.error().message);
    }

    const std::string& actualHash = storedHashResult.value();
    if (actualHash != expectedHash) {
        return core::Result<bool>::failure(
            core::ErrorCode::FileWriteFailed,
            "Stored import is damaged: SHA-256 mismatch for "
                + storedPath.generic_string()
                + " (expected " + expectedHash
                + ", found " + actualHash + ")");
    }

    return core::Result<bool>::success(true);
}

bool hasContentAddressedName(
    const std::filesystem::path& path,
    const std::string& hash)
{
    const std::string fileName = path.filename().string();
    return fileName.starts_with(hash + ".")
        && !fileName.ends_with(".tmp");
}

}  // namespace

ImportService::ImportService(std::filesystem::path projectRoot)
    : projectRoot_(std::move(projectRoot))
{
}

std::size_t ImportService::addGeneratedSignal(core::Signal signal)
{
    const std::size_t entryIndex = entries_.size();
    const std::string displayName = chooseDisplayName(signal, {});

    entries_.push_back(SignalEntry{
        displayName,
        {},
        {},
        {},
        std::move(signal)
    });

    return entryIndex;
}

core::Result<io::ImportPreview> ImportService::previewCsv(
    const std::filesystem::path& sourcePath,
    const io::CsvImportOptions& options) const
{
    return io::previewCsv(sourcePath, options);
}

core::Result<io::ImportPreview> ImportService::previewWav(
    const std::filesystem::path& sourcePath) const
{
    return io::previewWav(sourcePath);
}

core::Result<std::size_t> ImportService::importCsv(
    const std::filesystem::path& sourcePath,
    const io::CsvImportOptions& options)
{
    auto snapshotResult = createSourceSnapshot(projectRoot_, sourcePath);
    if (!snapshotResult) {
        return core::Result<std::size_t>::failure(snapshotResult.error());
    }

    const std::filesystem::path snapshotPath =
        std::move(snapshotResult).value();
    const TemporarySnapshotGuard snapshotGuard(snapshotPath);

    auto hashResult = io::calculateSha256(snapshotPath);
    if (!hashResult) {
        return core::Result<std::size_t>::failure(sourceFacingError(
            hashResult.error(), snapshotPath, sourcePath));
    }

    std::string hash = std::move(hashResult).value();
    const auto storedRelativePath =
        makeStoredRelativePath(sourcePath, hash, ".csv");

    auto duplicateResult = isDuplicate(hash, storedRelativePath);
    if (!duplicateResult) {
        return core::Result<std::size_t>::failure(duplicateResult.error());
    }
    if (duplicateResult.value()) {
        return duplicateFileError(sourcePath);
    }

    auto signalResult = io::readCsv(snapshotPath, options);
    if (!signalResult) {
        return core::Result<std::size_t>::failure(sourceFacingError(
            signalResult.error(), snapshotPath, sourcePath));
    }

    return storeImportedSignal(
        sourcePath,
        snapshotPath,
        std::move(hash),
        storedRelativePath,
        std::move(signalResult).value());
}

core::Result<std::size_t> ImportService::importWav(
    const std::filesystem::path& sourcePath,
    const io::WavImportOptions& options)
{
    auto snapshotResult = createSourceSnapshot(projectRoot_, sourcePath);
    if (!snapshotResult) {
        return core::Result<std::size_t>::failure(snapshotResult.error());
    }

    const std::filesystem::path snapshotPath =
        std::move(snapshotResult).value();
    const TemporarySnapshotGuard snapshotGuard(snapshotPath);

    auto hashResult = io::calculateSha256(snapshotPath);
    if (!hashResult) {
        return core::Result<std::size_t>::failure(sourceFacingError(
            hashResult.error(), snapshotPath, sourcePath));
    }

    std::string hash = std::move(hashResult).value();
    const auto storedRelativePath =
        makeStoredRelativePath(sourcePath, hash, ".wav");

    auto duplicateResult = isDuplicate(hash, storedRelativePath);
    if (!duplicateResult) {
        return core::Result<std::size_t>::failure(duplicateResult.error());
    }
    if (duplicateResult.value()) {
        return duplicateFileError(sourcePath);
    }

    auto signalResult = io::readWav(snapshotPath, options);
    if (!signalResult) {
        return core::Result<std::size_t>::failure(sourceFacingError(
            signalResult.error(), snapshotPath, sourcePath));
    }

    return storeImportedSignal(
        sourcePath,
        snapshotPath,
        std::move(hash),
        storedRelativePath,
        std::move(signalResult).value());
}

const std::vector<SignalEntry>& ImportService::entries() const noexcept
{
    return entries_;
}

core::Result<bool> ImportService::isDuplicate(
    const std::string& hash,
    const std::filesystem::path& storedRelativePath) const
{
    const auto matchingEntry = std::find_if(
        entries_.begin(),
        entries_.end(),
        [&hash](const SignalEntry& entry) {
            return !entry.hash.empty() && entry.hash == hash;
        });

    if (matchingEntry != entries_.end()) {
        if (matchingEntry->storedRelativePath.empty()) {
            return core::Result<bool>::failure(
                core::ErrorCode::FileWriteFailed,
                "An imported signal record has no stored file path.");
        }

        auto validationResult = validateStoredFile(
            projectRoot_ / matchingEntry->storedRelativePath,
            hash);
        if (!validationResult) {
            return validationResult;
        }
        if (!validationResult.value()) {
            return core::Result<bool>::failure(
                core::ErrorCode::FileWriteFailed,
                "Stored import is missing: "
                    + (projectRoot_ / matchingEntry->storedRelativePath)
                        .generic_string());
        }

        return core::Result<bool>::success(true);
    }

    auto exactTargetResult = validateStoredFile(
        projectRoot_ / storedRelativePath,
        hash);
    if (!exactTargetResult) {
        return exactTargetResult;
    }
    if (exactTargetResult.value()) {
        return core::Result<bool>::success(true);
    }

    const auto directory = importsDirectory(projectRoot_);
    std::error_code error;
    const bool directoryExists = std::filesystem::exists(directory, error);
    if (error) {
        return core::Result<bool>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot inspect the project import folder: " + error.message());
    }
    if (!directoryExists) {
        return core::Result<bool>::success(false);
    }

    std::filesystem::directory_iterator item(directory, error);
    const std::filesystem::directory_iterator end;

    while (!error && item != end) {
        if (hasContentAddressedName(item->path(), hash)) {
            auto validationResult = validateStoredFile(item->path(), hash);
            if (!validationResult) {
                return validationResult;
            }
            if (validationResult.value()) {
                return core::Result<bool>::success(true);
            }
        }

        item.increment(error);
    }

    if (error) {
        return core::Result<bool>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot inspect the project import folder: " + error.message());
    }

    return core::Result<bool>::success(false);
}

core::Result<std::size_t> ImportService::storeImportedSignal(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& snapshotPath,
    std::string hash,
    std::filesystem::path storedRelativePath,
    core::Signal signal)
{
    const std::filesystem::path destinationPath =
        projectRoot_ / storedRelativePath;

    std::error_code error;
    std::filesystem::rename(snapshotPath, destinationPath, error);
    if (error) {
        auto duplicateResult = isDuplicate(hash, storedRelativePath);
        if (duplicateResult && duplicateResult.value()) {
            return duplicateFileError(sourcePath);
        }
        if (!duplicateResult) {
            return core::Result<std::size_t>::failure(
                duplicateResult.error());
        }

        return importStorageError(sourcePath, error.message());
    }

    const std::size_t entryIndex = entries_.size();
    const std::string displayName = chooseDisplayName(signal, sourcePath);

    try {
        entries_.push_back(SignalEntry{
            displayName,
            std::move(hash),
            sourcePath,
            std::move(storedRelativePath),
            std::move(signal)
        });
    } catch (const std::exception& exception) {
        std::error_code cleanupError;
        std::filesystem::remove(destinationPath, cleanupError);
        return importStorageError(
            sourcePath,
            "cannot register the imported signal ("
                + std::string(exception.what()) + ")");
    }

    return core::Result<std::size_t>::success(entryIndex);
}

}  // namespace signallab::app
