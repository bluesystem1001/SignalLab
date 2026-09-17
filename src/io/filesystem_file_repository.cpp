#include "signallab/io/filesystem_file_repository.hpp"

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <stop_token>
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
constexpr qint64 fileBlockSize = 64 * 1024;

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
        if (path_.empty()) {
            return;
        }
        std::error_code ignoredError;
        std::filesystem::remove(path_, ignoredError);
    }

    TemporarySnapshotGuard(const TemporarySnapshotGuard&) = delete;
    TemporarySnapshotGuard& operator=(const TemporarySnapshotGuard&) = delete;

    void release()
    {
        path_.clear();
    }

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

template<typename T>
core::Result<T> cancelledFileOperation(
    const std::filesystem::path& sourcePath,
    const std::string& operation)
{
    return core::Result<T>::failure(
        core::ErrorCode::Cancelled,
        operation + " was cancelled for " + sourcePath.generic_string());
}

core::Result<std::filesystem::path> createSourceSnapshot(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    const std::stop_token& stopToken)
{
    if (stopToken.stop_requested()) {
        return cancelledFileOperation<std::filesystem::path>(
            sourcePath,
            "Source snapshot");
    }

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
        if (stopToken.stop_requested()) {
            return cancelledFileOperation<std::filesystem::path>(
                sourcePath,
                "Source snapshot");
        }

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

        // NewOnly reserves the candidate without overwriting another
        // importer's file. It is then reopened for a simple block copy.
        QFile reservation(QString::fromStdWString(snapshotPath.wstring()));
        if (!reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            std::error_code existenceError;
            const bool alreadyExists =
                std::filesystem::exists(snapshotPath, existenceError);
            if (!existenceError && alreadyExists) {
                continue;
            }

            return core::Result<std::filesystem::path>::failure(
                core::ErrorCode::FileWriteFailed,
                "Cannot snapshot source file " + sourcePath.generic_string()
                    + ": " + reservation.errorString().toStdString());
        }
        reservation.close();

        TemporarySnapshotGuard snapshotGuard(snapshotPath);
        QFile sourceFile(QString::fromStdWString(sourcePath.wstring()));
        if (!sourceFile.open(QIODevice::ReadOnly)) {
            return core::Result<std::filesystem::path>::failure(
                core::ErrorCode::FileOpenFailed,
                "Cannot read source file " + sourcePath.generic_string()
                    + ": " + sourceFile.errorString().toStdString());
        }

        QFile snapshotFile(QString::fromStdWString(snapshotPath.wstring()));
        if (!snapshotFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return core::Result<std::filesystem::path>::failure(
                core::ErrorCode::FileWriteFailed,
                "Cannot snapshot source file " + sourcePath.generic_string()
                    + ": " + snapshotFile.errorString().toStdString());
        }

        while (!sourceFile.atEnd()) {
            if (stopToken.stop_requested()) {
                return cancelledFileOperation<std::filesystem::path>(
                    sourcePath,
                    "Source snapshot");
            }

            const QByteArray block = sourceFile.read(fileBlockSize);
            if (block.isEmpty()
                && sourceFile.error() != QFileDevice::NoError) {
                return core::Result<std::filesystem::path>::failure(
                    core::ErrorCode::FileOpenFailed,
                    "Cannot read source file " + sourcePath.generic_string()
                        + ": " + sourceFile.errorString().toStdString());
            }

            if (stopToken.stop_requested()) {
                return cancelledFileOperation<std::filesystem::path>(
                    sourcePath,
                    "Source snapshot");
            }

            if (snapshotFile.write(block) != block.size()) {
                return core::Result<std::filesystem::path>::failure(
                    core::ErrorCode::FileWriteFailed,
                    "Cannot snapshot source file "
                        + sourcePath.generic_string() + ": "
                        + snapshotFile.errorString().toStdString());
            }
        }

        if (sourceFile.error() != QFileDevice::NoError) {
            return core::Result<std::filesystem::path>::failure(
                core::ErrorCode::FileOpenFailed,
                "Cannot finish reading source file "
                    + sourcePath.generic_string() + ": "
                    + sourceFile.errorString().toStdString());
        }
        if (!snapshotFile.flush()) {
            return core::Result<std::filesystem::path>::failure(
                core::ErrorCode::FileWriteFailed,
                "Cannot finish snapshotting source file "
                    + sourcePath.generic_string() + ": "
                    + snapshotFile.errorString().toStdString());
        }
        if (stopToken.stop_requested()) {
            return cancelledFileOperation<std::filesystem::path>(
                sourcePath,
                "Source snapshot");
        }

        snapshotFile.close();
        sourceFile.close();
        snapshotGuard.release();
        return core::Result<std::filesystem::path>::success(snapshotPath);
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
    const std::string& expectedHash,
    const std::stop_token& stopToken)
{
    if (stopToken.stop_requested()) {
        return cancelledFileOperation<bool>(
            storedPath,
            "Stored file validation");
    }

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

    auto storedHashResult = calculateSha256(storedPath, stopToken);
    if (!storedHashResult) {
        if (storedHashResult.error().code == core::ErrorCode::Cancelled) {
            return core::Result<bool>::failure(storedHashResult.error());
        }
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
    const std::string& hash,
    const std::stop_token& stopToken)
{
    if (stopToken.stop_requested()) {
        return cancelledFileOperation<
            std::optional<std::filesystem::path>>(
                preferredRelativePath,
                "Stored file lookup");
    }

    auto preferredPathResult = resolveStoredPath(
        projectRoot,
        preferredRelativePath);
    if (!preferredPathResult) {
        return core::Result<std::optional<std::filesystem::path>>::failure(
            preferredPathResult.error());
    }

    auto preferredValidation = validateStoredPath(
        preferredPathResult.value(),
        hash,
        stopToken);
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
        if (stopToken.stop_requested()) {
            return cancelledFileOperation<
                std::optional<std::filesystem::path>>(
                    preferredRelativePath,
                    "Stored file lookup");
        }

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
                hash,
                stopToken);
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
    core::Signal signal,
    const std::stop_token& stopToken)
{
    auto existingFileResult = findStoredFile(
        projectRoot,
        preferredRelativePath,
        hash,
        stopToken);
    if (!existingFileResult) {
        return core::Result<repository::FileImport>::failure(
            storedFileErrorForSource(
                existingFileResult.error(),
                sourcePath));
    }

    if (existingFileResult.value()) {
        if (stopToken.stop_requested()) {
            return cancelledFileOperation<repository::FileImport>(
                sourcePath,
                "File import");
        }
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
    // This is the final cancellation point before the temporary snapshot is
    // atomically promoted to a permanent content-addressed archive.
    if (stopToken.stop_requested()) {
        return cancelledFileOperation<repository::FileImport>(
            sourcePath,
            "File import");
    }

    std::error_code error;
    std::filesystem::rename(snapshotPath, destinationPath, error);
    if (error) {
        // Another importer may have stored the same bytes just before rename.
        auto racedFileResult = findStoredFile(
            projectRoot,
            preferredRelativePath,
            hash,
            stopToken);
        if (!racedFileResult) {
            return core::Result<repository::FileImport>::failure(
                storedFileErrorForSource(
                    racedFileResult.error(),
                    sourcePath));
        }
        if (racedFileResult.value()) {
            if (stopToken.stop_requested()) {
                return cancelledFileOperation<repository::FileImport>(
                    sourcePath,
                    "File import");
            }
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
    const repository::CsvImportOptions& options,
    std::stop_token stopToken) const
{
    return io::previewCsv(sourcePath, options, 5, stopToken);
}

core::Result<repository::ImportPreview>
FilesystemFileRepository::previewWav(
    const std::filesystem::path& sourcePath,
    std::stop_token stopToken) const
{
    return io::previewWav(sourcePath, 5, stopToken);
}

core::Result<repository::FileImport> FilesystemFileRepository::importCsv(
    const std::filesystem::path& sourcePath,
    const repository::CsvImportOptions& options,
    std::stop_token stopToken)
{
    auto snapshotResult = createSourceSnapshot(
        projectRoot_,
        sourcePath,
        stopToken);
    if (!snapshotResult) {
        return core::Result<repository::FileImport>::failure(
            snapshotResult.error());
    }

    const std::filesystem::path snapshotPath =
        std::move(snapshotResult).value();
    const TemporarySnapshotGuard snapshotGuard(snapshotPath);

    auto hashResult = calculateSha256(snapshotPath, stopToken);
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

    auto signalResult = readCsv(snapshotPath, options, stopToken);
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
        std::move(signalResult).value(),
        stopToken);
}

core::Result<repository::FileImport> FilesystemFileRepository::importWav(
    const std::filesystem::path& sourcePath,
    const repository::WavImportOptions& options,
    std::stop_token stopToken)
{
    auto snapshotResult = createSourceSnapshot(
        projectRoot_,
        sourcePath,
        stopToken);
    if (!snapshotResult) {
        return core::Result<repository::FileImport>::failure(
            snapshotResult.error());
    }

    const std::filesystem::path snapshotPath =
        std::move(snapshotResult).value();
    const TemporarySnapshotGuard snapshotGuard(snapshotPath);

    auto hashResult = calculateSha256(snapshotPath, stopToken);
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

    auto signalResult = readWav(snapshotPath, options, stopToken);
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
        std::move(signalResult).value(),
        stopToken);
}

core::Result<bool> FilesystemFileRepository::validateStoredFile(
    const std::filesystem::path& storedRelativePath,
    const std::string& expectedHash,
    std::stop_token stopToken) const
{
    if (stopToken.stop_requested()) {
        return cancelledFileOperation<bool>(
            storedRelativePath,
            "Stored file validation");
    }

    auto pathResult = resolveStoredPath(projectRoot_, storedRelativePath);
    if (!pathResult) {
        return core::Result<bool>::failure(pathResult.error());
    }

    return validateStoredPath(
        pathResult.value(),
        expectedHash,
        stopToken);
}

core::Result<bool> FilesystemFileRepository::removeStoredFile(
    const std::filesystem::path& storedRelativePath,
    std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return cancelledFileOperation<bool>(
            storedRelativePath,
            "Stored file removal");
    }

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

    if (stopToken.stop_requested()) {
        return cancelledFileOperation<bool>(
            storedRelativePath,
            "Stored file removal");
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
