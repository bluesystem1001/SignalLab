#include "signallab/io/filesystem_file_repository.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "signallab/core/error.hpp"
#include "signallab/io/csv_reader.hpp"
#include "signallab/io/hash_service.hpp"
#include "signallab/io/wav_reader.hpp"

namespace signallab::io {
namespace {

std::atomic<std::uint64_t> snapshotCounter{0};

core::Result<std::filesystem::path> resolveStoredPath(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& storedRelativePath);

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
    const std::string& hash)
{
    return std::filesystem::path("data")
        / "imports"
        / (hash + ".source");
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
    replaceAll(error.message, snapshotPath.string(), sourcePath.string());
    replaceAll(
        error.message,
        snapshotPath.generic_string(),
        sourcePath.generic_string());
    return error;
}

core::Error storedFileErrorForSource(
    core::Error error,
    const std::filesystem::path& sourcePath)
{
    error.message = "Cannot import " + sourcePath.string()
        + ": " + error.message;
    return error;
}

core::Error storageError(
    const std::filesystem::path& sourcePath,
    const std::string& details)
{
    return core::Error{
        core::ErrorCode::FileWriteFailed,
        "Cannot store imported file " + sourcePath.generic_string()
            + ": " + details
    };
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

    const std::filesystem::path boundaryProbe =
        std::filesystem::path("data")
        / "imports"
        / ".boundary-check";
    auto boundaryResult = resolveStoredPath(projectRoot, boundaryProbe);
    if (!boundaryResult) {
        return core::Result<std::filesystem::path>::failure(
            boundaryResult.error());
    }

    const std::filesystem::path directory = importsDirectory(projectRoot);
    std::filesystem::create_directories(directory, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot create project import folder for "
                + sourcePath.generic_string() + ": " + error.message());
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
        const std::filesystem::path snapshotPath =
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

core::Result<std::uint64_t> readSnapshotSize(
    const std::filesystem::path& snapshotPath,
    const std::filesystem::path& sourcePath)
{
    std::error_code error;
    const std::uintmax_t byteSize =
        std::filesystem::file_size(snapshotPath, error);
    if (error) {
        return core::Result<std::uint64_t>::failure(
            core::ErrorCode::FileOpenFailed,
            "Cannot read the size of " + sourcePath.generic_string()
                + ": " + error.message());
    }

    if (byteSize > std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<std::uint64_t>::failure(
            core::ErrorCode::FileOpenFailed,
            "The source file is too large: " + sourcePath.generic_string());
    }

    return core::Result<std::uint64_t>::success(
        static_cast<std::uint64_t>(byteSize));
}

bool isStrictDescendant(
    const std::filesystem::path& child,
    const std::filesystem::path& parent)
{
    auto childPart = child.begin();
    auto parentPart = parent.begin();

    while (parentPart != parent.end()) {
        if (childPart == child.end() || *childPart != *parentPart) {
            return false;
        }

        ++childPart;
        ++parentPart;
    }

    return childPart != child.end();
}

core::Result<std::filesystem::path> resolveStoredPath(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& storedRelativePath)
{
    if (storedRelativePath.empty()
        || storedRelativePath.is_absolute()
        || storedRelativePath.has_root_name()
        || storedRelativePath.has_root_directory()) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::InvalidParameter,
            "Stored import path must be relative and inside data/imports: "
                + storedRelativePath.generic_string());
    }

    std::error_code error;
    const std::filesystem::path rootPath = std::filesystem::absolute(
        importsDirectory(projectRoot),
        error).lexically_normal();
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot resolve the project import folder: " + error.message());
    }

    const std::filesystem::path targetPath = std::filesystem::absolute(
        projectRoot / storedRelativePath,
        error).lexically_normal();
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot resolve stored import path "
                + storedRelativePath.generic_string() + ": "
                + error.message());
    }

    if (!isStrictDescendant(targetPath, rootPath)) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::InvalidParameter,
            "Stored import path is outside data/imports: "
                + storedRelativePath.generic_string());
    }

    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(rootPath, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot inspect the project import folder: " + error.message());
    }

    const std::filesystem::path absoluteProjectRoot =
        std::filesystem::absolute(projectRoot, error).lexically_normal();
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot resolve the project folder: " + error.message());
    }

    const std::filesystem::path canonicalProjectRoot =
        std::filesystem::weakly_canonical(absoluteProjectRoot, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot inspect the project folder: " + error.message());
    }

    if (!isStrictDescendant(canonicalRoot, canonicalProjectRoot)) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::InvalidParameter,
            "The project import folder leaves the project through a link.");
    }

    const std::filesystem::path canonicalTarget =
        std::filesystem::weakly_canonical(targetPath, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot inspect stored import path "
                + storedRelativePath.generic_string() + ": "
                + error.message());
    }

    if (!isStrictDescendant(canonicalTarget, canonicalRoot)) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::InvalidParameter,
            "Stored import path leaves data/imports through a link: "
                + storedRelativePath.generic_string());
    }

    return core::Result<std::filesystem::path>::success(targetPath);
}

