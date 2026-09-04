#include "signallab/core/signal_generator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "signallab/core/error.hpp"

namespace signallab::core {
namespace {

Result<Signal> generatorError(ErrorCode code, const std::string& message)
{
    return Result<Signal>::failure(Error{code, message});
}

bool isFrequencyValid(double frequencyHz, double sampleRate)
{
    const double nyquistFrequency = sampleRate / 2.0;
    return frequencyHz >= 0.0 && frequencyHz <= nyquistFrequency;
}

bool calculateSampleCount(
    double sampleRate,
    double durationSeconds,
    std::size_t& sampleCount)
{
    const double exactSampleCount = sampleRate * durationSeconds;
    if (!std::isfinite(exactSampleCount)) {
        return false;
    }

    const double roundedSampleCount = std::round(exactSampleCount);
    const double maximumSampleCount =
        static_cast<double>(std::numeric_limits<std::size_t>::max());

    if (roundedSampleCount >= maximumSampleCount) {
        return false;
    }

    sampleCount = static_cast<std::size_t>(
        std::max(1.0, roundedSampleCount));
    return true;
}

}  // namespace

Result<Signal> generateSineWave(
    double frequencyHz,
    double amplitude,
    double sampleRate,
    double durationSeconds,
    SignalMetadata metadata)
{
    if (!std::isfinite(frequencyHz)
        || !std::isfinite(amplitude)
        || !std::isfinite(sampleRate)
        || !std::isfinite(durationSeconds)) {
        return generatorError(
            ErrorCode::InvalidParameter,
            "Generator parameters must be finite numbers");
    }

    if (sampleRate <= 0.0) {
        return generatorError(
            ErrorCode::InvalidSampleRate,
            "Sample rate must be greater than zero");
    }

    if (durationSeconds <= 0.0) {
        return generatorError(
            ErrorCode::InvalidDuration,
            "Duration must be greater than zero");
    }

    if (amplitude < 0.0) {
        return generatorError(
            ErrorCode::InvalidAmplitude,
            "Amplitude cannot be negative");
    }

    if (!isFrequencyValid(frequencyHz, sampleRate)) {
        return generatorError(
            ErrorCode::InvalidFrequency,
            "Frequency must be between zero and the Nyquist frequency");
    }

    std::size_t sampleCount = 0;
    if (!calculateSampleCount(sampleRate, durationSeconds, sampleCount)) {
        return generatorError(
            ErrorCode::InvalidDuration,
            "The requested signal contains too many samples");
    }

    std::vector<double> samples(sampleCount);
    const double angularFrequency = 2.0 * std::numbers::pi * frequencyHz;

    for (std::size_t index = 0; index < sampleCount; ++index) {
        const double timeSeconds = static_cast<double>(index) / sampleRate;
        samples[index] = amplitude * std::sin(angularFrequency * timeSeconds);
    }

    return Signal::create(std::move(samples), sampleRate, std::move(metadata));
}

Result<Signal> generateDualTone(
    double firstFrequencyHz,
    double firstAmplitude,
    double secondFrequencyHz,
    double secondAmplitude,
    double sampleRate,
    double durationSeconds,
    SignalMetadata metadata)
{
    if (!std::isfinite(firstFrequencyHz)
        || !std::isfinite(firstAmplitude)
        || !std::isfinite(secondFrequencyHz)
        || !std::isfinite(secondAmplitude)
        || !std::isfinite(sampleRate)
        || !std::isfinite(durationSeconds)) {
        return generatorError(
            ErrorCode::InvalidParameter,
            "Generator parameters must be finite numbers");
    }

    if (sampleRate <= 0.0) {
        return generatorError(
            ErrorCode::InvalidSampleRate,
            "Sample rate must be greater than zero");
    }

    if (durationSeconds <= 0.0) {
        return generatorError(
            ErrorCode::InvalidDuration,
            "Duration must be greater than zero");
    }

    if (firstAmplitude < 0.0 || secondAmplitude < 0.0) {
        return generatorError(
            ErrorCode::InvalidAmplitude,
            "Amplitudes cannot be negative");
    }

    if (!isFrequencyValid(firstFrequencyHz, sampleRate)
        || !isFrequencyValid(secondFrequencyHz, sampleRate)) {
        return generatorError(
            ErrorCode::InvalidFrequency,
            "Frequencies must be between zero and the Nyquist frequency");
    }

    std::size_t sampleCount = 0;
    if (!calculateSampleCount(sampleRate, durationSeconds, sampleCount)) {
        return generatorError(
            ErrorCode::InvalidDuration,
            "The requested signal contains too many samples");
    }

    std::vector<double> samples(sampleCount);
    const double firstAngularFrequency =
        2.0 * std::numbers::pi * firstFrequencyHz;
    const double secondAngularFrequency =
        2.0 * std::numbers::pi * secondFrequencyHz;

    for (std::size_t index = 0; index < sampleCount; ++index) {
        const double timeSeconds = static_cast<double>(index) / sampleRate;
        const double firstTone =
            firstAmplitude * std::sin(firstAngularFrequency * timeSeconds);
        const double secondTone =
            secondAmplitude * std::sin(secondAngularFrequency * timeSeconds);
        samples[index] = firstTone + secondTone;
    }

    return Signal::create(std::move(samples), sampleRate, std::move(metadata));
}

}  // namespace signallab::core
