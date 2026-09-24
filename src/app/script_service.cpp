#include "signallab/app/script_service.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "signallab/core/error.hpp"
#include "signallab/parser/ast.hpp"
#include "signallab/parser/lexer.hpp"
#include "signallab/parser/parser.hpp"
#include "signallab/parser/semantic_checker.hpp"

namespace signallab::app {
namespace {

void appendDiagnostics(
    std::vector<parser::Diagnostic>& destination,
    std::vector<parser::Diagnostic> source)
{
    destination.insert(
        destination.end(),
        std::make_move_iterator(source.begin()),
        std::make_move_iterator(source.end()));
}

const parser::Diagnostic* firstError(
    const std::vector<parser::Diagnostic>& diagnostics)
{
    const auto found = std::min_element(
        diagnostics.begin(),
        diagnostics.end(),
        [](const parser::Diagnostic& left,
           const parser::Diagnostic& right) {
            const bool leftIsError =
                left.severity == parser::DiagnosticSeverity::Error;
            const bool rightIsError =
                right.severity == parser::DiagnosticSeverity::Error;
            if (leftIsError != rightIsError) {
                return leftIsError;
            }
            if (!leftIsError) {
                return false;
            }
            if (left.span.begin.offset != right.span.begin.offset) {
                return left.span.begin.offset < right.span.begin.offset;
            }
            return left.stage < right.stage;
        });
    return found != diagnostics.end()
            && found->severity == parser::DiagnosticSeverity::Error
        ? &*found
        : nullptr;
}

repository::ScriptErrorLocation historyLocation(
    parser::SourceLocation location)
{
    return repository::ScriptErrorLocation{
        static_cast<std::int64_t>(location.line),
        static_cast<std::int64_t>(location.column)};
}

std::string diagnosticMessage(const parser::Diagnostic& diagnostic)
{
    return diagnostic.code + ": " + diagnostic.message;
}

std::string runtimeMessage(
    const script_runtime::RuntimeIssue& issue)
{
    return issue.code + ": " + issue.message;
}

}  // namespace

ScriptService::ScriptService(
    ImportService& importService,
    repository::ScriptRepository& scriptRepository,
    repository::ScriptFileGateway& fileGateway)
    : importService_(importService),
      scriptRepository_(scriptRepository),
      fileGateway_(fileGateway)
{
}

core::Result<ScriptExecutionResult> ScriptService::execute(
    std::string source,
    std::stop_token stopToken)
{
    // The RUNNING row deliberately precedes every front-end operation so
    // even lexical failures have a durable source, hash, and lifecycle.
    auto created = scriptRepository_.createScriptHistory(source);
    if (!created) {
        return core::Result<ScriptExecutionResult>::failure(
            created.error());
    }

    ScriptExecutionResult result;
    result.historyId = created.value();

    try {

        parser::Lexer lexer(source);
        parser::LexResult lexed = lexer.tokenize();
        parser::Parser parser(lexed.tokens);
        parser::ParseResult parsed = parser.parse();
        result.astText = parser::formatAst(parsed.program);

        parser::SemanticChecker checker;
        parser::SemanticResult checked = checker.check(parsed.program);

        result.diagnostics.reserve(
            lexed.diagnostics.size()
            + parsed.diagnostics.size()
            + checked.diagnostics.size());
        appendDiagnostics(result.diagnostics, std::move(lexed.diagnostics));
        appendDiagnostics(result.diagnostics, std::move(parsed.diagnostics));
        appendDiagnostics(result.diagnostics, std::move(checked.diagnostics));

        if (const parser::Diagnostic* error = firstError(result.diagnostics)) {
            result.status = ScriptExecutionStatus::Failed;
            auto finished = finishHistory(
                result.historyId,
                repository::ScriptHistoryStatus::Failed,
                historyLocation(error->span.begin),
                diagnosticMessage(*error));
            if (!finished) {
                return core::Result<ScriptExecutionResult>::failure(
                    finished.error());
            }
            return core::Result<ScriptExecutionResult>::success(
                std::move(result));
        }

        auto evaluated = [&]() -> script_runtime::EvaluationResult {
            if (stopToken.stop_requested()) {
                return {
                    script_runtime::EvaluationStatus::Cancelled,
                    {},
                    script_runtime::RuntimeIssue{
                        "RUN001",
                        "SignalScript execution was cancelled.",
                        parsed.program.span}};
            }

            script_runtime::Evaluator evaluator(importService_, fileGateway_);
            return evaluator.evaluate(
                parsed.program,
                checked.model,
                stopToken);
        }();

        result.output = std::move(evaluated.output);
        result.runtimeIssue = std::move(evaluated.issue);

        repository::ScriptHistoryStatus historyStatus =
            repository::ScriptHistoryStatus::Failed;
        std::optional<repository::ScriptErrorLocation> errorLocation;
        std::string errorMessage;

        switch (evaluated.status) {
        case script_runtime::EvaluationStatus::Succeeded:
            result.status = ScriptExecutionStatus::Succeeded;
            historyStatus = repository::ScriptHistoryStatus::Succeeded;
            break;
        case script_runtime::EvaluationStatus::Failed:
            result.status = ScriptExecutionStatus::Failed;
            historyStatus = repository::ScriptHistoryStatus::Failed;
            break;
        case script_runtime::EvaluationStatus::Cancelled:
            result.status = ScriptExecutionStatus::Cancelled;
            historyStatus = repository::ScriptHistoryStatus::Cancelled;
            break;
        }

        if (result.runtimeIssue.has_value()) {
            errorLocation = historyLocation(
                result.runtimeIssue->span.begin);
            errorMessage = runtimeMessage(*result.runtimeIssue);
        } else if (historyStatus
                   != repository::ScriptHistoryStatus::Succeeded) {
            // Evaluator failures are expected to carry an issue, but keeping
            // the persisted lifecycle valid is more important than relying
            // on that internal invariant.
            errorMessage = historyStatus
                    == repository::ScriptHistoryStatus::Cancelled
                ? "RUN001: SignalScript execution was cancelled."
                : "RUN000: SignalScript execution failed without a diagnostic.";
        }

        auto finished = finishHistory(
            result.historyId,
            historyStatus,
            errorLocation,
            std::move(errorMessage));
        if (!finished) {
            return core::Result<ScriptExecutionResult>::failure(
                finished.error());
        }
        return core::Result<ScriptExecutionResult>::success(
            std::move(result));
    } catch (const std::exception& exception) {
        // Repository methods expose failures through core::Result. This
        // boundary is for unexpected front-end/runtime exceptions after the
        // durable RUNNING row has already been created.
        result.status = ScriptExecutionStatus::Failed;
        result.runtimeIssue = script_runtime::RuntimeIssue{
            "RUN027",
            "SignalScript execution failed unexpectedly: "
                + std::string(exception.what()),
            {}};
        const std::string message = runtimeMessage(*result.runtimeIssue);
        auto finished = finishHistory(
            result.historyId,
            repository::ScriptHistoryStatus::Failed,
            std::nullopt,
            message);
        if (!finished) {
            // A terminal-history failure remains an infrastructure error; it
            // must not be hidden behind the recovered runtime exception.
            return core::Result<ScriptExecutionResult>::failure(
                finished.error());
        }
        return core::Result<ScriptExecutionResult>::success(
            std::move(result));
    }
}

core::Result<std::size_t> ScriptService::interruptRunningScripts()
{
    return scriptRepository_.interruptRunningScripts();
}

core::Result<std::vector<repository::ScriptHistoryRecord>>
ScriptService::history(std::size_t maximumCount)
{
    return scriptRepository_.listScriptHistory(maximumCount);
}

core::Result<bool> ScriptService::finishHistory(
    std::int64_t historyId,
    repository::ScriptHistoryStatus status,
    std::optional<repository::ScriptErrorLocation> location,
    std::string message)
{
    auto finished = scriptRepository_.finishScriptHistory(
        historyId,
        repository::ScriptHistoryFinish{
            status,
            std::move(location),
            std::move(message)});
    if (!finished) {
        return core::Result<bool>::failure(finished.error());
    }
    if (!finished.value()) {
        return core::Result<bool>::failure(
            core::ErrorCode::DatabaseQueryFailed,
            "The script history record was already terminal or missing.");
    }
    return core::Result<bool>::success(true);
}

}  // namespace signallab::app
