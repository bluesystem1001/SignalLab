#include "signallab/core/signal.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace signallab::core {

Result<Signal> Signal::create(
    std::vector<double> samples,
    double sample_rate,
    SignalMetadata metadata,
    std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return Result<Signal>::failure(
            ErrorCode::Cancelled,
            "Signal creation was cancelled.");
    }
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
        return Result<Signal>::failure(
            ErrorCode::InvalidSampleRate,
            "The sample rate must be a finite number greater than zero.");
    }

    if (samples.empty()) {
        return Result<Signal>::failure(
            ErrorCode::EmptySamples,
            "A signal must contain at least one sample.");
    }

    for (std::size_t index = 0; index < samples.size(); ++index) {
        if ((index % 4096U) == 0U && stopToken.stop_requested()) {
            return Result<Signal>::failure(
                ErrorCode::Cancelled,
                "Signal creation was cancelled.");
        }
        if (!std::isfinite(samples[index])) {
            return Result<Signal>::failure(
                ErrorCode::NonFiniteSample,
                "Every sample must be a finite number.");
        }
    }

    if (stopToken.stop_requested()) {
        return Result<Signal>::failure(
            ErrorCode::Cancelled,
            "Signal creation was cancelled.");
    }

    return Result<Signal>::success(
        Signal(std::move(samples), sample_rate, std::move(metadata)));
}

Signal::Signal(
    std::vector<double> samples,
    double sample_rate,
    SignalMetadata metadata)
    : samples_(std::move(samples)),
      sample_rate_(sample_rate),
      metadata_(std::move(metadata))
{
}

std::span<const double> Signal::samples() const noexcept
{
    return {samples_.data(), samples_.size()};
}

double Signal::sampleRate() const noexcept
{
    return sample_rate_;
}

std::size_t Signal::sampleCount() const noexcept
{
    return samples_.size();
}

double Signal::durationSeconds() const noexcept
{
    return static_cast<double>(samples_.size()) / sample_rate_;
}

double Signal::sampleTimeSeconds(std::size_t index) const
{
    if (index >= samples_.size()) {
        throw std::out_of_range("The sample index is outside the signal.");
    }

    return static_cast<double>(index) / sample_rate_;
}

std::pair<double, double> Signal::sampleRange() const noexcept
{
    const auto [minimum, maximum] =
        std::minmax_element(samples_.begin(), samples_.end());
    return {*minimum, *maximum};
}

const SignalMetadata& Signal::metadata() const noexcept
{
    return metadata_;
}

}  // namespace signallab::core
