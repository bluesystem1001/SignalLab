#include "signallab/parser/parser.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace signallab::parser {
namespace {

const Token& fallbackEofToken() noexcept
{
    static const Token token{
        TokenKind::EndOfFile,
        {},
        {},
        SourceSpan{SourceLocation{}, SourceLocation{}}};
    return token;
}

const std::string& stringLiteral(const Token& token)
{
    return std::get<std::string>(token.literal);
}

class ExpressionDepthGuard {
public:
    explicit ExpressionDepthGuard(std::size_t& depth) noexcept
        : depth_(depth)
    {
        ++depth_;
    }

    ~ExpressionDepthGuard()
    {
        --depth_;
    }

    ExpressionDepthGuard(const ExpressionDepthGuard&) = delete;
    ExpressionDepthGuard& operator=(const ExpressionDepthGuard&) = delete;

private:
    std::size_t& depth_;
};

}  // namespace

Parser::Parser(std::span<const Token> tokens)
    : tokens_(tokens)
{
}

ParseResult Parser::parse()
{
    currentIndex_ = 0;
    nextNodeId_ = 1;
    expressionDepth_ = 0;
    result_ = {};
    const SourceLocation programBegin = current().span.begin;

    while (!isAtEnd()) {
        try {
            result_.program.statements.push_back(parseStatement());
        } catch (const ParseFailure&) {
            synchronize();
        }
    }

    result_.program.span = SourceSpan{
        programBegin,
        current().span.end};
    return std::move(result_);
}

Statement Parser::parseStatement()
{
    if (check(TokenKind::Load)) {
        return parseLoadStatement();
    }
    if (check(TokenKind::Save)) {
        return parseSaveStatement();
    }
    if (check(TokenKind::Plot)) {
        return parsePlotStatement();
    }
    if (match(TokenKind::Let)) {
        return parseAssignmentStatement(true);
    }
    if (check(TokenKind::Identifier)
        && peek(1).kind == TokenKind::Equal) {
        return parseAssignmentStatement(false);
    }
    return parseExpressionStatement();
}

Statement Parser::parseLoadStatement()
{
    const Token& load = consume(
        TokenKind::Load,
        "PAR001",
        "Expected 'load'.");
    const Token& path = consume(
        TokenKind::String,
        "PAR002",
        "Expected a string path after 'load'.");

    std::optional<std::string> alias;
    std::optional<SourceSpan> aliasSpan;
    if (match(TokenKind::As)) {
        const Token& name = consume(
            TokenKind::Identifier,
            "PAR003",
            "Expected an identifier after 'as'.");
        alias = name.lexeme;
        aliasSpan = name.span;
    }

    const Token& semicolon = consume(
        TokenKind::Semicolon,
        "PAR004",
        "Expected ';' after the load statement.");
    return Statement{
        nextNodeId(),
        SourceSpan{load.span.begin, semicolon.span.end},
        LoadStatement{
            stringLiteral(path),
            path.span,
            std::move(alias),
            aliasSpan}};
}

Statement Parser::parseSaveStatement()
{
    const Token& save = consume(
        TokenKind::Save,
        "PAR005",
        "Expected 'save'.");
    ExpressionPtr value = parseExpression();
    consume(
        TokenKind::To,
        "PAR006",
        "Expected 'to' between the saved expression and output path.");
    const Token& path = consume(
        TokenKind::String,
        "PAR007",
        "Expected a string output path after 'to'.");
    const Token& semicolon = consume(
        TokenKind::Semicolon,
        "PAR008",
        "Expected ';' after the save statement.");
    return Statement{
        nextNodeId(),
        SourceSpan{save.span.begin, semicolon.span.end},
        SaveStatement{
            std::move(value),
            stringLiteral(path),
            path.span}};
}

