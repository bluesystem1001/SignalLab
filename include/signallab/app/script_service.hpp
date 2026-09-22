#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "signallab/app/import_service.hpp"
#include "signallab/app/script_runtime/evaluator.hpp"
#include "signallab/core/result.hpp"
#include "signallab/parser/diagnostic.hpp"
#include "signallab/repository/script_file_gateway.hpp"
#include "signallab/repository/script_repository.hpp"

namespace signallab::app {

enum class ScriptExecutionStatus {
    Succeeded,
    Failed,
    Cancelled
};

struct ScriptExecutionResult {
    std::int64_t historyId{0};
    ScriptExecutionStatus status{ScriptExecutionStatus::Failed};
    std::string astText;
    std::vector<parser::Diagnostic> diagnostics;
    std::optional<script_runtime::RuntimeIssue> runtimeIssue;
    script_runtime::EvaluationOutput output;
};

class ScriptService {
public:
    ScriptService(
        ImportService& importService,
        repository::ScriptRepository& scriptRepository,
        repository::ScriptFileGateway& fileGateway);

    // Language and runtime failures are successful service calls whose
    // ScriptExecutionResult status is Failed or Cancelled. Infrastructure
    // failures that prevent history from being durably completed use the
    // outer core::Result failure channel.
    [[nodiscard]] core::Result<ScriptExecutionResult> execute(
        std::string source,
        std::stop_token stopToken = {});

    [[nodiscard]] core::Result<std::size_t> interruptRunningScripts();

    [[nodiscard]] core::Result<std::vector<repository::ScriptHistoryRecord>>
    history(std::size_t maximumCount = 100);

private:
    [[nodiscard]] core::Result<bool> finishHistory(
        std::int64_t historyId,
        repository::ScriptHistoryStatus status,
        std::optional<repository::ScriptErrorLocation> location,
        std::string message);

    ImportService& importService_;
    repository::ScriptRepository& scriptRepository_;
    repository::ScriptFileGateway& fileGateway_;
};

}  // namespace signallab::app
