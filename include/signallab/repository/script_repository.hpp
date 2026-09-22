#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "signallab/core/result.hpp"

namespace signallab::repository {

enum class ScriptHistoryStatus {
    Running,
    Succeeded,
    Failed,
    Cancelled,
    Interrupted,
    // Rows written before stage 6 accepted any non-empty status. Preserve
    // those rows instead of making the entire history unreadable.
    Legacy
};

struct ScriptErrorLocation {
    std::int64_t line;
    std::int64_t column;
};

struct ScriptHistoryFinish {
    ScriptHistoryStatus status;
    std::optional<ScriptErrorLocation> errorLocation;
    std::string errorMessage;
};

struct ScriptHistoryRecord {
    std::int64_t id;
    std::string scriptText;
    std::string sourceHash;
    ScriptHistoryStatus status;
    // Retains the exact persisted spelling for display and legacy records.
    std::string statusText;
    std::string createdUtc;
    std::string finishedUtc;
    std::optional<ScriptErrorLocation> errorLocation;
    std::string errorMessage;
};

class ScriptRepository {
public:
    virtual ~ScriptRepository() = default;

    // Creates a RUNNING record before lexing or parsing starts. The repository
    // hashes the exact UTF-8 source bytes rather than trusting a caller hash.
    [[nodiscard]] virtual core::Result<std::int64_t> createScriptHistory(
        std::string scriptText) = 0;

    // Only a RUNNING record may make one transition to a terminal state.
    // Returns false when another terminal action already won the race.
    [[nodiscard]] virtual core::Result<bool> finishScriptHistory(
        std::int64_t historyId,
        ScriptHistoryFinish finish) = 0;

    // Marks records abandoned by a previous process as INTERRUPTED.
    [[nodiscard]] virtual core::Result<std::size_t>
    interruptRunningScripts() = 0;

    [[nodiscard]] virtual core::Result<std::vector<ScriptHistoryRecord>>
    listScriptHistory(std::size_t maximumCount) = 0;
};

}  // namespace signallab::repository
