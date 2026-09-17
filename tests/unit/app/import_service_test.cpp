#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "signallab/app/import_service.hpp"
#include "signallab/core/error.hpp"
#include "signallab/core/signal.hpp"
#include "signallab/database/database_connection.hpp"
#include "signallab/database/migration_runner.hpp"
#include "signallab/database/sqlite_signal_repository.hpp"
#include "signallab/io/csv_reader.hpp"
#include "signallab/io/filesystem_file_repository.hpp"
#include "signallab/io/hash_service.hpp"
#include "signallab/io/import_types.hpp"

namespace {

class TemporaryProject {
public:
    TemporaryProject()
    {
        const auto uniqueNumber =
            std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path()
            / ("signallab-import-test-" + std::to_string(uniqueNumber));
        std::filesystem::create_directories(root_);
    }

    ~TemporaryProject()
    {
        std::error_code ignoredError;
        std::filesystem::remove_all(root_, ignoredError);
    }

    TemporaryProject(const TemporaryProject&) = delete;
    TemporaryProject& operator=(const TemporaryProject&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept
    {
        return root_;
    }

private:
    std::filesystem::path root_;
};

class ImportTestContext {
public:
    explicit ImportTestContext(const std::filesystem::path& projectRoot)
        : database(projectRoot / "data" / "test.sqlite3"),
          migrationRunner(database),
          fileRepository(projectRoot),
          signalRepository(database),
          service(fileRepository, signalRepository)
    {
    }

    bool initialize()
    {
        auto openResult = database.open();
        if (!openResult) {
            errorMessage = openResult.error().message;
            return false;
        }

        auto migrationResult = migrationRunner.run(
            std::filesystem::path(SIGNALLAB_SOURCE_DIR) / "migrations");
        if (!migrationResult) {
            errorMessage = migrationResult.error().message;
            return false;
        }
        return true;
    }

