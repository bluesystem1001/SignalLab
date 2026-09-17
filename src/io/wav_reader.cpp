#include "signallab/io/wav_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace signallab::io {
namespace {

constexpr std::size_t fileBlockSize = 64U * 1024U;

struct ParsedWav {
    std::vector<std::uint8_t> bytes;
    std::uint16_t channelCount{0};
    std::uint32_t sampleRate{0};
    std::uint16_t blockAlign{0};
    std::size_t dataOffset{0};
    std::size_t dataSize{0};
};

std::string describeError(
    const std::filesystem::path& path,
    std::size_t byteOffset,
    const std::string& detail)
{
    return path.string() + " at byte offset "
        + std::to_string(byteOffset) + ": " + detail;
}

template<typename T>
core::Result<T> wavFailure(
    core::ErrorCode code,
    const std::filesystem::path& path,
    std::size_t byteOffset,
    const std::string& detail)
{
    return core::Result<T>::failure(
        code,
        describeError(path, byteOffset, detail));
}

template<typename T>
core::Result<T> cancelledWav(
    const std::filesystem::path& path,
    std::size_t byteOffset)
{
    return wavFailure<T>(
        core::ErrorCode::Cancelled,
        path,
        byteOffset,
        "WAV reading was cancelled");
}

bool matchesFourCharacterCode(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::string_view expected)
{
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (bytes[offset + index]
            != static_cast<std::uint8_t>(expected[index])) {
            return false;
        }
    }
    return true;
}

