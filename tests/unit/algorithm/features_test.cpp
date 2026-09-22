#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <stop_token>
#include <utility>
#include <vector>

#include "signallab/algorithm/features.hpp"
#include "signallab/algorithm/spectrum.hpp"
#include "signallab/core/error.hpp"
#include "signallab/core/signal.hpp"

namespace signallab::algorithm {
namespace {

Spectrum makeSpectrum(
    std::vector<double> amplitudes,
    std::vector<double> powers,
    double sampleRate)
{
    const std::size_t fftSize = (amplitudes.size() - 1) * 2;
    std::vector<double> frequencies;
    frequencies.reserve(amplitudes.size());
    for (std::size_t index = 0; index < amplitudes.size(); ++index) {
        frequencies.push_back(
            static_cast<double>(index) * sampleRate /
            static_cast<double>(fftSize));
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

TEST(FeaturesTest, CalculatesMeanPopulationVarianceRmsAndPeakToPeak)
{
    const std::vector<double> samples{1.0, 2.0, 3.0, 4.0};

    const auto result = calculateBasicFeatures(samples);

    ASSERT_TRUE(result);
    EXPECT_DOUBLE_EQ(result.value().mean, 2.5);
    EXPECT_DOUBLE_EQ(result.value().variance, 1.25);
    EXPECT_NEAR(result.value().rms, std::sqrt(7.5), 1.0e-12);
    EXPECT_DOUBLE_EQ(result.value().peakToPeak, 3.0);
}

TEST(FeaturesTest, CalculatesFeaturesFromAnImmutableSignal)
{
    auto signalResult = core::Signal::create(
        {-2.0, 0.0, 2.0},
        1000.0,
        core::SignalMetadata{"feature input", "V", "unit test"});
    ASSERT_TRUE(signalResult);

    const auto result = calculateBasicFeatures(signalResult.value());

    ASSERT_TRUE(result);
    EXPECT_DOUBLE_EQ(result.value().mean, 0.0);
    EXPECT_NEAR(result.value().variance, 8.0 / 3.0, 1.0e-12);
    EXPECT_NEAR(result.value().rms, std::sqrt(8.0 / 3.0), 1.0e-12);
    EXPECT_DOUBLE_EQ(result.value().peakToPeak, 4.0);
}

TEST(FeaturesTest, RejectsEmptyAndNonFiniteInput)
{
    const std::vector<double> empty;
    const std::vector<double> nonFinite{
        1.0,
        std::numeric_limits<double>::quiet_NaN()};

    const auto emptyResult = calculateBasicFeatures(empty);
    const auto nonFiniteResult = calculateBasicFeatures(nonFinite);

    ASSERT_FALSE(emptyResult);
    EXPECT_EQ(emptyResult.error().code, core::ErrorCode::EmptySamples);
    ASSERT_FALSE(nonFiniteResult);
    EXPECT_EQ(nonFiniteResult.error().code, core::ErrorCode::NonFiniteSample);
}

TEST(FeaturesTest, HonorsCancellation)
{
    const std::vector<double> samples{1.0, 2.0, 3.0};
    std::stop_source stopSource;
    stopSource.request_stop();

    const auto result = calculateBasicFeatures(
        samples,
        stopSource.get_token());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::Cancelled);
}

TEST(FeaturesTest, CalculatesEnergyInsideAnInclusiveFrequencyBand)
{
    const Spectrum spectrum = makeSpectrum(
        {0.0, 1.0, 2.0, 3.0, 4.0},
        {0.5, 1.0, 2.0, 4.0, 8.0},
        80.0);

    const auto result = calculateBandEnergy(spectrum, 10.0, 30.0);

    ASSERT_TRUE(result);
    EXPECT_DOUBLE_EQ(result.value(), 7.0);
}

TEST(FeaturesTest, RejectsInvalidBandsAndMalformedSpectra)
{
    Spectrum malformed = makeSpectrum(
        {0.0, 1.0, 2.0, 3.0, 4.0},
        {0.0, 1.0, 1.0, 1.0, 1.0},
        80.0);
    malformed.powers.pop_back();

    EXPECT_FALSE(calculateBandEnergy(malformed, 0.0, 10.0));

    Spectrum unknownWindow = makeSpectrum(
        {0.0, 1.0, 2.0, 3.0, 4.0},
        {0.0, 1.0, 1.0, 1.0, 1.0},
        80.0);
    unknownWindow.windowType = static_cast<WindowType>(99);
    EXPECT_FALSE(calculateBandEnergy(unknownWindow, 0.0, 10.0));

    const Spectrum valid = makeSpectrum(
        {0.0, 1.0, 2.0, 3.0, 4.0},
        {0.0, 1.0, 1.0, 1.0, 1.0},
        80.0);
    EXPECT_FALSE(calculateBandEnergy(valid, -1.0, 10.0));
    EXPECT_FALSE(calculateBandEnergy(valid, 20.0, 10.0));
    EXPECT_FALSE(calculateBandEnergy(valid, 0.0, 41.0));
}

TEST(FeaturesTest, HandlesDeterministicWhiteNoise)
{
    std::mt19937 generator(20260908U);
    std::vector<double> samples(65536);
    for (double& sample : samples) {
        const double unitValue =
            static_cast<double>(generator()) /
            static_cast<double>(generator.max());
        sample = 2.0 * unitValue - 1.0;
    }

    const auto result = calculateBasicFeatures(samples);

    ASSERT_TRUE(result);
    EXPECT_NEAR(result.value().mean, 0.0, 0.02);
    EXPECT_NEAR(result.value().variance, 1.0 / 3.0, 0.02);
    EXPECT_NEAR(result.value().rms, std::sqrt(1.0 / 3.0), 0.02);
    EXPECT_LE(result.value().peakToPeak, 2.0);
    EXPECT_GT(result.value().peakToPeak, 1.9);
}

}  // namespace
}  // namespace signallab::algorithm