    signallab::database::DatabaseConnection database;
    signallab::database::MigrationRunner migrationRunner;
    signallab::io::FilesystemFileRepository fileRepository;
    signallab::database::SqliteSignalRepository signalRepository;
    signallab::app::ImportService service;
    std::string errorMessage;
};

class FailingSignalRepository final
    : public signallab::repository::SignalRepository {
public:
    explicit FailingSignalRepository(bool lookupFails = false)
        : lookupFails_(lookupFails)
    {
    }

    signallab::core::Result<signallab::repository::StoredSignal> saveSignal(
        signallab::repository::SignalRegistration) override
    {
        return signallab::core::Result<
            signallab::repository::StoredSignal>::failure(
            signallab::core::ErrorCode::DatabaseQueryFailed,
            "Injected database save failure.");
    }

    signallab::core::Result<
        std::optional<signallab::repository::StoredSignal>>
    findBySourceKey(const std::string&) override
    {
        if (lookupFails_) {
            return signallab::core::Result<
                std::optional<signallab::repository::StoredSignal>>::failure(
                signallab::core::ErrorCode::DatabaseQueryFailed,
                "Injected source lookup failure.");
        }
        return signallab::core::Result<
            std::optional<signallab::repository::StoredSignal>>::success(
            std::nullopt);
    }

    signallab::core::Result<
        std::optional<signallab::repository::StoredSignal>>
    findByRecordId(std::int64_t, std::stop_token) override
    {
        return signallab::core::Result<
            std::optional<signallab::repository::StoredSignal>>::success(
            std::nullopt);
    }

    signallab::core::Result<
        std::optional<signallab::repository::StoredSignal>>
    findByFileHash(const std::string&) override
    {
        if (lookupFails_) {
            return signallab::core::Result<
                std::optional<signallab::repository::StoredSignal>>::failure(
                signallab::core::ErrorCode::DatabaseQueryFailed,
                "Injected file lookup failure.");
        }
        return signallab::core::Result<
            std::optional<signallab::repository::StoredSignal>>::success(
            std::nullopt);
    }

    signallab::core::Result<
        std::vector<signallab::repository::StoredSignal>>
    listSignals() override
    {
        return signallab::core::Result<
            std::vector<signallab::repository::StoredSignal>>::success({});
    }

    signallab::core::Result<bool> removeSignal(std::int64_t) override
    {
        return signallab::core::Result<bool>::success(false);
    }

private:
    bool lookupFails_;
};

std::size_t countStoredFiles(const std::filesystem::path& projectRoot)
{
    const auto importsDirectory = projectRoot / "data" / "imports";
    if (!std::filesystem::exists(importsDirectory)) {
        return 0;
    }

    std::size_t count = 0;
    for (const auto& item :
         std::filesystem::directory_iterator(importsDirectory)) {
        if (item.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

std::size_t countTemporaryFiles(const std::filesystem::path& projectRoot)
{
    const auto importsDirectory = projectRoot / "data" / "imports";
    if (!std::filesystem::exists(importsDirectory)) {
        return 0;
    }

    std::size_t count = 0;
    for (const auto& item :
         std::filesystem::directory_iterator(importsDirectory)) {
        if (item.path().filename().string().ends_with(".tmp")) {
            ++count;
        }
    }
    return count;
}

signallab::io::CsvImportOptions makeCsvOptions()
{
    signallab::io::CsvImportOptions options;
    options.delimiter = signallab::io::CsvDelimiter::Comma;
    options.hasHeader = true;
    options.valueColumn = 1;
    options.timeColumn = 0;
    options.metadata.name = "Imported CSV";
    options.metadata.unit = "V";
    options.metadata.source = "Unit test";
    return options;
}

}  // namespace

TEST(HashServiceTest, CalculatesTheExpectedSha256Digest)
{
    TemporaryProject project;
    const auto sourcePath = project.root() / "abc.txt";

    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        ASSERT_TRUE(sourceFile.is_open());
        sourceFile << "abc";
        ASSERT_TRUE(sourceFile.good());
    }

    const auto hashResult = signallab::io::calculateSha256(sourcePath);

    ASSERT_TRUE(hashResult);
    EXPECT_EQ(
        hashResult.value(),
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");
}

TEST(ImportServiceTest, RestoresOneIdempotentGeneratedSignal)
{
    TemporaryProject project;
    ImportTestContext context(project.root());
    ASSERT_TRUE(context.initialize()) << context.errorMessage;

    auto firstSignal = signallab::core::Signal::create(
        {0.0, 1.0, 0.0, -1.0},
        1'000.0,
        signallab::core::SignalMetadata{
            "Generated test",
            "V",
            "unit test"});
    ASSERT_TRUE(firstSignal);
    const auto firstAdd = context.service.addGeneratedSignal(
        std::move(firstSignal).value(),
        "generated:test:sine");
    ASSERT_TRUE(firstAdd);
    EXPECT_EQ(firstAdd.value(), 0U);

    signallab::app::ImportService restartedService(
        context.fileRepository,
        context.signalRepository);
    const auto restoreResult = restartedService.restoreSignals();
    ASSERT_TRUE(restoreResult);
    EXPECT_EQ(restoreResult.value(), 1U);

    auto repeatedSignal = signallab::core::Signal::create(
        {0.0, 1.0, 0.0, -1.0},
        1'000.0,
        signallab::core::SignalMetadata{
            "Generated test",
            "V",
            "unit test"});
    ASSERT_TRUE(repeatedSignal);
    const auto repeatedAdd = restartedService.addGeneratedSignal(
        std::move(repeatedSignal).value(),
        "generated:test:sine");

    ASSERT_TRUE(repeatedAdd);
    EXPECT_EQ(repeatedAdd.value(), 0U);
    EXPECT_EQ(restartedService.entries().size(), 1U);
    EXPECT_TRUE(restartedService.entries().front().hash.empty());
    EXPECT_EQ(
        restartedService.entries().front().sampleHash.size(),
        64U);
}

TEST(ImportServiceTest, RepeatedCsvImportKeepsOnlyOneStoredFile)
{
    TemporaryProject project;
    const auto sourcePath = project.root() / "example.csv";

    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        ASSERT_TRUE(sourceFile.is_open());
        sourceFile
            << "time,value\n"
            << "0.000,0.0\n"
            << "0.001,1.0\n"
            << "0.002,0.0\n"
            << "0.003,-1.0\n";
        ASSERT_TRUE(sourceFile.good());
    }

    ImportTestContext context(project.root());
    ASSERT_TRUE(context.initialize()) << context.errorMessage;
    auto& service = context.service;
    const auto options = makeCsvOptions();

    const auto previewResult = service.previewCsv(sourcePath, options);
    ASSERT_TRUE(previewResult);
    EXPECT_EQ(previewResult.value().sampleCount, 4U);

    const auto firstImport = service.importCsv(sourcePath, options);
    ASSERT_TRUE(firstImport);
    EXPECT_EQ(firstImport.value(), 0U);

    ASSERT_EQ(service.entries().size(), 1U);
    const auto& entry = service.entries().front();
    EXPECT_EQ(entry.displayName, "Imported CSV");
    EXPECT_EQ(entry.sourcePath, sourcePath);
    EXPECT_FALSE(entry.hash.empty());
    EXPECT_EQ(
        entry.parametersJson,
        "{\"delimiter\":\"comma\",\"has_header\":true,"
        "\"metadata\":{\"name\":\"Imported CSV\","
        "\"source\":\"Unit test\",\"unit\":\"V\"},"
        "\"sample_rate_if_no_time_column\":48000,"
        "\"time_column\":0,\"value_column\":1}");
    EXPECT_EQ(
        entry.sourceKey,
        "file-sha256:" + entry.hash
            + "|options-json:{\"delimiter\":\"comma\","
              "\"has_header\":true,"
              "\"sample_rate_if_no_time_column\":48000,"
              "\"time_column\":0,\"value_column\":1}");
    EXPECT_EQ(
        entry.storedRelativePath.filename(),
        std::filesystem::path(entry.hash + ".source"));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        project.root() / entry.storedRelativePath));
    EXPECT_EQ(countStoredFiles(project.root()), 1U);
    EXPECT_EQ(countTemporaryFiles(project.root()), 0U);

