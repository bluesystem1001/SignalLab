#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "signallab/io/csv_reader.hpp"

namespace signallab::io {
namespace {

class TemporaryCsvFile {
public:
    explicit TemporaryCsvFile(const std::string& contents)
    {
        const auto timestamp =
            std::chrono::high_resolution_clock::now()
                .time_since_epoch()
                .count();
        const auto* testInformation =
            ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string testName = testInformation == nullptr
            ? "unknown"
            : std::string(testInformation->test_suite_name())
                + "_" + testInformation->name();

        path_ = std::filesystem::temp_directory_path()
            / ("signallab_csv_reader_test_" + testName
               + "_" + std::to_string(timestamp)
               + "_" + std::to_string(nextId_.fetch_add(1))
               + ".csv");

        std::ofstream output(path_, std::ios::binary);
        if (!output) {
            throw std::runtime_error("Could not create a temporary CSV test file.");
        }

        output << contents;
    }

    ~TemporaryCsvFile()
    {
        std::error_code ignoredError;
        std::filesystem::remove(path_, ignoredError);
    }

    TemporaryCsvFile(const TemporaryCsvFile&) = delete;
    TemporaryCsvFile& operator=(const TemporaryCsvFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    inline static std::atomic<unsigned int> nextId_{0};
    std::filesystem::path path_;
};

void expectLocation(
    const core::Error& error,
    const std::filesystem::path& path,
    std::size_t row,
    std::size_t column)
{
    EXPECT_NE(error.message.find(path.string()), std::string::npos);
    EXPECT_NE(
        error.message.find("row " + std::to_string(row)),
        std::string::npos);
    EXPECT_NE(
        error.message.find("column " + std::to_string(column)),
        std::string::npos);
}

TEST(CsvReaderTest, ReadsCommaSeparatedFileWithHeaderAndQuotes)
{
    const TemporaryCsvFile file(
        "time,value,note\n"
        "0.000,1.5,plain\n"
        "0.001,-2.0,\"contains, comma\"\n"
        "0.002,4.0,\"escaped \"\"quote\"\"\"\n");

    CsvImportOptions options;
    options.metadata = {"CSV signal", "V", "test file"};

    auto previewResult = previewCsv(file.path(), options, 3);
    ASSERT_TRUE(previewResult);

    const ImportPreview& preview = previewResult.value();
    EXPECT_EQ(preview.sampleCount, 3U);
    EXPECT_EQ(preview.columnCount, 3U);
    EXPECT_EQ(preview.channelCount, 1U);
    ASSERT_TRUE(preview.sampleRate.has_value());
    EXPECT_NEAR(*preview.sampleRate, 1000.0, 1.0e-9);
    EXPECT_EQ(preview.columnNames[1], "value");
    ASSERT_EQ(preview.rows.size(), 3U);
    EXPECT_EQ(preview.rows[1][2], "contains, comma");
    EXPECT_EQ(preview.rows[2][2], "escaped \"quote\"");

    auto signalResult = readCsv(file.path(), options);
    ASSERT_TRUE(signalResult);

    const core::Signal& signal = signalResult.value();
    EXPECT_EQ(signal.sampleCount(), 3U);
    EXPECT_NEAR(signal.sampleRate(), 1000.0, 1.0e-9);
    EXPECT_DOUBLE_EQ(signal.samples()[0], 1.5);
    EXPECT_DOUBLE_EQ(signal.samples()[1], -2.0);
    EXPECT_DOUBLE_EQ(signal.samples()[2], 4.0);
    EXPECT_EQ(signal.metadata().name, "CSV signal");
}

TEST(CsvReaderTest, ReadsTabSeparatedFileWithoutHeader)
{
    const TemporaryCsvFile file(
        "1.0\tfirst\n"
        "-1.0\tsecond\n");

    CsvImportOptions options;
    options.delimiter = CsvDelimiter::Tab;
    options.hasHeader = false;
    options.valueColumn = 0;
    options.timeColumn.reset();
    options.sampleRateIfNoTimeColumn = 8000.0;

    auto previewResult = previewCsv(file.path(), options);
    ASSERT_TRUE(previewResult);
    EXPECT_EQ(previewResult.value().columnNames[0], "Column 1");
    EXPECT_EQ(previewResult.value().rows.size(), 2U);
    ASSERT_TRUE(previewResult.value().sampleRate.has_value());
    EXPECT_DOUBLE_EQ(*previewResult.value().sampleRate, 8000.0);

    auto signalResult = readCsv(file.path(), options);
    ASSERT_TRUE(signalResult);
    EXPECT_DOUBLE_EQ(signalResult.value().sampleRate(), 8000.0);
    EXPECT_DOUBLE_EQ(signalResult.value().samples()[1], -1.0);
}

TEST(CsvReaderTest, ReportsBadNumberAfterThePreviewLimit)
{
    const TemporaryCsvFile file(
        "time,value\n"
        "0.000,1.0\n"
        "0.001,not-a-number\n");

    CsvImportOptions options;
    auto result = previewCsv(file.path(), options, 1);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidCsv);
    expectLocation(result.error(), file.path(), 3, 2);
}

TEST(CsvReaderTest, PreviewCountsAllRowsButKeepsOnlyTheRequestedRows)
{
    const TemporaryCsvFile file(
        "time,value\n"
        "0.000,1.0\n"
        "0.001,2.0\n"
        "0.002,3.0\n"
        "0.003,4.0\n");

    CsvImportOptions options;
    auto result = previewCsv(file.path(), options, 2);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().sampleCount, 4U);
    EXPECT_EQ(result.value().rows.size(), 2U);
    EXPECT_EQ(result.value().rows[0][1], "1.0");
    EXPECT_EQ(result.value().rows[1][1], "2.0");
}

TEST(CsvReaderTest, AcceptsSixDecimalTimestampsAt44100Hz)
{
    constexpr std::size_t sampleCount = 100;
    constexpr double expectedSampleRate = 44100.0;

    std::ostringstream contents;
    contents << "time,value\n" << std::fixed << std::setprecision(6);
    for (std::size_t index = 0; index < sampleCount; ++index) {
        contents
            << static_cast<double>(index) / expectedSampleRate
            << ',' << static_cast<double>(index)
            << '\n';
    }

    const TemporaryCsvFile file(contents.str());
    CsvImportOptions options;
    auto result = readCsv(file.path(), options);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().sampleCount(), sampleCount);
    EXPECT_NEAR(result.value().sampleRate(), expectedSampleRate, 5.0);
}

