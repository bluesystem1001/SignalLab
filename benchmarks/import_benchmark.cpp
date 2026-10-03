#include "signallab/io/csv_reader.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr std::size_t kTargetFileBytes = 10U * 1024U * 1024U;
constexpr std::size_t kWarmupIterations = 1U;
constexpr std::size_t kIterations = 5U;
constexpr double kSampleRate = 48'000.0;
constexpr char kHeader[] = "value\n";
constexpr char kSampleRow[] = "1.0000000000000000\n";

class TemporaryCsv final {
public:
    explicit TemporaryCsv(std::filesystem::path path)
        : path_(std::move(path))
    {
    }

    ~TemporaryCsv()
    {
        std::error_code ignoredError;
        std::filesystem::remove(path_, ignoredError);
    }

    TemporaryCsv(const TemporaryCsv&) = delete;
    TemporaryCsv& operator=(const TemporaryCsv&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    [[nodiscard]] bool remove(std::string& errorMessage)
    {
        std::error_code error;
        const bool removed = std::filesystem::remove(path_, error);
        if (error) {
            errorMessage = error.message();
            return false;
        }

        if (!removed) {
            errorMessage = "the generated CSV disappeared before cleanup";
            return false;
        }

        return true;
    }

private:
    std::filesystem::path path_;
};

struct TimingSummary {
    double averageMilliseconds;
    double minimumMilliseconds;
};

TimingSummary summarize(const std::vector<double>& elapsedMilliseconds)
{
    const double total = std::accumulate(
        elapsedMilliseconds.begin(),
        elapsedMilliseconds.end(),
        0.0);

    return TimingSummary{
        total / static_cast<double>(elapsedMilliseconds.size()),
        *std::min_element(
            elapsedMilliseconds.begin(),
            elapsedMilliseconds.end())
    };
}

std::filesystem::path makeTemporaryPath()
{
    std::error_code error;
    const std::filesystem::path temporaryDirectory =
        std::filesystem::temp_directory_path(error);
    if (error) {
        return {};
    }

    const auto timestamp = std::chrono::high_resolution_clock::now()
        .time_since_epoch()
        .count();
    return temporaryDirectory
        / ("signallab_stage8_import_benchmark_"
           + std::to_string(timestamp) + ".csv");
}

bool generateCsv(
    const std::filesystem::path& path,
    std::size_t rowCount,
    std::string& errorMessage)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        errorMessage = "could not create the temporary CSV";
        return false;
    }

    output.write(kHeader, static_cast<std::streamsize>(sizeof(kHeader) - 1U));
    for (std::size_t row = 0; row < rowCount; ++row) {
        output.write(
            kSampleRow,
            static_cast<std::streamsize>(sizeof(kSampleRow) - 1U));
    }

    output.close();
    if (!output) {
        errorMessage = "failed while writing the temporary CSV";
        return false;
    }

    return true;
}

bool validateSignal(
    const signallab::core::Signal& signal,
    std::size_t expectedSampleCount,
    double& checksum,
    std::string& errorMessage)
{
    if (signal.sampleCount() != expectedSampleCount) {
        errorMessage = "the parsed sample count does not match the generated CSV";
        return false;
    }

    if (signal.sampleRate() != kSampleRate) {
        errorMessage = "the parsed sample rate does not match the import options";
        return false;
    }

    checksum = std::accumulate(
        signal.samples().begin(),
        signal.samples().end(),
        0.0);
    const double expectedChecksum = static_cast<double>(expectedSampleCount);
    if (checksum != expectedChecksum) {
        errorMessage = "the parsed sample checksum does not match the generated CSV";
        return false;
    }

    return true;
}

}  // namespace

