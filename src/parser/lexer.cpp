#include "signallab/parser/lexer.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <system_error>
#include <utility>

namespace signallab::parser {
namespace {

bool isAsciiLetter(char value) noexcept
{
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z');
}

bool isAsciiDigit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

bool isIdentifierStart(char value) noexcept
{
    return isAsciiLetter(value) || value == '_';
}

bool isIdentifierContinue(char value) noexcept
{
    return isIdentifierStart(value) || isAsciiDigit(value);
}

std::size_t utf8SequenceLength(
    std::string_view source,
    std::size_t index) noexcept
{
    if (index >= source.size()) {
        return 0;
    }

    const auto first = static_cast<unsigned char>(source[index]);
    std::size_t length = 0;
    if (first < 0x80U) {
        return 1;
    }
    if (first >= 0xC2U && first <= 0xDFU) {
        length = 2;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        length = 3;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        length = 4;
    } else {
        return 0;
    }

    if (index + length > source.size()) {
        return 0;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto byte = static_cast<unsigned char>(source[index + offset]);
        if ((byte & 0xC0U) != 0x80U) {
            return 0;
        }
    }

    // Reject overlong encodings, UTF-16 surrogate code points, and values
    // above U+10FFFF.
    const auto second = static_cast<unsigned char>(source[index + 1]);
    if ((first == 0xE0U && second < 0xA0U)
        || (first == 0xEDU && second >= 0xA0U)
        || (first == 0xF0U && second < 0x90U)
        || (first == 0xF4U && second >= 0x90U)) {
        return 0;
    }
    return length;
}

TokenKind keywordKind(std::string_view text) noexcept
{
    if (text == "let") {
        return TokenKind::Let;
    }
    if (text == "load") {
        return TokenKind::Load;
    }
    if (text == "save") {
        return TokenKind::Save;
    }
    if (text == "plot") {
        return TokenKind::Plot;
    }
    if (text == "as") {
        return TokenKind::As;
    }
    if (text == "to") {
        return TokenKind::To;
    }
    if (text == "true" || text == "false") {
        return TokenKind::Boolean;
    }
    return TokenKind::Identifier;
}

}  // namespace

Lexer::Lexer(std::string_view source)
    : source_(source)
{
}

LexResult Lexer::tokenize()
{
    // A Lexer represents one pass. Resetting also makes an accidental second
    // call deterministic rather than appending duplicate tokens.
    index_ = 0;
    line_ = 1;
    column_ = 1;
    result_ = {};

    while (!isAtEnd()) {
        skipWhitespace();
        if (isAtEnd()) {
            break;
        }

        const char byte = currentByte();
        if (isIdentifierStart(byte)) {
            scanIdentifier();
        } else if (isAsciiDigit(byte)
                   || (byte == '-' && isAsciiDigit(nextByte()))
                   || (byte == '-' && nextByte() == '.'
                       && index_ + 2U < source_.size()
                       && isAsciiDigit(source_[index_ + 2U]))
                   || (byte == '.' && isAsciiDigit(nextByte()))) {
            scanNumber();
        } else if (byte == '"') {
            scanString();
        } else {
            scanPunctuation();
        }
    }

    const SourceLocation end = location();
    result_.tokens.push_back(Token{
        TokenKind::EndOfFile,
        {},
        {},
        SourceSpan{end, end}});
    return std::move(result_);
}

bool Lexer::isAtEnd() const noexcept
{
    return index_ >= source_.size();
}

char Lexer::currentByte() const noexcept
{
    return isAtEnd() ? '\0' : source_[index_];
}

char Lexer::nextByte() const noexcept
{
    return index_ + 1U >= source_.size() ? '\0' : source_[index_ + 1U];
}

SourceLocation Lexer::location() const noexcept
{
    return SourceLocation{index_, line_, column_};
}

void Lexer::skipWhitespace()
{
    while (!isAtEnd()) {
        const char value = currentByte();
        if (value == ' ' || value == '\t' || value == '\v'
            || value == '\f') {
            advanceAscii();
        } else if (value == '\r' || value == '\n') {
            advanceNewline();
        } else if (value == '#') {
            while (!isAtEnd() && currentByte() != '\r'
                   && currentByte() != '\n') {
                advanceCodePoint();
            }
        } else {
            return;
        }
    }
}

void Lexer::scanIdentifier()
{
    const SourceLocation begin = location();
    while (!isAtEnd() && isIdentifierContinue(currentByte())) {
        advanceAscii();
    }

    const std::string_view text = source_.substr(
        begin.offset,
        index_ - begin.offset);
    const TokenKind kind = keywordKind(text);
    if (kind == TokenKind::Boolean) {
        addToken(kind, begin, text == "true");
    } else {
        addToken(kind, begin);
    }
}

void Lexer::scanNumber()
{
    const SourceLocation begin = location();
    if (currentByte() == '-') {
        advanceAscii();
    }

    while (isAsciiDigit(currentByte())) {
        advanceAscii();
    }
    if (currentByte() == '.') {
        advanceAscii();
        while (isAsciiDigit(currentByte())) {
            advanceAscii();
        }
    }

    bool validExponent = true;
    if (currentByte() == 'e' || currentByte() == 'E') {
        advanceAscii();
        if (currentByte() == '+' || currentByte() == '-') {
            advanceAscii();
        }
        if (!isAsciiDigit(currentByte())) {
            validExponent = false;
        }
        while (isAsciiDigit(currentByte())) {
            advanceAscii();
        }
    }

    const std::string_view text = source_.substr(
        begin.offset,
        index_ - begin.offset);
    double value = 0.0;
    const auto conversion = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value,
        std::chars_format::general);
    const bool valid = validExponent
        && conversion.ec == std::errc{}
        && conversion.ptr == text.data() + text.size()
        && std::isfinite(value);
    if (!valid) {
        addToken(TokenKind::Invalid, begin);
        addDiagnostic(
            "LEX004",
            "The numeric literal is invalid or outside the finite range.",
            SourceSpan{begin, location()});
        return;
    }
    addToken(TokenKind::Number, begin, value);
}