Statement Parser::parsePlotStatement()
{
    const Token& plot = consume(
        TokenKind::Plot,
        "PAR009",
        "Expected 'plot'.");
    ExpressionPtr value = parseExpression();

    std::optional<std::string> title;
    std::optional<SourceSpan> titleSpan;
    if (match(TokenKind::As)) {
        const Token& titleToken = consume(
            TokenKind::String,
            "PAR010",
            "Expected a string title after 'as'.");
        title = stringLiteral(titleToken);
        titleSpan = titleToken.span;
    }

    const Token& semicolon = consume(
        TokenKind::Semicolon,
        "PAR011",
        "Expected ';' after the plot statement.");
    return Statement{
        nextNodeId(),
        SourceSpan{plot.span.begin, semicolon.span.end},
        PlotStatement{
            std::move(value),
            std::move(title),
            titleSpan}};
}

Statement Parser::parseAssignmentStatement(bool declaration)
{
    const SourceLocation begin = declaration
        ? previous().span.begin
        : current().span.begin;
    const Token& name = consume(
        TokenKind::Identifier,
        "PAR012",
        "Expected an identifier on the left side of the assignment.");
    consume(
        TokenKind::Equal,
        "PAR013",
        "Expected '=' after the assignment name.");
    ExpressionPtr value = parseExpression();
    const Token& semicolon = consume(
        TokenKind::Semicolon,
        "PAR014",
        "Expected ';' after the assignment.");
    return Statement{
        nextNodeId(),
        SourceSpan{begin, semicolon.span.end},
        AssignmentStatement{
            declaration,
            name.lexeme,
            name.span,
            std::move(value)}};
}

Statement Parser::parseExpressionStatement()
{
    const SourceLocation begin = current().span.begin;
    ExpressionPtr value = parseExpression();
    const Token& semicolon = consume(
        TokenKind::Semicolon,
        "PAR015",
        "Expected ';' after the expression.");
    return Statement{
        nextNodeId(),
        SourceSpan{begin, semicolon.span.end},
        ExpressionStatement{std::move(value)}};
}

ExpressionPtr Parser::parseExpression()
{
    if (expressionDepth_ >= kMaximumExpressionNestingDepth) {
        report(
            "PAR022",
            "Expression nesting exceeds the supported depth of "
                + std::to_string(kMaximumExpressionNestingDepth) + ".",
            current().span);
        throw ParseFailure{};
    }
    const ExpressionDepthGuard depthGuard(expressionDepth_);
    return parsePipeline();
}

ExpressionPtr Parser::parsePipeline()
{
    ExpressionPtr input = parsePrimary();
    std::size_t stageCount = 0;
    while (match(TokenKind::PipeGreater)) {
        if (stageCount >= kMaximumPipelineStageCount) {
            report(
                "PAR023",
                "A pipeline exceeds the supported limit of "
                    + std::to_string(kMaximumPipelineStageCount)
                    + " stages.",
                previous().span);
            throw ParseFailure{};
        }
        ++stageCount;
        if (!check(TokenKind::Identifier)
            || peek(1).kind != TokenKind::LeftParenthesis) {
            report(
                "PAR016",
                "Expected a function call after the pipeline operator '|>'.",
                current().span);
            throw ParseFailure{};
        }
        ExpressionPtr stage = parseCall();
        const SourceSpan span{input->span.begin, stage->span.end};
        input = makeExpression(
            span,
            PipelineExpression{std::move(input), std::move(stage)});
    }
    return input;
}

ExpressionPtr Parser::parsePrimary()
{
    if (match(TokenKind::Number)) {
        const Token& token = previous();
        return makeExpression(
            token.span,
            LiteralExpression{std::get<double>(token.literal)});
    }
    if (match(TokenKind::String)) {
        const Token& token = previous();
        return makeExpression(
            token.span,
            LiteralExpression{stringLiteral(token)});
    }
    if (match(TokenKind::Boolean)) {
        const Token& token = previous();
        return makeExpression(
            token.span,
            LiteralExpression{std::get<bool>(token.literal)});
    }
    if (check(TokenKind::Identifier)
        && peek(1).kind == TokenKind::LeftParenthesis) {
        return parseCall();
    }
    if (match(TokenKind::Identifier)) {
        const Token& token = previous();
        return makeExpression(
            token.span,
            VariableExpression{token.lexeme, token.span});
    }
    if (match(TokenKind::LeftParenthesis)) {
        const Token& opening = previous();
        ExpressionPtr inner = parseExpression();
        const Token& closing = consume(
            TokenKind::RightParenthesis,
            "PAR017",
            "Expected ')' after the grouped expression.");
        return makeExpression(
            SourceSpan{opening.span.begin, closing.span.end},
            GroupingExpression{std::move(inner)});
    }

    report(
        "PAR018",
        std::string("Expected an expression, but found ")
            + tokenKindName(current().kind) + ".",
        current().span);
    throw ParseFailure{};
}

