#include "signallab/algorithm/fir_filter.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stop_token>
#include <utility>
#include <vector>

#include "signallab/algorithm/window.hpp"
#include "signallab/core/error.hpp"

namespace signallab::algorithm {
namespace {

constexpr std::size_t cancellationBatchOperationCount = 4096U;

core::Result<std::vector<double>> coefficientError(const char* message)
{
    return core::Result<std::vector<double>>::failure(
        core::ErrorCode::InvalidParameter,
        message);
}

core::Result<core::Signal> cancelledFir()
{
    return core::Result<core::Signal>::failure(
        core::ErrorCode::Cancelled,
        "FIR filtering was cancelled.");
}

bool areDesignParametersValid(
    std::size_t tapCount,
    double cutoffHz,
    double sampleRate)
{
    return tapCount >= 3U
        && tapCount % 2U == 1U
        && std::isfinite(cutoffHz)
        && std::isfinite(sampleRate)
        && sampleRate > 0.0
        && cutoffHz > 0.0
        && cutoffHz < sampleRate / 2.0;
}

}  // namespace

core::Result<std::vector<double>> designLowPassFir(
    std::size_t tapCount,
    double cutoffHz,
    double sampleRate)
{
    if (!areDesignParametersValid(tapCount, cutoffHz, sampleRate)) {
        return coefficientError(
            "FIR design needs an odd tap count of at least 3 and a finite "
            "cutoff strictly between zero and the Nyquist frequency.");
    }

    auto windowResult = makeWindow(WindowType::Hamming, tapCount);
    if (!windowResult) {
        return core::Result<std::vector<double>>::failure(
            windowResult.error());
    }
    const std::vector<double>& window = windowResult.value();

    std::vector<double> coefficients(tapCount);
    const std::size_t centerIndex = tapCount / 2U;
    const double normalizedCutoff = cutoffHz / sampleRate;

    for (std::size_t index = 0; index < tapCount; ++index) {
        const long long distance = static_cast<long long>(index)
            - static_cast<long long>(centerIndex);

        double idealCoefficient = 0.0;
        if (distance == 0) {
            idealCoefficient = 2.0 * normalizedCutoff;
        } else {
            const double distanceAsDouble = static_cast<double>(distance);
            idealCoefficient = std::sin(
                2.0 * std::numbers::pi * normalizedCutoff
                    * distanceAsDouble)
                / (std::numbers::pi * distanceAsDouble);
        }

        coefficients[index] = idealCoefficient * window[index];
    }

    long double sum = 0.0L;
    for (double coefficient : coefficients) {
        sum += static_cast<long double>(coefficient);
    }

    if (sum == 0.0L || !std::isfinite(sum)) {
        return coefficientError(
            "The FIR design could not produce finite coefficients.");
    }

    for (double& coefficient : coefficients) {
        coefficient = static_cast<double>(
            static_cast<long double>(coefficient) / sum);
    }

    return core::Result<std::vector<double>>::success(
        std::move(coefficients));
}

core::Result<std::vector<double>> designHighPassFir(
    std::size_t tapCount,
    double cutoffHz,
    double sampleRate)
{
    auto lowPassResult = designLowPassFir(tapCount, cutoffHz, sampleRate);
    if (!lowPassResult) {
        return core::Result<std::vector<double>>::failure(
            lowPassResult.error());
    }

    std::vector<double> coefficients = std::move(lowPassResult).value();
    for (double& coefficient : coefficients) {
        coefficient = -coefficient;
    }
    coefficients[tapCount / 2U] += 1.0;

    return core::Result<std::vector<double>>::success(
        std::move(coefficients));
}

core::Result<core::Signal> applyFir(
    const core::Signal& signal,
    std::span<const double> coefficients,
    FirBoundaryStrategy boundary,
    std::stop_token stopToken)
{
    if (boundary != FirBoundaryStrategy::ZeroPadding) {
        return core::Result<core::Signal>::failure(
            core::ErrorCode::InvalidParameter,
            "The requested FIR boundary strategy is not supported.");
    }

    if (coefficients.empty()) {
        return core::Result<core::Signal>::failure(
            core::ErrorCode::InvalidParameter,
            "An FIR filter must contain at least one coefficient.");
    }

    if (stopToken.stop_requested()) {
        return cancelledFir();
    }

    for (std::size_t index = 0; index < coefficients.size(); ++index) {
        if (index % cancellationBatchOperationCount == 0U
            && stopToken.stop_requested()) {
            return cancelledFir();
        }
        if (!std::isfinite(coefficients[index])) {
            return core::Result<core::Signal>::failure(
                core::ErrorCode::InvalidParameter,
                "Every FIR coefficient must be finite.");
        }
    }

    if (stopToken.stop_requested()) {
        return cancelledFir();
    }

    const std::span<const double> input = signal.samples();
    std::vector<double> output(input.size(), 0.0);
    std::size_t operationsSinceCancellationCheck = 0;

    for (std::size_t outputIndex = 0;
         outputIndex < input.size();
         ++outputIndex) {
        long double value = 0.0L;
        const std::size_t usableCoefficientCount =
            std::min(coefficients.size(), outputIndex + 1U);

        for (std::size_t coefficientIndex = 0;
             coefficientIndex < usableCoefficientCount;
             ++coefficientIndex) {
            if (operationsSinceCancellationCheck
                == cancellationBatchOperationCount) {
                if (stopToken.stop_requested()) {
                    return cancelledFir();
                }
                operationsSinceCancellationCheck = 0;
            }

            value += static_cast<long double>(coefficients[coefficientIndex])
                * static_cast<long double>(
                    input[outputIndex - coefficientIndex]);
            ++operationsSinceCancellationCheck;
        }

        output[outputIndex] = static_cast<double>(value);
    }

    if (stopToken.stop_requested()) {
        return cancelledFir();
    }

    return core::Signal::create(
        std::move(output),
        signal.sampleRate(),
        signal.metadata());
}

}  // namespace signallab::algorithm
