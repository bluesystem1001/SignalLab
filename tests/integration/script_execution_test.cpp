#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <numbers>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include "signallab/app/analysis_service.hpp"
#include "signallab/app/import_service.hpp"
#include "signallab/app/script_runtime/value.hpp"
#include "signallab/app/script_service.hpp"
#include "signallab/database/database_executor.hpp"
#include "signallab/io/filesystem_file_repository.hpp"
#include "signallab/io/filesystem_script_file_gateway.hpp"
#include "signallab/repository/script_repository.hpp"

namespace {

std::atomic<unsigned long long> temporaryProjectCounter{0};

class TemporaryScriptProject {
public:
    explicit TemporaryScriptProject(const std::string& purpose)
    {
        const auto clockValue =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const auto counterValue = temporaryProjectCounter.fetch_add(
            1,
            std::memory_order_relaxed);
        root_ = std::filesystem::temp_directory_path()
            / ("signallab-stage6-script-" + purpose + "-"
               + std::to_string(clockValue) + "-"
               + std::to_string(counterValue));
        std::filesystem::create_directories(inputRoot());
        std::filesystem::create_directories(outputRoot());
    }

    ~TemporaryScriptProject()
    {
        std::error_code ignoredError;
        std::filesystem::remove_all(root_, ignoredError);
    }

    TemporaryScriptProject(const TemporaryScriptProject&) = delete;
    TemporaryScriptProject& operator=(const TemporaryScriptProject&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept
    {
        return root_;
    }

    [[nodiscard]] std::filesystem::path inputRoot() const
    {
        return root_ / "script-input";
    }

    [[nodiscard]] std::filesystem::path outputRoot() const
    {
        return root_ / "script-output";
    }

    [[nodiscard]] std::filesystem::path databasePath() const
    {
        return root_ / "stage6.sqlite3";
    }

private:
    std::filesystem::path root_;
};

class ThrowingOutputGateway final
    : public signallab::repository::ScriptFileGateway {
public:
    ThrowingOutputGateway(
        std::filesystem::path inputRoot,
        std::filesystem::path outputRoot)
        : delegate_(std::move(inputRoot), std::move(outputRoot))
    {
    }

    signallab::core::Result<std::filesystem::path> resolveInput(
        const std::filesystem::path& relativePath) const override
    {
        return delegate_.resolveInput(relativePath);
    }

    signallab::core::Result<std::filesystem::path> writeOutput(
        const std::filesystem::path&,
        std::string_view,
        std::stop_token) override
    {
        throw std::runtime_error("injected script output exception");
    }

private:
    signallab::io::FilesystemScriptFileGateway delegate_;
};

std::filesystem::path migrationsDirectory()
{
    return std::filesystem::path(SIGNALLAB_SOURCE_DIR) / "migrations";
}

const signallab::app::script_runtime::FeatureValue* findFeature(
    const signallab::app::script_runtime::FeatureSetValue& features,
    std::string_view name)
{
    const auto found = std::find_if(
        features.values.begin(),
        features.values.end(),
        [name](const auto& feature) {
            return feature.name == name;
        });
    return found == features.values.end() ? nullptr : &*found;
}

class ScriptExecutionIntegrationTest : public ::testing::Test {
protected:
    explicit ScriptExecutionIntegrationTest(
        std::string purpose = "execution")
        : project_(std::move(purpose)),
          database_(project_.databasePath(), migrationsDirectory()),
          files_(project_.root()),
          imports_(files_, database_),
          gateway_(project_.inputRoot(), project_.outputRoot()),
          service_(imports_, database_, gateway_)
    {
    }

    void SetUp() override
    {
        const auto started = database_.start();
        ASSERT_TRUE(started) << started.error().message;
    }

    void TearDown() override
    {
        if (database_.isRunning()) {
            const auto stopped = database_.shutdown();
            EXPECT_TRUE(stopped) << stopped.error().message;
        }
    }

