#pragma once

#include <filesystem>
#include <stop_token>
#include <string_view>

#include "signallab/repository/script_file_gateway.hpp"

namespace signallab::io {

class FilesystemScriptFileGateway final :
    public repository::ScriptFileGateway {
public:
    FilesystemScriptFileGateway(
        std::filesystem::path readRoot,
        std::filesystem::path outputRoot);

    [[nodiscard]] core::Result<std::filesystem::path> resolveInput(
        const std::filesystem::path& relativePath) const override;

    [[nodiscard]] core::Result<std::filesystem::path> writeOutput(
        const std::filesystem::path& relativePath,
        std::string_view utf8Text,
        std::stop_token stopToken = {}) override;

private:
    std::filesystem::path readRoot_;
    std::filesystem::path outputRoot_;
};

}  // namespace signallab::io