std::uint16_t readUnsigned16(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset])
        | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::uint32_t readUnsigned32(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

int readSigned16(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset)
{
    const int unsignedValue = readUnsigned16(bytes, offset);
    if (unsignedValue >= 32768) {
        return unsignedValue - 65536;
    }
    return unsignedValue;
}
double normalizedSample(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset)
{
    return static_cast<double>(readSigned16(bytes, offset)) / 32768.0;
}

core::Result<std::vector<std::uint8_t>> readFileBytes(
    const std::filesystem::path& path,
    const std::stop_token& stopToken)
{
    if (stopToken.stop_requested()) {
        return cancelledWav<std::vector<std::uint8_t>>(path, 0);
    }

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        std::error_code fileSystemError;
        const bool exists = std::filesystem::exists(path, fileSystemError);
        core::ErrorCode code = core::ErrorCode::FileOpenFailed;
        std::string detail = "could not open WAV file";

        if (fileSystemError) {
            detail += " (could not inspect the path: "
                + fileSystemError.message() + ")";
        } else if (!exists) {
            code = core::ErrorCode::FileNotFound;
            detail = "WAV file does not exist";
        }

        return wavFailure<std::vector<std::uint8_t>>(
            code,
            path,
            0,
            detail);
    }

    const std::streampos endPosition = input.tellg();
    if (endPosition == std::streampos(-1)) {
        return wavFailure<std::vector<std::uint8_t>>(
            core::ErrorCode::FileOpenFailed,
            path,
            0,
            "could not determine WAV file size");
    }

    const std::streamoff fileSizeAsOffset =
        static_cast<std::streamoff>(endPosition);
    const auto fileSize = static_cast<std::uintmax_t>(fileSizeAsOffset);
    if (fileSize > std::numeric_limits<std::size_t>::max()
        || fileSize > static_cast<std::uintmax_t>(
            std::numeric_limits<std::streamsize>::max())) {
        return wavFailure<std::vector<std::uint8_t>>(
            core::ErrorCode::FileOpenFailed,
            path,
            0,
            "WAV file is too large to read");
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
    input.seekg(0, std::ios::beg);

    std::size_t totalBytesRead = 0;
    while (totalBytesRead < bytes.size()) {
        if (stopToken.stop_requested()) {
            return cancelledWav<std::vector<std::uint8_t>>(
                path,
                totalBytesRead);
        }

        const std::size_t bytesRemaining = bytes.size() - totalBytesRead;
        const std::size_t nextBlockSize =
            std::min(fileBlockSize, bytesRemaining);
        input.read(
            reinterpret_cast<char*>(bytes.data() + totalBytesRead),
            static_cast<std::streamsize>(nextBlockSize));

        const std::size_t blockBytesRead = static_cast<std::size_t>(
            std::max<std::streamsize>(0, input.gcount()));
        if (stopToken.stop_requested()) {
            return cancelledWav<std::vector<std::uint8_t>>(
                path,
                totalBytesRead + blockBytesRead);
        }
        if (blockBytesRead != nextBlockSize) {
            return wavFailure<std::vector<std::uint8_t>>(
                core::ErrorCode::TruncatedFile,
                path,
                totalBytesRead + blockBytesRead,
                "file ended while reading WAV data");
        }

        totalBytesRead += blockBytesRead;
    }

    if (stopToken.stop_requested()) {
        return cancelledWav<std::vector<std::uint8_t>>(
            path,
            totalBytesRead);
    }

    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<ParsedWav> parseWav(
    const std::filesystem::path& path,
    const std::stop_token& stopToken)
{
    auto bytesResult = readFileBytes(path, stopToken);
    if (!bytesResult) {
        return core::Result<ParsedWav>::failure(bytesResult.error());
    }

    std::vector<std::uint8_t> bytes = std::move(bytesResult).value();
    if (stopToken.stop_requested()) {
        return cancelledWav<ParsedWav>(path, bytes.size());
    }
    if (bytes.size() < 12) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::TruncatedFile,
            path,
            bytes.size(),
            "file ended before the RIFF/WAVE header was complete");
    }

    if (!matchesFourCharacterCode(bytes, 0, "RIFF")
        || !matchesFourCharacterCode(bytes, 8, "WAVE")) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::InvalidParameter,
            path,
            0,
            "invalid RIFF/WAVE header");
    }

    const std::uint64_t declaredRiffEnd =
        static_cast<std::uint64_t>(readUnsigned32(bytes, 4)) + 8U;
    if (declaredRiffEnd < 12U) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::InvalidParameter,
            path,
            4,
            "invalid RIFF size");
    }
    if (declaredRiffEnd > bytes.size()) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::TruncatedFile,
            path,
            bytes.size(),
            "file ended before the declared RIFF size");
    }

    const std::size_t riffEnd = static_cast<std::size_t>(declaredRiffEnd);
    bool foundFormat = false;
    bool foundData = false;
    std::uint16_t audioFormat = 0;
    std::uint16_t channelCount = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t byteRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    std::size_t formatOffset = 0;
    std::size_t dataOffset = 0;
    std::size_t dataSize = 0;

    std::size_t chunkOffset = 12;
    while (chunkOffset < riffEnd) {
        if (stopToken.stop_requested()) {
            return cancelledWav<ParsedWav>(path, chunkOffset);
        }

        if (riffEnd - chunkOffset < 8) {
            const bool fileAlsoEndsInsideHeader =
                bytes.size() - chunkOffset < 8;
            return wavFailure<ParsedWav>(
                fileAlsoEndsInsideHeader
                    ? core::ErrorCode::TruncatedFile
                    : core::ErrorCode::InvalidParameter,
                path,
                fileAlsoEndsInsideHeader ? bytes.size() : riffEnd,
                fileAlsoEndsInsideHeader
                    ? "file ended inside a WAV chunk header"
                    : "declared RIFF size ends inside a WAV chunk header");
        }

        const std::uint32_t chunkSize = readUnsigned32(bytes, chunkOffset + 4);
        const std::uint64_t chunkDataOffset =
            static_cast<std::uint64_t>(chunkOffset) + 8U;
        const std::uint64_t chunkDataEnd = chunkDataOffset + chunkSize;
        const std::uint64_t paddedChunkEnd = chunkDataEnd + (chunkSize % 2U);

        if (paddedChunkEnd > riffEnd) {
            const bool fileAlsoEndsInsideChunk =
                paddedChunkEnd > bytes.size();
            return wavFailure<ParsedWav>(
                fileAlsoEndsInsideChunk
                    ? core::ErrorCode::TruncatedFile
                    : core::ErrorCode::InvalidParameter,
                path,
                fileAlsoEndsInsideChunk ? bytes.size() : riffEnd,
                fileAlsoEndsInsideChunk
                    ? "file ended inside a WAV chunk"
                    : "WAV chunk extends past the declared RIFF boundary");
        }

        if (matchesFourCharacterCode(bytes, chunkOffset, "fmt ")
            && !foundFormat) {
            if (chunkSize < 16) {
                return wavFailure<ParsedWav>(
                    core::ErrorCode::TruncatedFile,
                    path,
                    static_cast<std::size_t>(chunkDataEnd),
                    "fmt chunk is shorter than 16 bytes");
            }

            formatOffset = static_cast<std::size_t>(chunkDataOffset);
            audioFormat = readUnsigned16(bytes, formatOffset);
            channelCount = readUnsigned16(bytes, formatOffset + 2);
            sampleRate = readUnsigned32(bytes, formatOffset + 4);
            byteRate = readUnsigned32(bytes, formatOffset + 8);
            blockAlign = readUnsigned16(bytes, formatOffset + 12);
            bitsPerSample = readUnsigned16(bytes, formatOffset + 14);
            foundFormat = true;
        } else if (matchesFourCharacterCode(bytes, chunkOffset, "data")
                   && !foundData) {
            dataOffset = static_cast<std::size_t>(chunkDataOffset);
            dataSize = static_cast<std::size_t>(chunkSize);
            foundData = true;
        }

        chunkOffset = static_cast<std::size_t>(paddedChunkEnd);
    }

    if (!foundFormat) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::InvalidParameter,
            path,
            12,
            "WAV file has no fmt chunk");
    }
    if (!foundData) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::InvalidParameter,
            path,
            riffEnd,
            "WAV file has no data chunk");
    }

    if (audioFormat != 1) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::UnsupportedWavFormat,
            path,
            formatOffset,
            "unsupported WAV format: only PCM format 1 is accepted");
    }
    if (bitsPerSample != 16) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::UnsupportedWavFormat,
            path,
            formatOffset + 14,
            "unsupported WAV format: only 16-bit samples are accepted");
    }
    if (channelCount != 1 && channelCount != 2) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::UnsupportedWavFormat,
            path,
            formatOffset + 2,
            "unsupported WAV format: only mono or stereo is accepted");
    }
    if (sampleRate == 0) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::UnsupportedWavFormat,
            path,
            formatOffset + 4,
            "unsupported WAV format: sample rate is zero");
    }

    const std::uint16_t expectedBlockAlign =
        static_cast<std::uint16_t>(channelCount * 2U);
    if (blockAlign != expectedBlockAlign) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::UnsupportedWavFormat,
            path,
            formatOffset + 12,
            "unsupported WAV format: block alignment is inconsistent");
    }

    const std::uint64_t expectedByteRate =
        static_cast<std::uint64_t>(sampleRate) * expectedBlockAlign;
    if (byteRate != expectedByteRate) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::UnsupportedWavFormat,
            path,
            formatOffset + 8,
            "unsupported WAV format: byte rate is inconsistent");
    }

    if (dataSize == 0) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::EmptySamples,
            path,
            dataOffset,
            "WAV data chunk contains no samples");
    }
    if (dataSize % blockAlign != 0) {
        return wavFailure<ParsedWav>(
            core::ErrorCode::TruncatedFile,
            path,
            dataOffset + dataSize,
            "data chunk ends inside a sample frame");
    }

    if (stopToken.stop_requested()) {
        return cancelledWav<ParsedWav>(path, dataOffset + dataSize);
    }

    ParsedWav parsed;
    parsed.bytes = std::move(bytes);
    parsed.channelCount = channelCount;
    parsed.sampleRate = sampleRate;
    parsed.blockAlign = blockAlign;
    parsed.dataOffset = dataOffset;
    parsed.dataSize = dataSize;
    return core::Result<ParsedWav>::success(std::move(parsed));
}

