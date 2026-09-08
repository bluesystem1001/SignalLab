#include "signallab/app/analysis_service.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "signallab/algorithm/preprocessing.hpp"
#include "signallab/algorithm/quality_metrics.hpp"
#include "signallab/core/error.hpp"

namespace signallab::app {

core::Result<AnalysisResult> analyzeSignal(
    const core::Signal& signal,
    const AnalysisOptions& options)
{
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

    auto centeredSignalResult = algorithm::removeDc(signal);
    if (!centeredSignalResult) {
        return core::Result<AnalysisResult>::failure(
            centeredSignalResult.error());
    }

    auto spectrumResult = algorithm::computeSpectrum(
        centeredSignalResult.value(),
        options.windowType);
    if (!spectrumResult) {
        return core::Result<AnalysisResult>::failure(
            spectrumResult.error());
    }

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

    return core::Result<AnalysisResult>::success(AnalysisResult{
        std::move(featuresResult).value(),
        std::move(spectrum),
        std::move(peaks),
        dominantPeak,
        snrDb,
        thdPercent
    });
}

}  // namespace signallab::app
