#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "signallab/parser/ast.hpp"
#include "signallab/parser/diagnostic.hpp"

namespace signallab::parser {

enum class ScriptType {
    Number,
    String,
    Boolean,
    Signal,
    Spectrum,
    FeatureSet,
    Unit,
    Invalid
};

[[nodiscard]] constexpr const char* scriptTypeName(ScriptType type) noexcept
{
    switch (type) {
    case ScriptType::Number:
        return "Number";
    case ScriptType::String:
        return "String";
    case ScriptType::Boolean:
        return "Boolean";
    case ScriptType::Signal:
        return "Signal";
    case ScriptType::Spectrum:
        return "Spectrum";
    case ScriptType::FeatureSet:
        return "FeatureSet";
    case ScriptType::Unit:
        return "Unit";
    case ScriptType::Invalid:
        return "Invalid";
    }
    return "Invalid";
}

enum class BuiltinId {
    Lowpass,
    Normalize,
    Fft,
    Features
};

// The current FIR implementation is a direct convolution. This bound keeps
// coefficient allocation finite while still supporting unusually long FIRs.
inline constexpr std::size_t kMaximumLowpassOrder = 65'535U;

struct BoundArgument {
    std::string parameterName;
    NodeId expressionId{0};
    bool pipelineInput{false};
};

struct BoundCall {
    BuiltinId builtin{BuiltinId::Lowpass};
    ScriptType resultType{ScriptType::Invalid};
    std::vector<BoundArgument> arguments;
};

struct SemanticModel {
    std::map<NodeId, ScriptType> expressionTypes;
    std::map<NodeId, BoundCall> calls;
    std::map<std::string, ScriptType> finalSymbols;
};

struct SemanticResult {
    SemanticModel model;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool hasErrors() const
    {
        return parser::hasErrors(diagnostics);
    }
};

class SemanticChecker {
public:
    [[nodiscard]] SemanticResult check(const Program& program);

private:
    struct TypedValue {
        ScriptType type{ScriptType::Invalid};
        std::optional<LiteralValue> constant;
    };

    struct Symbol {
        ScriptType type{ScriptType::Invalid};
        std::optional<LiteralValue> constant;
        SourceSpan declarationSpan{};
    };

    struct PipelineInput {
        NodeId expressionId{0};
        TypedValue value;
        SourceSpan span{};
    };

    void checkStatement(const Statement& statement);
    [[nodiscard]] TypedValue checkExpression(const Expression& expression);
    [[nodiscard]] TypedValue checkCall(
        const Expression& expression,
        const CallExpression& call,
        std::optional<PipelineInput> pipelineInput);

    void report(
        std::string code,
        std::string message,
        SourceSpan span);
    void recordType(NodeId id, ScriptType type);

    std::map<std::string, Symbol> symbols_;
    std::size_t expressionDepth_{0};
    SemanticResult result_;
};

}  // namespace signallab::parser