void Lexer::scanString()
{
    const SourceLocation begin = location();
    advanceAscii();  // Opening quote.

    std::string decoded;
    while (!isAtEnd()) {
        if (currentByte() == '"') {
            advanceAscii();
            addToken(TokenKind::String, begin, std::move(decoded));
            return;
        }
        if (currentByte() == '\r' || currentByte() == '\n') {
            addDiagnostic(
                "LEX002",
                "A string literal must be closed before the end of the line.",
                SourceSpan{begin, location()});
            addToken(TokenKind::String, begin, std::move(decoded));
            return;
        }
        if (currentByte() == '\\') {
            const SourceLocation escapeBegin = location();
            advanceAscii();
            if (isAtEnd() || currentByte() == '\r'
                || currentByte() == '\n') {
                addDiagnostic(
                    "LEX002",
                    "A string literal ends inside an escape sequence.",
                    SourceSpan{begin, location()});
                addToken(TokenKind::String, begin, std::move(decoded));
                return;
            }

            const char escaped = currentByte();
            advanceCodePoint();
            switch (escaped) {
            case '"':
                decoded.push_back('"');
                break;
            case '\\':
                decoded.push_back('\\');
                break;
            case 'n':
                decoded.push_back('\n');
                break;
            case 'r':
                decoded.push_back('\r');
                break;
            case 't':
                decoded.push_back('\t');
                break;
            default:
                decoded.push_back(escaped);
                addDiagnostic(
                    "LEX003",
                    "The string contains an unsupported escape sequence.",
                    SourceSpan{escapeBegin, location()});
                break;
            }
            continue;
        }

        const auto byte = static_cast<unsigned char>(currentByte());
        if (byte >= 0x80U) {
            const std::size_t length = utf8SequenceLength(source_, index_);
            if (length == 0U) {
                const SourceLocation invalidBegin = location();
                decoded.push_back(currentByte());
                advanceAscii();
                addDiagnostic(
                    "LEX005",
                    "The source contains an invalid UTF-8 sequence.",
                    SourceSpan{invalidBegin, location()});
                continue;
            }
            decoded.append(source_.substr(index_, length));
            advanceCodePoint();
            continue;
        }

        decoded.push_back(currentByte());
        advanceAscii();
    }

    addDiagnostic(
        "LEX002",
        "A string literal must be closed before the end of the source.",
        SourceSpan{begin, location()});
    addToken(TokenKind::String, begin, std::move(decoded));
}

void Lexer::scanPunctuation()
{
    const SourceLocation begin = location();
    switch (currentByte()) {
    case '=':
        advanceAscii();
        addToken(TokenKind::Equal, begin);
        return;
    case ',':
        advanceAscii();
        addToken(TokenKind::Comma, begin);
        return;
    case ';':
        advanceAscii();
        addToken(TokenKind::Semicolon, begin);
        return;
    case '(':
        advanceAscii();
        addToken(TokenKind::LeftParenthesis, begin);
        return;
    case ')':
        advanceAscii();
        addToken(TokenKind::RightParenthesis, begin);
        return;
    case '|':
        advanceAscii();
        if (currentByte() == '>') {
            advanceAscii();
            addToken(TokenKind::PipeGreater, begin);
            return;
        }
        addToken(TokenKind::Invalid, begin);
        addDiagnostic(
            "LEX001",
            "Expected '>' after '|' to form the pipeline operator '|>'.",
            SourceSpan{begin, location()});
        return;
    default:
        advanceCodePoint();
        addToken(TokenKind::Invalid, begin);
        addDiagnostic(
            "LEX001",
            "The source contains a character that SignalScript does not "
            "recognize.",
            SourceSpan{begin, location()});
        return;
    }
}

void Lexer::advanceAscii()
{
    if (!isAtEnd()) {
        ++index_;
        ++column_;
    }
}

void Lexer::advanceCodePoint()
{
    if (isAtEnd()) {
        return;
    }
    if (currentByte() == '\r' || currentByte() == '\n') {
        advanceNewline();
        return;
    }

    const std::size_t length = utf8SequenceLength(source_, index_);
    index_ += length == 0U ? 1U : length;
    ++column_;
}

void Lexer::advanceNewline()
{
    if (isAtEnd()) {
        return;
    }
    if (currentByte() == '\r' && nextByte() == '\n') {
        index_ += 2U;
    } else {
        ++index_;
    }
    ++line_;
    column_ = 1;
}

void Lexer::addToken(
    TokenKind kind,
    SourceLocation begin,
    TokenLiteral literal)
{
    result_.tokens.push_back(Token{
        kind,
        std::string(source_.substr(begin.offset, index_ - begin.offset)),
        std::move(literal),
        SourceSpan{begin, location()}});
}

void Lexer::addDiagnostic(
    std::string code,
    std::string message,
    SourceSpan span)
{
    result_.diagnostics.push_back(Diagnostic{
        DiagnosticStage::Lexer,
        DiagnosticSeverity::Error,
        std::move(code),
        std::move(message),
        span});
}

}  // namespace signallab::parser
