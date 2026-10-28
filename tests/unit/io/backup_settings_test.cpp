#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "signallab/io/backup_settings.hpp"

namespace signallab::io {
namespace {

class BackupSettingsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(temporary_.isValid());
    }

    std::filesystem::path path() const
    {
        return std::filesystem::path(temporary_.path().toStdWString()) / "backup-settings.json";
    }

    void write(const QByteArray& bytes)
    {
        QFile file(QString::fromStdWString(path().wstring()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(bytes), bytes.size());
    }

    QByteArray read() const
    {
        QFile file(QString::fromStdWString(path().wstring()));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    }

    QTemporaryDir temporary_;
};

TEST_F(BackupSettingsTest, UsesDefaultsAndPersistsConnectionWithoutPlaintextPassword)
{
    const auto missing = loadBackupSettings(path());
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing.value().connection.host, "127.0.0.1");
    EXPECT_EQ(missing.value().connection.port, 3306U);
    EXPECT_EQ(missing.value().connection.username, "root");
    EXPECT_EQ(missing.value().connection.databaseName, "signallab_backup");
    EXPECT_TRUE(missing.value().connection.password.empty());
    EXPECT_FALSE(missing.value().rememberPassword);
    EXPECT_FALSE(std::filesystem::exists(path()));

    BackupSettings settings;
    settings.connection.host = "localhost";
    settings.connection.port = 3307;
    settings.connection.username = "backup_user";
    settings.connection.databaseName = "backup_test";
    settings.connection.password = "test-only-private-password";
    ASSERT_TRUE(saveBackupSettings(path(), settings));
    const auto bytes = read();
    EXPECT_FALSE(bytes.contains(QByteArray::fromStdString(settings.connection.password)));
    const auto object = QJsonDocument::fromJson(bytes).object();
    EXPECT_FALSE(object.contains("password"));
    EXPECT_FALSE(object.contains("protectedPassword"));
    const auto loaded = loadBackupSettings(path());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded.value().connection.host, settings.connection.host);
    EXPECT_EQ(loaded.value().connection.port, settings.connection.port);
    EXPECT_EQ(loaded.value().connection.username, settings.connection.username);
    EXPECT_EQ(loaded.value().connection.databaseName, settings.connection.databaseName);
    EXPECT_TRUE(loaded.value().connection.password.empty());
    EXPECT_FALSE(loaded.value().rememberPassword);
}

TEST_F(BackupSettingsTest, RememberedPasswordUsesUserEncryptionAndCanBeForgotten)
{
    BackupSettings settings;
    settings.rememberPassword = true;
    settings.connection.password = "test-only-DPAPI-password-\xe5\xaf\x86\xe7\xa0\x81";
    const auto saved = saveBackupSettings(path(), settings);
#ifdef _WIN32
    ASSERT_TRUE(saved) << saved.error().message;
    const auto bytes = read();
    EXPECT_FALSE(bytes.contains(QByteArray::fromStdString(settings.connection.password)));
    const auto object = QJsonDocument::fromJson(bytes).object();
    EXPECT_FALSE(object.contains("password"));
    EXPECT_FALSE(object.value("protectedPassword").toString().isEmpty());
    const auto loaded = loadBackupSettings(path());
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_TRUE(loaded.value().rememberPassword);
    EXPECT_EQ(loaded.value().connection.password, settings.connection.password);

    settings.rememberPassword = false;
    ASSERT_TRUE(saveBackupSettings(path(), settings));
    EXPECT_FALSE(QJsonDocument::fromJson(read()).object().contains("protectedPassword"));
    const auto forgotten = loadBackupSettings(path());
    ASSERT_TRUE(forgotten);
    EXPECT_TRUE(forgotten.value().connection.password.empty());
#else
    ASSERT_FALSE(saved);
    EXPECT_FALSE(std::filesystem::exists(path()));
#endif
}

TEST_F(BackupSettingsTest, RejectsMalformedAndCorruptedEncryptedPasswords)
{
    const std::vector<QString> invalidPasswords{
        QString(), QStringLiteral("!invalid base64!"),
        QString::fromLatin1(QByteArray("not a DPAPI encrypted blob").toBase64())
    };
    for (const auto& encrypted : invalidPasswords) {
        QJsonObject object{
            {"formatVersion", 1}, {"rememberPassword", true},
            {"protectedPassword", encrypted}
        };
        write(QJsonDocument(object).toJson());
        const auto loaded = loadBackupSettings(path());
        EXPECT_FALSE(loaded);
    }
}

TEST_F(BackupSettingsTest, RejectsMalformedJsonUnsupportedVersionsAndInvalidPorts)
{
    const std::vector<QByteArray> invalidDocuments{
        "{", "[]", "{}", R"({"formatVersion":2})",
        R"({"formatVersion":1,"port":0})",
        R"({"formatVersion":1,"port":65536})"
    };
    for (const auto& document : invalidDocuments) {
        write(document);
        const auto loaded = loadBackupSettings(path());
        ASSERT_FALSE(loaded) << document.constData();
        EXPECT_EQ(loaded.error().code, core::ErrorCode::InvalidParameter);
    }
}

TEST_F(BackupSettingsTest, ReportsIoFailuresAndPreservesPreviouslySavedSettings)
{
    BackupSettings settings;
    ASSERT_TRUE(saveBackupSettings(path(), settings));
    const auto original = read();
    settings.connection.port = 0;
    EXPECT_FALSE(saveBackupSettings(path(), settings));
    EXPECT_EQ(read(), original);

    settings.connection.port = 3306;
    const auto blockedParent = path() / "cannot-create.json";
    const auto failedWrite = saveBackupSettings(blockedParent, settings);
    ASSERT_FALSE(failedWrite);
    EXPECT_EQ(failedWrite.error().code, core::ErrorCode::FileWriteFailed);
    EXPECT_EQ(read(), original);

    const auto directory = std::filesystem::path(temporary_.path().toStdWString());
    EXPECT_FALSE(loadBackupSettings(directory));
    write(QByteArray(65537, 'x'));
    const auto tooLarge = loadBackupSettings(path());
    ASSERT_FALSE(tooLarge);
    EXPECT_EQ(tooLarge.error().code, core::ErrorCode::FileOpenFailed);
}

}
}
