#pragma once

#include <cstddef>
#include <filesystem>

#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"
#include "signallab/io/import_types.hpp"

namespace signallab::io {

[[nodiscard]] core::Result<ImportPreview> previewWav(
    const std::filesystem::path& path,
    std::size_t maxSamples = 5);

[[nodiscard]] core::Result<core::Signal> readWav(
    const std::filesystem::path& path,
    const WavImportOptions& options);

}  // namespace signallab::io
