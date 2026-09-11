#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "signallab/core/result.hpp"

namespace signallab::repository {

struct ScriptHistoryRecord {
    std::int64_t id;
    std::string scriptText;
    std::string status;
    std::string createdUtc;
    std::string finishedUtc;
    std::string errorMessage;
};

class ScriptRepository {
public:
    virtual ~ScriptRepository() = default;

    [[nodiscard]] virtual core::Result<std::int64_t> addScriptHistory(
        const std::string& scriptText,
        const std::string& status,
        const std::string& errorMessage) = 0;

    [[nodiscard]] virtual core::Result<std::vector<ScriptHistoryRecord>>
    listScriptHistory(std::size_t maximumCount) = 0;
};

}  // namespace signallab::repository
