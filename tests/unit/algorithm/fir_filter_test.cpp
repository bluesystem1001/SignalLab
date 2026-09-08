#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include "signallab/algorithm/fir_filter.hpp"
#include "signallab/core/error.hpp"
#include "signallab/core/signal.hpp"

namespace signallab::algorithm {
namespace {

core::Signal makeTestSignal(std::vector<double> samples)
{
    auto result = core::Signal::create(
        std::move(samples),
        1000.0,
        {"FIR test", "V", "unit test"});
    return std::move(result).value();
}

TEST(FirFilterTest, LowPassDesignIsSymmetricAndHasUnitDcGain)
{
    auto result = designLowPassFir(31U, 100.0, 1000.0);

    ASSERT_TRUE(result);
    const std::vector<double>& coefficients = result.value();
    ASSERT_EQ(coefficients.size(), 31U);
    for (std::size_t index = 0; index < coefficients.size(); ++index) {
        EXPECT_NEAR(
            coefficients[index],
            coefficients[coefficients.size() - 1U - index],
            1e-14);
        EXPECT_TRUE(std::isfinite(coefficients[index]));
    }
    EXPECT_NEAR(
        std::accumulate(coefficients.begin(), coefficients.end(), 0.0),
        1.0,
        1e-12);
}

TEST(FirFilterTest, HighPassDesignRejectsDc)
{
    auto result = designHighPassFir(31U, 100.0, 1000.0);

    ASSERT_TRUE(result);
    EXPECT_NEAR(
        std::accumulate(result.value().begin(), result.value().end(), 0.0),
        0.0,
        1e-12);
}

TEST(FirFilterTest, AppliesCausalConvolutionToAnImpulse)
{
    const core::Signal impulse = makeTestSignal({1.0, 0.0, 0.0, 0.0});
    const std::vector<double> coefficients{0.25, 0.5, 0.25};

    auto result = applyFir(impulse, coefficients);

    ASSERT_TRUE(result);
    const std::span<const double> output = result.value().samples();
    ASSERT_EQ(output.size(), impulse.sampleCount());
    EXPECT_DOUBLE_EQ(output[0], 0.25);
    EXPECT_DOUBLE_EQ(output[1], 0.5);
    EXPECT_DOUBLE_EQ(output[2], 0.25);
    EXPECT_DOUBLE_EQ(output[3], 0.0);
    EXPECT_DOUBLE_EQ(result.value().sampleRate(), impulse.sampleRate());
    EXPECT_EQ(result.value().metadata().name, impulse.metadata().name);
}

TEST(FirFilterTest, ZeroPadsSamplesBeforeTheBeginning)
{
    const core::Signal signal = makeTestSignal({1.0, 1.0});
    const std::vector<double> coefficients{1.0, 1.0};

    auto result = applyFir(signal, coefficients);

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().sampleCount(), 2U);
    EXPECT_DOUBLE_EQ(result.value().samples()[0], 1.0);
    EXPECT_DOUBLE_EQ(result.value().samples()[1], 2.0);
}

TEST(FirFilterTest, LowAndHighPassHaveExpectedSteadyDcResponses)
{
    const core::Signal dcSignal = makeTestSignal(
        std::vector<double>(64U, 1.0));
    auto lowPass = designLowPassFir(31U, 100.0, 1000.0);
    auto highPass = designHighPassFir(31U, 100.0, 1000.0);
    ASSERT_TRUE(lowPass);
    ASSERT_TRUE(highPass);

    auto lowOutput = applyFir(dcSignal, lowPass.value());
    auto highOutput = applyFir(dcSignal, highPass.value());

    ASSERT_TRUE(lowOutput);
    ASSERT_TRUE(highOutput);
    EXPECT_NEAR(lowOutput.value().samples().back(), 1.0, 1e-12);
    EXPECT_NEAR(highOutput.value().samples().back(), 0.0, 1e-12);
}

TEST(FirFilterTest, RejectsInvalidDesignParameters)
{
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    const auto expectInvalid = [](auto result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
        EXPECT_FALSE(result.error().message.empty());
    };

    expectInvalid(designLowPassFir(1U, 100.0, 1000.0));
    expectInvalid(designLowPassFir(4U, 100.0, 1000.0));
    expectInvalid(designLowPassFir(5U, 0.0, 1000.0));
    expectInvalid(designLowPassFir(5U, -1.0, 1000.0));
    expectInvalid(designLowPassFir(5U, 500.0, 1000.0));
    expectInvalid(designLowPassFir(5U, 501.0, 1000.0));
    expectInvalid(designLowPassFir(5U, nan, 1000.0));
    expectInvalid(designLowPassFir(5U, 100.0, 0.0));
    expectInvalid(designLowPassFir(5U, 100.0, infinity));
    expectInvalid(designHighPassFir(2U, 100.0, 1000.0));
    expectInvalid(designHighPassFir(5U, 100.0, nan));
}

TEST(FirFilterTest, RejectsInvalidApplicationParameters)
{
    const core::Signal signal = makeTestSignal({1.0, 2.0});
    auto emptyResult = applyFir(signal, std::span<const double>{});
    const std::vector<double> nonFiniteCoefficients{
        1.0,
        std::numeric_limits<double>::quiet_NaN()
    };
    auto nonFiniteResult = applyFir(signal, nonFiniteCoefficients);
    auto unknownBoundaryResult = applyFir(
        signal,
        std::vector<double>{1.0},
        static_cast<FirBoundaryStrategy>(99));

    ASSERT_FALSE(emptyResult);
    ASSERT_FALSE(nonFiniteResult);
    ASSERT_FALSE(unknownBoundaryResult);
    EXPECT_EQ(emptyResult.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_EQ(
        nonFiniteResult.error().code,
        core::ErrorCode::InvalidParameter);
    EXPECT_EQ(
        unknownBoundaryResult.error().code,
        core::ErrorCode::InvalidParameter);
}

}  // namespace
}  // namespace signallab::algorithm
