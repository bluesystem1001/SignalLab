#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "signallab/algorithm/features.hpp"
#include "signallab/algorithm/peak_detector.hpp"
#include "signallab/algorithm/spectrum.hpp"
#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"
#include "signallab/repository/signal_repository.hpp"
#include "signallab/repository/task_repository.hpp"

namespace signallab::app {

inline constexpr std::string_view kAnalysisServiceVersion{
    "analysis-service/1.0.0"};

struct AnalysisOptions {
    algorithm::WindowType windowType{algorithm::WindowType::Hann};
    double minimumPeakAmplitudeRatio{0.01};
    double minimumPeakDistanceHz{0.0};
    std::size_t maximumPeakCount{10};
    std::size_t metricGuardBins{1};
    std::size_t thdHarmonicCount{5};
};

struct AnalysisResult {
    algorithm::BasicFeatures features;
    algorithm::Spectrum spectrum;
    std::vector<algorithm::SpectrumPeak> peaks;
    // These values stay empty when the signal has no usable fundamental or
    // when that signal does not permit the selected metric definition.
    std::optional<algorithm::SpectrumPeak> dominantPeak;
    std::optional<double> snrDb;
    std::optional<double> thdPercent;
};

// Runs one complete analysis synchronously. Stage 5 will move this call to a
// background task without changing the algorithm interfaces.
[[nodiscard]] core::Result<AnalysisResult> analyzeSignal(
    const core::Signal& signal,
    const AnalysisOptions& options = {});

// Adds persistence and history around the pure analyzeSignal function. The
// algorithm remains synchronous in stage 4; stage 5 can schedule this service
// without changing the repository contract.
class AnalysisService {
public:
    AnalysisService(
        repository::TaskRepository& taskRepository,
        repository::SignalRepository& signalRepository);

    [[nodiscard]] core::Result<std::size_t> interruptRunningTasks();

    [[nodiscard]] core::Result<AnalysisResult> analyze(
        std::int64_t signalRecordId,
        const AnalysisOptions& options = {});

    [[nodiscard]] core::Result<std::vector<repository::AnalysisTaskRecord>>
    history(std::size_t maximumCount = 100);

    [[nodiscard]] core::Result<
        std::vector<repository::FeatureResultRecord>>
    featureResults(std::int64_t taskId);

private:
    repository::TaskRepository& taskRepository_;
    repository::SignalRepository& signalRepository_;
};

}  // namespace signallab::app
