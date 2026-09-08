#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "signallab/app/import_service.hpp"
#include "signallab/core/error.hpp"
#include "signallab/io/csv_reader.hpp"
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

    signallab::app::ImportService service(project.root());
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
        entry.storedRelativePath.filename(),
        std::filesystem::path(entry.hash + ".csv"));
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

    signallab::app::ImportService service(project.root());

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
    {
        signallab::app::ImportService firstService(project.root());
        const auto firstImport = firstService.importCsv(sourcePath, options);
        ASSERT_TRUE(firstImport);
    }

    signallab::app::ImportService restartedService(project.root());
    const auto repeatedImport =
        restartedService.importCsv(copiedSourcePath, options);

    ASSERT_FALSE(repeatedImport);
    EXPECT_EQ(
        repeatedImport.error().code,
        signallab::core::ErrorCode::DuplicateFile);
    EXPECT_TRUE(restartedService.entries().empty());
    EXPECT_EQ(countStoredFiles(project.root()), 1U);
    EXPECT_EQ(countTemporaryFiles(project.root()), 0U);
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
    std::filesystem::path storedPath;
    {
        signallab::app::ImportService firstService(project.root());
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

    signallab::app::ImportService restartedService(project.root());
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
