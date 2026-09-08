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
