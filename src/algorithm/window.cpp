#include "signallab/algorithm/window.hpp"

#include <cmath>
#include <numbers>

#include "signallab/core/error.hpp"

namespace signallab::algorithm {
namespace {

core::Result<std::vector<double>> windowError(const char* message)
{
    return core::Result<std::vector<double>>::failure(
        core::ErrorCode::InvalidParameter,
        message);
}

}  // namespace

core::Result<std::vector<double>> makeWindow(
    WindowType type,
    std::size_t size)
{
    if (size == 0U) {
        return windowError("A window must contain at least one value.");
    }

    if (type != WindowType::Rectangular
        && type != WindowType::Hann
        && type != WindowType::Hamming) {
        return windowError("The requested window type is not supported.");
    }

    std::vector<double> window(size, 1.0);
    if (type == WindowType::Rectangular || size == 1U) {
        return core::Result<std::vector<double>>::success(std::move(window));
    }

    const double denominator = static_cast<double>(size - 1U);
    for (std::size_t index = 0; index < size; ++index) {
        const double angle = 2.0 * std::numbers::pi
            * static_cast<double>(index) / denominator;

        if (type == WindowType::Hann) {
            window[index] = 0.5 - 0.5 * std::cos(angle);
        } else {
            window[index] = 0.54 - 0.46 * std::cos(angle);
        }
    }

    return core::Result<std::vector<double>>::success(std::move(window));
}

core::Result<double> coherentGain(std::span<const double> window)
{
    if (window.empty()) {
        return core::Result<double>::failure(
            core::ErrorCode::InvalidParameter,
            "A window must contain at least one value.");
    }

    long double sum = 0.0L;
    for (double value : window) {
        if (!std::isfinite(value)) {
            return core::Result<double>::failure(
                core::ErrorCode::InvalidParameter,
                "Every window value must be finite.");
        }
        sum += static_cast<long double>(value);
    }

    const double gain = static_cast<double>(
        sum / static_cast<long double>(window.size()));
    return core::Result<double>::success(gain);
}

core::Result<std::vector<double>> applyWindow(
    std::span<const double> samples,
    WindowType type)
{
    if (samples.empty()) {
        return windowError("At least one sample is required.");
    }

    for (double sample : samples) {
        if (!std::isfinite(sample)) {
            return windowError("Every sample must be finite.");
        }
    }

    auto windowResult = makeWindow(type, samples.size());
    if (!windowResult) {
        return core::Result<std::vector<double>>::failure(
            windowResult.error());
    }

    std::vector<double> output = std::move(windowResult).value();
    for (std::size_t index = 0; index < samples.size(); ++index) {
        output[index] *= samples[index];
    }

    return core::Result<std::vector<double>>::success(std::move(output));
}

}  // namespace signallab::algorithm
