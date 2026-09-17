#pragma once

#include <cstddef>
#include <filesystem>
#include <stop_token>

#include "signallab/core/result.hpp"
#include "signallab/io/import_types.hpp"

namespace signallab::io {

[[nodiscard]] core::Result<repository::ImportPreview> previewCsv(
    const std::filesystem::path& path,
    const repository::CsvImportOptions& options,
    std::size_t maxRows = 5,
    std::stop_token stopToken = {});

[[nodiscard]] core::Result<core::Signal> readCsv(
    const std::filesystem::path& path,
    const repository::CsvImportOptions& options,
    std::stop_token stopToken = {});

}  // namespace signallab::io
