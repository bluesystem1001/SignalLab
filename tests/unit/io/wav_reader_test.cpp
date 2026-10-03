#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "signallab/io/wav_reader.hpp"

namespace signallab::io {
namespace {

void appendText(std::vector<std::uint8_t>& bytes, std::string_view text)
{
    for (const char character : text) {
        bytes.push_back(static_cast<std::uint8_t>(character));
    }
}

void appendUnsigned16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void appendUnsigned32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void writeUnsigned32(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint32_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

void writeUnsigned16(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void appendChunk(
    std::vector<std::uint8_t>& wav,
    std::string_view id,
    const std::vector<std::uint8_t>& data)
{
    appendText(wav, id);
    appendUnsigned32(wav, static_cast<std::uint32_t>(data.size()));
    wav.insert(wav.end(), data.begin(), data.end());
    if (data.size() % 2U != 0U) {
        wav.push_back(0);
    }
}

struct WavFileParts {
    bool includeUnknownOddChunk{false};
    bool includeFormatChunk{true};
    bool includeDataChunk{true};
};

std::vector<std::uint8_t> makeWavBytes(
    std::uint16_t audioFormat,
    std::uint16_t bitsPerSample,
    std::uint16_t channelCount,
    std::uint32_t sampleRate,
    const std::vector<std::int16_t>& interleavedSamples,
    WavFileParts parts = {})
{
    std::vector<std::uint8_t> wav;
    appendText(wav, "RIFF");
    appendUnsigned32(wav, 0);
    appendText(wav, "WAVE");

    if (parts.includeUnknownOddChunk) {
        appendChunk(wav, "JUNK", {1, 2, 3});
    }

    const std::uint16_t bytesPerSample =
        static_cast<std::uint16_t>(bitsPerSample / 8U);
    const std::uint16_t blockAlign =
        static_cast<std::uint16_t>(channelCount * bytesPerSample);

    if (parts.includeFormatChunk) {
        std::vector<std::uint8_t> formatData;
        appendUnsigned16(formatData, audioFormat);
        appendUnsigned16(formatData, channelCount);
        appendUnsigned32(formatData, sampleRate);
        appendUnsigned32(formatData, sampleRate * blockAlign);
        appendUnsigned16(formatData, blockAlign);
        appendUnsigned16(formatData, bitsPerSample);
        appendChunk(wav, "fmt ", formatData);
    }

    if (parts.includeDataChunk) {
        std::vector<std::uint8_t> sampleData;
        for (const std::int16_t sample : interleavedSamples) {
            appendUnsigned16(sampleData, static_cast<std::uint16_t>(sample));
        }
        appendChunk(wav, "data", sampleData);
    }

    writeUnsigned32(
        wav,
        4,
        static_cast<std::uint32_t>(wav.size() - 8U));
    return wav;
}

class TemporaryWavFile {
public:
    explicit TemporaryWavFile(const std::vector<std::uint8_t>& bytes)
    {
        static std::atomic<unsigned int> counter{0};
        const auto timestamp = std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count();
        path_ = std::filesystem::temp_directory_path()
            / ("signallab_wav_" + std::to_string(timestamp) + "_"
               + std::to_string(counter.fetch_add(1)) + ".wav");

        std::ofstream output(path_, std::ios::binary);
        if (!output) {
            throw std::runtime_error("Could not create temporary WAV file");
        }
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            throw std::runtime_error("Could not write temporary WAV file");
        }
    }

    TemporaryWavFile(const TemporaryWavFile&) = delete;
    TemporaryWavFile& operator=(const TemporaryWavFile&) = delete;

    ~TemporaryWavFile()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

WavImportOptions optionsFor(StereoChannel channel)
{
    WavImportOptions options;
    options.channel = channel;
    options.metadata = {"WAV test", "normalized", "temporary file"};
    return options;
}

TEST(WavReaderTest, PreviewsMonoPcm16AndSkipsUnknownPaddedChunk)
{
    const TemporaryWavFile file(makeWavBytes(
        1,
        16,
        1,
        48000,
        {0, 16384, -32768, 32767},
        WavFileParts{.includeUnknownOddChunk = true}));

    const auto result = previewWav(file.path(), 2);

    ASSERT_TRUE(result.hasValue());
    const ImportPreview& preview = result.value();
    EXPECT_EQ(preview.sourcePath, file.path());
    EXPECT_EQ(preview.format, ImportFormat::Wav);
    EXPECT_EQ(preview.sampleCount, 4U);
    EXPECT_EQ(preview.channelCount, 1U);
    EXPECT_EQ(preview.columnCount, 1U);
    ASSERT_TRUE(preview.sampleRate.has_value());
    EXPECT_DOUBLE_EQ(*preview.sampleRate, 48000.0);
    ASSERT_EQ(preview.rows.size(), 2U);
    EXPECT_NEAR(std::stod(preview.rows[0][0]), 0.0, 1e-12);
    EXPECT_NEAR(std::stod(preview.rows[1][0]), 0.5, 1e-12);

    const auto zeroRowResult = previewWav(file.path(), 0);
    ASSERT_TRUE(zeroRowResult.hasValue());
    EXPECT_EQ(zeroRowResult.value().sampleCount, 4U);
    EXPECT_TRUE(zeroRowResult.value().rows.empty());
}

TEST(WavReaderTest, ReadsRepositoryMonoPcm16ReferenceAsset)
{
    const std::filesystem::path sourceRoot(SIGNALLAB_SOURCE_DIR);
    const std::filesystem::path wavPath =
        sourceRoot / "data" / "samples" / "example_signal_pcm16.wav";
    ASSERT_TRUE(std::filesystem::is_regular_file(wavPath))
        << wavPath.string();

    const auto previewResult = previewWav(wavPath, 3);
    ASSERT_TRUE(previewResult) << previewResult.error().message;
    const auto& preview = previewResult.value();
    EXPECT_EQ(preview.sourcePath, wavPath);
    EXPECT_EQ(preview.format, ImportFormat::Wav);
    EXPECT_EQ(preview.sampleCount, 21U);
    EXPECT_EQ(preview.channelCount, 1U);
    EXPECT_EQ(preview.columnCount, 1U);
    ASSERT_TRUE(preview.sampleRate.has_value());
    EXPECT_DOUBLE_EQ(*preview.sampleRate, 1'000.0);
    ASSERT_EQ(preview.rows.size(), 3U);
    EXPECT_NEAR(std::stod(preview.rows[0][0]), 0.0, 1.0e-12);
    EXPECT_NEAR(std::stod(preview.rows[1][0]), 0.309017, 4.0e-5);

    auto options = optionsFor(StereoChannel::Left);
    options.metadata = {
        "Stage 8 WAV reference", "normalized", wavPath.string()};
    const auto signalResult = readWav(wavPath, options);
    ASSERT_TRUE(signalResult) << signalResult.error().message;
    const auto& signal = signalResult.value();
    ASSERT_EQ(signal.sampleCount(), 21U);
    EXPECT_DOUBLE_EQ(signal.sampleRate(), 1'000.0);
    EXPECT_NEAR(signal.samples().front(), 0.0, 1.0e-12);
    EXPECT_NEAR(signal.samples()[1], 0.309017, 4.0e-5);
    EXPECT_NEAR(signal.samples()[5], 1.0, 4.0e-5);
    EXPECT_NEAR(signal.samples()[15], -1.0, 4.0e-5);
    EXPECT_NEAR(signal.samples().back(), 0.0, 1.0e-12);
    EXPECT_EQ(signal.metadata().name, "Stage 8 WAV reference");
}

TEST(WavReaderTest, SelectsLeftRightAndAverageStereoSamples)
{
    const TemporaryWavFile file(makeWavBytes(
        1,
        16,
        2,
        48000,
        {32767, -32768, 16384, -16384}));

    const auto leftResult = readWav(
        file.path(),
        optionsFor(StereoChannel::Left));
    const auto rightResult = readWav(
        file.path(),
        optionsFor(StereoChannel::Right));
    const auto averageResult = readWav(
        file.path(),
        optionsFor(StereoChannel::Average));

    ASSERT_TRUE(leftResult.hasValue());
    ASSERT_TRUE(rightResult.hasValue());
    ASSERT_TRUE(averageResult.hasValue());
    EXPECT_EQ(leftResult.value().sampleCount(), 2U);
    EXPECT_NEAR(leftResult.value().samples()[0], 32767.0 / 32768.0, 1e-12);
    EXPECT_NEAR(leftResult.value().samples()[1], 0.5, 1e-12);
    EXPECT_NEAR(rightResult.value().samples()[0], -1.0, 1e-12);
    EXPECT_NEAR(rightResult.value().samples()[1], -0.5, 1e-12);
    EXPECT_NEAR(averageResult.value().samples()[0], -1.0 / 65536.0, 1e-12);
    EXPECT_NEAR(averageResult.value().samples()[1], 0.0, 1e-12);
}

TEST(WavReaderTest, ReportsTruncatedFilesWithPathAndByteOffset)
{
    std::vector<std::uint8_t> bytes = makeWavBytes(
        1,
        16,
        1,
        48000,
        {0, 1000});
    bytes.pop_back();
    const TemporaryWavFile file(bytes);

    const auto result = readWav(
        file.path(),
        optionsFor(StereoChannel::Average));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, core::ErrorCode::TruncatedFile);
    EXPECT_NE(result.error().message.find(file.path().string()), std::string::npos);
    EXPECT_NE(result.error().message.find("byte offset"), std::string::npos);
}

TEST(WavReaderTest, DistinguishesUnsupportedAudioFormat)
{
    const TemporaryWavFile file(makeWavBytes(
        3,
        16,
        1,
        48000,
        {0, 1000}));

    const auto result = readWav(
        file.path(),
        optionsFor(StereoChannel::Average));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, core::ErrorCode::UnsupportedWavFormat);
    EXPECT_NE(result.error().message.find("unsupported WAV format"), std::string::npos);
    EXPECT_NE(result.error().message.find("byte offset"), std::string::npos);
}

TEST(WavReaderTest, RejectsUnsupportedBitDepthAndChannelCount)
{
    const TemporaryWavFile twentyFourBitFile(makeWavBytes(
        1,
        24,
        1,
        48000,
        {0, 1000}));
    const TemporaryWavFile threeChannelFile(makeWavBytes(
        1,
        16,
        3,
        48000,
        {0, 1000, -1000}));

    const auto bitDepthResult = previewWav(twentyFourBitFile.path());
    const auto channelResult = previewWav(threeChannelFile.path());

    ASSERT_FALSE(bitDepthResult.hasValue());
    ASSERT_FALSE(channelResult.hasValue());
    EXPECT_EQ(
        bitDepthResult.error().code,
        core::ErrorCode::UnsupportedWavFormat);
    EXPECT_EQ(
        channelResult.error().code,
        core::ErrorCode::UnsupportedWavFormat);
}

TEST(WavReaderTest, RejectsInconsistentBlockAlignAndByteRate)
{
    std::vector<std::uint8_t> blockAlignBytes = makeWavBytes(
        1,
        16,
        1,
        48000,
        {0, 1000});
    writeUnsigned16(blockAlignBytes, 32, 1);
    const TemporaryWavFile blockAlignFile(blockAlignBytes);

    std::vector<std::uint8_t> byteRateBytes = makeWavBytes(
        1,
        16,
        1,
        48000,
        {0, 1000});
    writeUnsigned32(byteRateBytes, 28, 1234);
    const TemporaryWavFile byteRateFile(byteRateBytes);

    const auto blockAlignResult = previewWav(blockAlignFile.path());
    const auto byteRateResult = previewWav(byteRateFile.path());

    ASSERT_FALSE(blockAlignResult.hasValue());
    ASSERT_FALSE(byteRateResult.hasValue());
    EXPECT_EQ(
        blockAlignResult.error().code,
        core::ErrorCode::UnsupportedWavFormat);
    EXPECT_EQ(
        byteRateResult.error().code,
        core::ErrorCode::UnsupportedWavFormat);
    EXPECT_NE(
        byteRateResult.error().message.find("byte rate is inconsistent"),
        std::string::npos);
    EXPECT_NE(
        byteRateResult.error().message.find(byteRateFile.path().string()),
        std::string::npos);
    EXPECT_NE(
        byteRateResult.error().message.find("byte offset"),
        std::string::npos);
}

TEST(WavReaderTest, ReportsMissingFormatAndDataChunks)
{
    const TemporaryWavFile missingFormatFile(makeWavBytes(
        1,
        16,
        1,
        48000,
        {0, 1000},
        WavFileParts{.includeFormatChunk = false}));
    const TemporaryWavFile missingDataFile(makeWavBytes(
        1,
        16,
        1,
        48000,
        {0, 1000},
        WavFileParts{.includeDataChunk = false}));

    const auto missingFormatResult = previewWav(missingFormatFile.path());
    const auto missingDataResult = previewWav(missingDataFile.path());

    ASSERT_FALSE(missingFormatResult.hasValue());
    ASSERT_FALSE(missingDataResult.hasValue());
    EXPECT_EQ(
        missingFormatResult.error().code,
        core::ErrorCode::InvalidParameter);
    EXPECT_EQ(
        missingDataResult.error().code,
        core::ErrorCode::InvalidParameter);
    EXPECT_NE(
        missingFormatResult.error().message.find("no fmt chunk"),
        std::string::npos);
    EXPECT_NE(
        missingDataResult.error().message.find("no data chunk"),
        std::string::npos);
}

TEST(WavReaderTest, DistinguishesRiffBoundaryDamageFromPhysicalTruncation)
{
    std::vector<std::uint8_t> bytes = makeWavBytes(
        1,
        16,
        1,
        48000,
        {0, 1000});
    const std::uint32_t smallerRiffSize =
        static_cast<std::uint32_t>(bytes.size() - 10U);
    writeUnsigned32(bytes, 4, smallerRiffSize);
    const TemporaryWavFile file(bytes);

    const auto result = previewWav(file.path());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_NE(
        result.error().message.find("declared RIFF boundary"),
        std::string::npos);
    EXPECT_NE(result.error().message.find(file.path().string()), std::string::npos);
    EXPECT_NE(result.error().message.find("byte offset"), std::string::npos);
}

TEST(WavReaderTest, DistinguishesAnInvalidRiffHeader)
{
    std::vector<std::uint8_t> bytes = makeWavBytes(
        1,
        16,
        1,
        48000,
        {0, 1000});
    bytes[0] = static_cast<std::uint8_t>('X');
    const TemporaryWavFile file(bytes);

    const auto result = readWav(
        file.path(),
        optionsFor(StereoChannel::Average));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_NE(result.error().message.find("invalid RIFF/WAVE"), std::string::npos);
    EXPECT_NE(result.error().message.find("byte offset"), std::string::npos);
}

TEST(WavReaderTest, HonorsAPreRequestedCancellation)
{
    const TemporaryWavFile file(makeWavBytes(
        1,
        16,
        1,
        48000,
        {0, 1000, -1000}));
    std::stop_source stopSource;
    stopSource.request_stop();

    const auto previewResult = previewWav(
        file.path(),
        5,
        stopSource.get_token());
    const auto readResult = readWav(
        file.path(),
        optionsFor(StereoChannel::Average),
        stopSource.get_token());

    ASSERT_FALSE(previewResult.hasValue());
    EXPECT_EQ(previewResult.error().code, core::ErrorCode::Cancelled);
    ASSERT_FALSE(readResult.hasValue());
    EXPECT_EQ(readResult.error().code, core::ErrorCode::Cancelled);
}

}  // namespace
}  // namespace signallab::io
