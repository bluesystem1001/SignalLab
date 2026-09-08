#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "signallab/core/signal.hpp"

namespace signallab::io {

enum class ImportFormat {
    Csv,
    Wav
};

enum class CsvDelimiter {
    Comma,
    Tab
};

struct CsvImportOptions {
    CsvDelimiter delimiter{CsvDelimiter::Comma};
    bool hasHeader{true};
    std::size_t valueColumn{1};
    std::optional<std::size_t> timeColumn{0};
    double sampleRateIfNoTimeColumn{48000.0};
    core::SignalMetadata metadata;
};

enum class StereoChannel {
    Left,
    Right,
    Average
};

struct WavImportOptions {
    StereoChannel channel{StereoChannel::Average};
    core::SignalMetadata metadata;
};

struct ImportPreview {
    std::filesystem::path sourcePath;
    ImportFormat format{ImportFormat::Csv};
    std::size_t sampleCount{0};
    std::size_t columnCount{0};
    std::size_t channelCount{0};
    std::optional<double> sampleRate;
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> rows;
};

}  // namespace signallab::io