TEST(CsvReaderTest, RejectsUnequallySpacedTimeValues)
{
    const TemporaryCsvFile file(
        "time,value\n"
        "0.000,1.0\n"
        "0.001,2.0\n"
        "0.003,3.0\n");

    CsvImportOptions options;
    auto result = readCsv(file.path(), options);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidCsv);
    expectLocation(result.error(), file.path(), 4, 1);
}

TEST(CsvReaderTest, RejectsTimeValuesThatDoNotIncrease)
{
    const TemporaryCsvFile file(
        "time,value\n"
        "0.000,1.0\n"
        "0.001,2.0\n"
        "0.001,3.0\n");

    CsvImportOptions options;
    auto result = readCsv(file.path(), options);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidCsv);
    expectLocation(result.error(), file.path(), 4, 1);
}

TEST(CsvReaderTest, RejectsUnterminatedQuotedField)
{
    const TemporaryCsvFile file(
        "time,value\n"
        "0.000,\"1.0\n");

    CsvImportOptions options;
    auto result = previewCsv(file.path(), options);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidCsv);
    expectLocation(result.error(), file.path(), 2, 2);
}

TEST(CsvReaderTest, RejectsRowsWithDifferentColumnCounts)
{
    const TemporaryCsvFile file(
        "time,value\n"
        "0.000,1.0\n"
        "0.001,2.0,extra\n");

    CsvImportOptions options;
    auto result = previewCsv(file.path(), options);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidCsv);
    expectLocation(result.error(), file.path(), 3, 3);
}

TEST(CsvReaderTest, RejectsAHeaderWithoutSampleRows)
{
    const TemporaryCsvFile file("time,value\n");

    CsvImportOptions options;
    auto result = previewCsv(file.path(), options);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidCsv);
    expectLocation(result.error(), file.path(), 2, 1);
}

}  // namespace
}  // namespace signallab::io
