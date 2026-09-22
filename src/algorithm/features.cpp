#include "signallab/algorithm/features.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>

#include "signallab/core/error.hpp"

namespace signallab::algorithm {
namespace {

std::optional<core::Error> validateSpectrum(const Spectrum& spectrum)
{
    if (!std::isfinite(spectrum.sampleRate) || spectrum.sampleRate <= 0.0) {
        return core::Error{
            core::ErrorCode::InvalidParameter,
            "The spectrum sample rate must be finite and greater than zero."};
    }

    if (spectrum.inputSize == 0 || spectrum.fftSize == 0 ||
        spectrum.inputSize > spectrum.fftSize ||
        !std::has_single_bit(spectrum.fftSize) ||
        (spectrum.windowType != WindowType::Rectangular &&
         spectrum.windowType != WindowType::Hann &&
         spectrum.windowType != WindowType::Hamming)) {
        return core::Error{
            core::ErrorCode::InvalidParameter,
            "The spectrum sizes must describe a non-empty radix-2 transform."};
    }

    const std::size_t expectedBinCount = spectrum.fftSize / 2 + 1;
    if (spectrum.frequenciesHz.size() != expectedBinCount ||
        spectrum.amplitudes.size() != expectedBinCount ||
        spectrum.powers.size() != expectedBinCount) {
        return core::Error{
            core::ErrorCode::InvalidParameter,
            "The spectrum frequency, amplitude and power vectors must have "
            "fftSize / 2 + 1 entries."};
    }

    if (!std::isfinite(spectrum.coherentGain) ||
        spectrum.coherentGain <= 0.0) {
        return core::Error{
            core::ErrorCode::InvalidParameter,
            "The spectrum coherent gain must be finite and greater than zero."};
    }

    for (std::size_t index = 0; index < expectedBinCount; ++index) {
        const double frequency = spectrum.frequenciesHz[index];
        const double amplitude = spectrum.amplitudes[index];
        const double power = spectrum.powers[index];

        if (!std::isfinite(frequency) || frequency < 0.0 ||
            !std::isfinite(amplitude) || amplitude < 0.0 ||
            !std::isfinite(power) || power < 0.0) {
            return core::Error{
                core::ErrorCode::InvalidParameter,
                "Spectrum bins must contain finite, non-negative values."};
        }

        if (index > 0 &&
            frequency <= spectrum.frequenciesHz[index - 1]) {
            return core::Error{
                core::ErrorCode::InvalidParameter,
                "Spectrum frequencies must be strictly increasing."};
        }

        const double expectedFrequency =
            (static_cast<double>(index) /
             static_cast<double>(spectrum.fftSize)) *
            spectrum.sampleRate;
        const double tolerance =
            std::max(1.0, std::abs(expectedFrequency)) * 1.0e-9;
        if (std::abs(frequency - expectedFrequency) > tolerance) {
            return core::Error{
                core::ErrorCode::InvalidParameter,
                "Spectrum frequencies do not match its sample rate and FFT size."};
        }
    }

    return std::nullopt;
}

core::Result<BasicFeatures> cancelledFeatures()
{
    return core::Result<BasicFeatures>::failure(
        core::ErrorCode::Cancelled,
        "Basic feature calculation was cancelled.");
}

}  // namespace

core::Result<BasicFeatures> calculateBasicFeatures(
    std::span<const double> samples,
    std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return cancelledFeatures();
    }
    if (samples.empty()) {
        return core::Result<BasicFeatures>::failure(
            core::ErrorCode::EmptySamples,
            "Basic features require at least one sample.");
    }

    long double sum = 0.0L;
    long double squaredSum = 0.0L;
    double minimum = samples.front();
    double maximum = samples.front();

    for (std::size_t index = 0; index < samples.size(); ++index) {
        if ((index % 4096U) == 0U && stopToken.stop_requested()) {
            return cancelledFeatures();
        }
        const double sample = samples[index];
        if (!std::isfinite(sample)) {
            return core::Result<BasicFeatures>::failure(
                core::ErrorCode::NonFiniteSample,
                "Basic features require finite samples.");
        }

        sum += static_cast<long double>(sample);
        squaredSum += static_cast<long double>(sample) *
                      static_cast<long double>(sample);
        minimum = std::min(minimum, sample);
        maximum = std::max(maximum, sample);
    }

    const long double sampleCount =
        static_cast<long double>(samples.size());
    const long double mean = sum / sampleCount;

    long double squaredDifferenceSum = 0.0L;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if ((index % 4096U) == 0U && stopToken.stop_requested()) {
            return cancelledFeatures();
        }
        const double sample = samples[index];
        const long double difference =
            static_cast<long double>(sample) - mean;
        squaredDifferenceSum += difference * difference;
    }

    const BasicFeatures features{
        static_cast<double>(mean),
        static_cast<double>(squaredDifferenceSum / sampleCount),
        static_cast<double>(std::sqrt(squaredSum / sampleCount)),
        maximum - minimum};

    if (!std::isfinite(features.mean) ||
        !std::isfinite(features.variance) ||
        !std::isfinite(features.rms) ||
        !std::isfinite(features.peakToPeak)) {
        return core::Result<BasicFeatures>::failure(
            core::ErrorCode::InvalidParameter,
            "Basic feature calculation exceeded the finite numeric range.");
    }

    if (stopToken.stop_requested()) {
        return cancelledFeatures();
    }
    return core::Result<BasicFeatures>::success(features);
}

core::Result<BasicFeatures> calculateBasicFeatures(
    const core::Signal& signal,
    std::stop_token stopToken)
{
    return calculateBasicFeatures(signal.samples(), stopToken);
}

core::Result<double> calculateBandEnergy(
    const Spectrum& spectrum,
    double lowHz,
    double highHz)
{
    if (const auto error = validateSpectrum(spectrum); error.has_value()) {
        return core::Result<double>::failure(*error);
    }

    const double maximumFrequency = spectrum.frequenciesHz.back();
    if (!std::isfinite(lowHz) || !std::isfinite(highHz) ||
        lowHz < 0.0 || highHz < lowHz || highHz > maximumFrequency) {
        return core::Result<double>::failure(
            core::ErrorCode::InvalidParameter,
            "The frequency band must be finite, ordered and inside the spectrum.");
    }

    long double energy = 0.0L;
    for (std::size_t index = 0; index < spectrum.powers.size(); ++index) {
        const double frequency = spectrum.frequenciesHz[index];
        if (frequency >= lowHz && frequency <= highHz) {
            energy += static_cast<long double>(spectrum.powers[index]);
        }
    }

    const double result = static_cast<double>(energy);
    if (!std::isfinite(result)) {
        return core::Result<double>::failure(
            core::ErrorCode::InvalidParameter,
            "Band energy exceeded the finite numeric range.");
    }

    return core::Result<double>::success(result);
}

}  // namespace signallab::algorithm
