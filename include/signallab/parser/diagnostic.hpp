#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "signallab/parser/source_location.hpp"

namespace signallab::parser {

enum class DiagnosticStage {
    Lexer,
    Parser,
    Semantic
};

enum class DiagnosticSeverity {
    Warning,
    Error
};

struct Diagnostic {
    DiagnosticStage stage{DiagnosticStage::Lexer};
    DiagnosticSeverity severity{DiagnosticSeverity::Error};
    std::string code;
    std::string message;
    SourceSpan span{};
};

[[nodiscard]] inline bool hasErrors(
    const std::vector<Diagnostic>& diagnostics)
{
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error;
        });
}

}  // namespace signallab::parser
