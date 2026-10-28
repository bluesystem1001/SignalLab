#include "signallab/io/backup_settings.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace signallab::io {
namespace {

QString pathText(const std::filesystem::path& path)
{
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

core::Result<QByteArray> protectPassword(const QByteArray& input, bool decrypt)
{
#ifdef _WIN32
    if (input.size() > (decrypt ? 16384 : 4096)) {
        return core::Result<QByteArray>::failure(
            core::ErrorCode::InvalidParameter, "The saved password is too large.");
    }
    DATA_BLOB source{static_cast<DWORD>(input.size()),
                     reinterpret_cast<BYTE*>(const_cast<char*>(input.constData()))};
    DATA_BLOB destination{};
    const BOOL success = decrypt
        ? CryptUnprotectData(&source, nullptr, nullptr, nullptr, nullptr,
                             CRYPTPROTECT_UI_FORBIDDEN, &destination)
        : CryptProtectData(&source, L"SignalLab MariaDB backup", nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &destination);
    if (!success) {
        return core::Result<QByteArray>::failure(
            core::ErrorCode::FileOpenFailed,
            decrypt ? "Cannot decrypt the saved password for this Windows user; enter it again."
                    : "Windows could not encrypt the password for this user.");
    }
    QByteArray result(reinterpret_cast<const char*>(destination.pbData),
                      static_cast<qsizetype>(destination.cbData));
    SecureZeroMemory(destination.pbData, destination.cbData);
    LocalFree(destination.pbData);
    return core::Result<QByteArray>::success(std::move(result));
#else
    static_cast<void>(input);
    static_cast<void>(decrypt);
    return core::Result<QByteArray>::failure(core::ErrorCode::InvalidParameter,
        "Remembering a password requires Windows user encryption.");
#endif
}

}

std::filesystem::path defaultBackupSettingsPath()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#ifdef _WIN32
    return std::filesystem::path(directory.toStdWString()) / "SignalLab" / "mariadb-backup.json";
#else
    return std::filesystem::path(directory.toStdString()) / "SignalLab" / "mariadb-backup.json";
#endif
}

core::Result<BackupSettings> loadBackupSettings(const std::filesystem::path& path)
{
    BackupSettings settings;
    QFile file(pathText(path));
    if (!file.exists()) {
        return core::Result<BackupSettings>::success(std::move(settings));
    }
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) {
        return core::Result<BackupSettings>::failure(core::ErrorCode::FileOpenFailed,
            "Cannot read the local MariaDB backup settings.");
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return core::Result<BackupSettings>::failure(core::ErrorCode::InvalidParameter,
            "The local MariaDB backup settings are not valid JSON.");
    }
    const auto object = document.object();
    const auto port = object.value(QStringLiteral("port")).toInt(3306);
    if (object.value(QStringLiteral("formatVersion")).toInt() != 1
        || port < 1 || port > 65535) {
        return core::Result<BackupSettings>::failure(core::ErrorCode::InvalidParameter,
            "The MariaDB backup settings version or port is invalid.");
    }
    settings.connection.host = object.value(QStringLiteral("host")).toString(QStringLiteral("127.0.0.1")).toStdString();
    settings.connection.port = static_cast<unsigned int>(port);
    settings.connection.username = object.value(QStringLiteral("username")).toString(QStringLiteral("root")).toStdString();
    settings.connection.databaseName = object.value(QStringLiteral("databaseName")).toString(QStringLiteral("signallab_backup")).toStdString();
    settings.rememberPassword = object.value(QStringLiteral("rememberPassword")).toBool(false);
    if (settings.rememberPassword) {
        const auto encoded = object.value(QStringLiteral("protectedPassword")).toString().toLatin1();
        const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.isEmpty()) {
            return core::Result<BackupSettings>::failure(core::ErrorCode::InvalidParameter,
                "The encrypted MariaDB password is invalid; enter it again.");
        }
        auto password = protectPassword(decoded.decoded, true);
        if (!password) {
            return core::Result<BackupSettings>::failure(password.error());
        }
        settings.connection.password = password.value().toStdString();
    }
    return core::Result<BackupSettings>::success(std::move(settings));
}

core::Result<bool> saveBackupSettings(
    const std::filesystem::path& path, const BackupSettings& settings)
{
    if (path.empty() || settings.connection.host.empty() || settings.connection.username.empty()
        || settings.connection.databaseName.empty() || settings.connection.port == 0
        || settings.connection.port > 65535) {
        return core::Result<bool>::failure(core::ErrorCode::InvalidParameter,
            "Enter the MariaDB host, port, username and backup database name.");
    }
    QJsonObject object{
        {QStringLiteral("formatVersion"), 1},
        {QStringLiteral("host"), QString::fromStdString(settings.connection.host)},
        {QStringLiteral("port"), static_cast<int>(settings.connection.port)},
        {QStringLiteral("username"), QString::fromStdString(settings.connection.username)},
        {QStringLiteral("databaseName"), QString::fromStdString(settings.connection.databaseName)},
        {QStringLiteral("rememberPassword"), settings.rememberPassword}};
    if (settings.rememberPassword) {
        auto encrypted = protectPassword(QByteArray::fromStdString(settings.connection.password), false);
        if (!encrypted) {
            return core::Result<bool>::failure(encrypted.error());
        }
        object.insert(QStringLiteral("protectedPassword"), QString::fromLatin1(encrypted.value().toBase64()));
    }
    const auto filename = pathText(path);
    if (!QDir().mkpath(QFileInfo(filename).absolutePath())) {
        return core::Result<bool>::failure(core::ErrorCode::FileWriteFailed,
            "Cannot create the local backup settings directory.");
    }
    QSaveFile file(filename);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(bytes) != bytes.size() || !file.commit()) {
        return core::Result<bool>::failure(core::ErrorCode::FileWriteFailed,
            "Cannot save the local MariaDB backup settings.");
    }
    return core::Result<bool>::success(true);
}

}