core::Result<bool> validateStoredPath(
    const std::filesystem::path& storedPath,
    const std::string& expectedHash)
{
    if (expectedHash.empty()) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "Expected SHA-256 hash must not be empty.");
    }

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

    const std::filesystem::file_status status =
        std::filesystem::symlink_status(storedPath, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        const std::string details = error
            ? error.message()
            : "the stored path is not a regular file";
        return core::Result<bool>::failure(
            core::ErrorCode::FileWriteFailed,
            "Stored import is damaged: " + storedPath.generic_string()
                + " (" + details + ")");
    }

    auto storedHashResult = calculateSha256(storedPath);
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

core::Result<std::optional<std::filesystem::path>> findStoredFile(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& preferredRelativePath,
    const std::string& hash)
{
    auto preferredPathResult = resolveStoredPath(
        projectRoot,
        preferredRelativePath);
    if (!preferredPathResult) {
        return core::Result<std::optional<std::filesystem::path>>::failure(
            preferredPathResult.error());
    }

    auto preferredValidation = validateStoredPath(
        preferredPathResult.value(),
        hash);
    if (!preferredValidation) {
        return core::Result<std::optional<std::filesystem::path>>::failure(
            preferredValidation.error());
    }
    if (preferredValidation.value()) {
        return core::Result<std::optional<std::filesystem::path>>::success(
            preferredRelativePath);
    }

    const std::filesystem::path directory = importsDirectory(projectRoot);
    std::error_code error;
    const bool directoryExists = std::filesystem::exists(directory, error);
    if (error) {
        return core::Result<std::optional<std::filesystem::path>>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot inspect the project import folder: " + error.message());
    }
    if (!directoryExists) {
        return core::Result<std::optional<std::filesystem::path>>::success(
            std::nullopt);
    }

    std::filesystem::directory_iterator item(directory, error);
    const std::filesystem::directory_iterator end;

    while (!error && item != end) {
        if (item->path() != preferredPathResult.value()
            && hasContentAddressedName(item->path(), hash)) {
            const std::filesystem::path candidateRelativePath =
                std::filesystem::path("data")
                / "imports"
                / item->path().filename();
            auto candidatePathResult = resolveStoredPath(
                projectRoot,
                candidateRelativePath);
            if (!candidatePathResult) {
                return core::Result<
                    std::optional<std::filesystem::path>>::failure(
                        candidatePathResult.error());
            }

            auto validationResult = validateStoredPath(
                candidatePathResult.value(),
                hash);
            if (!validationResult) {
                return core::Result<
                    std::optional<std::filesystem::path>>::failure(
                        validationResult.error());
            }
            if (validationResult.value()) {
                // Older SignalLab versions kept the original extension in
                // the archive name. Reuse that path without renaming it:
                // an existing database row may still refer to the old name.
                return core::Result<
                    std::optional<std::filesystem::path>>::success(
                        candidateRelativePath);
            }
        }

        item.increment(error);
    }

    if (error) {
        return core::Result<std::optional<std::filesystem::path>>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot inspect the project import folder: " + error.message());
    }

    return core::Result<std::optional<std::filesystem::path>>::success(
        std::nullopt);
}

core::Result<repository::FileImport> storeOrReuseFile(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& snapshotPath,
    std::string hash,
    std::filesystem::path preferredRelativePath,
    std::uint64_t byteSize,
    repository::SignalSourceType sourceType,
    core::Signal signal)
{
    auto existingFileResult = findStoredFile(
        projectRoot,
        preferredRelativePath,
        hash);
    if (!existingFileResult) {
        return core::Result<repository::FileImport>::failure(
            storedFileErrorForSource(
                existingFileResult.error(),
                sourcePath));
    }

    if (existingFileResult.value()) {
        return core::Result<repository::FileImport>::success(
            repository::FileImport{
                sourceType,
                std::move(hash),
                sourcePath,
                *existingFileResult.value(),
                byteSize,
                false,
                std::move(signal)
            });
    }

    const std::filesystem::path destinationPath =
        projectRoot / preferredRelativePath;
    std::error_code error;
    std::filesystem::rename(snapshotPath, destinationPath, error);
    if (error) {
        // Another importer may have stored the same bytes just before rename.
        auto racedFileResult = findStoredFile(
            projectRoot,
            preferredRelativePath,
            hash);
        if (!racedFileResult) {
            return core::Result<repository::FileImport>::failure(
                storedFileErrorForSource(
                    racedFileResult.error(),
                    sourcePath));
        }
        if (racedFileResult.value()) {
            return core::Result<repository::FileImport>::success(
                repository::FileImport{
                    sourceType,
                    std::move(hash),
                    sourcePath,
                    *racedFileResult.value(),
                    byteSize,
                    false,
                    std::move(signal)
                });
        }

        return core::Result<repository::FileImport>::failure(
            storageError(sourcePath, error.message()));
    }

    return core::Result<repository::FileImport>::success(
        repository::FileImport{
            sourceType,
            std::move(hash),
            sourcePath,
            std::move(preferredRelativePath),
            byteSize,
            true,
            std::move(signal)
        });
}

}  // namespace

