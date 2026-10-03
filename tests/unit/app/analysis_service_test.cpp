#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "signallab/app/analysis_service.hpp"
#include "signallab/core/error.hpp"
#include "signallab/core/signal_generator.hpp"
#include "signallab/database/database_connection.hpp"
#include "signallab/database/migration_runner.hpp"
#include "signallab/database/sqlite_signal_repository.hpp"
#include "signallab/database/sqlite_task_repository.hpp"

namespace {

signallab::core::Signal makeAcceptanceSignal()
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t sampleCount = 65'536;

    signallab::core::SignalMetadata metadata;
    metadata.name = "FFT acceptance sine";
    metadata.unit = "amplitude";
    metadata.source = "unit test";

    auto result = signallab::core::generateSineWave(
        1000.0,
        1.0,
        sampleRate,
        static_cast<double>(sampleCount) / sampleRate,
        std::move(metadata));
    return std::move(result).value();
}

std::filesystem::path makeTemporaryDatabaseRoot()
{
    const auto uniqueNumber =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("signallab-analysis-test-" + std::to_string(uniqueNumber));
    std::filesystem::create_directories(root);
    return root;
}

}  // namespace

TEST(AnalysisServiceTest, RunsTheSynchronousAnalysisClosure)
{
    constexpr double sampleRate = 48000.0;
    constexpr double frequency = 1000.0;
    constexpr double amplitude = 1.0;
    constexpr double binWidth = sampleRate / 65'536.0;

    const auto signal = makeAcceptanceSignal();
    const auto result = signallab::app::analyzeSignal(signal);

    ASSERT_TRUE(result.hasValue());
    ASSERT_TRUE(result.value().dominantPeak.has_value());
    EXPECT_LE(
        std::abs(result.value().dominantPeak->frequencyHz - frequency),
        binWidth);
    EXPECT_LE(
        std::abs(result.value().dominantPeak->amplitude - amplitude)
            / amplitude,
        0.02);
    EXPECT_EQ(result.value().spectrum.inputSize, signal.sampleCount());
    EXPECT_EQ(result.value().spectrum.fftSize, signal.sampleCount());
    EXPECT_NEAR(result.value().features.rms, std::sqrt(0.5), 0.001);
    EXPECT_TRUE(result.value().snrDb.has_value());
    EXPECT_TRUE(result.value().thdPercent.has_value());
}

TEST(AnalysisServiceTest, RejectsInvalidOptions)
{
    const auto signal = makeAcceptanceSignal();
    signallab::app::AnalysisOptions options;
    options.minimumPeakAmplitudeRatio = 1.5;

    const auto result = signallab::app::analyzeSignal(signal, options);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(
        result.error().code,
        signallab::core::ErrorCode::InvalidParameter);

    options = {};
    options.thdHarmonicCount = 1;
    const auto harmonicResult = signallab::app::analyzeSignal(signal, options);
    ASSERT_FALSE(harmonicResult.hasValue());
    EXPECT_EQ(
        harmonicResult.error().code,
        signallab::core::ErrorCode::InvalidParameter);

    options = {};
    options.metricGuardBins = signal.sampleCount();
    const auto guardResult = signallab::app::analyzeSignal(signal, options);
    ASSERT_FALSE(guardResult.hasValue());
    EXPECT_EQ(
        guardResult.error().code,
        signallab::core::ErrorCode::InvalidParameter);

    options = {};
    options.minimumPeakDistanceHz = -1.0;
    EXPECT_FALSE(signallab::app::analyzeSignal(signal, options));

    options = {};
    options.maximumPeakCount = 0;
    EXPECT_FALSE(signallab::app::analyzeSignal(signal, options));
}

