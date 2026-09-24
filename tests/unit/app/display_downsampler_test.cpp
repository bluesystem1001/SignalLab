#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "signallab/app/display_downsampler.hpp"
#include "signallab/algorithm/spectrum.hpp"
#include "signallab/core/signal.hpp"

namespace {

signallab::core::Signal makeSignal(std::vector<double> samples)
{
    signallab::core::SignalMetadata metadata;
    metadata.name = "Test signal";
    metadata.unit = "V";
    metadata.source = "Unit test";

    auto result = signallab::core::Signal::create(
        std::move(samples),
        1000.0,
        std::move(metadata));
    return std::move(result).value();
}

}  // namespace

TEST(DisplayDownsamplerTest, KeepsEveryPointForASmallSignal)
{
    const auto signal = makeSignal({0.0, 1.0, -1.0, 0.5});

    const auto points = signallab::app::makeDisplayPoints(signal, 2);

    ASSERT_EQ(points.size(), signal.sampleCount());
    for (std::size_t index = 0; index < points.size(); ++index) {
        EXPECT_DOUBLE_EQ(
            points[index].timeSeconds,
            signal.sampleTimeSeconds(index));
        EXPECT_DOUBLE_EQ(points[index].value, signal.samples()[index]);
    }
}

TEST(DisplayDownsamplerTest, KeepsBucketExtremesInTheirOriginalOrder)
{
    const auto signal = makeSignal({0.0, 5.0, -3.0, 2.0});

    const auto points = signallab::app::makeDisplayPoints(signal, 1);

    ASSERT_EQ(points.size(), 2U);
    EXPECT_DOUBLE_EQ(points[0].value, 5.0);
    EXPECT_DOUBLE_EQ(points[1].value, -3.0);
    EXPECT_LT(points[0].timeSeconds, points[1].timeSeconds);
}