    void writeSineCsv(
        const std::filesystem::path& relativePath,
        std::size_t sampleCount = 1'024,
        double sampleRate = 1'024.0,
        double frequency = 64.0)
    {
        const auto destination = project_.inputRoot() / relativePath;
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream stream(destination, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << std::setprecision(17);
        stream << "time,value\n";
        for (std::size_t index = 0; index < sampleCount; ++index) {
            const double time = static_cast<double>(index) / sampleRate;
            const double value = std::sin(
                2.0 * std::numbers::pi * frequency * time);
            stream << time << ',' << value << '\n';
        }
        stream.close();
        ASSERT_TRUE(stream.good());
    }

    void writeQuarterRateCosineCsv(
        const std::filesystem::path& relativePath,
        std::size_t sampleCount = 1'024,
        double sampleRate = 1'024.0)
    {
        const auto destination = project_.inputRoot() / relativePath;
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream stream(destination, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        stream << std::setprecision(17);
        stream << "time,value\n";
        constexpr double cycle[]{1.0, 0.0, -1.0, 0.0};
        for (std::size_t index = 0; index < sampleCount; ++index) {
            stream << static_cast<double>(index) / sampleRate
                   << ',' << cycle[index % 4U] << '\n';
        }
        stream.close();
        ASSERT_TRUE(stream.good());
    }

    [[nodiscard]] signallab::repository::ScriptHistoryRecord
    onlyHistoryRecord()
    {
        auto history = service_.history(10);
        EXPECT_TRUE(history) << history.error().message;
        if (!history || history.value().empty()) {
            return {};
        }
        EXPECT_EQ(history.value().size(), 1U);
        return history.value().front();
    }

    TemporaryScriptProject project_;
    signallab::database::DatabaseExecutor database_;
    signallab::io::FilesystemFileRepository files_;
    signallab::app::ImportService imports_;
    signallab::io::FilesystemScriptFileGateway gateway_;
    signallab::app::ScriptService service_;
};

TEST_F(ScriptExecutionIntegrationTest,
       ExecutesTheRepositoryExampleAndPersistsSuccess)
{
    const std::filesystem::path repositorySamples =
        std::filesystem::path(SIGNALLAB_SOURCE_DIR) / "data" / "samples";
    const std::filesystem::path inputSample =
        project_.inputRoot() / "data" / "samples" / "example_signal.csv";
    std::filesystem::create_directories(inputSample.parent_path());
    std::error_code copyError;
    const bool copied = std::filesystem::copy_file(
        repositorySamples / "example_signal.csv",
        inputSample,
        std::filesystem::copy_options::none,
        copyError);
    ASSERT_TRUE(copied) << copyError.message();

    std::ifstream scriptStream(
        repositorySamples / "example_analysis.sig",
        std::ios::binary);
    ASSERT_TRUE(scriptStream.is_open());
    const std::string source{
        std::istreambuf_iterator<char>(scriptStream),
        std::istreambuf_iterator<char>()};
    ASSERT_FALSE(source.empty());

    auto executed = service_.execute(source);

    ASSERT_TRUE(executed) << executed.error().message;
    const auto& result = executed.value();
    EXPECT_EQ(result.status, signallab::app::ScriptExecutionStatus::Succeeded);
    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_FALSE(result.astText.empty());
    EXPECT_FALSE(result.runtimeIssue.has_value());
    EXPECT_TRUE(result.output.variables.contains("raw"));
    EXPECT_TRUE(result.output.variables.contains("clean"));
    EXPECT_TRUE(result.output.variables.contains("spec"));
    EXPECT_TRUE(result.output.variables.contains("result"));
    EXPECT_EQ(result.output.plots.size(), 2U);
    ASSERT_EQ(result.output.outputs.size(), 1U);
    EXPECT_EQ(
        result.output.outputs.front(),
        std::filesystem::canonical(project_.outputRoot())
            / "example_signal_features.json");
    EXPECT_TRUE(std::filesystem::is_regular_file(
        result.output.outputs.front()));

    const auto record = onlyHistoryRecord();
    EXPECT_EQ(record.id, result.historyId);
    EXPECT_EQ(record.scriptText, source);
    EXPECT_EQ(record.sourceHash.size(), 64U);
    EXPECT_EQ(
        record.status,
        signallab::repository::ScriptHistoryStatus::Succeeded);
    EXPECT_EQ(record.statusText, "SUCCEEDED");
    EXPECT_FALSE(record.finishedUtc.empty());
    EXPECT_FALSE(record.errorLocation.has_value());
    EXPECT_TRUE(record.errorMessage.empty());
}

TEST_F(ScriptExecutionIntegrationTest,
       FftAndFeaturesMatchTheSharedGuiAnalysisPath)
{
    writeQuarterRateCosineCsv("shared-analysis.csv");
    const std::string source =
        "load \"shared-analysis.csv\" as raw;\n"
        "spec = raw |> fft(window=\"rectangular\");\n"
        "result = features(spec, fundamental=256);\n"
        "save result to \"shared-features.json\";\n";

    auto executed = service_.execute(source);

    ASSERT_TRUE(executed) << executed.error().message;
    const auto& result = executed.value();
    ASSERT_EQ(
        result.status,
        signallab::app::ScriptExecutionStatus::Succeeded);
    ASSERT_TRUE(result.diagnostics.empty());
    ASSERT_FALSE(result.runtimeIssue.has_value());

    const auto rawVariable = result.output.variables.find("raw");
    const auto spectrumVariable = result.output.variables.find("spec");
    const auto featureVariable = result.output.variables.find("result");
    ASSERT_NE(rawVariable, result.output.variables.end());
    ASSERT_NE(spectrumVariable, result.output.variables.end());
    ASSERT_NE(featureVariable, result.output.variables.end());

    const auto* raw = std::get_if<
        signallab::app::script_runtime::SignalValue>(
        &rawVariable->second);
    const auto* scriptedSpectrum = std::get_if<
        signallab::app::script_runtime::SpectrumValuePtr>(
        &spectrumVariable->second);
    const auto* scriptedFeatures = std::get_if<
        signallab::app::script_runtime::FeatureSetValuePtr>(
        &featureVariable->second);
    ASSERT_NE(raw, nullptr);
    ASSERT_TRUE(*raw);
    ASSERT_NE(scriptedSpectrum, nullptr);
    ASSERT_TRUE(*scriptedSpectrum);
    ASSERT_TRUE((*scriptedSpectrum)->analysis);
    ASSERT_NE(scriptedFeatures, nullptr);
    ASSERT_TRUE(*scriptedFeatures);

    signallab::app::AnalysisOptions guiOptions;
    guiOptions.windowType = signallab::algorithm::WindowType::Rectangular;
    const auto directAnalysis = signallab::app::analyzeSignal(
        **raw,
        guiOptions);
    ASSERT_TRUE(directAnalysis) << directAnalysis.error().message;

    const auto& scriptedAnalysis = *(*scriptedSpectrum)->analysis;
    const auto& direct = directAnalysis.value();
    EXPECT_EQ(
        scriptedAnalysis.spectrum.windowType,
        signallab::algorithm::WindowType::Rectangular);
    EXPECT_EQ(
        scriptedAnalysis.spectrum.frequenciesHz,
        direct.spectrum.frequenciesHz);
    EXPECT_EQ(
        scriptedAnalysis.spectrum.amplitudes,
        direct.spectrum.amplitudes);
    EXPECT_EQ(scriptedAnalysis.spectrum.powers, direct.spectrum.powers);
    EXPECT_DOUBLE_EQ(scriptedAnalysis.features.mean, direct.features.mean);
    EXPECT_DOUBLE_EQ(
        scriptedAnalysis.features.variance,
        direct.features.variance);
    EXPECT_DOUBLE_EQ(scriptedAnalysis.features.rms, direct.features.rms);
    EXPECT_DOUBLE_EQ(
        scriptedAnalysis.features.peakToPeak,
        direct.features.peakToPeak);
    ASSERT_EQ(
        scriptedAnalysis.dominantPeak.has_value(),
        direct.dominantPeak.has_value());
    if (direct.dominantPeak.has_value()) {
        EXPECT_EQ(
            scriptedAnalysis.dominantPeak->binIndex,
            direct.dominantPeak->binIndex);
        EXPECT_DOUBLE_EQ(
            scriptedAnalysis.dominantPeak->frequencyHz,
            direct.dominantPeak->frequencyHz);
        EXPECT_DOUBLE_EQ(
            scriptedAnalysis.dominantPeak->amplitude,
            direct.dominantPeak->amplitude);
    }

    const auto* mean = findFeature(**scriptedFeatures, "mean");
    const auto* variance = findFeature(**scriptedFeatures, "variance");
    const auto* rms = findFeature(**scriptedFeatures, "rms");
    const auto* peakToPeak = findFeature(
        **scriptedFeatures,
        "peak_to_peak");
    const auto* fftSize = findFeature(**scriptedFeatures, "fft_size");
    ASSERT_NE(mean, nullptr);
    ASSERT_NE(variance, nullptr);
    ASSERT_NE(rms, nullptr);
    ASSERT_NE(peakToPeak, nullptr);
    ASSERT_NE(fftSize, nullptr);
    EXPECT_DOUBLE_EQ(mean->value, direct.features.mean);
    EXPECT_DOUBLE_EQ(variance->value, direct.features.variance);
    EXPECT_DOUBLE_EQ(rms->value, direct.features.rms);
    EXPECT_DOUBLE_EQ(peakToPeak->value, direct.features.peakToPeak);
    const std::string& signalUnit = (*raw)->metadata().unit;
    EXPECT_EQ(mean->unit, signalUnit);
    EXPECT_EQ(
        variance->unit,
        signalUnit.empty() ? std::string{} : signalUnit + "^2");
    EXPECT_EQ(rms->unit, signalUnit);
    EXPECT_EQ(peakToPeak->unit, signalUnit);
    EXPECT_DOUBLE_EQ(
        fftSize->value,
        static_cast<double>(direct.spectrum.fftSize));

    if (direct.dominantPeak.has_value()) {
        const auto* frequency = findFeature(
            **scriptedFeatures,
            "dominant_frequency");
        const auto* amplitude = findFeature(
            **scriptedFeatures,
            "amplitude");
        ASSERT_NE(frequency, nullptr);
        ASSERT_NE(amplitude, nullptr);
        EXPECT_DOUBLE_EQ(frequency->value, direct.dominantPeak->frequencyHz);
        EXPECT_DOUBLE_EQ(amplitude->value, direct.dominantPeak->amplitude);
        EXPECT_EQ(amplitude->unit, (*raw)->metadata().unit);
    }
    if (direct.snrDb.has_value()) {
        const auto* snr = findFeature(**scriptedFeatures, "snr");
        ASSERT_NE(snr, nullptr);
        EXPECT_DOUBLE_EQ(snr->value, *direct.snrDb);
    }
    if (direct.thdPercent.has_value()) {
        const auto* thd = findFeature(**scriptedFeatures, "thd");
        ASSERT_NE(thd, nullptr);
        EXPECT_DOUBLE_EQ(thd->value, *direct.thdPercent);
    }

    const auto* snr = findFeature(**scriptedFeatures, "snr");
    ASSERT_NE(snr, nullptr);
    ASSERT_TRUE(std::isinf(snr->value));
    EXPECT_GT(snr->value, 0.0);

    ASSERT_EQ(result.output.outputs.size(), 1U);
    std::ifstream jsonStream(
        result.output.outputs.front(),
        std::ios::binary);
    ASSERT_TRUE(jsonStream.is_open());
    const std::string jsonBytes{
        std::istreambuf_iterator<char>(jsonStream),
        std::istreambuf_iterator<char>()};
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray(jsonBytes.data(), static_cast<qsizetype>(jsonBytes.size())),
        &parseError);
    ASSERT_EQ(parseError.error, QJsonParseError::NoError)
        << parseError.errorString().toStdString();
    ASSERT_TRUE(document.isObject());
    const QJsonArray exportedFeatures =
        document.object().value(QStringLiteral("features")).toArray();
    ASSERT_EQ(
        exportedFeatures.size(),
        static_cast<qsizetype>((*scriptedFeatures)->values.size()));

    bool foundNullSnr = false;
    for (const QJsonValue& entry : exportedFeatures) {
        ASSERT_TRUE(entry.isObject());
        const QJsonObject feature = entry.toObject();
        ASSERT_TRUE(feature.value(QStringLiteral("name")).isString());
        ASSERT_TRUE(feature.value(QStringLiteral("unit")).isString());
        if (feature.value(QStringLiteral("name")).toString()
            == QStringLiteral("snr")) {
            EXPECT_TRUE(feature.value(QStringLiteral("value")).isNull());
            foundNullSnr = true;
        } else {
            EXPECT_TRUE(feature.value(QStringLiteral("value")).isDouble());
        }
    }
    EXPECT_TRUE(foundNullSnr);
}

TEST_F(ScriptExecutionIntegrationTest,
       ReopensTheSameInputPathAndObservesChangedFileContents)
{
    constexpr std::string_view source =
        "load \"changing.csv\" as raw;\n"
        "spec = raw |> fft(window=\"hann\");\n";
    const auto executeAndReadDominantFrequency = [this, source]() {
        auto executed = service_.execute(std::string(source));
        EXPECT_TRUE(executed) << executed.error().message;
        if (!executed) {
            return 0.0;
        }
        EXPECT_EQ(
            executed.value().status,
            signallab::app::ScriptExecutionStatus::Succeeded);
        EXPECT_TRUE(executed.value().diagnostics.empty());
        EXPECT_FALSE(executed.value().runtimeIssue.has_value());
        const auto spectrum = executed.value().output.variables.find("spec");
        EXPECT_NE(spectrum, executed.value().output.variables.end());
        if (spectrum == executed.value().output.variables.end()) {
            return 0.0;
        }
        const auto* value = std::get_if<
            signallab::app::script_runtime::SpectrumValuePtr>(
            &spectrum->second);
        EXPECT_NE(value, nullptr);
        if (value == nullptr || !*value || !(*value)->analysis) {
            return 0.0;
        }
        EXPECT_TRUE((*value)->analysis->dominantPeak.has_value());
        if (!(*value)->analysis->dominantPeak.has_value()) {
            return 0.0;
        }
        return (*value)->analysis->dominantPeak->frequencyHz;
    };

    writeSineCsv("changing.csv", 1'024, 1'024.0, 64.0);
    const double firstFrequency = executeAndReadDominantFrequency();
    auto firstSignals = database_.listSignals();
    ASSERT_TRUE(firstSignals) << firstSignals.error().message;
    ASSERT_EQ(firstSignals.value().size(), 1U);
    const std::int64_t firstRecordId = firstSignals.value().front().recordId;

    const double repeatedFrequency = executeAndReadDominantFrequency();
    auto repeatedSignals = database_.listSignals();
    ASSERT_TRUE(repeatedSignals) << repeatedSignals.error().message;
    ASSERT_EQ(repeatedSignals.value().size(), 1U);
    EXPECT_EQ(repeatedSignals.value().front().recordId, firstRecordId);

    writeSineCsv("changing.csv", 1'024, 1'024.0, 128.0);
    const double secondFrequency = executeAndReadDominantFrequency();
    auto changedSignals = database_.listSignals();
    ASSERT_TRUE(changedSignals) << changedSignals.error().message;
    EXPECT_EQ(changedSignals.value().size(), 2U);

    EXPECT_NEAR(firstFrequency, 64.0, 0.1);
    EXPECT_NEAR(repeatedFrequency, 64.0, 0.1);
    EXPECT_NEAR(secondFrequency, 128.0, 0.1);
    EXPECT_GT(secondFrequency - firstFrequency, 60.0);

    auto history = service_.history(10);
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history.value().size(), 3U);
    for (const auto& record : history.value()) {
        EXPECT_EQ(
            record.status,
            signallab::repository::ScriptHistoryStatus::Succeeded);
    }
}

TEST_F(ScriptExecutionIntegrationTest,
       PersistsTheExactLocationOfTheFirstSyntaxError)
{
    const std::string source =
        "let prefix = 1;\n"
        "value = fft(prefix;\n";

    auto executed = service_.execute(source);

    ASSERT_TRUE(executed) << executed.error().message;
    const auto& result = executed.value();
    EXPECT_EQ(result.status, signallab::app::ScriptExecutionStatus::Failed);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().code, "PAR021");
    EXPECT_EQ(result.diagnostics.front().span.begin.line, 2U);
    EXPECT_EQ(result.diagnostics.front().span.begin.column, 19U);
    EXPECT_FALSE(result.runtimeIssue.has_value());

