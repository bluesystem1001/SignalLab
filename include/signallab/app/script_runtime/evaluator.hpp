#pragma once

#include <optional>
#include <stop_token>
#include <string>

#include "signallab/app/import_service.hpp"
#include "signallab/app/script_runtime/value.hpp"
#include "signallab/parser/ast.hpp"
#include "signallab/parser/semantic_checker.hpp"
#include "signallab/repository/script_file_gateway.hpp"

namespace signallab::app::script_runtime {

enum class EvaluationStatus {
    Succeeded,
    Failed,
    Cancelled
};

struct RuntimeIssue {
    std::string code;
    std::string message;
    parser::SourceSpan span{};
};

struct EvaluationResult {
    EvaluationStatus status{EvaluationStatus::Failed};
    EvaluationOutput output;
    std::optional<RuntimeIssue> issue;
};

// Evaluates an already checked AST. The evaluator has a closed set of
// built-ins and receives no process, network, dynamic-library, or raw
// filesystem capability.
class Evaluator {
public:
    Evaluator(
        ImportService& importService,
        repository::ScriptFileGateway& fileGateway);

    [[nodiscard]] EvaluationResult evaluate(
        const parser::Program& program,
        const parser::SemanticModel& semanticModel,
        std::stop_token stopToken = {});

private:
    struct ValueResult {
        std::optional<ScriptValue> value;
        std::optional<RuntimeIssue> issue;
        bool cancelled{false};
    };

    [[nodiscard]] ValueResult evaluateExpression(
        const parser::Expression& expression,
        std::optional<ScriptValue> pipelineInput = std::nullopt);
    [[nodiscard]] ValueResult evaluateCall(
        const parser::Expression& expression,
        const parser::CallExpression& call,
        std::optional<ScriptValue> pipelineInput);
    [[nodiscard]] std::optional<RuntimeIssue> evaluateStatement(
        const parser::Statement& statement);

    [[nodiscard]] ValueResult loadSignal(
        const parser::LoadStatement& statement,
        parser::SourceSpan statementSpan);
    [[nodiscard]] ValueResult callLowpass(
        const std::map<std::string, ScriptValue>& arguments,
        parser::SourceSpan span);
    [[nodiscard]] ValueResult callNormalize(
        const std::map<std::string, ScriptValue>& arguments,
        parser::SourceSpan span);
    [[nodiscard]] ValueResult callFft(
        const std::map<std::string, ScriptValue>& arguments,
        parser::SourceSpan span);
    [[nodiscard]] ValueResult callFeatures(
        const std::map<std::string, ScriptValue>& arguments,
        parser::SourceSpan span);

    [[nodiscard]] std::optional<RuntimeIssue> saveValue(
        const ScriptValue& value,
        const std::string& path,
        parser::SourceSpan span);

    ImportService& importService_;
    repository::ScriptFileGateway& fileGateway_;
    const parser::SemanticModel* semanticModel_{nullptr};
    std::stop_token stopToken_;
    EvaluationOutput output_;
    std::map<std::filesystem::path, SignalValue> loadedSignals_;
    std::size_t expressionDepth_{0};
    bool cancelled_{false};
};

}  // namespace signallab::app::script_runtime
