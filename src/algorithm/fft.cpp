#include "signallab/algorithm/fft.hpp"

#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace signallab::algorithm {
namespace {

constexpr std::size_t cancellationBatchSize = 4096U;

bool shouldStop(
    const std::stop_token& stopToken,
    std::size_t completedItemCount)
{
    return completedItemCount % cancellationBatchSize == 0U
        && stopToken.stop_requested();
}

bool reorderByReversedBits(
    std::vector<std::complex<double>>& values,
    const std::stop_token& stopToken)
{
    const std::size_t count = values.size();
    std::size_t reversed = 0;

    for (std::size_t index = 1; index < count; ++index) {
        if (shouldStop(stopToken, index)) {
            return false;
        }

        std::size_t bit = count / 2;

        while ((reversed & bit) != 0U) {
            reversed ^= bit;
            bit /= 2;
        }
        reversed ^= bit;

        if (index < reversed) {
            std::swap(values[index], values[reversed]);
        }
    }

    return !stopToken.stop_requested();
}

bool runButterflyStages(
    std::vector<std::complex<double>>& values,
    const std::stop_token& stopToken)
{
    const std::size_t count = values.size();

    for (std::size_t blockSize = 2; blockSize <= count;) {
        // Every FFT stage is a cancellation boundary. Large stages also
        // check after each small batch of butterfly operations.
        if (stopToken.stop_requested()) {
            return false;
        }

        const double angle =
            -2.0 * std::numbers::pi_v<double> / static_cast<double>(blockSize);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        const std::size_t halfSize = blockSize / 2;
        std::size_t completedButterflies = 0;

        for (std::size_t blockStart = 0; blockStart < count; blockStart += blockSize) {
            std::complex<double> weight(1.0, 0.0);

            for (std::size_t offset = 0; offset < halfSize; ++offset) {
                if (shouldStop(stopToken, completedButterflies)) {
                    return false;
                }

                const std::complex<double> even = values[blockStart + offset];
                const std::complex<double> odd =
                    weight * values[blockStart + offset + halfSize];

                values[blockStart + offset] = even + odd;
                values[blockStart + offset + halfSize] = even - odd;
                weight *= step;
                ++completedButterflies;
            }
        }

        if (blockSize == count) {
            break;
        }
        blockSize *= 2;
    }

    return !stopToken.stop_requested();
}

core::Result<std::vector<std::complex<double>>> cancelledFft()
{
    return core::Result<std::vector<std::complex<double>>>::failure(
        core::ErrorCode::Cancelled,
        "FFT calculation was cancelled.");
}

}  // namespace

core::Result<std::vector<std::complex<double>>> radix2Fft(
    std::span<const std::complex<double>> input,
    std::stop_token stopToken)
{
    if (input.empty()) {
        return core::Result<std::vector<std::complex<double>>>::failure(
            core::ErrorCode::InvalidParameter,
            "FFT input must not be empty.");
    }

    if (!std::has_single_bit(input.size())) {
        return core::Result<std::vector<std::complex<double>>>::failure(
            core::ErrorCode::InvalidParameter,
            "FFT input length must be a power of two.");
    }

    if (stopToken.stop_requested()) {
        return cancelledFft();
    }

    for (std::size_t index = 0; index < input.size(); ++index) {
        if (shouldStop(stopToken, index)) {
            return cancelledFft();
        }

        if (!std::isfinite(input[index].real())
            || !std::isfinite(input[index].imag())) {
            return core::Result<std::vector<std::complex<double>>>::failure(
                core::ErrorCode::InvalidParameter,
                "FFT value at index " + std::to_string(index)
                    + " must have finite real and imaginary parts.");
        }
    }

    std::vector<std::complex<double>> output(input.begin(), input.end());
    if (!reorderByReversedBits(output, stopToken)
        || !runButterflyStages(output, stopToken)) {
        return cancelledFft();
    }

    for (std::size_t index = 0; index < output.size(); ++index) {
        if (shouldStop(stopToken, index)) {
            return cancelledFft();
        }

        if (!std::isfinite(output[index].real())
            || !std::isfinite(output[index].imag())) {
            return core::Result<std::vector<std::complex<double>>>::failure(
                core::ErrorCode::InvalidParameter,
                "FFT output at index " + std::to_string(index)
                    + " exceeded the finite numeric range.");
        }
    }

    if (stopToken.stop_requested()) {
        return cancelledFft();
    }

    return core::Result<std::vector<std::complex<double>>>::success(std::move(output));
}

}  // namespace signallab::algorithm