    const auto storedHash = signallab::io::calculateSha256(
        project.root() / entry.storedRelativePath);
    ASSERT_TRUE(storedHash);
    EXPECT_EQ(storedHash.value(), entry.hash);

    const auto storedSignal = signallab::io::readCsv(
        project.root() / entry.storedRelativePath,
        options);
    ASSERT_TRUE(storedSignal);
    ASSERT_EQ(
        storedSignal.value().sampleCount(),
        entry.signal.sampleCount());
    for (std::size_t index = 0;
         index < entry.signal.sampleCount();
         ++index) {
        EXPECT_DOUBLE_EQ(
            storedSignal.value().samples()[index],
            entry.signal.samples()[index]);
    }

    const auto secondImport = service.importCsv(sourcePath, options);
    ASSERT_FALSE(secondImport);
    EXPECT_EQ(
        secondImport.error().code,
        signallab::core::ErrorCode::DuplicateFile);
    EXPECT_EQ(service.entries().size(), 1U);
    EXPECT_EQ(countStoredFiles(project.root()), 1U);
    EXPECT_EQ(countTemporaryFiles(project.root()), 0U);
}

TEST(ImportServiceTest, ReusesOneFileForDifferentLogicalSignals)
{
    TemporaryProject project;
    const auto sourcePath = project.root() / "two-columns.csv";

    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        ASSERT_TRUE(sourceFile.is_open());
        sourceFile
            << "time,left,right\n"
            << "0.000,1.0,10.0\n"
            << "0.001,2.0,20.0\n";
    }

