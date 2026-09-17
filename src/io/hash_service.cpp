#include "signallab/io/hash_service.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QFileDevice>
#include <QString>

#include <cstddef>
#include <stop_token>
#include <string>

#include "signallab/core/error.hpp"

namespace signallab::io {
namespace {

constexpr qint64 hashBlockSize = 64 * 1024;

std::string pathForMessage(const std::filesystem::path& path)
{
    return path.generic_string();
}

core::Result<std::string> cancelledHash(
    const std::filesystem::path& path)
{
    return core::Result<std::string>::failure(
        core::ErrorCode::Cancelled,
        "File hashing was cancelled: " + pathForMessage(path));
}

}  // namespace

core::Result<std::string> calculateSha256(
    const std::filesystem::path& path,
    std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return cancelledHash(path);
    }

    QFile file(QString::fromStdWString(path.wstring()));

    if (!file.exists()) {
        return core::Result<std::string>::failure(
            core::ErrorCode::FileNotFound,
            "File not found: " + pathForMessage(path));
    }

    if (!file.open(QIODevice::ReadOnly)) {
        return core::Result<std::string>::failure(
            core::ErrorCode::FileOpenFailed,
            "Cannot open file for hashing: " + pathForMessage(path)
                + " (" + file.errorString().toStdString() + ")");
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);

    while (!file.atEnd()) {
        if (stopToken.stop_requested()) {
            return cancelledHash(path);
        }

        const QByteArray block = file.read(hashBlockSize);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) {
            return core::Result<std::string>::failure(
                core::ErrorCode::FileOpenFailed,
                "Cannot read file while hashing: " + pathForMessage(path)
                    + " (" + file.errorString().toStdString() + ")");
        }

        if (stopToken.stop_requested()) {
            return cancelledHash(path);
        }

        hash.addData(block);
    }

    if (file.error() != QFileDevice::NoError) {
        return core::Result<std::string>::failure(
            core::ErrorCode::FileOpenFailed,
            "Cannot finish reading file for hashing: " + pathForMessage(path)
                + " (" + file.errorString().toStdString() + ")");
    }

    if (stopToken.stop_requested()) {
        return cancelledHash(path);
    }

    return core::Result<std::string>::success(
        hash.result().toHex().toStdString());
}

}  // namespace signallab::io
