#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "signallab/algorithm/features.hpp"
#include "signallab/algorithm/peak_detector.hpp"
#include "signallab/algorithm/spectrum.hpp"
#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"

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

}  // namespace signallab::app
