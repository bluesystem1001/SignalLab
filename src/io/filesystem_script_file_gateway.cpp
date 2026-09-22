#include "signallab/io/filesystem_script_file_gateway.hpp"

#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QString>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "signallab/core/error.hpp"

namespace signallab::io {
namespace {

constexpr std::size_t outputBlockSize = 64U * 1024U;
constexpr int maximumTemporaryFileAttempts = 32;
std::atomic<std::uint64_t> temporaryFileCounter{0};

std::string pathForMessage(const std::filesystem::path& path)
{
    return path.generic_string();
}

core::Result<bool> invalidRelativePath(
    const std::filesystem::path& path,
    std::string reason)
{
    return core::Result<bool>::failure(
        core::ErrorCode::InvalidParameter,
        "Invalid script file path '" + pathForMessage(path)
            + "': " + std::move(reason));
}

core::Result<bool> validateRelativeFilePath(
    const std::filesystem::path& path)
{
    if (path.empty()) {
        return invalidRelativePath(path, "the path is empty.");
    }
    if (path.is_absolute()
        || path.has_root_name()
        || path.has_root_directory()) {
        return invalidRelativePath(path, "an absolute path is not allowed.");
    }
    if (!path.has_filename()
        || path.filename() == "."
        || path.filename() == "..") {
        return invalidRelativePath(path, "a file name is required.");
    }

    for (const auto& component : path) {
        if (component == "..") {
            return invalidRelativePath(
                path,
                "parent-directory traversal is not allowed.");
        }

        const auto native = component.native();
        if (std::find(native.begin(), native.end(), '\0') != native.end()) {
            return invalidRelativePath(path, "NUL characters are not allowed.");
        }
#ifdef _WIN32
        if (native.find(L':') != std::wstring::npos) {
            return invalidRelativePath(
                path,
                "Windows alternate data streams are not allowed.");
        }
#endif
    }

    return core::Result<bool>::success(true);
}

bool isWithinRoot(
    const std::filesystem::path& path,
    const std::filesystem::path& root)
{
    auto pathPart = path.begin();
    auto rootPart = root.begin();
    while (rootPart != root.end()) {
        if (pathPart == path.end() || *pathPart != *rootPart) {
            return false;
        }
        ++pathPart;
        ++rootPart;
    }
    return true;
}

bool traversesFilesystemLink(
    const std::filesystem::path& root,
    const std::filesystem::path& relativePath)
{
    std::filesystem::path current = root;
    for (const auto& component : relativePath) {
        if (component == ".") {
            continue;
        }

        current /= component;
        const QFileInfo information(
            QString::fromStdWString(current.wstring()));
        if (information.isSymbolicLink() || information.isJunction()) {
            return true;
        }
        if (!information.exists()) {
            break;
        }
    }
    return false;
}

core::Result<std::filesystem::path> canonicalDirectory(
    const std::filesystem::path& configuredRoot,
    bool createIfMissing,
    core::ErrorCode errorCode,
    const char* description)
{
    std::error_code error;
    if (createIfMissing) {
        std::filesystem::create_directories(configuredRoot, error);
        if (error) {
            return core::Result<std::filesystem::path>::failure(
                errorCode,
                std::string("Cannot create the script ") + description
                    + " root '" + pathForMessage(configuredRoot)
                    + "': " + error.message());
        }
    }

    const bool exists = std::filesystem::exists(configuredRoot, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            errorCode,
            std::string("Cannot inspect the script ") + description
                + " root '" + pathForMessage(configuredRoot)
                + "': " + error.message());
    }
    if (!exists) {
        return core::Result<std::filesystem::path>::failure(
            errorCode,
            std::string("The script ") + description + " root does not exist: "
                + pathForMessage(configuredRoot));
    }
    if (!std::filesystem::is_directory(configuredRoot, error) || error) {
        const std::string detail = error
            ? error.message()
            : "the configured root is not a directory";
        return core::Result<std::filesystem::path>::failure(
            errorCode,
            std::string("Cannot use the script ") + description + " root '"
                + pathForMessage(configuredRoot) + "': " + detail);
    }

