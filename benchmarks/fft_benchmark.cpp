#include "signallab/algorithm/fft.hpp"
#include "signallab/algorithm/peak_detector.hpp"
#include "signallab/algorithm/spectrum.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <vector>

namespace {

constexpr double kSampleRate = 48'000.0;
constexpr std::size_t kSampleCount = 65'536;
constexpr double kFrequency = 1'000.0;
constexpr std::size_t kIterations = 20;
constexpr std::size_t kWarmupIterations = 3;
constexpr std::size_t kToneBin = 1'365;

std::vector<double> makeSamples()
{
    std::vector<double> samples;
    samples.reserve(kSampleCount);

    for (std::size_t index = 0; index < kSampleCount; ++index) {
        const double time = static_cast<double>(index) / kSampleRate;
        const double sample =
            std::sin(2.0 * std::numbers::pi_v<double> * kFrequency * time);
        samples.push_back(sample);
    }

    return samples;
}

std::vector<std::complex<double>> makeComplexInput(
    const std::vector<double>& samples)
{
    std::vector<std::complex<double>> input;
    input.reserve(samples.size());

    for (double sample : samples) {
        input.emplace_back(sample, 0.0);
    }

    return input;
}

struct TimingSummary {
    double averageMilliseconds;
    double minimumMilliseconds;
};

TimingSummary summarize(const std::vector<double>& elapsedMilliseconds)
{
    double total = 0.0;
    for (double elapsed : elapsedMilliseconds) {
        total += elapsed;
    }

    return TimingSummary{
        total / static_cast<double>(elapsedMilliseconds.size()),
        *std::min_element(
            elapsedMilliseconds.begin(),
            elapsedMilliseconds.end())
    };
}

}  // namespace

int main()
{
    const auto samples = makeSamples();
    const auto fftInput = makeComplexInput(samples);

    for (std::size_t iteration = 0;
         iteration < kWarmupIterations;
         ++iteration) {
        const auto fftResult = signallab::algorithm::radix2Fft(fftInput);
        const auto spectrumResult = signallab::algorithm::computeSpectrum(
            samples,
            kSampleRate,
            signallab::algorithm::WindowType::Hann);
        if (!fftResult.hasValue() || !spectrumResult.hasValue()) {
            std::cerr << "FFT benchmark warm-up failed.\n";
            return 1;
        }
    }

    std::vector<double> fftMilliseconds;
    fftMilliseconds.reserve(kIterations);
    double fftChecksum = 0.0;

    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = signallab::algorithm::radix2Fft(fftInput);
        const auto finish = std::chrono::steady_clock::now();

        if (!result.hasValue()) {
            std::cerr << "FFT benchmark failed: " << result.error().message << '\n';
            return 1;
        }

        fftChecksum += std::abs(result.value()[iteration]);
        const std::chrono::duration<double, std::milli> elapsed = finish - start;
        fftMilliseconds.push_back(elapsed.count());
    }

    std::vector<double> spectrumMilliseconds;
    spectrumMilliseconds.reserve(kIterations);
    double spectrumChecksum = 0.0;

    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = signallab::algorithm::computeSpectrum(
            samples,
            kSampleRate,
            signallab::algorithm::WindowType::Hann);
        const auto finish = std::chrono::steady_clock::now();

        if (!result.hasValue()) {
            std::cerr << "Spectrum benchmark failed: "
                      << result.error().message << '\n';
            return 1;
        }

        spectrumChecksum += result.value().amplitudes[kToneBin];
        const std::chrono::duration<double, std::milli> elapsed = finish - start;
        spectrumMilliseconds.push_back(elapsed.count());
    }

    const auto spectrumResult = signallab::algorithm::computeSpectrum(
        samples,
        kSampleRate,
        signallab::algorithm::WindowType::Hann);
    if (!spectrumResult.hasValue()) {
        std::cerr << "Acceptance spectrum failed: "
                  << spectrumResult.error().message << '\n';
        return 1;
    }

    const auto peakResult = signallab::algorithm::detectPeaks(
        spectrumResult.value(),
        signallab::algorithm::PeakOptions{0.1, 100.0, 5});
    if (!peakResult.hasValue() || peakResult.value().empty()) {
        std::cerr << "Acceptance peak detection failed.\n";
        return 1;
    }

    const auto strongestPeak = std::max_element(
        peakResult.value().begin(),
        peakResult.value().end(),
        [](const signallab::algorithm::SpectrumPeak& left,
           const signallab::algorithm::SpectrumPeak& right) {
            return left.amplitude < right.amplitude;
        });

    const TimingSummary fftTiming = summarize(fftMilliseconds);
    const TimingSummary spectrumTiming = summarize(spectrumMilliseconds);
    const double frequencyError =
        std::abs(strongestPeak->frequencyHz - kFrequency);
    const double amplitudeErrorPercent =
        std::abs(strongestPeak->amplitude - 1.0) * 100.0;

    std::cout << "SignalLab FFT benchmark\n"
              << "version: " << signallab::algorithm::kFftVersion << '\n'
              << "spectrum_version: "
              << signallab::algorithm::kSpectrumVersion << '\n'
              << "samples: " << kSampleCount << '\n'
              << "warmup_iterations: " << kWarmupIterations << '\n'
              << "iterations: " << kIterations << '\n'
              << std::fixed << std::setprecision(3)
              << "fft_average_ms: " << fftTiming.averageMilliseconds << '\n'
              << "fft_minimum_ms: " << fftTiming.minimumMilliseconds << '\n'
              << "spectrum_average_ms: "
              << spectrumTiming.averageMilliseconds << '\n'
              << "spectrum_minimum_ms: "
              << spectrumTiming.minimumMilliseconds << '\n'
              << "fft_checksum: " << fftChecksum << '\n'
              << "spectrum_checksum: " << spectrumChecksum << '\n'
              << "peak_frequency_hz: " << strongestPeak->frequencyHz << '\n'
              << "frequency_error_hz: " << frequencyError << '\n'
              << "peak_amplitude: " << strongestPeak->amplitude << '\n'
              << "amplitude_error_percent: "
              << amplitudeErrorPercent << '\n';

    return 0;
}
