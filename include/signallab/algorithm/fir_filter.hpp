#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"

namespace signallab::algorithm {

inline constexpr std::string_view kFirFilterAlgorithmVersion = "0.3.0";

enum class FirBoundaryStrategy {
    ZeroPadding
};

// Time complexity: O(T). Extra space: O(T), where T is tapCount.
[[nodiscard]] core::Result<std::vector<double>> designLowPassFir(
    std::size_t tapCount,
    double cutoffHz,
    double sampleRate);

// Time complexity: O(T). Extra space: O(T), where T is tapCount.
[[nodiscard]] core::Result<std::vector<double>> designHighPassFir(
    std::size_t tapCount,
    double cutoffHz,
    double sampleRate);

// Time complexity: O(N * T). Extra space: O(N), where N is the sample
// count and T is coefficients.size().
[[nodiscard]] core::Result<core::Signal> applyFir(
    const core::Signal& signal,
    std::span<const double> coefficients,
    FirBoundaryStrategy boundary = FirBoundaryStrategy::ZeroPadding);

}  // namespace signallab::algorithm
