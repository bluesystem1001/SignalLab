#include "signallab/algorithm/preprocessing.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "signallab/core/error.hpp"

namespace signallab::algorithm {

core::Result<core::Signal> removeDc(const core::Signal& signal)
{
    long double sum = 0.0L;
    for (double sample : signal.samples()) {
        sum += static_cast<long double>(sample);
    }

    const long double mean =
        sum / static_cast<long double>(signal.sampleCount());

    std::vector<double> output;
    output.reserve(signal.sampleCount());
    for (double sample : signal.samples()) {
        output.push_back(static_cast<double>(
            static_cast<long double>(sample) - mean));
    }

    return core::Signal::create(
        std::move(output),
        signal.sampleRate(),
        signal.metadata());
}

core::Result<core::Signal> normalizePeak(
    const core::Signal& signal,
    double targetPeak)
{
    if (!std::isfinite(targetPeak) || targetPeak <= 0.0) {
        return core::Result<core::Signal>::failure(
            core::ErrorCode::InvalidParameter,
            "The target peak must be a finite number greater than zero.");
    }

    double currentPeak = 0.0;
    for (double sample : signal.samples()) {
        currentPeak = std::max(currentPeak, std::abs(sample));
    }

    if (currentPeak == 0.0) {
        return core::Result<core::Signal>::failure(
            core::ErrorCode::InvalidParameter,
            "A zero signal cannot be peak-normalized.");
    }

    std::vector<double> output;
    output.reserve(signal.sampleCount());
    for (double sample : signal.samples()) {
        output.push_back((sample / currentPeak) * targetPeak);
    }

    return core::Signal::create(
        std::move(output),
        signal.sampleRate(),
        signal.metadata());
}

core::Result<core::Signal> sliceSignal(
    const core::Signal& signal,
    std::size_t firstIndex,
    std::size_t sampleCount)
{
    if (sampleCount == 0U) {
        return core::Result<core::Signal>::failure(
            core::ErrorCode::InvalidParameter,
            "A signal slice must contain at least one sample.");
    }

    if (firstIndex >= signal.sampleCount()
        || sampleCount > signal.sampleCount() - firstIndex) {
        return core::Result<core::Signal>::failure(
            core::ErrorCode::InvalidParameter,
            "The requested slice is outside the signal.");
    }

    const std::span<const double> input = signal.samples();
    std::vector<double> output(
        input.begin() + static_cast<std::ptrdiff_t>(firstIndex),
        input.begin() + static_cast<std::ptrdiff_t>(firstIndex + sampleCount));

    return core::Signal::create(
        std::move(output),
        signal.sampleRate(),
        signal.metadata());
}

}  // namespace signallab::algorithm
