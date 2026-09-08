#include "signallab/algorithm/peak_detector.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "signallab/core/error.hpp"

namespace signallab::algorithm {
namespace {

std::optional<core::Error> validateSpectrum(const Spectrum& spectrum)
{
    const std::size_t expectedBinCount = spectrum.fftSize / 2 + 1;

    if (!std::isfinite(spectrum.sampleRate) || spectrum.sampleRate <= 0.0 ||
        spectrum.inputSize == 0 || spectrum.fftSize == 0 ||
        spectrum.inputSize > spectrum.fftSize ||
        !std::has_single_bit(spectrum.fftSize) ||
        (spectrum.windowType != WindowType::Rectangular &&
         spectrum.windowType != WindowType::Hann &&
         spectrum.windowType != WindowType::Hamming) ||
        !std::isfinite(spectrum.coherentGain) ||
        spectrum.coherentGain <= 0.0) {
        return core::Error{
            core::ErrorCode::InvalidParameter,
            "The spectrum metadata is invalid."};
    }

    if (spectrum.frequenciesHz.size() != expectedBinCount ||
        spectrum.amplitudes.size() != expectedBinCount ||
        spectrum.powers.size() != expectedBinCount) {
        return core::Error{
            core::ErrorCode::InvalidParameter,
            "The spectrum vectors must all contain fftSize / 2 + 1 entries."};
    }

    for (std::size_t index = 0; index < expectedBinCount; ++index) {
        const double frequency = spectrum.frequenciesHz[index];
        const double amplitude = spectrum.amplitudes[index];
        const double power = spectrum.powers[index];
        const double expectedFrequency =
            (static_cast<double>(index) /
             static_cast<double>(spectrum.fftSize)) *
            spectrum.sampleRate;
        const double tolerance =
            std::max(1.0, std::abs(expectedFrequency)) * 1.0e-9;

        if (!std::isfinite(frequency) || frequency < 0.0 ||
            !std::isfinite(amplitude) || amplitude < 0.0 ||
            !std::isfinite(power) || power < 0.0 ||
            std::abs(frequency - expectedFrequency) > tolerance) {
            return core::Error{
                core::ErrorCode::InvalidParameter,
                "The spectrum bins are invalid or inconsistent with the FFT size."};
        }

        if (index > 0 &&
            frequency <= spectrum.frequenciesHz[index - 1]) {
            return core::Error{
                core::ErrorCode::InvalidParameter,
                "Spectrum frequencies must be strictly increasing."};
        }
    }

    return std::nullopt;
}

SpectrumPeak interpolatePeak(const Spectrum& spectrum, std::size_t binIndex)
{
    const double left = spectrum.amplitudes[binIndex - 1];
    const double center = spectrum.amplitudes[binIndex];
    const double right = spectrum.amplitudes[binIndex + 1];

    double offset = 0.0;
    double amplitude = center;

    if (left > 0.0 && center > 0.0 && right > 0.0) {
        const double logLeft = std::log(left);
        const double logCenter = std::log(center);
        const double logRight = std::log(right);
        const double denominator =
            logLeft - 2.0 * logCenter + logRight;

        if (denominator != 0.0 && std::isfinite(denominator)) {
            offset = 0.5 * (logLeft - logRight) / denominator;
            offset = std::clamp(offset, -0.5, 0.5);

            const double logAmplitude =
                logCenter - 0.25 * (logLeft - logRight) * offset;
            const double refinedAmplitude = std::exp(logAmplitude);
            if (std::isfinite(refinedAmplitude)) {
                amplitude = refinedAmplitude;
            }
        }
    }

    const double binWidth =
        (spectrum.frequenciesHz[binIndex + 1] -
         spectrum.frequenciesHz[binIndex - 1]) /
        2.0;

    return SpectrumPeak{
        binIndex,
        spectrum.frequenciesHz[binIndex] + offset * binWidth,
        amplitude};
}

}  // namespace

core::Result<std::vector<SpectrumPeak>> detectPeaks(
    const Spectrum& spectrum,
    const PeakOptions& options)
{
    if (const auto error = validateSpectrum(spectrum); error.has_value()) {
        return core::Result<std::vector<SpectrumPeak>>::failure(*error);
    }

    if (!std::isfinite(options.minimumAmplitude) ||
        options.minimumAmplitude < 0.0 ||
        !std::isfinite(options.minimumDistanceHz) ||
        options.minimumDistanceHz < 0.0 ||
        options.maximumPeakCount == 0) {
        return core::Result<std::vector<SpectrumPeak>>::failure(
            core::ErrorCode::InvalidParameter,
            "Peak thresholds must be finite and non-negative, and the maximum "
            "peak count must be greater than zero.");
    }

    std::vector<SpectrumPeak> candidates;
    if (spectrum.amplitudes.size() >= 3) {
        for (std::size_t index = 1;
             index + 1 < spectrum.amplitudes.size();
             ++index) {
            const double amplitude = spectrum.amplitudes[index];
            if (amplitude >= options.minimumAmplitude &&
                amplitude > spectrum.amplitudes[index - 1] &&
                amplitude > spectrum.amplitudes[index + 1]) {
                candidates.push_back(interpolatePeak(spectrum, index));
            }
        }
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const SpectrumPeak& left, const SpectrumPeak& right) {
            if (left.amplitude == right.amplitude) {
                return left.binIndex < right.binIndex;
            }
            return left.amplitude > right.amplitude;
        });

    std::vector<SpectrumPeak> selected;
    selected.reserve(std::min(options.maximumPeakCount, candidates.size()));

    for (const SpectrumPeak& candidate : candidates) {
        bool isFarEnough = true;
        for (const SpectrumPeak& existing : selected) {
            if (std::abs(candidate.frequencyHz - existing.frequencyHz) <
                options.minimumDistanceHz) {
                isFarEnough = false;
                break;
            }
        }

        if (isFarEnough) {
            selected.push_back(candidate);
            if (selected.size() == options.maximumPeakCount) {
                break;
            }
        }
    }

    std::sort(
        selected.begin(),
        selected.end(),
        [](const SpectrumPeak& left, const SpectrumPeak& right) {
            return left.frequencyHz < right.frequencyHz;
        });

    return core::Result<std::vector<SpectrumPeak>>::success(
        std::move(selected));
}

}  // namespace signallab::algorithm
