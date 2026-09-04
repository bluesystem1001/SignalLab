#include <gtest/gtest.h>

#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "signallab/core/signal.hpp"

namespace signallab::core {
namespace {

SignalMetadata testMetadata()
{
    return SignalMetadata{"test signal", "V", "unit test"};
}

TEST(SignalTest, RejectsInvalidSampleRates)
{
    const std::vector<double> invalid_rates{
        0.0,
        -1.0,
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()
    };

    for (double sample_rate : invalid_rates) {
        auto result = Signal::create({1.0}, sample_rate, testMetadata());

        ASSERT_FALSE(result.hasValue());
        EXPECT_FALSE(static_cast<bool>(result));
        EXPECT_EQ(result.error().code, ErrorCode::InvalidSampleRate);
        EXPECT_FALSE(result.error().message.empty());
    }
}

TEST(SignalTest, RejectsEmptySamples)
{
    auto result = Signal::create({}, 1000.0, testMetadata());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::EmptySamples);
}

TEST(SignalTest, RejectsNonFiniteSamples)
{
    const std::vector<std::vector<double>> invalid_samples{
        {0.0, std::numeric_limits<double>::infinity()},
        {std::numeric_limits<double>::quiet_NaN(), 1.0}
    };

    for (const auto& samples : invalid_samples) {
        auto result = Signal::create(samples, 1000.0, testMetadata());

        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::NonFiniteSample);
    }
}

TEST(SignalTest, ReportsCountDurationAndSampleTimes)
{
    auto result = Signal::create(
        {0.0, 1.0, 2.0, 3.0},
        8.0,
        testMetadata());
    ASSERT_TRUE(result);

    const Signal& signal = result.value();
    EXPECT_EQ(signal.sampleCount(), 4U);
    EXPECT_DOUBLE_EQ(signal.sampleRate(), 8.0);
    EXPECT_DOUBLE_EQ(signal.durationSeconds(), 0.5);
    EXPECT_DOUBLE_EQ(signal.sampleTimeSeconds(0), 0.0);
    EXPECT_DOUBLE_EQ(signal.sampleTimeSeconds(2), 0.25);
    EXPECT_THROW((void)signal.sampleTimeSeconds(4), std::out_of_range);
}

TEST(SignalTest, ExposesReadOnlySamplesAndTheirRange)
{
    static_assert(std::is_same_v<
                  decltype(std::declval<const Signal&>().samples()),
                  std::span<const double>>);
    static_assert(std::is_same_v<
                  decltype(std::declval<std::span<const double>>()[0]),
                  const double&>);

    auto result = Signal::create(
        {1.5, -2.0, 0.25, 4.0},
        100.0,
        testMetadata());
    ASSERT_TRUE(result);

    const Signal& signal = result.value();
    const std::span<const double> samples = signal.samples();
    EXPECT_EQ(samples.size(), 4U);
    EXPECT_DOUBLE_EQ(samples.front(), 1.5);
    EXPECT_DOUBLE_EQ(samples.back(), 4.0);

    const auto [minimum, maximum] = signal.sampleRange();
    EXPECT_DOUBLE_EQ(minimum, -2.0);
    EXPECT_DOUBLE_EQ(maximum, 4.0);
}

TEST(SignalTest, KeepsMetadataAndSupportsMovingAResultValue)
{
    auto result = Signal::create({0.0, 1.0}, 2.0, testMetadata());
    ASSERT_TRUE(result);

    Signal signal = std::move(result).value();
    EXPECT_EQ(signal.metadata().name, "test signal");
    EXPECT_EQ(signal.metadata().unit, "V");
    EXPECT_EQ(signal.metadata().source, "unit test");
}

}  // namespace
}  // namespace signallab::core