ExpressionPtr Parser::parseCall()
{
    const Token& callee = consume(
        TokenKind::Identifier,
        "PAR019",
        "Expected a function name.");
    consume(
        TokenKind::LeftParenthesis,
        "PAR020",
        "Expected '(' after the function name.");

    std::vector<CallArgument> arguments;
    if (!check(TokenKind::RightParenthesis)) {
        do {
            std::optional<std::string> name;
            std::optional<SourceSpan> nameSpan;
            SourceLocation argumentBegin = current().span.begin;
            if (check(TokenKind::Identifier)
                && peek(1).kind == TokenKind::Equal) {
                const Token& nameToken = advance();
                name = nameToken.lexeme;
                nameSpan = nameToken.span;
                argumentBegin = nameToken.span.begin;
                advance();  // '='
            }
            ExpressionPtr value = parseExpression();
            const SourceLocation argumentEnd = value->span.end;
            arguments.push_back(CallArgument{
                std::move(name),
                nameSpan,
                std::move(value),
                SourceSpan{argumentBegin, argumentEnd}});
        } while (match(TokenKind::Comma));
    }

    const Token& closing = consume(
        TokenKind::RightParenthesis,
        "PAR021",
        "Expected ')' after the function arguments.");
    return makeExpression(
        SourceSpan{callee.span.begin, closing.span.end},
        CallExpression{callee.lexeme, callee.span, std::move(arguments)});
}

bool Parser::isAtEnd() const noexcept
{
    return current().kind == TokenKind::EndOfFile;
}

const Token& Parser::current() const noexcept
{
    if (tokens_.empty() || currentIndex_ >= tokens_.size()) {
        return fallbackEofToken();
    }
    return tokens_[currentIndex_];
}

const Token& Parser::previous() const noexcept
{
    if (tokens_.empty() || currentIndex_ == 0U) {
        return fallbackEofToken();
    }
    const std::size_t previousIndex = currentIndex_ <= tokens_.size()
        ? currentIndex_ - 1U
        : tokens_.size() - 1U;
    return tokens_[previousIndex];
}

const Token& Parser::peek(std::size_t distance) const noexcept
{
    if (tokens_.empty()) {
        return fallbackEofToken();
    }
    const std::size_t desired = currentIndex_ + distance;
    return desired < tokens_.size()
        ? tokens_[desired]
        : fallbackEofToken();
}

bool Parser::check(TokenKind kind) const noexcept
{
    return current().kind == kind;
}

bool Parser::match(TokenKind kind) noexcept
{
    if (!check(kind)) {
        return false;
    }
    advance();
    return true;
}

const Token& Parser::advance() noexcept
{
    if (!isAtEnd()) {
        ++currentIndex_;
    }
    return previous();
}

const Token& Parser::consume(
    TokenKind kind,
    std::string code,
    std::string message)
{
    if (check(kind)) {
        return advance();
    }
    report(std::move(code), std::move(message), current().span);
    throw ParseFailure{};
}

void Parser::report(
    std::string code,
    std::string message,
    SourceSpan span)
{
    result_.diagnostics.push_back(Diagnostic{
        DiagnosticStage::Parser,
        DiagnosticSeverity::Error,
        std::move(code),
        std::move(message),
        span});
}

void Parser::synchronize() noexcept
{
    while (!isAtEnd()) {
        if (current().kind == TokenKind::Semicolon) {
            advance();
            return;
        }
        advance();
    }
}

NodeId Parser::nextNodeId() noexcept
{
    return nextNodeId_++;
}

ExpressionPtr Parser::makeExpression(
    SourceSpan span,
    ExpressionNode node)
{
    return std::make_unique<Expression>(Expression{
        nextNodeId(),
        span,
        std::move(node)});
}

}  // namespace signallab::parser
