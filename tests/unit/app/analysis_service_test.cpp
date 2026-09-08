#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <utility>

#include "signallab/app/analysis_service.hpp"
#include "signallab/core/error.hpp"
#include "signallab/core/signal_generator.hpp"

namespace {

signallab::core::Signal makeAcceptanceSignal()
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t sampleCount = 65'536;

    signallab::core::SignalMetadata metadata;
    metadata.name = "FFT acceptance sine";
    metadata.unit = "amplitude";
    metadata.source = "unit test";

    auto result = signallab::core::generateSineWave(
        1000.0,
        1.0,
        sampleRate,
        static_cast<double>(sampleCount) / sampleRate,
        std::move(metadata));
    return std::move(result).value();
}

}  // namespace

TEST(AnalysisServiceTest, RunsTheSynchronousAnalysisClosure)
{
    constexpr double sampleRate = 48000.0;
    constexpr double frequency = 1000.0;
    constexpr double amplitude = 1.0;
    constexpr double binWidth = sampleRate / 65'536.0;

    const auto signal = makeAcceptanceSignal();
    const auto result = signallab::app::analyzeSignal(signal);

    ASSERT_TRUE(result.hasValue());
    ASSERT_TRUE(result.value().dominantPeak.has_value());
    EXPECT_LE(
        std::abs(result.value().dominantPeak->frequencyHz - frequency),
        binWidth);
    EXPECT_LE(
        std::abs(result.value().dominantPeak->amplitude - amplitude)
            / amplitude,
        0.02);
    EXPECT_EQ(result.value().spectrum.inputSize, signal.sampleCount());
    EXPECT_EQ(result.value().spectrum.fftSize, signal.sampleCount());
    EXPECT_NEAR(result.value().features.rms, std::sqrt(0.5), 0.001);
    EXPECT_TRUE(result.value().snrDb.has_value());
    EXPECT_TRUE(result.value().thdPercent.has_value());
}

TEST(AnalysisServiceTest, RejectsInvalidOptions)
{
    const auto signal = makeAcceptanceSignal();
    signallab::app::AnalysisOptions options;
    options.minimumPeakAmplitudeRatio = 1.5;

    const auto result = signallab::app::analyzeSignal(signal, options);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(
        result.error().code,
        signallab::core::ErrorCode::InvalidParameter);

    options = {};
    options.thdHarmonicCount = 1;
    const auto harmonicResult = signallab::app::analyzeSignal(signal, options);
    ASSERT_FALSE(harmonicResult.hasValue());
    EXPECT_EQ(
        harmonicResult.error().code,
        signallab::core::ErrorCode::InvalidParameter);

    options = {};
    options.metricGuardBins = signal.sampleCount();
    const auto guardResult = signallab::app::analyzeSignal(signal, options);
    ASSERT_FALSE(guardResult.hasValue());
    EXPECT_EQ(
        guardResult.error().code,
        signallab::core::ErrorCode::InvalidParameter);

    options = {};
    options.minimumPeakDistanceHz = -1.0;
    EXPECT_FALSE(signallab::app::analyzeSignal(signal, options));

    options = {};
    options.maximumPeakCount = 0;
    EXPECT_FALSE(signallab::app::analyzeSignal(signal, options));
}