std::size_t frameCount(const ParsedWav& wav)
{
    return wav.dataSize / wav.blockAlign;
}

double sampleForChannel(
    const ParsedWav& wav,
    std::size_t frameIndex,
    std::size_t channelIndex)
{
    const std::size_t offset = wav.dataOffset
        + frameIndex * wav.blockAlign
        + channelIndex * 2U;
    return normalizedSample(wav.bytes, offset);
}

}  // namespace

core::Result<repository::ImportPreview> previewWav(
    const std::filesystem::path& path,
    std::size_t maxSamples,
    std::stop_token stopToken)
{
    auto parsedResult = parseWav(path, stopToken);
    if (!parsedResult) {
        return core::Result<repository::ImportPreview>::failure(
            parsedResult.error());
    }

    const ParsedWav& wav = parsedResult.value();
    repository::ImportPreview preview;
    preview.sourcePath = path;
    preview.format = repository::ImportFormat::Wav;
    preview.sampleCount = frameCount(wav);
    preview.columnCount = wav.channelCount;
    preview.channelCount = wav.channelCount;
    preview.sampleRate = static_cast<double>(wav.sampleRate);

    if (wav.channelCount == 1) {
        preview.columnNames = {"Sample"};
    } else {
        preview.columnNames = {"Left", "Right"};
    }

    const std::size_t previewCount =
        std::min(maxSamples, preview.sampleCount);
    preview.rows.reserve(previewCount);

    for (std::size_t frame = 0; frame < previewCount; ++frame) {
        if (stopToken.stop_requested()) {
            return cancelledWav<repository::ImportPreview>(
                path,
                wav.dataOffset + frame * wav.blockAlign);
        }

        std::vector<std::string> row;
        row.reserve(wav.channelCount);
        for (std::size_t channel = 0; channel < wav.channelCount; ++channel) {
            row.push_back(std::to_string(sampleForChannel(wav, frame, channel)));
        }
        preview.rows.push_back(std::move(row));
    }

    if (stopToken.stop_requested()) {
        return cancelledWav<repository::ImportPreview>(
            path,
            wav.dataOffset + previewCount * wav.blockAlign);
    }

    return core::Result<repository::ImportPreview>::success(
        std::move(preview));
}