int main()
{
    const std::filesystem::path temporaryPath = makeTemporaryPath();
    if (temporaryPath.empty()) {
        std::cerr << "CSV benchmark failed: could not resolve the temporary directory.\n";
        return 1;
    }

    TemporaryCsv temporaryCsv(temporaryPath);
    constexpr std::size_t headerBytes = sizeof(kHeader) - 1U;
    constexpr std::size_t rowBytes = sizeof(kSampleRow) - 1U;
    constexpr std::size_t sampleCount =
        (kTargetFileBytes - headerBytes + rowBytes - 1U) / rowBytes;

    std::string errorMessage;
    if (!generateCsv(temporaryCsv.path(), sampleCount, errorMessage)) {
        std::cerr << "CSV benchmark failed: " << errorMessage << ".\n";
        return 1;
    }

    std::error_code fileSizeError;
    const std::uintmax_t fileBytes =
        std::filesystem::file_size(temporaryCsv.path(), fileSizeError);
    if (fileSizeError || fileBytes < kTargetFileBytes) {
        std::cerr << "CSV benchmark failed: the generated file is smaller than 10 MiB.\n";
        return 1;
    }

    signallab::repository::CsvImportOptions options;
    options.hasHeader = true;
    options.valueColumn = 0U;
    options.timeColumn.reset();
    options.sampleRateIfNoTimeColumn = kSampleRate;
    options.metadata = {"Stage 8 CSV benchmark", "V", "generated benchmark"};

    double checksum = 0.0;
    for (std::size_t iteration = 0;
         iteration < kWarmupIterations;
         ++iteration) {
        auto result = signallab::io::readCsv(temporaryCsv.path(), options);
        if (!result.hasValue()) {
            std::cerr << "CSV benchmark warm-up failed: "
                      << result.error().message << '\n';
            return 1;
        }
        if (!validateSignal(
                result.value(),
                sampleCount,
                checksum,
                errorMessage)) {
            std::cerr << "CSV benchmark warm-up failed: "
                      << errorMessage << ".\n";
            return 1;
        }
    }

    std::vector<double> elapsedMilliseconds;
    elapsedMilliseconds.reserve(kIterations);

    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        auto result = signallab::io::readCsv(temporaryCsv.path(), options);
        const auto finish = std::chrono::steady_clock::now();

        if (!result.hasValue()) {
            std::cerr << "CSV benchmark failed: "
                      << result.error().message << '\n';
            return 1;
        }

        if (!validateSignal(
                result.value(),
                sampleCount,
                checksum,
                errorMessage)) {
            std::cerr << "CSV benchmark failed: " << errorMessage << ".\n";
            return 1;
        }

        const std::chrono::duration<double, std::milli> elapsed = finish - start;
        elapsedMilliseconds.push_back(elapsed.count());
    }

    const TimingSummary timing = summarize(elapsedMilliseconds);
    const double fileMebibytes =
        static_cast<double>(fileBytes) / (1024.0 * 1024.0);
    const double averageThroughput =
        fileMebibytes / (timing.averageMilliseconds / 1'000.0);
    const double peakThroughput =
        fileMebibytes / (timing.minimumMilliseconds / 1'000.0);

    if (!temporaryCsv.remove(errorMessage)) {
        std::cerr << "CSV benchmark failed during cleanup: "
                  << errorMessage << ".\n";
        return 1;
    }

    std::cout << "SignalLab CSV import benchmark\n"
              << "benchmark_version: csv-read/1.0.0\n"
              << "target_file_bytes: " << kTargetFileBytes << '\n'
              << "file_bytes: " << fileBytes << '\n'
              << "samples: " << sampleCount << '\n'
              << "warmup_iterations: " << kWarmupIterations << '\n'
              << "iterations: " << kIterations << '\n'
              << std::fixed << std::setprecision(3)
              << "csv_average_ms: " << timing.averageMilliseconds << '\n'
              << "csv_minimum_ms: " << timing.minimumMilliseconds << '\n'
              << "csv_average_mib_per_second: " << averageThroughput << '\n'
              << "csv_peak_mib_per_second: " << peakThroughput << '\n'
              << "expected_checksum: "
              << static_cast<double>(sampleCount) << '\n'
              << "checksum: " << checksum << '\n'
              << "temporary_file_removed: true\n";

    return 0;
}
