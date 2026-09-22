#include "signallab/algorithm/quality_metrics.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>

#include "signallab/core/error.hpp"

namespace signallab::algorithm {
namespace {

std::optional<core::Error> cancelledQualityMetric()
{
    return core::Error{
        core::ErrorCode::Cancelled,
        "Quality metric calculation was cancelled."};
}

std::optional<core::Error> validateSpectrum(
    const Spectrum& spectrum,
    std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return cancelledQualityMetric();
    }
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
        if ((index % 4096U) == 0U && stopToken.stop_requested()) {
            return cancelledQualityMetric();
        }
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

core::Result<std::size_t> findNearestNonDcBin(
    const Spectrum& spectrum,
    double frequencyHz)
{
    if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0 ||
        frequencyHz > spectrum.frequenciesHz.back()) {
        return core::Result<std::size_t>::failure(
            core::ErrorCode::InvalidParameter,
            "The fundamental frequency must be finite, above DC and inside "
            "the spectrum.");
    }

    const auto firstNonDc = spectrum.frequenciesHz.begin() + 1;
    const auto upper = std::lower_bound(
        firstNonDc,
        spectrum.frequenciesHz.end(),
        frequencyHz);

    if (upper == spectrum.frequenciesHz.end()) {
        return core::Result<std::size_t>::success(
            spectrum.frequenciesHz.size() - 1);
    }

    const std::size_t upperIndex =
        static_cast<std::size_t>(upper - spectrum.frequenciesHz.begin());
    if (upper == firstNonDc) {
        return core::Result<std::size_t>::success(upperIndex);
    }

    const std::size_t lowerIndex = upperIndex - 1;
    const double lowerDistance =
        frequencyHz - spectrum.frequenciesHz[lowerIndex];
    const double upperDistance =
        spectrum.frequenciesHz[upperIndex] - frequencyHz;

    if (lowerDistance <= upperDistance) {
        return core::Result<std::size_t>::success(lowerIndex);
    }
    return core::Result<std::size_t>::success(upperIndex);
}

std::size_t neighborhoodStart(std::size_t center, std::size_t guardBins)
{
    if (center <= guardBins) {
        return 1;
    }
    return center - guardBins;
}

std::size_t neighborhoodEnd(
    std::size_t center,
    std::size_t guardBins,
    std::size_t lastBin)
{
    if (guardBins > lastBin - center) {
        return lastBin;
    }
    return center + guardBins;
}

core::Result<long double> sumPower(
    const Spectrum& spectrum,
    std::size_t firstBin,
    std::size_t lastBin,
    std::stop_token stopToken)
{
    long double result = 0.0L;
    for (std::size_t index = firstBin; index <= lastBin; ++index) {
        if ((index % 4096U) == 0U && stopToken.stop_requested()) {
            return core::Result<long double>::failure(
                *cancelledQualityMetric());
        }
        result += static_cast<long double>(spectrum.powers[index]);
    }
    if (stopToken.stop_requested()) {
        return core::Result<long double>::failure(
            *cancelledQualityMetric());
    }
    return core::Result<long double>::success(result);
}

}  // namespace

core::Result<double> calculateSnrDb(
    const Spectrum& spectrum,
    double fundamentalHz,
    std::size_t guardBins,
    std::stop_token stopToken)
{
    if (const auto error = validateSpectrum(spectrum, stopToken);
        error.has_value()) {
        return core::Result<double>::failure(*error);
    }

    if (spectrum.powers.size() < 2 || guardBins >= spectrum.powers.size()) {
        return core::Result<double>::failure(
            core::ErrorCode::InvalidParameter,
            "SNR requires a non-DC bin and a guard smaller than the bin count.");
    }

    const auto binResult = findNearestNonDcBin(spectrum, fundamentalHz);
    if (!binResult) {
        return core::Result<double>::failure(binResult.error());
    }

    const std::size_t center = binResult.value();
    const std::size_t lastBin = spectrum.powers.size() - 1;
    const std::size_t firstSignalBin = neighborhoodStart(center, guardBins);
    const std::size_t lastSignalBin =
        neighborhoodEnd(center, guardBins, lastBin);

    auto signalPowerResult = sumPower(
        spectrum,
        firstSignalBin,
        lastSignalBin,
        stopToken);
    if (!signalPowerResult) {
        return core::Result<double>::failure(signalPowerResult.error());
    }
    auto totalPowerResult = sumPower(spectrum, 1, lastBin, stopToken);
    if (!totalPowerResult) {
        return core::Result<double>::failure(totalPowerResult.error());
    }
    const long double signalPower = signalPowerResult.value();
    const long double totalNonDcPower = totalPowerResult.value();
    long double noisePower = totalNonDcPower - signalPower;

    if (signalPower <= 0.0L) {
        return core::Result<double>::failure(
            core::ErrorCode::InvalidParameter,
            "SNR requires positive power around the fundamental frequency.");
    }

    if (noisePower < 0.0L &&
        std::abs(noisePower) <= totalNonDcPower * 1.0e-15L) {
        noisePower = 0.0L;
    }

    if (noisePower < 0.0L) {
        return core::Result<double>::failure(
            core::ErrorCode::InvalidParameter,
            "The spectrum power values produced a negative noise power.");
    }

    if (noisePower == 0.0L) {
        return core::Result<double>::success(
            std::numeric_limits<double>::infinity());
    }

    const long double ratio = signalPower / noisePower;
    const double snrDb = static_cast<double>(10.0L * std::log10(ratio));
    return core::Result<double>::success(snrDb);
}

