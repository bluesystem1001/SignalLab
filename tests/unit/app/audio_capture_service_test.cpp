#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

#include "signallab/algorithm/fir_filter.hpp"
#include "signallab/app/audio_capture_service.hpp"

namespace {

class FakeAudioInput final : public signallab::repository::AudioInput {
public:
    signallab::core::Result<std::vector<signallab::repository::AudioInputDevice>>
    devices() const override
    {
        return signallab::core::Result<std::vector<signallab::repository::AudioInputDevice>>
            ::success({{"fake", "Test microphone"}});
    }

    signallab::core::Result<bool> start(
        const std::string&,
        signallab::repository::AudioInputFormat,
        std::function<void(std::span<const double>)> onSamples,
        std::function<void(std::string)> onError) override
    {
        if (failStart) {
            return signallab::core::Result<bool>::failure(
                signallab::core::ErrorCode::FileOpenFailed, "Device unavailable");
        }
        samplesCallback = std::move(onSamples);
        errorCallback = std::move(onError);
        return signallab::core::Result<bool>::success(true);
    }

    void stop() override
    {
        if (samplesCallback && !finalSamples.empty()) {
            samplesCallback(finalSamples);
            finalSamples.clear();
        }
        samplesCallback = {};
        errorCallback = {};
    }

    bool failStart{false};
    std::function<void(std::span<const double>)> samplesCallback;
    std::function<void(std::string)> errorCallback;
    std::vector<double> finalSamples;
};

class AudioCaptureServiceTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto input = std::make_unique<FakeAudioInput>();
        fake = input.get();
        service = std::make_unique<signallab::app::AudioCaptureService>(std::move(input));
        options.sampleRate = 16000;
        options.maximumDurationSeconds = 1;
        options.filterType = signallab::app::AudioFilterType::None;
    }

    FakeAudioInput* fake{};
    std::unique_ptr<signallab::app::AudioCaptureService> service;
    signallab::app::AudioCaptureOptions options;
};

}

TEST_F(AudioCaptureServiceTest, StopsAndRetainsFinalPartialBufferForSaving)
{
    ASSERT_TRUE(service->start(options));
    const std::vector<double> samples{0.25, -0.75, 0.5};
    fake->samplesCallback(samples);
    EXPECT_FALSE(service->recordedSignal(false));
    EXPECT_FALSE(service->start(options));
    fake->finalSamples = {0.125};
    service->stop();
    service->stop();
    const auto raw = service->recordedSignal(false);
    const auto filtered = service->recordedSignal(true);
    ASSERT_TRUE(raw);
    ASSERT_TRUE(filtered);
    EXPECT_EQ(raw.value().sampleCount(), 4U);
    EXPECT_DOUBLE_EQ(raw.value().samples().back(), 0.125);
    EXPECT_TRUE(std::equal(raw.value().samples().begin(), raw.value().samples().end(),
                           filtered.value().samples().begin()));
    EXPECT_FALSE(service->snapshot().recording);
}

TEST_F(AudioCaptureServiceTest, FiltersContinuouslyAcrossDeviceBuffers)
{
    options.filterType = signallab::app::AudioFilterType::LowPass;
    options.cutoffHz = 1000;
    options.tapCount = 63;
    ASSERT_TRUE(service->start(options));
    std::vector<double> samples(1101);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] = std::sin(2 * std::numbers::pi * 3000
            * static_cast<double>(index) / options.sampleRate);
    }
    const std::span<const double> input(samples);
    fake->samplesCallback(input.first(123));
    fake->samplesCallback(input.subspan(123, 333));
    fake->samplesCallback(input.subspan(456));
    service->stop();
    const auto raw = service->recordedSignal(false);
    const auto filtered = service->recordedSignal(true);
    ASSERT_TRUE(raw);
    ASSERT_TRUE(filtered);
    const auto coefficients = signallab::algorithm::designLowPassFir(
        options.tapCount, options.cutoffHz, options.sampleRate);
    ASSERT_TRUE(coefficients);
    const auto reference = signallab::algorithm::applyFir(raw.value(), coefficients.value());
    ASSERT_TRUE(reference);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        EXPECT_NEAR(filtered.value().samples()[index], reference.value().samples()[index], 1e-12);
    }
}

TEST_F(AudioCaptureServiceTest, EnforcesDurationLimitAndBoundsPreview)
{
    ASSERT_TRUE(service->start(options));
    const std::vector<double> samples(17000, 0.3);
    fake->samplesCallback(samples);
    fake->samplesCallback(samples);
    auto state = service->snapshot(99);
    EXPECT_TRUE(state.limitReached);
    EXPECT_EQ(state.sampleCount, 16000U);
    EXPECT_LE(state.preview.size(), 99U);
    EXPECT_DOUBLE_EQ(state.peakLevel, 0.3);
    EXPECT_TRUE(service->snapshot(0).preview.empty());
    EXPECT_LE(service->snapshot(100000).preview.size(), 4000U);
    EXPECT_EQ(service->snapshot(1).preview.size(), 1U);
    service->stop();
    EXPECT_EQ(service->recordedSignal(false).value().sampleCount(), 16000U);
}

