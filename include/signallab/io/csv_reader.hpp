#pragma once

#include <cstddef>
#include <filesystem>

#include "signallab/core/result.hpp"
#include "signallab/io/import_types.hpp"

namespace signallab::io {

[[nodiscard]] core::Result<ImportPreview> previewCsv(
    const std::filesystem::path& path,
    const CsvImportOptions& options,
    std::size_t maxRows = 5);

[[nodiscard]] core::Result<core::Signal> readCsv(
    const std::filesystem::path& path,
    const CsvImportOptions& options);

}  // namespace signallab::io
