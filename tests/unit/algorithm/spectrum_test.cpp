#include "signallab/algorithm/spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <numeric>
#include <stop_token>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "signallab/core/signal.hpp"

namespace signallab::algorithm {
namespace {

constexpr double kTightTolerance = 1.0e-12;

std::vector<double> makeTone(
    std::size_t sampleCount,
    double sampleRate,
    double frequency,
    double amplitude)
{
    std::vector<double> samples;
    samples.reserve(sampleCount);

    for (std::size_t index = 0; index < sampleCount; ++index) {
        const double time = static_cast<double>(index) / sampleRate;
        samples.push_back(
            amplitude * std::sin(2.0 * std::numbers::pi_v<double> * frequency * time));
    }

    return samples;
}

TEST(SpectrumTest, MeasuresConstantSignalAtDc)
{
    const std::vector<double> samples(8, 2.5);

    const auto result = computeSpectrum(samples, 800.0, WindowType::Rectangular);

    ASSERT_TRUE(result.hasValue());
    EXPECT_NEAR(result.value().amplitudes[0], 2.5, kTightTolerance);
    EXPECT_NEAR(result.value().powers[0], 6.25, kTightTolerance);
    for (std::size_t index = 1; index < result.value().amplitudes.size(); ++index) {
        EXPECT_NEAR(result.value().amplitudes[index], 0.0, kTightTolerance);
    }
}

TEST(SpectrumTest, PadsNonPowerOfTwoInputAndBuildsFrequencyAxis)
{
    const std::vector<double> samples = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};

    const auto result = computeSpectrum(samples, 800.0, WindowType::Rectangular);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().inputSize, 6U);
    EXPECT_EQ(result.value().fftSize, 8U);
    ASSERT_EQ(result.value().frequenciesHz.size(), 5U);
    EXPECT_NEAR(result.value().frequenciesHz[0], 0.0, kTightTolerance);
    EXPECT_NEAR(result.value().frequenciesHz[1], 100.0, kTightTolerance);
    EXPECT_NEAR(result.value().frequenciesHz[4], 400.0, kTightTolerance);
}

TEST(SpectrumTest, FindsTwoBinCenteredTonesWithCorrectAmplitudeAndPower)
{
    constexpr std::size_t sampleCount = 1'024;
    constexpr double sampleRate = 1'024.0;
    auto samples = makeTone(sampleCount, sampleRate, 64.0, 1.25);
    const auto secondTone = makeTone(sampleCount, sampleRate, 200.0, 0.4);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] += secondTone[index];
    }

    const auto result = computeSpectrum(samples, sampleRate, WindowType::Rectangular);

    ASSERT_TRUE(result.hasValue());
    EXPECT_NEAR(result.value().frequenciesHz[64], 64.0, kTightTolerance);
    EXPECT_NEAR(result.value().frequenciesHz[200], 200.0, kTightTolerance);
    EXPECT_NEAR(result.value().amplitudes[64], 1.25, 1.0e-10);
    EXPECT_NEAR(result.value().amplitudes[200], 0.4, 1.0e-10);

    const double totalPower = std::accumulate(
        result.value().powers.begin(), result.value().powers.end(), 0.0);
    const double expectedPower = (1.25 * 1.25 + 0.4 * 0.4) / 2.0;
    EXPECT_NEAR(totalPower, expectedPower, 1.0e-10);
}

TEST(SpectrumTest, OneSidedPowerObeysParsevalAfterZeroPadding)
{
    const std::vector<double> samples = {0.5, -1.0, 2.0, 0.25, -0.75, 1.5};
    const auto windowResult = makeWindow(WindowType::Hamming, samples.size());
    ASSERT_TRUE(windowResult.hasValue());

    double windowedEnergy = 0.0;
    double windowEnergy = 0.0;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const double coefficient = windowResult.value()[index];
        const double windowedSample = samples[index] * coefficient;
        windowedEnergy += windowedSample * windowedSample;
        windowEnergy += coefficient * coefficient;
    }

    const auto result = computeSpectrum(samples, 2'000.0, WindowType::Hamming);

    ASSERT_TRUE(result.hasValue());
    const double spectralPower = std::accumulate(
        result.value().powers.begin(), result.value().powers.end(), 0.0);
    EXPECT_NEAR(spectralPower, windowedEnergy / windowEnergy, 1.0e-12);
}

TEST(SpectrumTest, AcceptsSignalObject)
{
    auto signalResult = core::Signal::create(
        makeTone(64, 640.0, 50.0, 1.0),
        640.0,
        core::SignalMetadata{"tone", "V", "test"});
    ASSERT_TRUE(signalResult.hasValue());

    const auto result = computeSpectrum(signalResult.value(), WindowType::Rectangular);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().inputSize, 64U);
    EXPECT_DOUBLE_EQ(result.value().sampleRate, 640.0);
}

TEST(SpectrumTest, RejectsEmptyInput)
{
    const std::vector<double> samples;

    const auto result = computeSpectrum(samples, 1'000.0);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, core::ErrorCode::EmptySamples);
}

TEST(SpectrumTest, RejectsInvalidSampleRate)
{
    const std::vector<double> samples = {1.0, 2.0};

    const auto zeroResult = computeSpectrum(samples, 0.0);
    const auto infiniteResult =
        computeSpectrum(samples, std::numeric_limits<double>::infinity());

    ASSERT_FALSE(zeroResult.hasValue());
    EXPECT_EQ(zeroResult.error().code, core::ErrorCode::InvalidSampleRate);
    ASSERT_FALSE(infiniteResult.hasValue());
    EXPECT_EQ(infiniteResult.error().code, core::ErrorCode::InvalidSampleRate);
}

TEST(SpectrumTest, RejectsNonFiniteSample)
{
    const std::vector<double> samples = {
        1.0,
        std::numeric_limits<double>::quiet_NaN(),
    };

    const auto result = computeSpectrum(samples, 1'000.0);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, core::ErrorCode::NonFiniteSample);
}

TEST(SpectrumTest, RejectsNonFiniteDerivedPower)
{
    const std::vector<double> samples = {1.0e200};

    const auto result = computeSpectrum(
        samples,
        1'000.0,
        WindowType::Rectangular);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
}

TEST(SpectrumTest, RejectsAFrequencyAxisThatCannotBeRepresented)
{
    const std::vector<double> samples = {1.0, 0.0, 0.0, 0.0};

    const auto result = computeSpectrum(
        samples,
        std::numeric_limits<double>::denorm_min(),
        WindowType::Rectangular);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
}

TEST(SpectrumTest, PassesAPreRequestedCancellationToTheFft)
{
    const std::vector<double> samples(8U, 1.0);
    std::stop_source stopSource;
    stopSource.request_stop();

    const auto result = computeSpectrum(
        samples,
        1'000.0,
        WindowType::Hann,
        stopSource.get_token());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, core::ErrorCode::Cancelled);
}

TEST(SpectrumTest, PublishesAlgorithmVersion)
{
    EXPECT_EQ(kSpectrumVersion, std::string_view("spectrum-one-sided/1.0.0"));
}

}  // namespace
}  // namespace signallab::algorithm