    ImportTestContext context(project.root());
    ASSERT_TRUE(context.initialize()) << context.errorMessage;

    auto leftOptions = makeCsvOptions();
    leftOptions.valueColumn = 1;
    leftOptions.metadata.name = "Left column";
    const auto leftImport = context.service.importCsv(
        sourcePath,
        leftOptions);
    ASSERT_TRUE(leftImport);

    auto rightOptions = leftOptions;
    rightOptions.valueColumn = 2;
    rightOptions.metadata.name = "Right column";
    const auto rightImport = context.service.importCsv(
        sourcePath,
        rightOptions);
    ASSERT_TRUE(rightImport);

    ASSERT_EQ(context.service.entries().size(), 2U);
    EXPECT_EQ(countStoredFiles(project.root()), 1U);
    EXPECT_EQ(
        context.service.entries()[0].storedRelativePath,
        context.service.entries()[1].storedRelativePath);
    EXPECT_EQ(
        context.service.entries()[0].hash,
        context.service.entries()[1].hash);
    EXPECT_DOUBLE_EQ(
        context.service.entries()[0].signal.samples()[0],
        1.0);
    EXPECT_DOUBLE_EQ(
        context.service.entries()[1].signal.samples()[0],
        10.0);
}

TEST(ImportServiceTest, EscapesMetadataInCanonicalParameters)
{
    TemporaryProject project;
    const auto sourcePath = project.root() / "escaped-metadata.csv";
    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        ASSERT_TRUE(sourceFile.is_open());
        sourceFile
            << "time,value\n"
            << "0.000,1.0\n"
            << "0.001,2.0\n";
    }

    ImportTestContext context(project.root());
    ASSERT_TRUE(context.initialize()) << context.errorMessage;
    auto options = makeCsvOptions();
    options.metadata.name = "A \"quoted\" name";
    options.metadata.source = "folder\\file\nline";
    options.metadata.unit = "V\tR";

    const auto importResult = context.service.importCsv(sourcePath, options);

    ASSERT_TRUE(importResult);
    ASSERT_EQ(context.service.entries().size(), 1U);
    EXPECT_EQ(
        context.service.entries().front().parametersJson,
        "{\"delimiter\":\"comma\",\"has_header\":true,"
        "\"metadata\":{\"name\":\"A \\\"quoted\\\" name\","
        "\"source\":\"folder\\\\file\\nline\","
        "\"unit\":\"V\\tR\"},"
        "\"sample_rate_if_no_time_column\":48000,"
        "\"time_column\":0,\"value_column\":1}");
}

TEST(ImportServiceTest, InvalidCsvDoesNotLeaveAStoredOrTemporaryFile)
{
    TemporaryProject project;
    const auto sourcePath = project.root() / "invalid.csv";

    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        ASSERT_TRUE(sourceFile.is_open());
        sourceFile
            << "time,value\n"
            << "0.000,not-a-number\n"
            << "0.001,1.0\n";
        ASSERT_TRUE(sourceFile.good());
    }

    ImportTestContext context(project.root());
    ASSERT_TRUE(context.initialize()) << context.errorMessage;
    auto& service = context.service;

    const auto importResult = service.importCsv(
        sourcePath,
        makeCsvOptions());

    ASSERT_FALSE(importResult);
    EXPECT_EQ(
        importResult.error().code,
        signallab::core::ErrorCode::InvalidCsv);
    EXPECT_NE(
        importResult.error().message.find(sourcePath.string()),
        std::string::npos);
    EXPECT_EQ(
        importResult.error().message.find(".signallab-import-"),
        std::string::npos);
    EXPECT_TRUE(service.entries().empty());
    EXPECT_EQ(countStoredFiles(project.root()), 0U);
    EXPECT_EQ(countTemporaryFiles(project.root()), 0U);
}

