#include "signallab/app/analysis_service.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

#include "signallab/algorithm/fft.hpp"
#include "signallab/algorithm/preprocessing.hpp"
#include "signallab/algorithm/quality_metrics.hpp"
#include "signallab/algorithm/window.hpp"
#include "signallab/core/error.hpp"

namespace signallab::app {
namespace {

std::string jsonNumber(double value)
{
    if (!std::isfinite(value)) {
        return "null";
    }
    if (value == 0.0) {
        return "0";
    }

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << value;
    return stream.str();
}

std::string windowTypeName(algorithm::WindowType windowType)
{
    switch (windowType) {
    case algorithm::WindowType::Rectangular:
        return "rectangular";
    case algorithm::WindowType::Hann:
        return "hann";
    case algorithm::WindowType::Hamming:
        return "hamming";
    }
    return "unknown";
}

std::string analysisParametersJson(const AnalysisOptions& options)
{
    // Keys are deliberately kept in alphabetical order.
    return "{\"maximum_peak_count\":"
        + std::to_string(options.maximumPeakCount)
        + ",\"metric_guard_bins\":"
        + std::to_string(options.metricGuardBins)
        + ",\"minimum_peak_amplitude_ratio\":"
        + jsonNumber(options.minimumPeakAmplitudeRatio)
        + ",\"minimum_peak_distance_hz\":"
        + jsonNumber(options.minimumPeakDistanceHz)
        + ",\"thd_harmonic_count\":"
        + std::to_string(options.thdHarmonicCount)
        + ",\"window_type\":\"" + windowTypeName(options.windowType)
        + "\"}";
}

std::string analysisAlgorithmVersion()
{
    std::string version;
    version += "analysis=";
    version += kAnalysisServiceVersion;
    version += "|preprocessing=";
    version += algorithm::kPreprocessingAlgorithmVersion;
    version += "|window=";
    version += algorithm::kWindowAlgorithmVersion;
    version += "|fft=";
    version += algorithm::kFftVersion;
    version += "|spectrum=";
    version += algorithm::kSpectrumVersion;
    version += "|features=";
    version += algorithm::kFeaturesVersion;
    version += "|peak_detector=";
    version += algorithm::kPeakDetectorVersion;
    version += "|quality_metrics=";
    version += algorithm::kQualityMetricsVersion;
    return version;
}

std::string squaredUnit(const std::string& unit)
{
    if (unit.empty()) {
        return {};
    }
    return unit + "^2";
}

void addFeature(
    std::vector<repository::FeatureValue>& values,
    std::string name,
    double value,
    std::string unit)
{
    values.push_back(repository::FeatureValue{
        std::move(name),
        value,
        std::move(unit)
    });
}

std::vector<repository::FeatureValue> makeFeatureValues(
    const AnalysisResult& result,
    const core::Signal& signal)
{
    const std::string& signalUnit = signal.metadata().unit;
    std::vector<repository::FeatureValue> values;
    values.reserve(10);

    addFeature(values, "mean", result.features.mean, signalUnit);
    addFeature(
        values,
        "variance",
        result.features.variance,
        squaredUnit(signalUnit));
    addFeature(values, "rms", result.features.rms, signalUnit);
    addFeature(
        values,
        "peak_to_peak",
        result.features.peakToPeak,
        signalUnit);

    if (result.dominantPeak.has_value()) {
        addFeature(
            values,
            "dominant_frequency",
            result.dominantPeak->frequencyHz,
            "Hz");
        addFeature(
            values,
            "amplitude",
            result.dominantPeak->amplitude,
            signalUnit);
    }
    if (result.snrDb.has_value()) {
        addFeature(values, "snr", *result.snrDb, "dB");
    }
    if (result.thdPercent.has_value()) {
        addFeature(values, "thd", *result.thdPercent, "%");
    }

    addFeature(
        values,
        "fft_size",
        static_cast<double>(result.spectrum.fftSize),
        "samples");
    return values;
}

core::Result<AnalysisResult> cancelledAnalysis()
{
    return core::Result<AnalysisResult>::failure(
        core::ErrorCode::Cancelled,
        "The analysis was cancelled.");
}

void reportProgress(
    const AnalysisProgressCallback& callback,
    int percent,
    std::string message)
{
    if (callback) {
        callback(percent, std::move(message));
    }
}

}  // namespace

core::Result<AnalysisResult> analyzeSignal(
    const core::Signal& signal,
    const AnalysisOptions& options,
    std::stop_token stopToken,
    AnalysisProgressCallback progressCallback)
{
    if (stopToken.stop_requested()) {
        return cancelledAnalysis();
    }
    reportProgress(progressCallback, 0, "Checking analysis options");

    if (!std::isfinite(options.minimumPeakAmplitudeRatio)
        || options.minimumPeakAmplitudeRatio < 0.0
        || options.minimumPeakAmplitudeRatio > 1.0) {
        return core::Result<AnalysisResult>::failure(
            core::ErrorCode::InvalidParameter,
            "The minimum peak amplitude ratio must be between 0 and 1.");
    }

    if (!std::isfinite(options.minimumPeakDistanceHz)
        || options.minimumPeakDistanceHz < 0.0) {
        return core::Result<AnalysisResult>::failure(
            core::ErrorCode::InvalidParameter,
            "The minimum peak distance must be a finite non-negative number.");
    }

    if (options.maximumPeakCount == 0) {
        return core::Result<AnalysisResult>::failure(
            core::ErrorCode::InvalidParameter,
            "At least one peak must be requested.");
    }

    if (options.thdHarmonicCount < 2) {
        return core::Result<AnalysisResult>::failure(
            core::ErrorCode::InvalidParameter,
            "The THD harmonic count must be at least two.");
    }

    auto featuresResult = algorithm::calculateBasicFeatures(signal);
    if (!featuresResult) {
        return core::Result<AnalysisResult>::failure(
            featuresResult.error());
    }
    if (stopToken.stop_requested()) {
        return cancelledAnalysis();
    }
    reportProgress(progressCallback, 15, "Calculated time-domain features");

    auto centeredSignalResult = algorithm::removeDc(signal);
    if (!centeredSignalResult) {
        return core::Result<AnalysisResult>::failure(
            centeredSignalResult.error());
    }
    if (stopToken.stop_requested()) {
        return cancelledAnalysis();
    }
    reportProgress(progressCallback, 25, "Removed the DC component");

    auto spectrumResult = algorithm::computeSpectrum(
        centeredSignalResult.value(),
        options.windowType,
        stopToken);
    if (!spectrumResult) {
        return core::Result<AnalysisResult>::failure(
            spectrumResult.error());
    }
    if (stopToken.stop_requested()) {
        return cancelledAnalysis();
    }
    reportProgress(progressCallback, 75, "Calculated the spectrum");

    algorithm::Spectrum spectrum = std::move(spectrumResult).value();
    if (spectrum.powers.size() >= 2
        && options.metricGuardBins >= spectrum.powers.size()) {
        return core::Result<AnalysisResult>::failure(
            core::ErrorCode::InvalidParameter,
            "The metric guard must be smaller than the spectrum bin count.");
    }

    double largestAmplitude = 0.0;
    for (std::size_t index = 1;
         index < spectrum.amplitudes.size();
         ++index) {
        if ((index % 4096U) == 0U && stopToken.stop_requested()) {
            return cancelledAnalysis();
        }
        largestAmplitude = std::max(
            largestAmplitude,
            spectrum.amplitudes[index]);
    }

    std::vector<algorithm::SpectrumPeak> peaks;
    if (largestAmplitude > 0.0) {
        algorithm::PeakOptions peakOptions;
        peakOptions.minimumAmplitude =
            largestAmplitude * options.minimumPeakAmplitudeRatio;
        peakOptions.minimumDistanceHz = options.minimumPeakDistanceHz;
        peakOptions.maximumPeakCount = options.maximumPeakCount;

        auto peaksResult = algorithm::detectPeaks(spectrum, peakOptions);
        if (!peaksResult) {
            return core::Result<AnalysisResult>::failure(
                peaksResult.error());
        }
        peaks = std::move(peaksResult).value();
    }
    if (stopToken.stop_requested()) {
        return cancelledAnalysis();
    }
    reportProgress(progressCallback, 88, "Detected spectrum peaks");

    std::optional<algorithm::SpectrumPeak> dominantPeak;
    if (!peaks.empty()) {
        dominantPeak = *std::max_element(
            peaks.begin(),
            peaks.end(),
            [](const algorithm::SpectrumPeak& left,
               const algorithm::SpectrumPeak& right) {
                return left.amplitude < right.amplitude;
            });
    }

    std::optional<double> snrDb;
    std::optional<double> thdPercent;
    if (dominantPeak.has_value() && dominantPeak->frequencyHz > 0.0) {
        auto snrResult = algorithm::calculateSnrDb(
            spectrum,
            dominantPeak->frequencyHz,
            options.metricGuardBins);
        if (snrResult) {
            snrDb = snrResult.value();
        }

        auto thdResult = algorithm::calculateThdPercent(
            spectrum,
            dominantPeak->frequencyHz,
            options.thdHarmonicCount,
            options.metricGuardBins);
        if (thdResult) {
            thdPercent = thdResult.value();
        }
    }

    if (stopToken.stop_requested()) {
        return cancelledAnalysis();
    }
    reportProgress(progressCallback, 100, "Analysis complete");

    return core::Result<AnalysisResult>::success(AnalysisResult{
        std::move(featuresResult).value(),
        std::move(spectrum),
        std::move(peaks),
        dominantPeak,
        snrDb,
        thdPercent
    });
}

AnalysisService::AnalysisService(
    repository::TaskRepository& taskRepository,
    repository::SignalRepository& signalRepository)
    : taskRepository_(taskRepository),
      signalRepository_(signalRepository)
{
}

core::Result<std::size_t> AnalysisService::interruptRunningTasks()
{
    return taskRepository_.interruptRunningTasks();
}

core::Result<AnalysisResult> AnalysisService::analyze(
    std::int64_t signalRecordId,
    const AnalysisOptions& options,
    std::stop_token stopToken,
    AnalysisProgressCallback progressCallback)
{
    if (stopToken.stop_requested()) {
        return cancelledAnalysis();
    }

    auto storedSignalResult = signalRepository_.findByRecordId(
        signalRecordId,
        stopToken);
    if (!storedSignalResult) {
        return core::Result<AnalysisResult>::failure(
            storedSignalResult.error());
    }
    if (stopToken.stop_requested()) {
        return cancelledAnalysis();
    }
    if (!storedSignalResult.value().has_value()) {
        return core::Result<AnalysisResult>::failure(
            core::ErrorCode::InvalidParameter,
            "The selected signal record does not exist.");
    }

    std::optional<repository::StoredSignal> storedSignalValue =
        std::move(storedSignalResult).value();
    repository::StoredSignal storedSignal =
        std::move(*storedSignalValue);
    std::string signalName = storedSignal.signal.metadata().name;
    if (signalName.empty() && !storedSignal.sourcePath.empty()) {
        signalName = storedSignal.sourcePath.filename().string();
    }
    if (signalName.empty()) {
        signalName = "Signal #" + std::to_string(storedSignal.recordId);
    }

    const repository::AnalysisTaskStart taskStart{
        storedSignal.recordId,
        signalName,
        "signal-analysis",
        analysisAlgorithmVersion(),
        analysisParametersJson(options),
        storedSignal.sampleHash
    };

    auto taskIdResult = taskRepository_.createTask(taskStart);
    if (!taskIdResult) {
        return core::Result<AnalysisResult>::failure(taskIdResult.error());
    }
    const std::int64_t taskId = taskIdResult.value();

    auto markCancelled = [this, taskId]() {
        return taskRepository_.cancelTask(
            taskId,
            "The user cancelled this analysis.");
    };

    if (stopToken.stop_requested()) {
        auto cancelResult = markCancelled();
        if (!cancelResult) {
            return core::Result<AnalysisResult>::failure(
                cancelResult.error());
        }
        return cancelledAnalysis();
    }

    auto startResult = taskRepository_.startTask(taskId);
    if (!startResult) {
        return core::Result<AnalysisResult>::failure(startResult.error());
    }
    if (!startResult.value()) {
        return core::Result<AnalysisResult>::failure(
            core::ErrorCode::DatabaseQueryFailed,
            "The pending analysis task could not be marked RUNNING.");
    }

    auto scaledProgress = [progressCallback](
                              int percent,
                              std::string message) {
        if (progressCallback) {
            progressCallback(
                std::clamp(percent * 95 / 100, 0, 95),
                std::move(message));
        }
    };
    auto analysisResult = analyzeSignal(
        storedSignal.signal,
        options,
        stopToken,
        scaledProgress);
    if (!analysisResult) {
        const core::Error algorithmError = analysisResult.error();
        auto terminalResult = algorithmError.code == core::ErrorCode::Cancelled
            ? markCancelled()
            : taskRepository_.failTask(taskId, algorithmError.message);
        if (!terminalResult) {
            return core::Result<AnalysisResult>::failure(
                terminalResult.error());
        }
        if (!terminalResult.value()) {
            return core::Result<AnalysisResult>::failure(
                core::ErrorCode::DatabaseQueryFailed,
                "The analysis task could not enter its terminal state.");
        }
        return core::Result<AnalysisResult>::failure(algorithmError);
    }

    const auto values = makeFeatureValues(
        analysisResult.value(),
        storedSignal.signal);
    if (stopToken.stop_requested()) {
        auto cancelResult = markCancelled();
        if (!cancelResult) {
            return core::Result<AnalysisResult>::failure(
                cancelResult.error());
        }
        return cancelledAnalysis();
    }

    auto completionResult = taskRepository_.completeTask(taskId, values);
    if (!completionResult) {
        return core::Result<AnalysisResult>::failure(
            completionResult.error());
    }

    reportProgress(progressCallback, 100, "Analysis saved");

    return analysisResult;
}

core::Result<std::vector<repository::AnalysisTaskRecord>>
AnalysisService::history(std::size_t maximumCount)
{
    return taskRepository_.listTasks(maximumCount);
}

core::Result<std::vector<repository::FeatureResultRecord>>
AnalysisService::featureResults(std::int64_t taskId)
{
    return taskRepository_.listFeatureResults(taskId);
}

}  // namespace signallab::app
