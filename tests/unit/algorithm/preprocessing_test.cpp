#include <gtest/gtest.h>

#include <limits>
#include <stop_token>
#include <utility>
#include <vector>

#include "signallab/algorithm/preprocessing.hpp"
#include "signallab/core/error.hpp"
#include "signallab/core/signal.hpp"

namespace signallab::algorithm {
namespace {

core::SignalMetadata testMetadata()
{
    return {"preprocessing test", "V", "unit test"};
}

core::Signal makeTestSignal(std::vector<double> samples)
{
    auto result = core::Signal::create(
        std::move(samples),
        1000.0,
        testMetadata());
    return std::move(result).value();
}

TEST(PreprocessingTest, RemovesTheMeanFromAConstantOffsetSignal)
{
    const core::Signal signal = makeTestSignal({2.0, 4.0, 6.0, 8.0});

    auto result = removeDc(signal);

    ASSERT_TRUE(result);
    const core::Signal& output = result.value();
    ASSERT_EQ(output.sampleCount(), 4U);
    EXPECT_DOUBLE_EQ(output.samples()[0], -3.0);
    EXPECT_DOUBLE_EQ(output.samples()[1], -1.0);
    EXPECT_DOUBLE_EQ(output.samples()[2], 1.0);
    EXPECT_DOUBLE_EQ(output.samples()[3], 3.0);
    EXPECT_DOUBLE_EQ(output.sampleRate(), signal.sampleRate());
    EXPECT_EQ(output.metadata().name, signal.metadata().name);
}

TEST(PreprocessingTest, NormalizesToTheRequestedPeak)
{
    const core::Signal signal = makeTestSignal({-2.0, 1.0, 0.5});

    auto result = normalizePeak(signal, 0.5);

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().sampleCount(), 3U);
    EXPECT_DOUBLE_EQ(result.value().samples()[0], -0.5);
    EXPECT_DOUBLE_EQ(result.value().samples()[1], 0.25);
    EXPECT_DOUBLE_EQ(result.value().samples()[2], 0.125);
}

TEST(PreprocessingTest, RejectsPeakNormalizationOfAZeroSignal)
{
    const core::Signal signal = makeTestSignal({0.0, 0.0, 0.0});

    auto result = normalizePeak(signal);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
}

TEST(PreprocessingTest, RejectsInvalidTargetPeaks)
{
    const core::Signal signal = makeTestSignal({1.0, -1.0});
    const std::vector<double> invalidTargets{
        0.0,
        -1.0,
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()
    };

    for (double target : invalidTargets) {
        auto result = normalizePeak(signal, target);

        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
        EXPECT_FALSE(result.error().message.empty());
    }
}

TEST(PreprocessingTest, HonorsCancellation)
{
    const core::Signal signal = makeTestSignal({1.0, -1.0, 0.5});
    std::stop_source stopSource;
    stopSource.request_stop();

    const auto removeResult = removeDc(signal, stopSource.get_token());
    const auto normalizeResult = normalizePeak(
        signal,
        1.0,
        stopSource.get_token());

    ASSERT_FALSE(removeResult);
    EXPECT_EQ(removeResult.error().code, core::ErrorCode::Cancelled);
    ASSERT_FALSE(normalizeResult);
    EXPECT_EQ(normalizeResult.error().code, core::ErrorCode::Cancelled);
}

TEST(PreprocessingTest, SlicesSamplesAndKeepsSignalProperties)
{
    const core::Signal signal = makeTestSignal({0.0, 1.0, 2.0, 3.0, 4.0});

    auto result = sliceSignal(signal, 1U, 3U);

    ASSERT_TRUE(result);
    const core::Signal& output = result.value();
    ASSERT_EQ(output.sampleCount(), 3U);
    EXPECT_DOUBLE_EQ(output.samples()[0], 1.0);
    EXPECT_DOUBLE_EQ(output.samples()[1], 2.0);
    EXPECT_DOUBLE_EQ(output.samples()[2], 3.0);
    EXPECT_DOUBLE_EQ(output.sampleRate(), signal.sampleRate());
    EXPECT_EQ(output.metadata().source, signal.metadata().source);
}

TEST(PreprocessingTest, RejectsSlicesOutsideTheSignal)
{
    const core::Signal signal = makeTestSignal({0.0, 1.0, 2.0});
    const std::vector<std::pair<std::size_t, std::size_t>> invalidSlices{
        {0U, 0U},
        {3U, 1U},
        {2U, 2U},
        {1U, std::numeric_limits<std::size_t>::max()}
    };

    for (const auto& [firstIndex, sampleCount] : invalidSlices) {
        auto result = sliceSignal(signal, firstIndex, sampleCount);

        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
        EXPECT_FALSE(result.error().message.empty());
    }
}

}  // namespace
}  // namespace signallab::algorithm