TEST(ImportServiceTest, DatabaseFailureRemovesANewArchive)
{
    TemporaryProject project;
    const auto sourcePath = project.root() / "database-failure.csv";

    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        ASSERT_TRUE(sourceFile.is_open());
        sourceFile
            << "time,value\n"
            << "0.000,1.0\n"
            << "0.001,2.0\n";
    }

    signallab::io::FilesystemFileRepository fileRepository(project.root());
    FailingSignalRepository signalRepository;
    signallab::app::ImportService service(
        fileRepository,
        signalRepository);

    const auto importResult = service.importCsv(
        sourcePath,
        makeCsvOptions());

    ASSERT_FALSE(importResult);
    EXPECT_EQ(
        importResult.error().code,
        signallab::core::ErrorCode::DatabaseQueryFailed);
    EXPECT_TRUE(service.entries().empty());
    EXPECT_EQ(countStoredFiles(project.root()), 0U);
    EXPECT_EQ(countTemporaryFiles(project.root()), 0U);
}

TEST(ImportServiceTest, KeepsANewArchiveWhenReferencesCannotBeChecked)
{
    TemporaryProject project;
    const auto sourcePath = project.root() / "uncertain-database.csv";
    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        ASSERT_TRUE(sourceFile.is_open());
        sourceFile
            << "time,value\n"
            << "0.000,1.0\n"
            << "0.001,2.0\n";
    }

    signallab::io::FilesystemFileRepository fileRepository(project.root());
    FailingSignalRepository signalRepository(true);
    signallab::app::ImportService service(
        fileRepository,
        signalRepository);

    const auto importResult = service.importCsv(
        sourcePath,
        makeCsvOptions());

    ASSERT_FALSE(importResult);
    EXPECT_NE(
        importResult.error().message.find("archive was kept"),
        std::string::npos);
    EXPECT_TRUE(service.entries().empty());
    EXPECT_EQ(countStoredFiles(project.root()), 1U);
    EXPECT_EQ(countTemporaryFiles(project.root()), 0U);
}

TEST(FilesystemFileRepositoryTest, RefusesToDeleteOutsideImportsDirectory)
{
    TemporaryProject project;
    const auto outsidePath = project.root() / "outside.txt";
    {
        std::ofstream outsideFile(outsidePath, std::ios::binary);
        ASSERT_TRUE(outsideFile.is_open());
        outsideFile << "must remain";
    }

    signallab::io::FilesystemFileRepository repository(project.root());
    const auto traversalResult = repository.removeStoredFile(
        std::filesystem::path("data/imports/../../outside.txt"));
    const auto absoluteResult = repository.removeStoredFile(outsidePath);

    ASSERT_FALSE(traversalResult);
    EXPECT_EQ(
        traversalResult.error().code,
        signallab::core::ErrorCode::InvalidParameter);
    ASSERT_FALSE(absoluteResult);
    EXPECT_EQ(
        absoluteResult.error().code,
        signallab::core::ErrorCode::InvalidParameter);
    EXPECT_TRUE(std::filesystem::is_regular_file(outsidePath));
}

TEST(FilesystemFileRepositoryTest, ReusesALegacyArchiveName)
{
    TemporaryProject project;
    const auto sourcePath = project.root() / "legacy.csv";
    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        ASSERT_TRUE(sourceFile.is_open());
        sourceFile
            << "time,value\n"
            << "0.000,1.0\n"
            << "0.001,2.0\n";
    }

    const auto hashResult = signallab::io::calculateSha256(sourcePath);
    ASSERT_TRUE(hashResult);
    const auto importsDirectory = project.root() / "data" / "imports";
    std::filesystem::create_directories(importsDirectory);
    const auto legacyPath = importsDirectory
        / (hashResult.value() + ".csv");
    std::filesystem::copy_file(sourcePath, legacyPath);

    signallab::io::FilesystemFileRepository repository(project.root());
    const auto importResult = repository.importCsv(
        sourcePath,
        makeCsvOptions());

    ASSERT_TRUE(importResult);
    EXPECT_FALSE(importResult.value().createdStoredFile);
    EXPECT_EQ(
        importResult.value().storedRelativePath.filename(),
        std::filesystem::path(hashResult.value() + ".csv"));
    EXPECT_TRUE(std::filesystem::exists(legacyPath));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        project.root() / importResult.value().storedRelativePath));
}

