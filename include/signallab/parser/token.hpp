#pragma once

#include <string>
#include <variant>

#include "signallab/parser/source_location.hpp"

namespace signallab::parser {

enum class TokenKind {
    EndOfFile,
    Invalid,
    Identifier,
    Number,
    String,
    Boolean,
    Let,
    Load,
    Save,
    Plot,
    As,
    To,
    Equal,
    Comma,
    Semicolon,
    LeftParenthesis,
    RightParenthesis,
    PipeGreater
};

using TokenLiteral = std::variant<std::monostate, double, std::string, bool>;

struct Token {
    TokenKind kind{TokenKind::Invalid};
    std::string lexeme;
    TokenLiteral literal{};
    SourceSpan span{};
};

[[nodiscard]] constexpr const char* tokenKindName(TokenKind kind) noexcept
{
    switch (kind) {
    case TokenKind::EndOfFile:
        return "end of file";
    case TokenKind::Invalid:
        return "invalid token";
    case TokenKind::Identifier:
        return "identifier";
    case TokenKind::Number:
        return "number";
    case TokenKind::String:
        return "string";
    case TokenKind::Boolean:
        return "boolean";
    case TokenKind::Let:
        return "'let'";
    case TokenKind::Load:
        return "'load'";
    case TokenKind::Save:
        return "'save'";
    case TokenKind::Plot:
        return "'plot'";
    case TokenKind::As:
        return "'as'";
    case TokenKind::To:
        return "'to'";
    case TokenKind::Equal:
        return "'='";
    case TokenKind::Comma:
        return "','";
    case TokenKind::Semicolon:
        return "';'";
    case TokenKind::LeftParenthesis:
        return "'('";
    case TokenKind::RightParenthesis:
        return "')'";
    case TokenKind::PipeGreater:
        return "'|>'";
    }
    return "token";
}

}  // namespace signallab::parser