TEST_F(AudioCaptureServiceTest, PreviewOnlyContainsTheLatestTwoSeconds)
{
    options.maximumDurationSeconds = 4;
    ASSERT_TRUE(service->start(options));
    const std::vector<double> samples(48000, 0.2);
    fake->samplesCallback(samples);
    const auto state = service->snapshot(100);
    ASSERT_FALSE(state.preview.empty());
    EXPECT_GE(state.preview.front().timeSeconds, 1.0);
    EXPECT_NEAR(state.preview.back().timeSeconds, 47999.0 / 16000, 1e-12);
    for (std::size_t index = 1; index < state.preview.size(); ++index) {
        EXPECT_LT(state.preview[index - 1].timeSeconds, state.preview[index].timeSeconds);
    }
}

TEST_F(AudioCaptureServiceTest, FailedRestartPreservesThePreviousRecording)
{
    ASSERT_TRUE(service->start(options));
    const std::vector<double> samples{0.2, -0.4};
    fake->samplesCallback(samples);
    service->stop();
    fake->failStart = true;
    options.sampleRate = 48000;
    EXPECT_FALSE(service->start(options));
    const auto saved = service->recordedSignal(false);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved.value().sampleCount(), 2U);
    EXPECT_DOUBLE_EQ(saved.value().sampleRate(), 16000);
    EXPECT_DOUBLE_EQ(saved.value().samples()[1], -0.4);
    fake->failStart = false;
    ASSERT_TRUE(service->start(options));
    EXPECT_EQ(service->snapshot().sampleCount, 0U);
}

TEST_F(AudioCaptureServiceTest, PreviewRetainsPositiveAndNegativeTransientPeaks)
{
    ASSERT_TRUE(service->start(options));
    std::vector<double> samples(16000, 0.0);
    samples[123] = -0.9;
    samples[124] = 0.8;
    fake->samplesCallback(samples);
    const auto state = service->snapshot(20);
    EXPECT_LE(state.preview.size(), 20U);
    EXPECT_TRUE(std::any_of(state.preview.begin(), state.preview.end(),
        [](const auto& point) { return point.raw == -0.9; }));
    EXPECT_TRUE(std::any_of(state.preview.begin(), state.preview.end(),
        [](const auto& point) { return point.raw == 0.8; }));
}

TEST_F(AudioCaptureServiceTest, InvalidOptionsDoNotStartTheDevice)
{
    options.sampleRate = 0;
    EXPECT_FALSE(service->start(options));
    options.sampleRate = 16000;
    options.maximumDurationSeconds = 301;
    EXPECT_FALSE(service->start(options));
    options.maximumDurationSeconds = 1;
    options.filterType = signallab::app::AudioFilterType::HighPass;
    options.cutoffHz = 8000;
    EXPECT_FALSE(service->start(options));
    options.cutoffHz = 1000;
    options.tapCount = 64;
    EXPECT_FALSE(service->start(options));
    options.tapCount = 257;
    EXPECT_FALSE(service->start(options));
    options.tapCount = 63;
    ASSERT_TRUE(service->start(options));
}

TEST_F(AudioCaptureServiceTest, DeviceFailureRetainsAlreadyCapturedAudio)
{
    ASSERT_TRUE(service->start(options));
    const std::vector<double> samples{0.1, 0.2};
    fake->samplesCallback(samples);
    fake->errorCallback("Microphone disconnected");
    fake->samplesCallback(samples);
    EXPECT_EQ(service->snapshot().error, "Microphone disconnected");
    service->stop();
    const auto saved = service->recordedSignal(false);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved.value().sampleCount(), 2U);
}

TEST_F(AudioCaptureServiceTest, RejectsNonFiniteAudioAndEmptySave)
{
    EXPECT_FALSE(service->recordedSignal(false));
    ASSERT_TRUE(service->start(options));
    const std::vector<double> samples{std::numeric_limits<double>::quiet_NaN()};
    fake->samplesCallback(samples);
    EXPECT_FALSE(service->snapshot().error.empty());
    EXPECT_EQ(service->snapshot().sampleCount, 0U);
    service->stop();
    EXPECT_FALSE(service->recordedSignal(true));
}

TEST(AudioCaptureUnavailableTest, MissingBackendFailsWithoutCrashing)
{
    signallab::app::AudioCaptureService service(nullptr);
    EXPECT_FALSE(service.devices());
    EXPECT_FALSE(service.start({}));
    service.stop();
}