TEST(ImportServiceTest, RestoresADatabaseEntryUsingALegacyArchiveName)
{
    TemporaryProject project;
    const auto sourcePath = project.root() / "legacy-restart.csv";
    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        ASSERT_TRUE(sourceFile.is_open());
        sourceFile
            << "time,value\n"
            << "0.000,1.0\n"
            << "0.001,2.0\n";
    }

    const auto hashResult = signallab::io::calculateSha256(sourcePath);
    ASSERT_TRUE(hashResult);
    const auto importsDirectory = project.root() / "data" / "imports";
    std::filesystem::create_directories(importsDirectory);
    const auto legacyPath = importsDirectory
        / (hashResult.value() + ".csv");
    ASSERT_TRUE(std::filesystem::copy_file(sourcePath, legacyPath));

    ImportTestContext context(project.root());
    ASSERT_TRUE(context.initialize()) << context.errorMessage;
    const auto importResult = context.service.importCsv(
        sourcePath,
        makeCsvOptions());
    ASSERT_TRUE(importResult) << importResult.error().message;
    ASSERT_EQ(context.service.entries().size(), 1U);
    EXPECT_EQ(
        context.service.entries().front().storedRelativePath.filename(),
        legacyPath.filename());

    signallab::app::ImportService restartedService(
        context.fileRepository,
        context.signalRepository);
    const auto restoreResult = restartedService.restoreSignals();

    ASSERT_TRUE(restoreResult) << restoreResult.error().message;
    EXPECT_EQ(restoreResult.value(), 1U);
    ASSERT_EQ(restartedService.entries().size(), 1U);
    EXPECT_EQ(
        restartedService.entries().front().storedRelativePath.filename(),
        legacyPath.filename());
    EXPECT_TRUE(std::filesystem::is_regular_file(legacyPath));
}

TEST(ImportServiceTest, DetectsTheSameContentAfterServiceRestart)
{
    TemporaryProject project;
    const auto sourcePath = project.root() / "restart.csv";
    const auto copiedSourcePath = project.root() / "restart-copy.txt";

    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        ASSERT_TRUE(sourceFile.is_open());
        sourceFile
            << "time,value\n"
            << "0.000,0.0\n"
            << "0.001,1.0\n";
    }

    ASSERT_TRUE(std::filesystem::copy_file(sourcePath, copiedSourcePath));

    const auto options = makeCsvOptions();
    ImportTestContext context(project.root());
    ASSERT_TRUE(context.initialize()) << context.errorMessage;
    {
        signallab::app::ImportService firstService(
            context.fileRepository,
            context.signalRepository);
        const auto firstImport = firstService.importCsv(sourcePath, options);
        ASSERT_TRUE(firstImport);
    }

    signallab::app::ImportService restartedService(
        context.fileRepository,
        context.signalRepository);
    const auto restoreResult = restartedService.restoreSignals();
    ASSERT_TRUE(restoreResult) << restoreResult.error().message;
    EXPECT_EQ(restoreResult.value(), 1U);
    auto copiedOptions = options;
    copiedOptions.metadata.name = "Renamed copy";
    copiedOptions.metadata.source = copiedSourcePath.string();
    const auto repeatedImport =
        restartedService.importCsv(copiedSourcePath, copiedOptions);

    ASSERT_FALSE(repeatedImport);
    EXPECT_EQ(
        repeatedImport.error().code,
        signallab::core::ErrorCode::DuplicateFile);
    EXPECT_EQ(restartedService.entries().size(), 1U);
    EXPECT_EQ(countStoredFiles(project.root()), 1U);
    EXPECT_EQ(countTemporaryFiles(project.root()), 0U);
}

