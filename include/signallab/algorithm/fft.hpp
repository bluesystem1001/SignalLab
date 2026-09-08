#pragma once

#include <complex>
#include <span>
#include <string_view>
#include <vector>

#include "signallab/core/result.hpp"

namespace signallab::algorithm {

inline constexpr std::string_view kFftVersion = "fft-radix2/1.0.0";

// Computes the forward discrete Fourier transform in O(N log N) time.
// The input length must be a non-zero power of two.
[[nodiscard]] core::Result<std::vector<std::complex<double>>> radix2Fft(
    std::span<const std::complex<double>> input);

}  // namespace signallab::algorithm
