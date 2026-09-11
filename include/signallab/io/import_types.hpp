#pragma once

#include "signallab/repository/file_repository.hpp"

namespace signallab::io {

// Compatibility names for code written before the repository layer existed.
// New interfaces should use the types from signallab::repository directly.
using ImportFormat = repository::ImportFormat;
using CsvDelimiter = repository::CsvDelimiter;
using CsvImportOptions = repository::CsvImportOptions;
using StereoChannel = repository::StereoChannel;
using WavImportOptions = repository::WavImportOptions;
using ImportPreview = repository::ImportPreview;

}  // namespace signallab::io
