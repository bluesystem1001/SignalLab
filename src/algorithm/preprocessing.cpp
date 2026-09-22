#include "signallab/algorithm/preprocessing.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "signallab/core/error.hpp"

namespace signallab::algorithm {
namespace {

constexpr std::size_t cancellationBatchSize = 4096U;

core::Result<core::Signal> cancelledPreprocessing()
{
    return core::Result<core::Signal>::failure(
        core::ErrorCode::Cancelled,
        "Signal preprocessing was cancelled.");
}

}  // namespace

core::Result<core::Signal> removeDc(
    const core::Signal& signal,
    std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return cancelledPreprocessing();
    }

    long double sum = 0.0L;
    const auto samples = signal.samples();
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if ((index % cancellationBatchSize) == 0U
            && stopToken.stop_requested()) {
            return cancelledPreprocessing();
        }
        sum += static_cast<long double>(samples[index]);
    }

    const long double mean =
        sum / static_cast<long double>(signal.sampleCount());

    std::vector<double> output;
    output.reserve(signal.sampleCount());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if ((index % cancellationBatchSize) == 0U
            && stopToken.stop_requested()) {
            return cancelledPreprocessing();
        }
        const double sample = samples[index];
        output.push_back(static_cast<double>(
            static_cast<long double>(sample) - mean));
    }

    return core::Signal::create(
        std::move(output),
        signal.sampleRate(),
        signal.metadata(),
        stopToken);
}

core::Result<core::Signal> normalizePeak(
    const core::Signal& signal,
    double targetPeak,
    std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return cancelledPreprocessing();
    }
    if (!std::isfinite(targetPeak) || targetPeak <= 0.0) {
        return core::Result<core::Signal>::failure(
            core::ErrorCode::InvalidParameter,
            "The target peak must be a finite number greater than zero.");
    }

    double currentPeak = 0.0;
    const auto samples = signal.samples();
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if ((index % cancellationBatchSize) == 0U
            && stopToken.stop_requested()) {
            return cancelledPreprocessing();
        }
        currentPeak = std::max(currentPeak, std::abs(samples[index]));
    }

    if (currentPeak == 0.0) {
        return core::Result<core::Signal>::failure(
            core::ErrorCode::InvalidParameter,
            "A zero signal cannot be peak-normalized.");
    }

    std::vector<double> output;
    output.reserve(signal.sampleCount());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if ((index % cancellationBatchSize) == 0U
            && stopToken.stop_requested()) {
            return cancelledPreprocessing();
        }
        output.push_back((samples[index] / currentPeak) * targetPeak);
    }

    return core::Signal::create(
        std::move(output),
        signal.sampleRate(),
        signal.metadata(),
        stopToken);
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
