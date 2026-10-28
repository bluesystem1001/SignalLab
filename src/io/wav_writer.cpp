#include "signallab/io/wav_writer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

#include <QSaveFile>
#include <QString>

namespace signallab::io {
namespace {

constexpr std::size_t samplesPerBlock = 16384U;
constexpr std::uint32_t bytesPerSample = 2U;
constexpr std::uint32_t riffHeaderPayloadSize = 36U;

QString pathToQString(const std::filesystem::path& path)
{
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    const auto utf8Path = path.u8string();
    return QString::fromUtf8(
        reinterpret_cast<const char*>(utf8Path.data()),
        static_cast<qsizetype>(utf8Path.size()));
#endif
}

core::Result<bool> cancelledWrite()
{
    return core::Result<bool>::failure(
        core::ErrorCode::Cancelled,
        "WAV writing was cancelled.");
}

core::Result<bool> fileFailure(
    core::ErrorCode code,
    const QSaveFile& file,
    const std::string& detail)
{
    return core::Result<bool>::failure(
        code,
        file.fileName().toStdString() + ": " + detail + " ("
            + file.errorString().toStdString() + ")");
}

void writeUnsigned16(
    std::span<std::uint8_t> bytes,
    std::size_t offset,
    std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void writeUnsigned32(
    std::span<std::uint8_t> bytes,
    std::size_t offset,
    std::uint32_t value)
{
    for (std::size_t byteIndex = 0; byteIndex < 4U; ++byteIndex) {
        bytes[offset + byteIndex] = static_cast<std::uint8_t>(
            (value >> (byteIndex * 8U)) & 0xffU);
    }
}

}

core::Result<bool> writePcm16Wav(
    const std::filesystem::path& path,
    const core::Signal& signal,
    std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return cancelledWrite();
    }

    const double sampleRate = signal.sampleRate();
    constexpr auto maximumFieldValue =
        std::numeric_limits<std::uint32_t>::max();
    if (!std::isfinite(sampleRate) || sampleRate < 1.0
        || std::floor(sampleRate) != sampleRate
        || sampleRate > static_cast<double>(maximumFieldValue / bytesPerSample)) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidSampleRate,
            "PCM16 WAV output requires an integer sample rate whose byte rate "
            "fits in a 32-bit WAV field.");
    }
    if (signal.sampleCount()
        > (maximumFieldValue - riffHeaderPayloadSize) / bytesPerSample) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "The signal exceeds the RIFF WAV file size limit.");
    }

    const auto outputSampleRate = static_cast<std::uint32_t>(sampleRate);
    const auto dataSize = static_cast<std::uint32_t>(
        signal.sampleCount() * bytesPerSample);
    std::array<std::uint8_t, 44> header{
        'R', 'I', 'F', 'F', 0, 0, 0, 0,
        'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
        16, 0, 0, 0, 1, 0, 1, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0
    };
    writeUnsigned32(header, 4U, riffHeaderPayloadSize + dataSize);
    writeUnsigned32(header, 24U, outputSampleRate);
    writeUnsigned32(header, 28U, outputSampleRate * bytesPerSample);
    writeUnsigned32(header, 40U, dataSize);

    QSaveFile output(pathToQString(path));
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        return fileFailure(
            core::ErrorCode::FileOpenFailed, output, "Could not open WAV output");
    }
    if (output.write(
            reinterpret_cast<const char*>(header.data()),
            static_cast<qint64>(header.size()))
        != static_cast<qint64>(header.size())) {
        return fileFailure(
            core::ErrorCode::FileWriteFailed, output, "Could not write WAV header");
    }

    const auto samples = signal.samples();
    std::array<std::uint8_t, samplesPerBlock * bytesPerSample> block{};
    for (std::size_t sampleOffset = 0; sampleOffset < samples.size();) {
        if (stopToken.stop_requested()) {
            return cancelledWrite();
        }
        const std::size_t blockSamples = std::min(
            samplesPerBlock, samples.size() - sampleOffset);
        for (std::size_t sampleIndex = 0; sampleIndex < blockSamples;
             ++sampleIndex) {
            const double normalized = std::clamp(
                samples[sampleOffset + sampleIndex], -1.0, 1.0);
            const long quantized = std::clamp(
                std::lround(normalized * 32768.0), -32768L, 32767L);
            writeUnsigned16(
                block, sampleIndex * bytesPerSample,
                static_cast<std::uint16_t>(quantized));
        }
        const auto blockSize = static_cast<qint64>(blockSamples * bytesPerSample);
        if (output.write(reinterpret_cast<const char*>(block.data()), blockSize)
            != blockSize) {
            return fileFailure(
                core::ErrorCode::FileWriteFailed, output, "Could not write WAV samples");
        }
        sampleOffset += blockSamples;
    }

    if (stopToken.stop_requested()) {
        return cancelledWrite();
    }
    if (!output.commit()) {
        return fileFailure(
            core::ErrorCode::FileWriteFailed, output, "Could not commit WAV output");
    }
    return core::Result<bool>::success(true);
}

}
