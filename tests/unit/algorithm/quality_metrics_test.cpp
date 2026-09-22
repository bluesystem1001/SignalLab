#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <stop_token>
#include <utility>
#include <vector>

#include "signallab/algorithm/quality_metrics.hpp"
#include "signallab/algorithm/spectrum.hpp"
#include "signallab/core/error.hpp"

namespace signallab::algorithm {
namespace {

Spectrum makeSpectrum(std::vector<double> powers, double sampleRate)
{
    const std::size_t fftSize = (powers.size() - 1) * 2;
    std::vector<double> frequencies;
    std::vector<double> amplitudes;
    frequencies.reserve(powers.size());
    amplitudes.reserve(powers.size());

    for (std::size_t index = 0; index < powers.size(); ++index) {
        frequencies.push_back(
            static_cast<double>(index) * sampleRate /
            static_cast<double>(fftSize));
        amplitudes.push_back(std::sqrt(powers[index]));
    }

    return Spectrum{
        sampleRate,
        fftSize,
        fftSize,
        WindowType::Rectangular,
        1.0,
        std::move(frequencies),
        std::move(amplitudes),
        std::move(powers)};
}

TEST(QualityMetricsTest, CalculatesSnrAndExcludesDcPower)
{
    const Spectrum spectrum = makeSpectrum(
        {1000.0, 4.0, 0.04, 0.01, 0.0},
        80.0);

    const auto result = calculateSnrDb(spectrum, 10.0, 0);

    ASSERT_TRUE(result);
    EXPECT_NEAR(result.value(), 10.0 * std::log10(80.0), 1.0e-12);
}

TEST(QualityMetricsTest, CountsSecondThroughKthHarmonicsForThd)
{
    const Spectrum spectrum = makeSpectrum(
        {1000.0, 4.0, 0.04, 0.09, 0.16},
        80.0);

    const auto result = calculateThdPercent(spectrum, 10.0, 4, 0);

    ASSERT_TRUE(result);
    EXPECT_NEAR(result.value(), std::sqrt(0.29 / 4.0) * 100.0, 1.0e-12);
}

TEST(QualityMetricsTest, ReturnsInfiniteSnrWhenThereIsNoNoise)
{
    const Spectrum spectrum = makeSpectrum(
        {2.0, 4.0, 0.0, 0.0, 0.0},
        80.0);

    const auto result = calculateSnrDb(spectrum, 10.0, 0);

    ASSERT_TRUE(result);
    EXPECT_TRUE(std::isinf(result.value()));
    EXPECT_GT(result.value(), 0.0);
}

TEST(QualityMetricsTest, StopsThdAtTheNyquistFrequency)
{
    const Spectrum spectrum = makeSpectrum(
        {0.0, 4.0, 1.0, 0.0, 0.0},
        80.0);

    const auto result = calculateThdPercent(spectrum, 20.0, 100, 0);

    ASSERT_TRUE(result);
    EXPECT_DOUBLE_EQ(result.value(), 0.0);
}

TEST(QualityMetricsTest, RejectsInvalidMetricParameters)
{
    const Spectrum spectrum = makeSpectrum(
        {0.0, 4.0, 1.0, 0.0, 0.0},
        80.0);

    EXPECT_FALSE(calculateSnrDb(spectrum, 0.0, 0));
    EXPECT_FALSE(calculateSnrDb(spectrum, 41.0, 0));
    EXPECT_FALSE(calculateSnrDb(
        spectrum,
        std::numeric_limits<double>::quiet_NaN(),
        0));
    EXPECT_FALSE(calculateSnrDb(spectrum, 10.0, spectrum.powers.size()));
    EXPECT_FALSE(calculateThdPercent(spectrum, 10.0, 1, 0));
    EXPECT_FALSE(calculateThdPercent(spectrum, 10.0, 5, spectrum.powers.size()));
}

TEST(QualityMetricsTest, RejectsZeroFundamentalAndMalformedSpectrum)
{
    const Spectrum zeroFundamental = makeSpectrum(
        {0.0, 0.0, 1.0, 0.0, 0.0},
        80.0);

    EXPECT_FALSE(calculateSnrDb(zeroFundamental, 10.0, 0));
    EXPECT_FALSE(calculateThdPercent(zeroFundamental, 10.0, 5, 0));

    Spectrum malformed = zeroFundamental;
    malformed.amplitudes.pop_back();
    const auto result = calculateSnrDb(malformed, 10.0, 0);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);

    Spectrum unknownWindow = zeroFundamental;
    unknownWindow.windowType = static_cast<WindowType>(99);
    EXPECT_FALSE(calculateSnrDb(unknownWindow, 10.0, 0));
}

TEST(QualityMetricsTest, RejectsOverlappingThdNeighborhoods)
{
    const Spectrum spectrum = makeSpectrum(
        {0.0, 4.0, 1.0, 0.0, 0.0},
        80.0);

    const auto result = calculateThdPercent(spectrum, 10.0, 2, 1);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
}

TEST(QualityMetricsTest, HonorsCancellation)
{
    const Spectrum spectrum = makeSpectrum(
        {0.0, 4.0, 1.0, 0.0, 0.0},
        80.0);
    std::stop_source stopSource;
    stopSource.request_stop();

    const auto snr = calculateSnrDb(
        spectrum,
        10.0,
        0U,
        stopSource.get_token());
    const auto thd = calculateThdPercent(
        spectrum,
        10.0,
        5U,
        0U,
        stopSource.get_token());

    ASSERT_FALSE(snr);
    EXPECT_EQ(snr.error().code, core::ErrorCode::Cancelled);
    ASSERT_FALSE(thd);
    EXPECT_EQ(thd.error().code, core::ErrorCode::Cancelled);
}

}  // namespace
}  // namespace signallab::algorithm