    const std::filesystem::path canonicalRoot =
        std::filesystem::canonical(configuredRoot, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            errorCode,
            std::string("Cannot resolve the script ") + description
                + " root '" + pathForMessage(configuredRoot)
                + "': " + error.message());
    }
    return core::Result<std::filesystem::path>::success(canonicalRoot);
}

core::Result<bool> cancelledWrite(const std::filesystem::path& path)
{
    return core::Result<bool>::failure(
        core::ErrorCode::Cancelled,
        "Script output was cancelled: " + pathForMessage(path));
}

core::Result<bool> validateUtf8(
    std::string_view text,
    const std::filesystem::path& path,
    std::stop_token stopToken)
{
    std::size_t index = 0;
    while (index < text.size()) {
        if ((index % outputBlockSize) == 0U
            && stopToken.stop_requested()) {
            return cancelledWrite(path);
        }

        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        if (first <= 0x7fU) {
            length = 1;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            length = 2;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4;
        } else {
            return core::Result<bool>::failure(
                core::ErrorCode::InvalidParameter,
                "Script output is not valid UTF-8: " + pathForMessage(path));
        }

        if (length > text.size() - index) {
            return core::Result<bool>::failure(
                core::ErrorCode::InvalidParameter,
                "Script output is not valid UTF-8: " + pathForMessage(path));
        }
        for (std::size_t continuation = 1;
             continuation < length;
             ++continuation) {
            const auto byte = static_cast<unsigned char>(
                text[index + continuation]);
            if ((byte & 0xc0U) != 0x80U) {
                return core::Result<bool>::failure(
                    core::ErrorCode::InvalidParameter,
                    "Script output is not valid UTF-8: "
                        + pathForMessage(path));
            }
        }

        if (length == 3) {
            const auto second = static_cast<unsigned char>(text[index + 1U]);
            if ((first == 0xe0U && second < 0xa0U)
                || (first == 0xedU && second > 0x9fU)) {
                return core::Result<bool>::failure(
                    core::ErrorCode::InvalidParameter,
                    "Script output is not valid UTF-8: "
                        + pathForMessage(path));
            }
        } else if (length == 4) {
            const auto second = static_cast<unsigned char>(text[index + 1U]);
            if ((first == 0xf0U && second < 0x90U)
                || (first == 0xf4U && second > 0x8fU)) {
                return core::Result<bool>::failure(
                    core::ErrorCode::InvalidParameter,
                    "Script output is not valid UTF-8: "
                        + pathForMessage(path));
            }
        }

        index += length;
    }

    if (stopToken.stop_requested()) {
        return cancelledWrite(path);
    }
    return core::Result<bool>::success(true);
}

class TemporaryOutputGuard {
public:
    explicit TemporaryOutputGuard(std::filesystem::path path)
        : path_(std::move(path))
    {
    }

    ~TemporaryOutputGuard()
    {
        if (path_.empty()) {
            return;
        }
        std::error_code ignoredError;
        std::filesystem::remove(path_, ignoredError);
    }

    TemporaryOutputGuard(const TemporaryOutputGuard&) = delete;
    TemporaryOutputGuard& operator=(const TemporaryOutputGuard&) = delete;

    void release() noexcept
    {
        path_.clear();
    }

private:
    std::filesystem::path path_;
};

std::filesystem::path temporaryOutputPath(
    const std::filesystem::path& parent)
{
    const auto clockValue = std::chrono::high_resolution_clock::now()
        .time_since_epoch()
        .count();
    const auto counterValue = temporaryFileCounter.fetch_add(
        1,
        std::memory_order_relaxed);
    return parent / (".signallab-script-output-"
        + std::to_string(clockValue) + "-"
        + std::to_string(counterValue) + ".tmp");
}

}  // namespace

FilesystemScriptFileGateway::FilesystemScriptFileGateway(
    std::filesystem::path readRoot,
    std::filesystem::path outputRoot)
    : readRoot_(std::move(readRoot)),
      outputRoot_(std::move(outputRoot))
{
}

