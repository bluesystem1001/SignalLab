#pragma once

#include <string_view>
#include <vector>

#include "signallab/parser/diagnostic.hpp"
#include "signallab/parser/token.hpp"

namespace signallab::parser {

struct LexResult {
    std::vector<Token> tokens;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool hasErrors() const
    {
        return parser::hasErrors(diagnostics);
    }
};

class Lexer {
public:
    explicit Lexer(std::string_view source);

    [[nodiscard]] LexResult tokenize();

private:
    [[nodiscard]] bool isAtEnd() const noexcept;
    [[nodiscard]] char currentByte() const noexcept;
    [[nodiscard]] char nextByte() const noexcept;
    [[nodiscard]] SourceLocation location() const noexcept;

    void skipWhitespace();
    void scanIdentifier();
    void scanNumber();
    void scanString();
    void scanPunctuation();

    void advanceAscii();
    void advanceCodePoint();
    void advanceNewline();

    void addToken(
        TokenKind kind,
        SourceLocation begin,
        TokenLiteral literal = {});
    void addDiagnostic(
        std::string code,
        std::string message,
        SourceSpan span);

    std::string_view source_;
    std::size_t index_{0};
    std::size_t line_{1};
    std::size_t column_{1};
    LexResult result_;
};

}  // namespace signallab::parser
