#pragma once

#include <cstddef>
#include <filesystem>
#include <stop_token>

#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"
#include "signallab/io/import_types.hpp"

namespace signallab::io {

[[nodiscard]] core::Result<repository::ImportPreview> previewWav(
    const std::filesystem::path& path,
    std::size_t maxSamples = 5,
    std::stop_token stopToken = {});

[[nodiscard]] core::Result<core::Signal> readWav(
    const std::filesystem::path& path,
    const repository::WavImportOptions& options,
    std::stop_token stopToken = {});

}  // namespace signallab::io
