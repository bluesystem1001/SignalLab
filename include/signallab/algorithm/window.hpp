#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "signallab/core/result.hpp"

namespace signallab::algorithm {

inline constexpr std::string_view kWindowAlgorithmVersion = "0.3.0";

enum class WindowType {
    Rectangular,
    Hann,
    Hamming
};

// Time complexity: O(N). Extra space: O(N), where N is size.
[[nodiscard]] core::Result<std::vector<double>> makeWindow(
    WindowType type,
    std::size_t size);

// Time complexity: O(N). Extra space: O(1), where N is window.size().
[[nodiscard]] core::Result<double> coherentGain(
    std::span<const double> window);

// Time complexity: O(N). Extra space: O(N), where N is samples.size().
[[nodiscard]] core::Result<std::vector<double>> applyWindow(
    std::span<const double> samples,
    WindowType type);

}  // namespace signallab::algorithm
