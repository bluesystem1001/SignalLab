#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "signallab/parser/source_location.hpp"

namespace signallab::parser {

using NodeId = std::uint32_t;
using LiteralValue = std::variant<double, std::string, bool>;

struct Expression;
using ExpressionPtr = std::unique_ptr<Expression>;

struct LiteralExpression {
    LiteralValue value;
};

struct VariableExpression {
    std::string name;
    SourceSpan nameSpan{};
};

struct CallArgument {
    std::optional<std::string> name;
    std::optional<SourceSpan> nameSpan;
    ExpressionPtr value;
    SourceSpan span{};
};

struct CallExpression {
    std::string callee;
    SourceSpan calleeSpan{};
    std::vector<CallArgument> arguments;
};

struct PipelineExpression {
    ExpressionPtr input;
    // The parser guarantees that stage contains a CallExpression.
    ExpressionPtr stage;
};

struct GroupingExpression {
    ExpressionPtr expression;
};

using ExpressionNode = std::variant<
    LiteralExpression,
    VariableExpression,
    CallExpression,
    PipelineExpression,
    GroupingExpression>;

struct Expression {
    NodeId id{0};
    SourceSpan span{};
    ExpressionNode node;
};

struct LoadStatement {
    std::string path;
    SourceSpan pathSpan{};
    std::optional<std::string> alias;
    std::optional<SourceSpan> aliasSpan;
};

struct SaveStatement {
    ExpressionPtr value;
    std::string path;
    SourceSpan pathSpan{};
};

struct PlotStatement {
    ExpressionPtr value;
    std::optional<std::string> title;
    std::optional<SourceSpan> titleSpan;
};

struct AssignmentStatement {
    bool declaration{false};
    std::string name;
    SourceSpan nameSpan{};
    ExpressionPtr value;
};

struct ExpressionStatement {
    ExpressionPtr value;
};

using StatementNode = std::variant<
    LoadStatement,
    SaveStatement,
    PlotStatement,
    AssignmentStatement,
    ExpressionStatement>;

struct Statement {
    NodeId id{0};
    SourceSpan span{};
    StatementNode node;
};

struct Program {
    std::vector<Statement> statements;
    SourceSpan span{};
};

// Returns a stable S-expression representation. It intentionally omits node
// ids and locations so it can be used in deterministic parser tests.
[[nodiscard]] std::string formatAst(const Program& program);

}  // namespace signallab::parser
