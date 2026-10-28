#include "signallab/app/backup_service.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace signallab::app {
namespace {

std::string uniqueId()
{
    std::random_device random;
    std::array<unsigned int, 16> bytes{};
    for (auto& byte : bytes) {
        byte = random() & 255U;
    }
    bytes[6] = (bytes[6] & 15U) | 64U;
    bytes[8] = (bytes[8] & 63U) | 128U;
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output << '-';
        }
        output << std::setw(2) << bytes[index];
    }
    return output.str();
}

std::string utcNow()
{
    const auto timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &timestamp);
#else
    gmtime_r(&timestamp, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

core::Result<bool> validate(const repository::BackupConnectionOptions& options)
{
    const auto validIdentifier = [](const std::string& name) {
        return !name.empty() && name.size() <= 64
            && ((name.front() >= 'a' && name.front() <= 'z')
                || (name.front() >= 'A' && name.front() <= 'Z'))
            && std::all_of(name.begin(), name.end(), [](unsigned char character) {
                return (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9') || character == '_';
            });
    };
    if (options.host.empty() || options.host.size() > 255 || options.username.empty()
        || options.username.size() > 128 || options.port == 0 || options.port > 65535
        || !validIdentifier(options.databaseName)) {
        return core::Result<bool>::failure(core::ErrorCode::InvalidParameter,
            "Enter a valid host, port and username. Backup database names allow only letters, digits and underscores.");
    }
    return core::Result<bool>::success(true);
}

class StagingDirectory {
public:
    explicit StagingDirectory(std::filesystem::path path) : path_(std::move(path)) {}
    ~StagingDirectory()
    {
        if (owned_) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    core::Result<bool> create()
    {
        std::error_code error;
        owned_ = std::filesystem::create_directory(path_, error);
        if (!owned_ || error) {
            return core::Result<bool>::failure(core::ErrorCode::FileWriteFailed,
                "Cannot create a unique staging directory for the backup.");
        }
        return core::Result<bool>::success(true);
    }
    const std::filesystem::path& path() const { return path_; }
    void release() { owned_ = false; }
private:
    std::filesystem::path path_;
    bool owned_{false};
};

core::Error cancelled()
{
    return {core::ErrorCode::Cancelled, "The backup operation was cancelled."};
}

}

BackupService::BackupService(std::shared_ptr<repository::BackupSource> source,
                             std::shared_ptr<repository::BackupRepository> repository)
    : source_(std::move(source)), repository_(std::move(repository))
{
}

core::Result<std::string> BackupService::testConnection(
    const repository::BackupConnectionOptions& options)
{
    const auto valid = validate(options);
    if (!valid) {
        return core::Result<std::string>::failure(valid.error());
    }
    if (!repository_) {
        return core::Result<std::string>::failure(core::ErrorCode::InvalidParameter,
            "The backup repository is unavailable.");
    }
    return repository_->testConnection(options);
}

core::Result<std::vector<repository::BackupRecord>> BackupService::list(
    const repository::BackupConnectionOptions& options)
{
    const auto valid = validate(options);
    if (!valid) {
        return core::Result<std::vector<repository::BackupRecord>>::failure(valid.error());
    }
    if (!repository_) {
        return core::Result<std::vector<repository::BackupRecord>>::failure(
            core::ErrorCode::InvalidParameter, "The backup repository is unavailable.");
    }
    return repository_->list(options);
}

core::Result<repository::BackupRecord> BackupService::backup(
    const repository::BackupConnectionOptions& options,
    const std::string& label,
    std::stop_token stopToken,
    repository::BackupProgress progress)
{
    const auto valid = validate(options);
    if (!valid) {
        return core::Result<repository::BackupRecord>::failure(valid.error());
    }
    if (!source_ || !repository_ || label.size() > 1000) {
        return core::Result<repository::BackupRecord>::failure(core::ErrorCode::InvalidParameter,
            "The backup service is unavailable or the backup label is too long.");
    }
    if (stopToken.stop_requested()) {
        return core::Result<repository::BackupRecord>::failure(cancelled());
    }
    std::error_code pathError;
    const auto temporaryRoot = std::filesystem::temp_directory_path(pathError);
    if (pathError) {
        return core::Result<repository::BackupRecord>::failure(core::ErrorCode::FileWriteFailed,
            "Cannot find a writable temporary directory.");
    }
    StagingDirectory staging(temporaryRoot / ("signallab-backup-" + uniqueId()));
    const auto created = staging.create();
    if (!created) {
        return core::Result<repository::BackupRecord>::failure(created.error());
    }
    auto snapshot = source_->snapshot(staging.path(), stopToken);
    if (!snapshot) {
        return core::Result<repository::BackupRecord>::failure(snapshot.error());
    }
    if (stopToken.stop_requested()) {
        return core::Result<repository::BackupRecord>::failure(cancelled());
    }
    repository::BackupRecord record;
    record.id = uniqueId();
    record.createdUtc = utcNow();
    record.label = label.empty() ? "SignalLab project backup" : label;
    record.sourceName = "SignalLab";
    record.fileCount = snapshot.value().size();
    constexpr std::uint64_t maximumBackupBytes = 8ULL * 1024 * 1024 * 1024;
    if (record.fileCount == 0 || record.fileCount > 10000) {
        return core::Result<repository::BackupRecord>::failure(core::ErrorCode::InvalidParameter,
            "The project snapshot contains an invalid number of files.");
    }
    for (const auto& file : snapshot.value()) {
        if (file.byteSize > maximumBackupBytes - record.totalBytes) {
            return core::Result<repository::BackupRecord>::failure(core::ErrorCode::InvalidParameter,
                "The project exceeds the 8 GiB backup size limit.");
        }
        record.totalBytes += file.byteSize;
    }
    return repository_->upload(options, std::move(record), snapshot.value(), stopToken, std::move(progress));
}

core::Result<std::filesystem::path> BackupService::restoreCopy(
    const repository::BackupConnectionOptions& options,
    const std::string& backupId,
    const std::filesystem::path& destination,
    std::stop_token stopToken,
    repository::BackupProgress progress)
{
    const auto valid = validate(options);
    if (!valid) {
        return core::Result<std::filesystem::path>::failure(valid.error());
    }
    if (!source_ || !repository_ || backupId.empty() || destination.empty()
        || destination.filename().empty() || destination.filename() == "."
        || destination.filename() == "..") {
        return core::Result<std::filesystem::path>::failure(core::ErrorCode::InvalidParameter,
            "Choose a new directory for the restored backup copy.");
    }
    if (stopToken.stop_requested()) {
        return core::Result<std::filesystem::path>::failure(cancelled());
    }
    std::error_code error;
    const auto absoluteDestination = std::filesystem::absolute(destination, error).lexically_normal();
    if (error || std::filesystem::exists(absoluteDestination, error)
        || error || std::filesystem::is_symlink(absoluteDestination, error)) {
        return core::Result<std::filesystem::path>::failure(core::ErrorCode::FileWriteFailed,
            "The restore destination already exists or cannot be inspected. Choose a new directory.");
    }
    error.clear();
    const auto parent = std::filesystem::canonical(absoluteDestination.parent_path(), error);
    if (error || !std::filesystem::is_directory(parent, error) || error) {
        return core::Result<std::filesystem::path>::failure(core::ErrorCode::FileWriteFailed,
            "The parent directory for the restore copy does not exist.");
    }
    const auto finalPath = parent / absoluteDestination.filename();
    StagingDirectory staging(parent / (".signallab-restore-" + uniqueId()));
    const auto created = staging.create();
    if (!created) {
        return core::Result<std::filesystem::path>::failure(created.error());
    }
    auto downloaded = repository_->download(options, backupId, staging.path(), stopToken, std::move(progress));
    if (!downloaded) {
        return core::Result<std::filesystem::path>::failure(downloaded.error());
    }
    const auto verified = source_->verify(staging.path(), downloaded.value(), stopToken);
    if (!verified) {
        return core::Result<std::filesystem::path>::failure(verified.error());
    }
    if (stopToken.stop_requested()) {
        return core::Result<std::filesystem::path>::failure(cancelled());
    }
    if (std::filesystem::exists(finalPath, error) || error) {
        return core::Result<std::filesystem::path>::failure(core::ErrorCode::FileWriteFailed,
            "The restore destination was created by another operation; no files were overwritten.");
    }
    std::filesystem::rename(staging.path(), finalPath, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(core::ErrorCode::FileWriteFailed,
            "Cannot move the verified backup into the destination directory.");
    }
    staging.release();
    return core::Result<std::filesystem::path>::success(finalPath);
}

}