core::Result<double> calculateThdPercent(
    const Spectrum& spectrum,
    double fundamentalHz,
    std::size_t harmonicCount,
    std::size_t guardBins,
    std::stop_token stopToken)
{
    if (const auto error = validateSpectrum(spectrum, stopToken);
        error.has_value()) {
        return core::Result<double>::failure(*error);
    }

    if (harmonicCount < 2 || spectrum.powers.size() < 2 ||
        guardBins >= spectrum.powers.size()) {
        return core::Result<double>::failure(
            core::ErrorCode::InvalidParameter,
            "THD requires at least two harmonics, a non-DC bin and a valid guard.");
    }

    const auto fundamentalBinResult =
        findNearestNonDcBin(spectrum, fundamentalHz);
    if (!fundamentalBinResult) {
        return core::Result<double>::failure(fundamentalBinResult.error());
    }

    const std::size_t lastBin = spectrum.powers.size() - 1;
    const std::size_t fundamentalBin = fundamentalBinResult.value();
    const std::size_t firstFundamentalBin =
        neighborhoodStart(fundamentalBin, guardBins);
    const std::size_t lastFundamentalBin =
        neighborhoodEnd(fundamentalBin, guardBins, lastBin);
    auto fundamentalPowerResult = sumPower(
        spectrum,
        firstFundamentalBin,
        lastFundamentalBin,
        stopToken);
    if (!fundamentalPowerResult) {
        return core::Result<double>::failure(
            fundamentalPowerResult.error());
    }
    const long double fundamentalPower = fundamentalPowerResult.value();

    if (fundamentalPower <= 0.0L) {
        return core::Result<double>::failure(
            core::ErrorCode::InvalidParameter,
            "THD requires positive power around the fundamental frequency.");
    }

    long double harmonicPower = 0.0L;
    std::size_t previousNeighborhoodEnd = lastFundamentalBin;
    const double maximumFrequency = spectrum.frequenciesHz.back();

    for (std::size_t harmonic = 2; harmonic <= harmonicCount; ++harmonic) {
        if (stopToken.stop_requested()) {
            return core::Result<double>::failure(
                *cancelledQualityMetric());
        }
        const double harmonicFrequency =
            static_cast<double>(harmonic) * fundamentalHz;
        if (!std::isfinite(harmonicFrequency) ||
            harmonicFrequency > maximumFrequency) {
            break;
        }

        const auto harmonicBinResult =
            findNearestNonDcBin(spectrum, harmonicFrequency);
        if (!harmonicBinResult) {
            return core::Result<double>::failure(harmonicBinResult.error());
        }

        const std::size_t harmonicBin = harmonicBinResult.value();
        const std::size_t firstHarmonicBin =
            neighborhoodStart(harmonicBin, guardBins);
        const std::size_t lastHarmonicBin =
            neighborhoodEnd(harmonicBin, guardBins, lastBin);

        if (firstHarmonicBin <= previousNeighborhoodEnd) {
            return core::Result<double>::failure(
                core::ErrorCode::InvalidParameter,
                "Fundamental and harmonic guard neighborhoods must not overlap.");
        }

        auto harmonicPowerResult = sumPower(
            spectrum,
            firstHarmonicBin,
            lastHarmonicBin,
            stopToken);
        if (!harmonicPowerResult) {
            return core::Result<double>::failure(
                harmonicPowerResult.error());
        }
        harmonicPower += harmonicPowerResult.value();
        previousNeighborhoodEnd = lastHarmonicBin;
    }

    const double thdPercent = static_cast<double>(
        std::sqrt(harmonicPower / fundamentalPower) * 100.0L);
    return core::Result<double>::success(thdPercent);
}

}  // namespace signallab::algorithm
