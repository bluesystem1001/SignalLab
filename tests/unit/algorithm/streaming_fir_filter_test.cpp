#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "signallab/algorithm/fir_filter.hpp"
#include "signallab/algorithm/streaming_fir_filter.hpp"
#include "signallab/core/signal.hpp"

namespace signallab::algorithm {
namespace {

TEST(StreamingFirFilterTest, MatchesBatchFilteringAcrossUnevenInputChunks)
{
    std::vector<double> samples(1027U);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] = std::sin(static_cast<double>(index) * 0.17)
            + 0.2 * std::cos(static_cast<double>(index) * 1.33);
    }
    auto signal = core::Signal::create(
        samples, 48000.0, {"Microphone", "normalized", "test"});
    ASSERT_TRUE(signal);
    const std::vector<double> coefficients{0.2, -0.4, 0.75, 0.125, -0.025, 0.15};
    const auto batch = applyFir(signal.value(), coefficients);
    ASSERT_TRUE(batch);
    auto filterResult = StreamingFirFilter::create(coefficients);
    ASSERT_TRUE(filterResult);
    auto filter = std::move(filterResult).value();
    constexpr std::array<std::size_t, 5> chunkSizes{1U, 127U, 3U, 256U, 17U};
    std::size_t offset = 0;
    std::size_t chunkIndex = 0;

    while (offset < samples.size()) {
        const std::size_t count = std::min(
            chunkSizes[chunkIndex % chunkSizes.size()], samples.size() - offset);
        const auto chunk = std::span<const double>(samples).subspan(offset, count);
        for (std::size_t index = 0; index < chunk.size(); ++index) {
            EXPECT_DOUBLE_EQ(
                filter.processSample(chunk[index]),
                batch.value().samples()[offset + index]);
        }
        offset += count;
        ++chunkIndex;
    }
}

TEST(StreamingFirFilterTest, ResetRemovesHistoryAndRestartsZeroPadding)
{
    auto filterResult = StreamingFirFilter::create({0.25, 0.5, 0.25});
    ASSERT_TRUE(filterResult);
    auto filter = std::move(filterResult).value();
    EXPECT_DOUBLE_EQ(filter.processSample(1.0), 0.25);
    EXPECT_DOUBLE_EQ(filter.processSample(1.0), 0.75);
    EXPECT_DOUBLE_EQ(filter.processSample(1.0), 1.0);
    EXPECT_DOUBLE_EQ(filter.processSample(1.0), 1.0);

    filter.reset();

    EXPECT_DOUBLE_EQ(filter.processSample(1.0), 0.25);
    EXPECT_DOUBLE_EQ(filter.processSample(0.0), 0.5);
    EXPECT_DOUBLE_EQ(filter.processSample(0.0), 0.25);
    EXPECT_DOUBLE_EQ(filter.processSample(0.0), 0.0);
}

TEST(StreamingFirFilterTest, IdentityFilterPreservesEverySample)
{
    auto filterResult = StreamingFirFilter::create({1.0});
    ASSERT_TRUE(filterResult);
    auto filter = std::move(filterResult).value();
    for (const double sample : {0.0, 0.75, -1.0, 1.0, -0.25, 0.0}) {
        EXPECT_DOUBLE_EQ(filter.processSample(sample), sample);
    }
}

TEST(StreamingFirFilterTest, HighPassRejectsSteadyDcAfterTheStartupTransient)
{
    auto coefficients = designHighPassFir(63U, 1000.0, 48000.0);
    ASSERT_TRUE(coefficients);
    auto filterResult = StreamingFirFilter::create(std::move(coefficients).value());
    ASSERT_TRUE(filterResult);
    auto filter = std::move(filterResult).value();
    for (std::size_t index = 0; index < 200U; ++index) {
        const double output = filter.processSample(0.5);
        if (index >= 62U) {
            EXPECT_NEAR(output, 0.0, 1.0e-12);
        }
    }
}

TEST(StreamingFirFilterTest, RejectsEmptyAndNonFiniteCoefficients)
{
    for (auto coefficients : std::vector<std::vector<double>>{
             {},
             {0.5, std::numeric_limits<double>::infinity()},
             {std::numeric_limits<double>::quiet_NaN()}}) {
        const auto result = StreamingFirFilter::create(std::move(coefficients));
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
    }
}

}
}