core::Result<core::Signal> readWav(
    const std::filesystem::path& path,
    const repository::WavImportOptions& options,
    std::stop_token stopToken)
{
    auto parsedResult = parseWav(path, stopToken);
    if (!parsedResult) {
        return core::Result<core::Signal>::failure(parsedResult.error());
    }

    const ParsedWav& wav = parsedResult.value();
    const std::size_t sampleCount = frameCount(wav);
    std::vector<double> samples(sampleCount);
    const std::size_t framesPerBlock = std::max<std::size_t>(
        1U,
        fileBlockSize / wav.blockAlign);

    for (std::size_t frame = 0; frame < sampleCount; ++frame) {
        if (frame % framesPerBlock == 0U
            && stopToken.stop_requested()) {
            return cancelledWav<core::Signal>(
                path,
                wav.dataOffset + frame * wav.blockAlign);
        }

        const double left = sampleForChannel(wav, frame, 0);

        if (wav.channelCount == 1
            || options.channel == repository::StereoChannel::Left) {
            samples[frame] = left;
            continue;
        }

        const double right = sampleForChannel(wav, frame, 1);
        if (options.channel == repository::StereoChannel::Right) {
            samples[frame] = right;
        } else {
            samples[frame] = (left + right) / 2.0;
        }
    }

    if (stopToken.stop_requested()) {
        return cancelledWav<core::Signal>(
            path,
            wav.dataOffset + wav.dataSize);
    }

    return core::Signal::create(
        std::move(samples),
        static_cast<double>(wav.sampleRate),
        options.metadata);
}

}  // namespace signallab::io
