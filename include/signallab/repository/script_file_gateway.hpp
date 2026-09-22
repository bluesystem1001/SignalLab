#pragma once

#include <filesystem>
#include <stop_token>
#include <string_view>

#include "signallab/core/result.hpp"

namespace signallab::repository {

// Provides the only filesystem capability exposed to SignalScript. Paths are
// relative to roots selected by the host application; implementations must
// not allow a script to escape those roots.
class ScriptFileGateway {
public:
    virtual ~ScriptFileGateway() = default;

    [[nodiscard]] virtual core::Result<std::filesystem::path> resolveInput(
        const std::filesystem::path& relativePath) const = 0;

    // Publishes one complete UTF-8 text file atomically. Existing files must
    // not be replaced.
    [[nodiscard]] virtual core::Result<std::filesystem::path> writeOutput(
        const std::filesystem::path& relativePath,
        std::string_view utf8Text,
        std::stop_token stopToken = {}) = 0;
};

}  // namespace signallab::repository
