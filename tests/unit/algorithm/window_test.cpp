#include <gtest/gtest.h>

#include <limits>
#include <span>
#include <vector>

#include "signallab/algorithm/window.hpp"
#include "signallab/core/error.hpp"

namespace signallab::algorithm {
namespace {

TEST(WindowTest, CreatesRectangularWindow)
{
    auto result = makeWindow(WindowType::Rectangular, 4U);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), std::vector<double>({1.0, 1.0, 1.0, 1.0}));
}

TEST(WindowTest, CreatesSymmetricHannWindow)
{
    auto result = makeWindow(WindowType::Hann, 5U);

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().size(), 5U);
    EXPECT_NEAR(result.value()[0], 0.0, 1e-12);
    EXPECT_NEAR(result.value()[1], 0.5, 1e-12);
    EXPECT_NEAR(result.value()[2], 1.0, 1e-12);
    EXPECT_NEAR(result.value()[3], 0.5, 1e-12);
    EXPECT_NEAR(result.value()[4], 0.0, 1e-12);
}

TEST(WindowTest, CreatesSymmetricHammingWindow)
{
    auto result = makeWindow(WindowType::Hamming, 5U);

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().size(), 5U);
    EXPECT_NEAR(result.value()[0], 0.08, 1e-12);
    EXPECT_NEAR(result.value()[1], 0.54, 1e-12);
    EXPECT_NEAR(result.value()[2], 1.0, 1e-12);
    EXPECT_NEAR(result.value()[3], 0.54, 1e-12);
    EXPECT_NEAR(result.value()[4], 0.08, 1e-12);
}

TEST(WindowTest, DefinesSingleValueHannAndHammingWindowsAsOne)
{
    auto hannResult = makeWindow(WindowType::Hann, 1U);
    auto hammingResult = makeWindow(WindowType::Hamming, 1U);

    ASSERT_TRUE(hannResult);
    ASSERT_TRUE(hammingResult);
    ASSERT_EQ(hannResult.value().size(), 1U);
    ASSERT_EQ(hammingResult.value().size(), 1U);
    EXPECT_DOUBLE_EQ(hannResult.value().front(), 1.0);
    EXPECT_DOUBLE_EQ(hammingResult.value().front(), 1.0);
}

TEST(WindowTest, ComputesCoherentGainAsTheArithmeticMean)
{
    auto rectangularResult = makeWindow(WindowType::Rectangular, 5U);
    auto hannResult = makeWindow(WindowType::Hann, 5U);
    auto hammingResult = makeWindow(WindowType::Hamming, 5U);
    ASSERT_TRUE(rectangularResult);
    ASSERT_TRUE(hannResult);
    ASSERT_TRUE(hammingResult);

    auto rectangularGain = coherentGain(rectangularResult.value());
    auto hannGain = coherentGain(hannResult.value());
    auto hammingGain = coherentGain(hammingResult.value());

    ASSERT_TRUE(rectangularGain);
    ASSERT_TRUE(hannGain);
    ASSERT_TRUE(hammingGain);
    EXPECT_DOUBLE_EQ(rectangularGain.value(), 1.0);
    EXPECT_NEAR(hannGain.value(), 0.4, 1e-12);
    EXPECT_NEAR(hammingGain.value(), 0.448, 1e-12);
}

TEST(WindowTest, MultipliesSamplesByTheSelectedWindow)
{
    const std::vector<double> samples{2.0, 2.0, 2.0, 2.0, 2.0};

    auto result = applyWindow(samples, WindowType::Hann);

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().size(), samples.size());
    EXPECT_NEAR(result.value()[0], 0.0, 1e-12);
    EXPECT_NEAR(result.value()[1], 1.0, 1e-12);
    EXPECT_NEAR(result.value()[2], 2.0, 1e-12);
    EXPECT_NEAR(result.value()[3], 1.0, 1e-12);
    EXPECT_NEAR(result.value()[4], 0.0, 1e-12);
}

TEST(WindowTest, RejectsInvalidWindowInputs)
{
    auto emptyWindow = makeWindow(WindowType::Hann, 0U);
    auto unknownWindow = makeWindow(static_cast<WindowType>(99), 4U);
    auto emptyGain = coherentGain(std::span<const double>{});
    const std::vector<double> nonFiniteWindow{
        1.0,
        std::numeric_limits<double>::infinity()
    };
    auto nonFiniteGain = coherentGain(nonFiniteWindow);
    auto emptyApplication = applyWindow(
        std::span<const double>{}, WindowType::Hamming);
    auto unknownApplication = applyWindow(
        std::vector<double>{1.0}, static_cast<WindowType>(99));
    const std::vector<double> nonFiniteSamples{
        std::numeric_limits<double>::quiet_NaN()
    };
    auto nonFiniteApplication = applyWindow(
        nonFiniteSamples, WindowType::Rectangular);

    EXPECT_FALSE(emptyWindow);
    EXPECT_FALSE(unknownWindow);
    EXPECT_FALSE(emptyGain);
    EXPECT_FALSE(nonFiniteGain);
    EXPECT_FALSE(emptyApplication);
    EXPECT_FALSE(unknownApplication);
    EXPECT_FALSE(nonFiniteApplication);
    EXPECT_EQ(emptyWindow.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_EQ(unknownWindow.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_EQ(emptyGain.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_EQ(nonFiniteGain.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_EQ(emptyApplication.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_EQ(unknownApplication.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_EQ(
        nonFiniteApplication.error().code,
        core::ErrorCode::InvalidParameter);
}

}  // namespace
}  // namespace signallab::algorithm
