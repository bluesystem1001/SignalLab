#include "signallab/algorithm/spectrum.hpp"

#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "signallab/algorithm/fft.hpp"

namespace signallab::algorithm {
namespace {

constexpr std::size_t cancellationBatchSize = 4096U;

core::Result<Spectrum> spectrumFailure(core::ErrorCode code, std::string message)
{
    return core::Result<Spectrum>::failure(code, std::move(message));
}

bool isNyquistBin(std::size_t index, std::size_t fftSize)
{
    return fftSize > 1U && index == fftSize / 2U;
}

bool shouldStop(
    const std::stop_token& stopToken,
    std::size_t completedItemCount)
{
    return completedItemCount % cancellationBatchSize == 0U
        && stopToken.stop_requested();
}

core::Result<Spectrum> cancelledSpectrum()
{
    return spectrumFailure(
        core::ErrorCode::Cancelled,
        "Spectrum calculation was cancelled.");
}

}  // namespace

core::Result<Spectrum> computeSpectrum(
    std::span<const double> samples,
    double sampleRate,
    WindowType windowType,
    std::stop_token stopToken)
{
    if (samples.empty()) {
        return spectrumFailure(
            core::ErrorCode::EmptySamples,
            "Spectrum input must not be empty.");
    }

    if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
        return spectrumFailure(
            core::ErrorCode::InvalidSampleRate,
            "Spectrum sample rate must be finite and greater than zero.");
    }

    if (stopToken.stop_requested()) {
        return cancelledSpectrum();
    }

    for (std::size_t index = 0; index < samples.size(); ++index) {
        if (shouldStop(stopToken, index)) {
            return cancelledSpectrum();
        }

        if (!std::isfinite(samples[index])) {
            return spectrumFailure(
                core::ErrorCode::NonFiniteSample,
                "Spectrum sample at index " + std::to_string(index)
                    + " must be finite.");
        }
    }

    const std::size_t largestSupportedPowerOfTwo =
        std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (samples.size() > largestSupportedPowerOfTwo) {
        return spectrumFailure(
            core::ErrorCode::InvalidParameter,
            "Spectrum input is too large for radix-2 zero padding.");
    }

    const auto windowResult = makeWindow(windowType, samples.size());
    if (!windowResult.hasValue()) {
        return core::Result<Spectrum>::failure(windowResult.error());
    }
    if (stopToken.stop_requested()) {
        return cancelledSpectrum();
    }

    const auto gainResult = coherentGain(windowResult.value());
    if (!gainResult.hasValue()) {
        return core::Result<Spectrum>::failure(gainResult.error());
    }
    const double gain = gainResult.value();
    if (!std::isfinite(gain) || gain <= 0.0) {
        return spectrumFailure(
            core::ErrorCode::InvalidParameter,
            "Spectrum window coherent gain must be finite and greater than zero.");
    }

    double windowEnergy = 0.0;
    for (std::size_t index = 0;
         index < windowResult.value().size();
         ++index) {
        if (shouldStop(stopToken, index)) {
            return cancelledSpectrum();
        }
        const double coefficient = windowResult.value()[index];
        windowEnergy += coefficient * coefficient;
    }
    if (!std::isfinite(windowEnergy) || windowEnergy <= 0.0) {
        return spectrumFailure(
            core::ErrorCode::InvalidParameter,
            "Spectrum window energy must be finite and greater than zero.");
    }

    const std::size_t fftSize = std::bit_ceil(samples.size());
    std::vector<std::complex<double>> fftInput(fftSize, {0.0, 0.0});
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if (shouldStop(stopToken, index)) {
            return cancelledSpectrum();
        }
        fftInput[index] = samples[index] * windowResult.value()[index];
    }

    auto fftResult = radix2Fft(fftInput, stopToken);
    if (!fftResult.hasValue()) {
        return core::Result<Spectrum>::failure(fftResult.error());
    }
    auto fftValues = std::move(fftResult).value();

    Spectrum spectrum{
        sampleRate,
        samples.size(),
        fftSize,
        windowType,
        gain,
        {},
        {},
        {},
    };

    const std::size_t binCount = fftSize / 2U + 1U;
    spectrum.frequenciesHz.reserve(binCount);
    spectrum.amplitudes.reserve(binCount);
    spectrum.powers.reserve(binCount);

    const double amplitudeDenominator = static_cast<double>(samples.size()) * gain;
    const double powerDenominator = static_cast<double>(fftSize) * windowEnergy;

    for (std::size_t index = 0; index < binCount; ++index) {
        if (shouldStop(stopToken, index)) {
            return cancelledSpectrum();
        }

        const bool doubleOneSidedValue = index != 0U && !isNyquistBin(index, fftSize);
        const double oneSidedFactor = doubleOneSidedValue ? 2.0 : 1.0;
        const double frequency =
            (static_cast<double>(index) / static_cast<double>(fftSize))
            * sampleRate;
        const double amplitude =
            oneSidedFactor * std::abs(fftValues[index])
            / amplitudeDenominator;
        const double power =
            oneSidedFactor * std::norm(fftValues[index])
            / powerDenominator;

        if (!std::isfinite(frequency)
            || !std::isfinite(amplitude)
            || !std::isfinite(power)) {
            return spectrumFailure(
                core::ErrorCode::InvalidParameter,
                "Spectrum calculation exceeded the finite numeric range at bin "
                    + std::to_string(index) + ".");
        }

        if (!spectrum.frequenciesHz.empty()
            && frequency <= spectrum.frequenciesHz.back()) {
            return spectrumFailure(
                core::ErrorCode::InvalidParameter,
                "The sample rate is too small to represent distinct "
                "spectrum frequencies.");
        }

        spectrum.frequenciesHz.push_back(frequency);
        spectrum.amplitudes.push_back(amplitude);
        spectrum.powers.push_back(power);
    }

    if (stopToken.stop_requested()) {
        return cancelledSpectrum();
    }

    return core::Result<Spectrum>::success(std::move(spectrum));
}

core::Result<Spectrum> computeSpectrum(
    const core::Signal& signal,
    WindowType windowType,
    std::stop_token stopToken)
{
    return computeSpectrum(
        signal.samples(),
        signal.sampleRate(),
        windowType,
        stopToken);
}

}  // namespace signallab::algorithm
