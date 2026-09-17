#pragma once

#include <cstddef>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

#include "signallab/algorithm/window.hpp"
#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"

namespace signallab::algorithm {

struct Spectrum {
    double sampleRate;
    std::size_t inputSize;
    std::size_t fftSize;
    WindowType windowType;
    double coherentGain;
    std::vector<double> frequenciesHz;
    std::vector<double> amplitudes;
    std::vector<double> powers;
};

inline constexpr std::string_view kSpectrumVersion = "spectrum-one-sided/1.0.0";

// Computes a one-sided spectrum in O(N log N) time.
// Non-power-of-two input is padded with zeros before the FFT.
[[nodiscard]] core::Result<Spectrum> computeSpectrum(
    std::span<const double> samples,
    double sampleRate,
    WindowType windowType = WindowType::Hann,
    std::stop_token stopToken = {});

[[nodiscard]] core::Result<Spectrum> computeSpectrum(
    const core::Signal& signal,
    WindowType windowType = WindowType::Hann,
    std::stop_token stopToken = {});

}  // namespace signallab::algorithm
