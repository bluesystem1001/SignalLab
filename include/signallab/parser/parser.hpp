#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "signallab/parser/ast.hpp"
#include "signallab/parser/diagnostic.hpp"
#include "signallab/parser/token.hpp"

namespace signallab::parser {

// Recursive front-end and runtime visitors rely on this parser invariant.
// Keeping the limit explicit prevents a script from exhausting the process
// stack before it can produce a normal diagnostic and terminal history row.
inline constexpr std::size_t kMaximumExpressionNestingDepth = 256U;
inline constexpr std::size_t kMaximumPipelineStageCount = 256U;

struct ParseResult {
    Program program;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool hasErrors() const
    {
        return parser::hasErrors(diagnostics);
    }
};

class Parser {
public:
    explicit Parser(std::span<const Token> tokens);

    [[nodiscard]] ParseResult parse();

private:
    struct ParseFailure final {
    };

    [[nodiscard]] Statement parseStatement();
    [[nodiscard]] Statement parseLoadStatement();
    [[nodiscard]] Statement parseSaveStatement();
    [[nodiscard]] Statement parsePlotStatement();
    [[nodiscard]] Statement parseAssignmentStatement(bool declaration);
    [[nodiscard]] Statement parseExpressionStatement();

    [[nodiscard]] ExpressionPtr parseExpression();
    [[nodiscard]] ExpressionPtr parsePipeline();
    [[nodiscard]] ExpressionPtr parsePrimary();
    [[nodiscard]] ExpressionPtr parseCall();

    [[nodiscard]] bool isAtEnd() const noexcept;
    [[nodiscard]] const Token& current() const noexcept;
    [[nodiscard]] const Token& previous() const noexcept;
    [[nodiscard]] const Token& peek(std::size_t distance) const noexcept;
    [[nodiscard]] bool check(TokenKind kind) const noexcept;
    [[nodiscard]] bool match(TokenKind kind) noexcept;
    const Token& advance() noexcept;
    const Token& consume(
        TokenKind kind,
        std::string code,
        std::string message);

    void report(
        std::string code,
        std::string message,
        SourceSpan span);
    void synchronize() noexcept;

    [[nodiscard]] NodeId nextNodeId() noexcept;
    [[nodiscard]] ExpressionPtr makeExpression(
        SourceSpan span,
        ExpressionNode node);

    std::span<const Token> tokens_;
    std::size_t currentIndex_{0};
    NodeId nextNodeId_{1};
    std::size_t expressionDepth_{0};
    ParseResult result_;
};

}  // namespace signallab::parser
