#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

#include "signallab/algorithm/peak_detector.hpp"
#include "signallab/algorithm/spectrum.hpp"
#include "signallab/core/error.hpp"

namespace signallab::algorithm {
namespace {

Spectrum makeSpectrum(std::vector<double> amplitudes, double sampleRate)
{
    const std::size_t fftSize = (amplitudes.size() - 1) * 2;
    std::vector<double> frequencies;
    std::vector<double> powers;
    frequencies.reserve(amplitudes.size());
    powers.reserve(amplitudes.size());

    for (std::size_t index = 0; index < amplitudes.size(); ++index) {
        frequencies.push_back(
            static_cast<double>(index) * sampleRate /
            static_cast<double>(fftSize));
        powers.push_back(amplitudes[index] * amplitudes[index]);
    }

    return Spectrum{
        sampleRate,
        fftSize,
        fftSize,
        WindowType::Rectangular,
        1.0,
        std::move(frequencies),
        std::move(amplitudes),
        std::move(powers)};
}

TEST(PeakDetectorTest, SuppressesWeakerPeaksInsideMinimumDistance)
{
    const Spectrum spectrum = makeSpectrum(
        {0.0, 1.0, 5.0, 1.0, 0.5, 4.0, 0.5, 3.0, 0.0},
        160.0);
    const PeakOptions options{0.0, 25.0, 10};

    const auto result = detectPeaks(spectrum, options);

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().size(), 2U);
    EXPECT_NEAR(result.value()[0].frequencyHz, 20.0, 1.0e-12);
    EXPECT_NEAR(result.value()[1].frequencyHz, 50.0, 1.0e-12);
    EXPECT_GT(result.value()[0].amplitude, result.value()[1].amplitude);
}

TEST(PeakDetectorTest, RefinesFrequencyAndAmplitudeInLogDomain)
{
    constexpr double expectedBinPosition = 2.25;
    constexpr double expectedAmplitude = 10.0;
    std::vector<double> amplitudes(5);
    for (std::size_t index = 0; index < amplitudes.size(); ++index) {
        const double distance =
            static_cast<double>(index) - expectedBinPosition;
        amplitudes[index] = expectedAmplitude * std::exp(-distance * distance);
    }

    const Spectrum spectrum = makeSpectrum(std::move(amplitudes), 800.0);
    const auto result = detectPeaks(spectrum);

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_EQ(result.value()[0].binIndex, 2U);
    EXPECT_NEAR(result.value()[0].frequencyHz, 225.0, 1.0e-12);
    EXPECT_NEAR(result.value()[0].amplitude, expectedAmplitude, 1.0e-12);
}

TEST(PeakDetectorTest, AppliesAmplitudeAndCountLimits)
{
    const Spectrum spectrum = makeSpectrum(
        {0.0, 1.0, 5.0, 1.0, 0.5, 4.0, 0.5, 3.0, 0.0},
        160.0);

    const auto result = detectPeaks(spectrum, PeakOptions{3.5, 0.0, 1});

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_EQ(result.value()[0].binIndex, 2U);
}

TEST(PeakDetectorTest, RejectsInvalidOptionsAndMalformedSpectrum)
{
    const Spectrum spectrum = makeSpectrum(
        {0.0, 1.0, 3.0, 1.0, 0.0},
        80.0);

    EXPECT_FALSE(detectPeaks(spectrum, PeakOptions{-1.0, 0.0, 10}));
    EXPECT_FALSE(detectPeaks(spectrum, PeakOptions{0.0, -1.0, 10}));
    EXPECT_FALSE(detectPeaks(spectrum, PeakOptions{0.0, 0.0, 0}));
    EXPECT_FALSE(detectPeaks(
        spectrum,
        PeakOptions{std::numeric_limits<double>::infinity(), 0.0, 10}));

    Spectrum malformed = spectrum;
    malformed.frequenciesHz.pop_back();
    const auto malformedResult = detectPeaks(malformed);
    ASSERT_FALSE(malformedResult);
    EXPECT_EQ(
        malformedResult.error().code,
        core::ErrorCode::InvalidParameter);

    Spectrum unknownWindow = spectrum;
    unknownWindow.windowType = static_cast<WindowType>(99);
    EXPECT_FALSE(detectPeaks(unknownWindow));
}

TEST(PeakDetectorTest, FindsOneKilohertzToneWithinOneFftBin)
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t sampleCount = 65536;
    constexpr double frequencyHz = 1000.0;
    constexpr double amplitude = 1.0;

    std::vector<double> samples(sampleCount);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const double time = static_cast<double>(index) / sampleRate;
        samples[index] = amplitude * std::sin(
            2.0 * std::numbers::pi * frequencyHz * time);
    }

    const auto spectrumResult = computeSpectrum(samples, sampleRate);
    ASSERT_TRUE(spectrumResult);
    const auto peakResult = detectPeaks(
        spectrumResult.value(),
        PeakOptions{0.1, 100.0, 5});
    ASSERT_TRUE(peakResult);
    ASSERT_FALSE(peakResult.value().empty());

    const auto strongest = std::max_element(
        peakResult.value().begin(),
        peakResult.value().end(),
        [](const SpectrumPeak& left, const SpectrumPeak& right) {
            return left.amplitude < right.amplitude;
        });
    const double fftBinWidth = sampleRate / static_cast<double>(sampleCount);

    EXPECT_NEAR(strongest->frequencyHz, frequencyHz, fftBinWidth);
    EXPECT_NEAR(strongest->amplitude, amplitude, amplitude * 0.02);
}

}  // namespace
}  // namespace signallab::algorithm