    const auto record = onlyHistoryRecord();
    EXPECT_EQ(
        record.status,
        signallab::repository::ScriptHistoryStatus::Failed);
    ASSERT_TRUE(record.errorLocation.has_value());
    EXPECT_EQ(record.errorLocation->line, 2);
    EXPECT_EQ(record.errorLocation->column, 19);
    EXPECT_NE(record.errorMessage.find("PAR021"), std::string::npos);
}

TEST_F(ScriptExecutionIntegrationTest,
       ConvertsUnexpectedRuntimeExceptionsToTerminalHistory)
{
    writeSineCsv("exception.csv", 16U, 1'024.0, 64.0);
    ThrowingOutputGateway throwingGateway(
        project_.inputRoot(),
        project_.outputRoot());
    signallab::app::ScriptService throwingService(
        imports_,
        database_,
        throwingGateway);

    auto executed = throwingService.execute(
        "load \"exception.csv\" as raw;\n"
        "save raw to \"never-created.csv\";\n");

    ASSERT_TRUE(executed) << executed.error().message;
    EXPECT_EQ(
        executed.value().status,
        signallab::app::ScriptExecutionStatus::Failed);
    ASSERT_TRUE(executed.value().runtimeIssue.has_value());
    EXPECT_EQ(executed.value().runtimeIssue->code, "RUN027");
    EXPECT_NE(
        executed.value().runtimeIssue->message.find(
            "injected script output exception"),
        std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(
        project_.outputRoot() / "never-created.csv"));

    const auto record = onlyHistoryRecord();
    EXPECT_EQ(
        record.status,
        signallab::repository::ScriptHistoryStatus::Failed);
    EXPECT_EQ(record.statusText, "FAILED");
    EXPECT_FALSE(record.finishedUtc.empty());
    EXPECT_FALSE(record.errorLocation.has_value());
    EXPECT_NE(record.errorMessage.find("RUN027"), std::string::npos);
    EXPECT_NE(
        record.errorMessage.find("injected script output exception"),
        std::string::npos);
}

TEST_F(ScriptExecutionIntegrationTest,
       RejectsInputTraversalAsARecordedRuntimeFailure)
{
    const std::string source =
        "load \"../outside.csv\" as escaped;\n";

    auto executed = service_.execute(source);

    ASSERT_TRUE(executed) << executed.error().message;
    const auto& result = executed.value();
    EXPECT_EQ(result.status, signallab::app::ScriptExecutionStatus::Failed);
    EXPECT_TRUE(result.diagnostics.empty());
    ASSERT_TRUE(result.runtimeIssue.has_value());
    EXPECT_EQ(result.runtimeIssue->code, "RUN010");
    EXPECT_EQ(result.runtimeIssue->span.begin.line, 1U);
    EXPECT_EQ(result.runtimeIssue->span.begin.column, 6U);

    const auto record = onlyHistoryRecord();
    EXPECT_EQ(
        record.status,
        signallab::repository::ScriptHistoryStatus::Failed);
    ASSERT_TRUE(record.errorLocation.has_value());
    EXPECT_EQ(record.errorLocation->line, 1);
    EXPECT_EQ(record.errorLocation->column, 6);
    EXPECT_NE(record.errorMessage.find("RUN010"), std::string::npos);
    EXPECT_NE(record.errorMessage.find("traversal"), std::string::npos);
}

TEST_F(ScriptExecutionIntegrationTest,
       RejectsUnsafeOutputPathsWithoutPublishingFilesAndRecordsFailures)
{
    writeSineCsv("wave.csv");
    const std::filesystem::path traversalTarget =
        project_.root() / "escaped.csv";
    const std::filesystem::path absoluteTarget =
        project_.root() / "absolute.csv";
    const std::string absoluteScriptPath = absoluteTarget.generic_string();
    const std::vector<std::string> sources{
        "load \"wave.csv\" as raw;\n"
        "save raw to \"../escaped.csv\";\n",
        "load \"wave.csv\" as raw;\n"
        "save raw to \"" + absoluteScriptPath + "\";\n"};

    std::vector<std::int64_t> historyIds;
    for (const auto& source : sources) {
        auto executed = service_.execute(source);
        ASSERT_TRUE(executed) << executed.error().message;
        const auto& result = executed.value();
        EXPECT_EQ(
            result.status,
            signallab::app::ScriptExecutionStatus::Failed);
        EXPECT_TRUE(result.diagnostics.empty());
        ASSERT_TRUE(result.runtimeIssue.has_value());
        EXPECT_EQ(result.runtimeIssue->code, "RUN025");
        EXPECT_EQ(result.runtimeIssue->span.begin.line, 2U);
        EXPECT_EQ(result.runtimeIssue->span.begin.column, 1U);
        EXPECT_TRUE(result.output.outputs.empty());
        historyIds.push_back(result.historyId);
    }

    EXPECT_FALSE(std::filesystem::exists(traversalTarget));
    EXPECT_FALSE(std::filesystem::exists(absoluteTarget));
    EXPECT_TRUE(std::filesystem::is_empty(project_.outputRoot()));

    auto history = service_.history(10);
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history.value().size(), historyIds.size());
    for (const std::int64_t historyId : historyIds) {
        const auto record = std::find_if(
            history.value().begin(),
            history.value().end(),
            [historyId](const auto& candidate) {
                return candidate.id == historyId;
            });
        ASSERT_NE(record, history.value().end());
        EXPECT_EQ(
            record->status,
            signallab::repository::ScriptHistoryStatus::Failed);
        ASSERT_TRUE(record->errorLocation.has_value());
        EXPECT_EQ(record->errorLocation->line, 2);
        EXPECT_EQ(record->errorLocation->column, 1);
        EXPECT_NE(record->errorMessage.find("RUN025"), std::string::npos);
    }
}

TEST_F(ScriptExecutionIntegrationTest,
       MapsARequestedStopToCancelledAndCompletesTheHistory)
{
    std::stop_source stopSource;
    ASSERT_TRUE(stopSource.request_stop());

    auto executed = service_.execute(
        "let answer = 42;\n",
        stopSource.get_token());

    ASSERT_TRUE(executed) << executed.error().message;
    const auto& result = executed.value();
    EXPECT_EQ(
        result.status,
        signallab::app::ScriptExecutionStatus::Cancelled);
    EXPECT_TRUE(result.diagnostics.empty());
    ASSERT_TRUE(result.runtimeIssue.has_value());
    EXPECT_EQ(result.runtimeIssue->code, "RUN001");

    const auto record = onlyHistoryRecord();
    EXPECT_EQ(
        record.status,
        signallab::repository::ScriptHistoryStatus::Cancelled);
    EXPECT_EQ(record.statusText, "CANCELLED");
    ASSERT_TRUE(record.errorLocation.has_value());
    EXPECT_EQ(record.errorLocation->line, 1);
    EXPECT_EQ(record.errorLocation->column, 1);
    EXPECT_NE(record.errorMessage.find("RUN001"), std::string::npos);
}

TEST_F(ScriptExecutionIntegrationTest,
       ExposesStartupInterruptionAndHistoryThroughTheService)
{
    auto running = database_.createScriptHistory("let pending = true;\n");
    ASSERT_TRUE(running) << running.error().message;

    auto interrupted = service_.interruptRunningScripts();

    ASSERT_TRUE(interrupted) << interrupted.error().message;
    EXPECT_EQ(interrupted.value(), 1U);
    const auto record = onlyHistoryRecord();
    EXPECT_EQ(record.id, running.value());
    EXPECT_EQ(
        record.status,
        signallab::repository::ScriptHistoryStatus::Interrupted);
    EXPECT_EQ(record.statusText, "INTERRUPTED");
    EXPECT_FALSE(record.finishedUtc.empty());
    EXPECT_FALSE(record.errorLocation.has_value());
    EXPECT_FALSE(record.errorMessage.empty());
}

}  // namespace