TEST(DisplayDownsamplerTest, KeepsExtremesFromOneMillionSamples)
{
    constexpr std::size_t sampleCount = 1'000'000;
    constexpr std::size_t pixelWidth = 1000;

    std::vector<double> samples(sampleCount);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const int repeatingValue = static_cast<int>(index % 101) - 50;
        samples[index] = static_cast<double>(repeatingValue);
    }

    samples[123'456] = -10'000.0;
    samples[654'321] = 10'000.0;
    const auto signal = makeSignal(std::move(samples));

    const auto points =
        signallab::app::makeDisplayPoints(signal, pixelWidth);

    EXPECT_LE(points.size(), pixelWidth * 2);

    const auto hasMinimum = std::any_of(
        points.begin(),
        points.end(),
        [](const signallab::app::DisplayPoint& point) {
            return point.value == -10'000.0;
        });
    const auto hasMaximum = std::any_of(
        points.begin(),
        points.end(),
        [](const signallab::app::DisplayPoint& point) {
            return point.value == 10'000.0;
        });

    EXPECT_TRUE(hasMinimum);
    EXPECT_TRUE(hasMaximum);

    for (std::size_t index = 1; index < points.size(); ++index) {
        EXPECT_LT(points[index - 1].timeSeconds, points[index].timeSeconds);
    }
}

TEST(DisplayDownsamplerTest, ReturnsNoPointsForZeroPixelWidth)
{
    const auto signal = makeSignal({1.0, 2.0, 3.0});

    const auto points = signallab::app::makeDisplayPoints(signal, 0);

    EXPECT_TRUE(points.empty());
}

TEST(DisplayDownsamplerTest, ReturnsNoPointsForAnEmptyOrReversedRange)
{
    const auto signal = makeSignal({1.0, 2.0, 3.0});

    EXPECT_TRUE(signallab::app::makeDisplayPoints(signal, 10, 1, 1).empty());
    EXPECT_TRUE(signallab::app::makeDisplayPoints(signal, 10, 2, 1).empty());
}

TEST(DisplayDownsamplerTest, ClampsRangeToTheAvailableSamples)
{
    const auto signal = makeSignal({1.0, 2.0, 3.0, 4.0});

    const auto points =
        signallab::app::makeDisplayPoints(signal, 10, 2, 100);

    ASSERT_EQ(points.size(), 2U);
    EXPECT_DOUBLE_EQ(points[0].timeSeconds, signal.sampleTimeSeconds(2));
    EXPECT_DOUBLE_EQ(points[0].value, 3.0);
    EXPECT_DOUBLE_EQ(points[1].timeSeconds, signal.sampleTimeSeconds(3));
    EXPECT_DOUBLE_EQ(points[1].value, 4.0);
    EXPECT_TRUE(
        signallab::app::makeDisplayPoints(signal, 10, 100, 200).empty());
}

TEST(DisplayDownsamplerTest, KeepsLocalRangeExtremesInOriginalOrder)
{
    const auto signal = makeSignal(
        {100.0, -100.0, 1.0, 8.0, -6.0, 4.0, 200.0});

    const auto points =
        signallab::app::makeDisplayPoints(signal, 1, 2, 6);

    ASSERT_EQ(points.size(), 2U);
    EXPECT_DOUBLE_EQ(points[0].value, 8.0);
    EXPECT_DOUBLE_EQ(points[1].value, -6.0);
    EXPECT_LT(points[0].timeSeconds, points[1].timeSeconds);
}

TEST(DisplayDownsamplerTest, LimitsOneMillionSampleVisibleRange)
{
    constexpr std::size_t sampleCount = 1'000'000;
    constexpr std::size_t firstIndex = 400'000;
    constexpr std::size_t pastLastIndex = 600'000;
    constexpr std::size_t pixelWidth = 800;

    std::vector<double> samples(sampleCount, 0.0);
    samples[399'999] = -20'000.0;
    samples[456'789] = -10'000.0;
    samples[543'210] = 10'000.0;
    samples[600'000] = 20'000.0;
    const auto signal = makeSignal(std::move(samples));

    const auto points = signallab::app::makeDisplayPoints(
        signal,
        pixelWidth,
        firstIndex,
        pastLastIndex);

    EXPECT_LE(points.size(), pixelWidth * 2);
    EXPECT_TRUE(std::any_of(
        points.begin(),
        points.end(),
        [](const signallab::app::DisplayPoint& point) {
            return point.value == -10'000.0;
        }));
    EXPECT_TRUE(std::any_of(
        points.begin(),
        points.end(),
        [](const signallab::app::DisplayPoint& point) {
            return point.value == 10'000.0;
        }));
    EXPECT_FALSE(std::any_of(
        points.begin(),
        points.end(),
        [](const signallab::app::DisplayPoint& point) {
            return point.value == -20'000.0 || point.value == 20'000.0;
        }));

    for (std::size_t index = 1; index < points.size(); ++index) {
        EXPECT_LT(points[index - 1].timeSeconds, points[index].timeSeconds);
    }
}

TEST(DisplayDownsamplerTest, LimitsALargeSpectrumAndKeepsItsPeak)
{
    constexpr std::size_t binCount = 500'001;
    constexpr std::size_t pixelWidth = 900;

    signallab::algorithm::Spectrum spectrum;
    spectrum.sampleRate = 1'000'000.0;
    spectrum.inputSize = 1'000'000;
    spectrum.fftSize = 1'000'000;
    spectrum.windowType = signallab::algorithm::WindowType::Rectangular;
    spectrum.coherentGain = 1.0;
    spectrum.frequenciesHz.resize(binCount);
    spectrum.amplitudes.assign(binCount, 0.01);
    spectrum.powers.assign(binCount, 0.0);

    for (std::size_t index = 0; index < binCount; ++index) {
        spectrum.frequenciesHz[index] = static_cast<double>(index);
    }
    spectrum.amplitudes[345'678] = 25.0;

    const auto points = signallab::app::makeSpectrumDisplayPoints(
        spectrum,
        pixelWidth);

    EXPECT_LE(points.size(), pixelWidth * 2);
    const auto peak = std::find_if(
        points.begin(),
        points.end(),
        [](const signallab::app::SpectrumDisplayPoint& point) {
            return point.amplitude == 25.0;
        });
    EXPECT_NE(peak, points.end());
}

TEST(DisplayDownsamplerTest, LimitsASpectrumRangeAndClampsItsEnd)
{
    signallab::algorithm::Spectrum spectrum;
    spectrum.frequenciesHz = {0.0, 10.0, 20.0, 30.0, 40.0, 50.0};
    spectrum.amplitudes = {100.0, 1.0, 9.0, -4.0, 3.0, 200.0};

    const auto points = signallab::app::makeSpectrumDisplayPoints(
        spectrum,
        1,
        1,
        5);

    ASSERT_EQ(points.size(), 2U);
    EXPECT_DOUBLE_EQ(points[0].frequencyHz, 20.0);
    EXPECT_DOUBLE_EQ(points[0].amplitude, 9.0);
    EXPECT_DOUBLE_EQ(points[1].frequencyHz, 30.0);
    EXPECT_DOUBLE_EQ(points[1].amplitude, -4.0);

    const auto clamped = signallab::app::makeSpectrumDisplayPoints(
        spectrum,
        10,
        4,
        100);
    ASSERT_EQ(clamped.size(), 2U);
    EXPECT_DOUBLE_EQ(clamped.front().frequencyHz, 40.0);
    EXPECT_DOUBLE_EQ(clamped.back().frequencyHz, 50.0);

    EXPECT_TRUE(signallab::app::makeSpectrumDisplayPoints(
                    spectrum,
                    10,
                    100,
                    200)
                    .empty());
}
