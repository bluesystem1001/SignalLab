#include "signallab/algorithm/fft.hpp"

#include <complex>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace signallab::algorithm {
namespace {

constexpr double kTolerance = 1.0e-12;

TEST(Radix2FftTest, TransformsConstantSignal)
{
    const std::vector<std::complex<double>> input(4, {2.0, 0.0});

    const auto result = radix2Fft(input);

    ASSERT_TRUE(result.hasValue());
    ASSERT_EQ(result.value().size(), 4U);
    EXPECT_NEAR(result.value()[0].real(), 8.0, kTolerance);
    EXPECT_NEAR(result.value()[0].imag(), 0.0, kTolerance);
    for (std::size_t index = 1; index < result.value().size(); ++index) {
        EXPECT_NEAR(result.value()[index].real(), 0.0, kTolerance);
        EXPECT_NEAR(result.value()[index].imag(), 0.0, kTolerance);
    }
}

TEST(Radix2FftTest, TransformsImpulseSignal)
{
    const std::vector<std::complex<double>> input = {
        {1.0, 0.0},
        {0.0, 0.0},
        {0.0, 0.0},
        {0.0, 0.0},
    };

    const auto result = radix2Fft(input);

    ASSERT_TRUE(result.hasValue());
    for (const auto value : result.value()) {
        EXPECT_NEAR(value.real(), 1.0, kTolerance);
        EXPECT_NEAR(value.imag(), 0.0, kTolerance);
    }
}

TEST(Radix2FftTest, MatchesFourPointDftCalculatedByHand)
{
    const std::vector<std::complex<double>> input = {
        {1.0, 0.0},
        {2.0, 0.0},
        {3.0, 0.0},
        {4.0, 0.0},
    };

    const auto result = radix2Fft(input);

    ASSERT_TRUE(result.hasValue());
    const std::vector<std::complex<double>> expected = {
        {10.0, 0.0},
        {-2.0, 2.0},
        {-2.0, 0.0},
        {-2.0, -2.0},
    };
    ASSERT_EQ(result.value().size(), expected.size());

    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_NEAR(result.value()[index].real(), expected[index].real(), kTolerance);
        EXPECT_NEAR(result.value()[index].imag(), expected[index].imag(), kTolerance);
    }
}

TEST(Radix2FftTest, RejectsEmptyInput)
{
    const std::vector<std::complex<double>> input;

    const auto result = radix2Fft(input);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
}

TEST(Radix2FftTest, RejectsLengthThatIsNotPowerOfTwo)
{
    const std::vector<std::complex<double>> input(3, {1.0, 0.0});

    const auto result = radix2Fft(input);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
}

TEST(Radix2FftTest, RejectsNonFiniteRealOrImaginaryParts)
{
    const std::vector<std::complex<double>> nonFiniteReal = {
        {1.0, 0.0},
        {std::numeric_limits<double>::quiet_NaN(), 0.0},
    };
    const std::vector<std::complex<double>> nonFiniteImaginary = {
        {1.0, 0.0},
        {0.0, std::numeric_limits<double>::infinity()},
    };

    const auto realResult = radix2Fft(nonFiniteReal);
    const auto imaginaryResult = radix2Fft(nonFiniteImaginary);

    ASSERT_FALSE(realResult.hasValue());
    EXPECT_EQ(realResult.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_NE(realResult.error().message.find("index 1"), std::string::npos);
    ASSERT_FALSE(imaginaryResult.hasValue());
    EXPECT_EQ(imaginaryResult.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_NE(imaginaryResult.error().message.find("index 1"), std::string::npos);
}

TEST(Radix2FftTest, RejectsArithmeticOverflow)
{
    const double maximum = std::numeric_limits<double>::max();
    const std::vector<std::complex<double>> input = {
        {maximum, 0.0},
        {maximum, 0.0},
    };

    const auto result = radix2Fft(input);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_NE(result.error().message.find("output"), std::string::npos);
}

TEST(Radix2FftTest, PublishesAlgorithmVersion)
{
    EXPECT_EQ(kFftVersion, std::string_view("fft-radix2/1.0.0"));
}

}  // namespace
}  // namespace signallab::algorithm