FilesystemFileRepository::FilesystemFileRepository(
    std::filesystem::path projectRoot)
    : projectRoot_(std::move(projectRoot))
{
}

core::Result<repository::ImportPreview>
FilesystemFileRepository::previewCsv(
    const std::filesystem::path& sourcePath,
    const repository::CsvImportOptions& options) const
{
    return io::previewCsv(sourcePath, options);
}

core::Result<repository::ImportPreview>
FilesystemFileRepository::previewWav(
    const std::filesystem::path& sourcePath) const
{
    return io::previewWav(sourcePath);
}

core::Result<repository::FileImport> FilesystemFileRepository::importCsv(
    const std::filesystem::path& sourcePath,
    const repository::CsvImportOptions& options)
{
    auto snapshotResult = createSourceSnapshot(projectRoot_, sourcePath);
    if (!snapshotResult) {
        return core::Result<repository::FileImport>::failure(
            snapshotResult.error());
    }

    const std::filesystem::path snapshotPath =
        std::move(snapshotResult).value();
    const TemporarySnapshotGuard snapshotGuard(snapshotPath);

    auto hashResult = calculateSha256(snapshotPath);
    if (!hashResult) {
        return core::Result<repository::FileImport>::failure(
            sourceFacingError(
                hashResult.error(),
                snapshotPath,
                sourcePath));
    }

    auto sizeResult = readSnapshotSize(snapshotPath, sourcePath);
    if (!sizeResult) {
        return core::Result<repository::FileImport>::failure(
            sizeResult.error());
    }

    auto signalResult = readCsv(snapshotPath, options);
    if (!signalResult) {
        return core::Result<repository::FileImport>::failure(
            sourceFacingError(
                signalResult.error(),
                snapshotPath,
                sourcePath));
    }

    std::string hash = std::move(hashResult).value();
    std::filesystem::path storedRelativePath = makeStoredRelativePath(hash);

    return storeOrReuseFile(
        projectRoot_,
        sourcePath,
        snapshotPath,
        std::move(hash),
        std::move(storedRelativePath),
        sizeResult.value(),
        repository::SignalSourceType::Csv,
        std::move(signalResult).value());
}

core::Result<repository::FileImport> FilesystemFileRepository::importWav(
    const std::filesystem::path& sourcePath,
    const repository::WavImportOptions& options)
{
    auto snapshotResult = createSourceSnapshot(projectRoot_, sourcePath);
    if (!snapshotResult) {
        return core::Result<repository::FileImport>::failure(
            snapshotResult.error());
    }

    const std::filesystem::path snapshotPath =
        std::move(snapshotResult).value();
    const TemporarySnapshotGuard snapshotGuard(snapshotPath);

    auto hashResult = calculateSha256(snapshotPath);
    if (!hashResult) {
        return core::Result<repository::FileImport>::failure(
            sourceFacingError(
                hashResult.error(),
                snapshotPath,
                sourcePath));
    }

    auto sizeResult = readSnapshotSize(snapshotPath, sourcePath);
    if (!sizeResult) {
        return core::Result<repository::FileImport>::failure(
            sizeResult.error());
    }

    auto signalResult = readWav(snapshotPath, options);
    if (!signalResult) {
        return core::Result<repository::FileImport>::failure(
            sourceFacingError(
                signalResult.error(),
                snapshotPath,
                sourcePath));
    }

    std::string hash = std::move(hashResult).value();
    std::filesystem::path storedRelativePath = makeStoredRelativePath(hash);

    return storeOrReuseFile(
        projectRoot_,
        sourcePath,
        snapshotPath,
        std::move(hash),
        std::move(storedRelativePath),
        sizeResult.value(),
        repository::SignalSourceType::Wav,
        std::move(signalResult).value());
}

core::Result<bool> FilesystemFileRepository::validateStoredFile(
    const std::filesystem::path& storedRelativePath,
    const std::string& expectedHash) const
{
    auto pathResult = resolveStoredPath(projectRoot_, storedRelativePath);
    if (!pathResult) {
        return core::Result<bool>::failure(pathResult.error());
    }

    return validateStoredPath(pathResult.value(), expectedHash);
}

core::Result<bool> FilesystemFileRepository::removeStoredFile(
    const std::filesystem::path& storedRelativePath)
{
    auto pathResult = resolveStoredPath(projectRoot_, storedRelativePath);
    if (!pathResult) {
        return core::Result<bool>::failure(pathResult.error());
    }

    const std::filesystem::path& storedPath = pathResult.value();
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

    const std::filesystem::file_status status =
        std::filesystem::symlink_status(storedPath, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        const std::string details = error
            ? error.message()
            : "the stored path is not a regular file";
        return core::Result<bool>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot remove stored import " + storedPath.generic_string()
                + ": " + details);
    }

    const bool removed = std::filesystem::remove(storedPath, error);
    if (error) {
        return core::Result<bool>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot remove stored import " + storedPath.generic_string()
                + ": " + error.message());
    }

    return core::Result<bool>::success(removed);
}

}  // namespace signallab::io
