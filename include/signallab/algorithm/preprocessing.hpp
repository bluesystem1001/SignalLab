#pragma once

#include <cstddef>
#include <string_view>

#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"

namespace signallab::algorithm {

inline constexpr std::string_view kPreprocessingAlgorithmVersion = "0.3.0";

// Time complexity: O(N). Extra space: O(N), where N is the sample count.
[[nodiscard]] core::Result<core::Signal> removeDc(
    const core::Signal& signal);

// Time complexity: O(N). Extra space: O(N), where N is the sample count.
[[nodiscard]] core::Result<core::Signal> normalizePeak(
    const core::Signal& signal,
    double targetPeak = 1.0);

// Time complexity: O(K). Extra space: O(K), where K is sampleCount.
[[nodiscard]] core::Result<core::Signal> sliceSignal(
    const core::Signal& signal,
    std::size_t firstIndex,
    std::size_t sampleCount);

}  // namespace signallab::algorithm
