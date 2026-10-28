#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <QTemporaryDir>

#include "signallab/io/wav_reader.hpp"
#include "signallab/io/wav_writer.hpp"

namespace signallab::io {
namespace {

class WavWriterTest : public testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(directory_.isValid());
    }

    [[nodiscard]] std::filesystem::path directoryPath() const
    {
#ifdef _WIN32
        return std::filesystem::path(directory_.path().toStdWString());
#else
        return std::filesystem::u8path(directory_.path().toStdString());
#endif
    }

    [[nodiscard]] core::Signal makeSignal(
        std::vector<double> samples,
        double sampleRate = 48000.0) const
    {
        return core::Signal::create(
            std::move(samples), sampleRate,
            {"Recorded microphone", "normalized", "unit test"}).value();
    }

    [[nodiscard]] std::string readBytes(const std::filesystem::path& path) const
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

private:
    QTemporaryDir directory_;
};

TEST_F(WavWriterTest, RoundTripsMultipleBlocksWithAUnicodeFilename)
{
    const auto path = directoryPath() / u8"麦克风录音.wav";
    std::vector<double> samples(40001U);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] = 0.71 * std::sin(static_cast<double>(index) * 0.173)
            + 0.12;
    }
    const auto signal = makeSignal(samples, 44100.0);

    const auto writeResult = writePcm16Wav(path, signal);

    ASSERT_TRUE(writeResult) << writeResult.error().message;
    EXPECT_TRUE(writeResult.value());
    EXPECT_EQ(std::filesystem::file_size(path), 44U + samples.size() * 2U);
    const auto preview = previewWav(path);
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(preview.value().channelCount, 1U);
    const auto readResult = readWav(path, {});
    ASSERT_TRUE(readResult) << readResult.error().message;
    EXPECT_DOUBLE_EQ(readResult.value().sampleRate(), 44100.0);
    ASSERT_EQ(readResult.value().sampleCount(), samples.size());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        EXPECT_NEAR(readResult.value().samples()[index], samples[index], 0.5 / 32768.0);
    }
}

TEST_F(WavWriterTest, ClampsFullScaleAndRoundsToNearestPcm16Sample)
{
    const auto path = directoryPath() / "clamped.wav";
    const auto signal = makeSignal({
        -2.0, -1.0, -0.5, -1.5 / 32768.0, -0.5 / 32768.0,
        0.0, 0.5 / 32768.0, 1.5 / 32768.0, 0.5, 1.0, 2.0});

    const auto writeResult = writePcm16Wav(path, signal);

    ASSERT_TRUE(writeResult) << writeResult.error().message;
    const auto readResult = readWav(path, {});
    ASSERT_TRUE(readResult) << readResult.error().message;
    const std::vector<int> expected{
        -32768, -32768, -16384, -2, -1, 0, 1, 2, 16384, 32767, 32767};
    ASSERT_EQ(readResult.value().sampleCount(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_DOUBLE_EQ(
            readResult.value().samples()[index],
            static_cast<double>(expected[index]) / 32768.0);
    }
}

TEST_F(WavWriterTest, RejectsRatesThatCannotBeRepresentedWithoutChangingTheSignal)
{
    const auto path = directoryPath() / "invalid-rate.wav";
    for (const double sampleRate : {
             0.5, 44100.5,
             static_cast<double>(std::numeric_limits<std::uint32_t>::max())}) {
        const auto signal = makeSignal({0.0, 0.25}, sampleRate);

        const auto result = writePcm16Wav(path, signal);

        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, core::ErrorCode::InvalidSampleRate);
        EXPECT_FALSE(std::filesystem::exists(path));
    }
}

TEST_F(WavWriterTest, CancellationPreservesExistingOutputAndCreatesNoNewFile)
{
    const auto existingPath = directoryPath() / "existing.wav";
    const auto newPath = directoryPath() / "cancelled.wav";
    const std::string original = "existing file must survive cancellation";
    {
        std::ofstream output(existingPath, std::ios::binary);
        output << original;
        ASSERT_TRUE(output);
    }
    const auto signal = makeSignal({0.0, 0.5, -0.5});
    std::stop_source stopSource;
    stopSource.request_stop();

    for (const auto& path : {existingPath, newPath}) {
        const auto result = writePcm16Wav(path, signal, stopSource.get_token());

        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, core::ErrorCode::Cancelled);
    }
    EXPECT_EQ(readBytes(existingPath), original);
    EXPECT_FALSE(std::filesystem::exists(newPath));
}

TEST_F(WavWriterTest, ReplacesAnExistingWavOnlyWithACompleteNewRecording)
{
    const auto path = directoryPath() / "replaced.wav";
    ASSERT_TRUE(writePcm16Wav(path, makeSignal({0.5, -0.5}, 8000.0)));
    const auto signal = makeSignal({-0.25, 0.25, 0.75}, 16000.0);

    const auto result = writePcm16Wav(path, signal);

    ASSERT_TRUE(result) << result.error().message;
    const auto imported = readWav(path, {});
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(imported.value().sampleCount(), 3U);
    EXPECT_DOUBLE_EQ(imported.value().sampleRate(), 16000.0);
    EXPECT_DOUBLE_EQ(imported.value().samples()[0], -0.25);
    EXPECT_DOUBLE_EQ(imported.value().samples()[2], 0.75);
}

TEST_F(WavWriterTest, ReportsAnUnwritableDestination)
{
    const auto path = directoryPath() / "missing-directory" / "recording.wav";

    const auto result = writePcm16Wav(path, makeSignal({0.5}));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::FileOpenFailed);
    EXPECT_FALSE(std::filesystem::exists(path));
}

}
}
