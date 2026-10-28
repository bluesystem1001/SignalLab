#pragma once

#include <filesystem>
#include <stop_token>

#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"

namespace signallab::io {

[[nodiscard]] core::Result<bool> writePcm16Wav(
    const std::filesystem::path& path,
    const core::Signal& signal,
    std::stop_token stopToken = {});

}
