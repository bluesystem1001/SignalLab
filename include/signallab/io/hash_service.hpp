#pragma once

#include <filesystem>
#include <string>

#include "signallab/core/result.hpp"

namespace signallab::io {

[[nodiscard]] core::Result<std::string> calculateSha256(
    const std::filesystem::path& path);

}  // namespace signallab::io