core::Result<std::filesystem::path>
FilesystemScriptFileGateway::resolveInput(
    const std::filesystem::path& relativePath) const
{
    const auto relativeCheck = validateRelativeFilePath(relativePath);
    if (!relativeCheck) {
        return core::Result<std::filesystem::path>::failure(
            relativeCheck.error());
    }

    auto rootResult = canonicalDirectory(
        readRoot_,
        false,
        core::ErrorCode::FileOpenFailed,
        "input");
    if (!rootResult) {
        return core::Result<std::filesystem::path>::failure(
            rootResult.error());
    }
    const std::filesystem::path root = std::move(rootResult).value();

    if (traversesFilesystemLink(root, relativePath)) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::InvalidParameter,
            "Script input must not traverse a symbolic link or reparse "
            "point: " + pathForMessage(relativePath));
    }

    std::error_code error;
    const std::filesystem::path candidate = root / relativePath;
    const auto status = std::filesystem::symlink_status(candidate, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            return core::Result<std::filesystem::path>::failure(
                core::ErrorCode::FileNotFound,
                "Script input was not found: "
                    + pathForMessage(relativePath));
        }
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileOpenFailed,
            "Cannot inspect script input '" + pathForMessage(relativePath)
                + "': " + error.message());
    }
    if (status.type() == std::filesystem::file_type::not_found) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileNotFound,
            "Script input was not found: " + pathForMessage(relativePath));
    }

    const std::filesystem::path resolved =
        std::filesystem::canonical(candidate, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileOpenFailed,
            "Cannot resolve script input '" + pathForMessage(relativePath)
                + "': " + error.message());
    }
    if (!isWithinRoot(resolved, root)) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::InvalidParameter,
            "Script input escapes its configured root: "
                + pathForMessage(relativePath));
    }
    if (!std::filesystem::is_regular_file(resolved, error) || error) {
        const std::string detail = error
            ? error.message()
            : "the input is not a regular file";
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileOpenFailed,
            "Cannot use script input '" + pathForMessage(relativePath)
                + "': " + detail);
    }

    return core::Result<std::filesystem::path>::success(resolved);
}