TEST(ImportServiceTest, ReimportRestoresAMissingArchivedFile)
{
    TemporaryProject project;
    const auto sourcePath = project.root() / "missing-archive.csv";
    const auto renamedSourcePath = project.root() / "renamed-source.txt";

    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        ASSERT_TRUE(sourceFile.is_open());
        sourceFile
            << "time,value\n"
            << "0.000,0.0\n"
            << "0.001,1.0\n";
    }
    std::filesystem::copy_file(sourcePath, renamedSourcePath);

    ImportTestContext context(project.root());
    ASSERT_TRUE(context.initialize()) << context.errorMessage;
    const auto options = makeCsvOptions();
    const auto firstImport = context.service.importCsv(sourcePath, options);
    ASSERT_TRUE(firstImport);

    const auto archivedPath = project.root()
        / context.service.entries().front().storedRelativePath;
    ASSERT_TRUE(std::filesystem::remove(archivedPath));
    ASSERT_FALSE(std::filesystem::exists(archivedPath));

    auto renamedOptions = options;
    renamedOptions.metadata.name = "Renamed source";
    renamedOptions.metadata.source = renamedSourcePath.string();
    const auto repeatedImport = context.service.importCsv(
        renamedSourcePath,
        renamedOptions);

    ASSERT_FALSE(repeatedImport);
    EXPECT_EQ(
        repeatedImport.error().code,
        signallab::core::ErrorCode::DuplicateFile);
    EXPECT_TRUE(std::filesystem::is_regular_file(archivedPath));
    EXPECT_EQ(countStoredFiles(project.root()), 1U);
}

TEST(ImportServiceTest, RejectsADamagedContentAddressedFile)
{
    TemporaryProject project;
    const auto sourcePath = project.root() / "damaged.csv";

    {
        std::ofstream sourceFile(sourcePath, std::ios::binary);
        ASSERT_TRUE(sourceFile.is_open());
        sourceFile
            << "time,value\n"
            << "0.000,0.0\n"
            << "0.001,1.0\n";
    }

    const auto options = makeCsvOptions();
    ImportTestContext context(project.root());
    ASSERT_TRUE(context.initialize()) << context.errorMessage;
    std::filesystem::path storedPath;
    {
        signallab::app::ImportService firstService(
            context.fileRepository,
            context.signalRepository);
        const auto firstImport = firstService.importCsv(sourcePath, options);
        ASSERT_TRUE(firstImport);
        storedPath = project.root()
            / firstService.entries().front().storedRelativePath;
    }

    {
        std::ofstream storedFile(
            storedPath,
            std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(storedFile.is_open());
        storedFile << "damaged";
    }

    signallab::app::ImportService restartedService(
        context.fileRepository,
        context.signalRepository);
    const auto restoreResult = restartedService.restoreSignals();
    ASSERT_FALSE(restoreResult);
    EXPECT_EQ(
        restoreResult.error().code,
        signallab::core::ErrorCode::FileWriteFailed);
    EXPECT_TRUE(restartedService.entries().empty());

    const auto importResult =
        restartedService.importCsv(sourcePath, options);

    ASSERT_FALSE(importResult);
    EXPECT_EQ(
        importResult.error().code,
        signallab::core::ErrorCode::FileWriteFailed);
    EXPECT_NE(
        importResult.error().message.find("damaged"),
        std::string::npos);
    EXPECT_TRUE(restartedService.entries().empty());
    EXPECT_EQ(countStoredFiles(project.root()), 1U);
    EXPECT_EQ(countTemporaryFiles(project.root()), 0U);
}
