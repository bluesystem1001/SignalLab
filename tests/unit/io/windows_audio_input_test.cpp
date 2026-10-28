#include <gtest/gtest.h>

#include <atomic>
#include <limits>
#include <set>
#include <span>
#include <string>

#include "signallab/io/windows_audio_input.hpp"

namespace signallab::io {
namespace {

TEST(WindowsAudioInputTest, StopIsSafeBeforeStartAndAfterFailedStart)
{
    WindowsAudioInput input;
    input.stop();
    input.stop();
    const auto result = input.start("", {}, {}, {});
    EXPECT_FALSE(result);
    input.stop();
    input.stop();
}

#ifdef _WIN32

TEST(WindowsAudioInputTest, ListsDevicesWithoutStartingCapture)
{
    WindowsAudioInput input;
    const auto result = input.devices();
    ASSERT_TRUE(result) << result.error().message;
    std::set<std::string> ids;
    for (const auto& device : result.value()) {
        EXPECT_TRUE(ids.insert(device.id).second);
    }
    if (!result.value().empty()) {
        EXPECT_TRUE(result.value().front().id.empty());
        EXPECT_FALSE(result.value().front().name.empty());
    }
    input.stop();
}

TEST(WindowsAudioInputTest, RejectsInvalidSampleRatesBeforeOpeningMicrophone)
{
    WindowsAudioInput input;
    std::atomic<bool> callbackReceived{false};
    const auto samples = [&callbackReceived](std::span<const double>) { callbackReceived = true; };
    const auto error = [&callbackReceived](std::string) { callbackReceived = true; };
    for (const unsigned int sampleRate : {0U, 7999U, 192001U, std::numeric_limits<unsigned int>::max()}) {
        const auto result = input.start("", {sampleRate}, samples, error);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, core::ErrorCode::InvalidSampleRate);
        input.stop();
    }
    EXPECT_FALSE(callbackReceived.load());
}

TEST(WindowsAudioInputTest, RejectsMalformedDeviceIdentifiersBeforeOpeningMicrophone)
{
    WindowsAudioInput input;
    std::atomic<bool> callbackReceived{false};
    const auto samples = [&callbackReceived](std::span<const double>) { callbackReceived = true; };
    const auto error = [&callbackReceived](std::string) { callbackReceived = true; };
    for (const std::string deviceId : {"-1", "microphone", "0trailing", " 0", "999999999999999999999999"}) {
        const auto result = input.start(deviceId, {}, samples, error);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
        input.stop();
    }
    EXPECT_FALSE(callbackReceived.load());
}

TEST(WindowsAudioInputTest, RequiresBothCallbacksBeforeOpeningMicrophone)
{
    WindowsAudioInput input;
    std::atomic<bool> callbackReceived{false};
    auto result = input.start("", {}, {}, [&callbackReceived](std::string) { callbackReceived = true; });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
    result = input.start("", {}, [&callbackReceived](std::span<const double>) { callbackReceived = true; }, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
    input.stop();
    EXPECT_FALSE(callbackReceived.load());
}

#else

TEST(WindowsAudioInputTest, ReportsUnsupportedPlatform)
{
    WindowsAudioInput input;
    EXPECT_FALSE(input.devices());
    const auto result = input.start("", {}, [](std::span<const double>) {}, [](std::string) {});
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("Windows"), std::string::npos);
}

#endif

}
}