core::Result<std::filesystem::path>
FilesystemScriptFileGateway::writeOutput(
    const std::filesystem::path& relativePath,
    std::string_view utf8Text,
    std::stop_token stopToken)
{
    const auto relativeCheck = validateRelativeFilePath(relativePath);
    if (!relativeCheck) {
        return core::Result<std::filesystem::path>::failure(
            relativeCheck.error());
    }
    if (stopToken.stop_requested()) {
        return core::Result<std::filesystem::path>::failure(
            cancelledWrite(relativePath).error());
    }

    const auto utf8Check = validateUtf8(utf8Text, relativePath, stopToken);
    if (!utf8Check) {
        return core::Result<std::filesystem::path>::failure(
            utf8Check.error());
    }

    auto rootResult = canonicalDirectory(
        outputRoot_,
        true,
        core::ErrorCode::FileWriteFailed,
        "output");
    if (!rootResult) {
        return core::Result<std::filesystem::path>::failure(
            rootResult.error());
    }
    const std::filesystem::path root = std::move(rootResult).value();

    if (traversesFilesystemLink(root, relativePath)) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::InvalidParameter,
            "Script output must not traverse a symbolic link or reparse "
            "point: " + pathForMessage(relativePath));
    }

    const std::filesystem::path lexicalParent =
        (root / relativePath).parent_path();
    std::error_code error;
    const std::filesystem::path prospectiveParent =
        std::filesystem::weakly_canonical(lexicalParent, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot resolve the script output folder for '"
                + pathForMessage(relativePath) + "': " + error.message());
    }
    if (!isWithinRoot(prospectiveParent, root)) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::InvalidParameter,
            "Script output escapes its configured root: "
                + pathForMessage(relativePath));
    }

    std::filesystem::create_directories(lexicalParent, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot create the script output folder for '"
                + pathForMessage(relativePath) + "': " + error.message());
    }
    const std::filesystem::path parent =
        std::filesystem::canonical(lexicalParent, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileWriteFailed,
            "Cannot resolve the script output folder for '"
                + pathForMessage(relativePath) + "': " + error.message());
    }
    if (!isWithinRoot(parent, root)) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::InvalidParameter,
            "Script output escapes its configured root: "
                + pathForMessage(relativePath));
    }
    if (traversesFilesystemLink(root, relativePath.parent_path())) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::InvalidParameter,
            "Script output must not traverse a symbolic link or reparse "
            "point: " + pathForMessage(relativePath));
    }

    const std::filesystem::path target = parent / relativePath.filename();
    const auto targetStatus = std::filesystem::symlink_status(target, error);
    bool targetIsMissing = false;
    if (error) {
        if (error != std::errc::no_such_file_or_directory) {
            return core::Result<std::filesystem::path>::failure(
                core::ErrorCode::FileWriteFailed,
                "Cannot inspect script output '"
                    + pathForMessage(relativePath)
                    + "': " + error.message());
        }
        targetIsMissing = true;
        error.clear();
    }
    if (!targetIsMissing
        && targetStatus.type() != std::filesystem::file_type::not_found) {
        return core::Result<std::filesystem::path>::failure(
            core::ErrorCode::FileWriteFailed,
            "Script output already exists and will not be replaced: "
                + pathForMessage(relativePath));
    }

    for (int attempt = 0;
         attempt < maximumTemporaryFileAttempts;
         ++attempt) {
        if (stopToken.stop_requested()) {
            return core::Result<std::filesystem::path>::failure(
                cancelledWrite(relativePath).error());
        }

        const std::filesystem::path temporaryPath =
            temporaryOutputPath(parent);
        QFile file(QString::fromStdWString(temporaryPath.wstring()));
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            std::error_code statusError;
            const auto temporaryStatus = std::filesystem::symlink_status(
                temporaryPath,
                statusError);
            if (!statusError
                && temporaryStatus.type()
                    != std::filesystem::file_type::not_found) {
                continue;
            }
            return core::Result<std::filesystem::path>::failure(
                core::ErrorCode::FileWriteFailed,
                "Cannot create a temporary script output for '"
                    + pathForMessage(relativePath) + "': "
                    + file.errorString().toStdString());
        }

        TemporaryOutputGuard temporaryGuard(temporaryPath);
        std::size_t offset = 0;
        while (offset < utf8Text.size()) {
            if (stopToken.stop_requested()) {
                file.close();
                return core::Result<std::filesystem::path>::failure(
                    cancelledWrite(relativePath).error());
            }

            const std::size_t byteCount = std::min(
                outputBlockSize,
                utf8Text.size() - offset);
            const qint64 written = file.write(
                utf8Text.data() + offset,
                static_cast<qint64>(byteCount));
            if (written != static_cast<qint64>(byteCount)) {
                const std::string detail = file.errorString().toStdString();
                file.close();
                return core::Result<std::filesystem::path>::failure(
                    core::ErrorCode::FileWriteFailed,
                    "Cannot write script output '"
                        + pathForMessage(relativePath) + "': " + detail);
            }
            offset += byteCount;

            if (stopToken.stop_requested()) {
                file.close();
                return core::Result<std::filesystem::path>::failure(
                    cancelledWrite(relativePath).error());
            }
        }

        if (!file.flush()) {
            const std::string detail = file.errorString().toStdString();
            file.close();
            return core::Result<std::filesystem::path>::failure(
                core::ErrorCode::FileWriteFailed,
                "Cannot flush script output '"
                    + pathForMessage(relativePath) + "': " + detail);
        }
        file.close();

        if (stopToken.stop_requested()) {
            return core::Result<std::filesystem::path>::failure(
                cancelledWrite(relativePath).error());
        }

        // Publishing with a hard-link gives us one atomic "create if absent"
        // operation on every filesystem supported by std::filesystem. A
        // check followed by rename is insufficient on POSIX because rename
        // is allowed to replace a destination created by a concurrent writer.
        std::error_code publishError;
        std::filesystem::create_hard_link(
            temporaryPath,
            target,
            publishError);
        if (publishError) {
            return core::Result<std::filesystem::path>::failure(
                core::ErrorCode::FileWriteFailed,
                "Cannot publish script output '"
                    + pathForMessage(relativePath) + "': "
                    + publishError.message());
        }

        // The target now names the complete file. The guard removes only the
        // private temporary name and therefore cannot invalidate the result.
        return core::Result<std::filesystem::path>::success(target);
    }

    return core::Result<std::filesystem::path>::failure(
        core::ErrorCode::FileWriteFailed,
        "Cannot reserve a temporary file for script output: "
            + pathForMessage(relativePath));
}

}  // namespace signallab::io
