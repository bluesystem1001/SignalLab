#pragma once

#include <filesystem>
#include <stop_token>
#include <string>

#include "signallab/core/result.hpp"

namespace signallab::io {

[[nodiscard]] core::Result<std::string> calculateSha256(
    const std::filesystem::path& path,
    std::stop_token stopToken = {});

}  // namespace signallab::io