TEST(AnalysisServiceTest, PersistsSuccessFailureAndHistoryAcrossRestart)
{
    const std::filesystem::path root = makeTemporaryDatabaseRoot();
    const std::filesystem::path databasePath = root / "analysis.sqlite3";
    const std::filesystem::path migrationsDirectory =
        std::filesystem::path(SIGNALLAB_SOURCE_DIR) / "migrations";
    std::string inputHash;
    std::string persistedParametersJson;
    std::string persistedAlgorithmVersion;
    std::int64_t signalRecordId = 0;
    std::int64_t successfulTaskId = 0;
    std::size_t successfulFeatureCount = 0;

    {
        signallab::database::DatabaseConnection database(databasePath);
        ASSERT_TRUE(database.open());
        signallab::database::MigrationRunner migrationRunner(database);
        ASSERT_TRUE(migrationRunner.run(migrationsDirectory));

        signallab::database::SqliteTaskRepository taskRepository(database);
        signallab::database::SqliteSignalRepository signalRepository(
            database);
        auto storedSignal = signalRepository.saveSignal(
            signallab::repository::SignalRegistration{
                signallab::repository::SignalSourceType::Generated,
                "analysis-test-signal",
                {},
                "{}",
                std::nullopt,
                makeAcceptanceSignal()});
        ASSERT_TRUE(storedSignal);
        inputHash = storedSignal.value().sampleHash;
        signalRecordId = storedSignal.value().recordId;

        signallab::app::AnalysisService service(
            taskRepository,
            signalRepository);

        const auto analysisResult = service.analyze(
            signalRecordId);
        ASSERT_TRUE(analysisResult) << analysisResult.error().message;

        auto historyResult = service.history();
        ASSERT_TRUE(historyResult);
        ASSERT_EQ(historyResult.value().size(), 1U);
        const auto& successfulTask = historyResult.value().front();
        EXPECT_EQ(
            successfulTask.status,
            signallab::repository::AnalysisTaskStatus::Succeeded);
        EXPECT_EQ(successfulTask.inputHash, inputHash);
        EXPECT_EQ(successfulTask.outputHash.size(), 64U);
        EXPECT_EQ(
            successfulTask.parametersJson,
            "{\"maximum_peak_count\":10,\"metric_guard_bins\":1,"
            "\"minimum_peak_amplitude_ratio\":0.01,"
            "\"minimum_peak_distance_hz\":0,"
            "\"thd_harmonic_count\":5,\"window_type\":\"hann\"}");
        EXPECT_NE(
            successfulTask.algorithmVersion.find("fft="),
            std::string::npos);
        successfulTaskId = successfulTask.id;
        persistedParametersJson = successfulTask.parametersJson;
        persistedAlgorithmVersion = successfulTask.algorithmVersion;

        const auto featureResult = service.featureResults(successfulTask.id);
        ASSERT_TRUE(featureResult);
        ASSERT_GE(featureResult.value().size(), 7U);
        successfulFeatureCount = featureResult.value().size();

        signallab::app::AnalysisOptions invalidOptions;
        invalidOptions.maximumPeakCount = 0;
        const auto failedAnalysis = service.analyze(
            signalRecordId,
            invalidOptions);
        ASSERT_FALSE(failedAnalysis);

        historyResult = service.history();
        ASSERT_TRUE(historyResult);
        ASSERT_EQ(historyResult.value().size(), 2U);
        EXPECT_EQ(
            historyResult.value().front().status,
            signallab::repository::AnalysisTaskStatus::Failed);
    }

    {
        signallab::database::DatabaseConnection reopenedDatabase(databasePath);
        ASSERT_TRUE(reopenedDatabase.open());
        signallab::database::MigrationRunner migrationRunner(
            reopenedDatabase);
        ASSERT_TRUE(migrationRunner.run(migrationsDirectory));
        signallab::database::SqliteTaskRepository taskRepository(
            reopenedDatabase);
        signallab::database::SqliteSignalRepository signalRepository(
            reopenedDatabase);
        signallab::app::AnalysisService restartedService(
            taskRepository,
            signalRepository);

        const auto historyResult = restartedService.history();
        ASSERT_TRUE(historyResult);
        ASSERT_EQ(historyResult.value().size(), 2U);
        const auto& restoredSuccessfulTask = historyResult.value().back();
        EXPECT_EQ(restoredSuccessfulTask.id, successfulTaskId);
        EXPECT_EQ(restoredSuccessfulTask.inputHash, inputHash);
        EXPECT_EQ(restoredSuccessfulTask.outputHash.size(), 64U);
        EXPECT_EQ(
            restoredSuccessfulTask.parametersJson,
            persistedParametersJson);
        EXPECT_EQ(
            restoredSuccessfulTask.algorithmVersion,
            persistedAlgorithmVersion);

        const auto restoredFeatures = restartedService.featureResults(
            successfulTaskId);
        ASSERT_TRUE(restoredFeatures);
        EXPECT_EQ(
            restoredFeatures.value().size(),
            successfulFeatureCount);

        const auto removed = restartedService.removeHistoryEntry(
            successfulTaskId);
        ASSERT_TRUE(removed);
        EXPECT_TRUE(removed.value());

        const auto historyAfterDelete = restartedService.history();
        ASSERT_TRUE(historyAfterDelete);
        ASSERT_EQ(historyAfterDelete.value().size(), 1U);
        EXPECT_NE(historyAfterDelete.value().front().id, successfulTaskId);

        const auto featuresAfterDelete = restartedService.featureResults(
            successfulTaskId);
        ASSERT_TRUE(featuresAfterDelete);
        EXPECT_TRUE(featuresAfterDelete.value().empty());

        const auto repeatedDelete = restartedService.removeHistoryEntry(
            successfulTaskId);
        ASSERT_TRUE(repeatedDelete);
        EXPECT_FALSE(repeatedDelete.value());
    }

    std::error_code ignoredError;
    std::filesystem::remove_all(root, ignoredError);
}
