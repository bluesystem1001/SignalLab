#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "signallab/core/signal_generator.hpp"

namespace signallab::core {
namespace {

SignalMetadata testMetadata()
{
    return SignalMetadata{"Generated signal", "V", "unit test"};
}

TEST(SignalGeneratorTest, GeneratesOneKilohertzSineWaveAt48Khz)
{
    const auto result = generateSineWave(
        1000.0,
        2.0,
        48000.0,
        0.01,
        testMetadata());

    ASSERT_TRUE(result.hasValue());
    const Signal& signal = result.value();

    EXPECT_EQ(signal.sampleCount(), 480U);
    EXPECT_DOUBLE_EQ(signal.sampleRate(), 48000.0);
    EXPECT_NEAR(signal.samples()[0], 0.0, 1e-12);
    EXPECT_NEAR(signal.samples()[12], 2.0, 1e-12);
}

TEST(SignalGeneratorTest, GeneratesTheSumOfTwoTones)
{
    const auto result = generateDualTone(
        1000.0,
        1.0,
        2000.0,
        0.5,
        48000.0,
        0.01,
        testMetadata());

    ASSERT_TRUE(result.hasValue());
    const Signal& signal = result.value();

    const double expectedAtIndex6 = std::sqrt(0.5) + 0.5;
    EXPECT_EQ(signal.sampleCount(), 480U);
    EXPECT_NEAR(signal.samples()[6], expectedAtIndex6, 1e-12);
    EXPECT_NEAR(signal.samples()[12], 1.0, 1e-12);
}

TEST(SignalGeneratorTest, RoundsTheSampleCountAndKeepsAtLeastOneSample)
{
    const auto roundedResult = generateSineWave(
        10.0,
        1.0,
        1000.0,
        0.0046,
        testMetadata());
    const auto shortResult = generateSineWave(
        10.0,
        1.0,
        1000.0,
        0.0001,
        testMetadata());

    ASSERT_TRUE(roundedResult.hasValue());
    ASSERT_TRUE(shortResult.hasValue());
    EXPECT_EQ(roundedResult.value().sampleCount(), 5U);
    EXPECT_EQ(shortResult.value().sampleCount(), 1U);
}

TEST(SignalGeneratorTest, RejectsInvalidSineWaveParameters)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();

    EXPECT_FALSE(generateSineWave(nan, 1.0, 48000.0, 0.01, testMetadata()).hasValue());
    EXPECT_FALSE(generateSineWave(1000.0, -1.0, 48000.0, 0.01, testMetadata()).hasValue());
    EXPECT_FALSE(generateSineWave(1000.0, 1.0, 0.0, 0.01, testMetadata()).hasValue());
    EXPECT_FALSE(generateSineWave(1000.0, 1.0, 48000.0, 0.0, testMetadata()).hasValue());
    EXPECT_FALSE(generateSineWave(-1.0, 1.0, 48000.0, 0.01, testMetadata()).hasValue());
    EXPECT_FALSE(generateSineWave(24001.0, 1.0, 48000.0, 0.01, testMetadata()).hasValue());
}

TEST(SignalGeneratorTest, RejectsInvalidDualToneParameters)
{
    EXPECT_FALSE(generateDualTone(
        1000.0,
        1.0,
        2000.0,
        -0.5,
        48000.0,
        0.01,
        testMetadata()).hasValue());

    EXPECT_FALSE(generateDualTone(
        1000.0,
        1.0,
        25000.0,
        0.5,
        48000.0,
        0.01,
        testMetadata()).hasValue());
}

}  // namespace
}  // namespace signallab::core
