#include "signallab/parser/ast.hpp"
#include "signallab/parser/lexer.hpp"
#include "signallab/parser/parser.hpp"
#include "signallab/parser/semantic_checker.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace signallab::parser {
namespace {

struct FrontendResult {
    LexResult lex;
    ParseResult parse;
    SemanticResult semantic;
};

FrontendResult runFrontend(std::string_view source)
{
    Lexer lexer(source);
    LexResult lex = lexer.tokenize();
    Parser parser(lex.tokens);
    ParseResult parse = parser.parse();
    SemanticChecker checker;
    SemanticResult semantic = checker.check(parse.program);
    return FrontendResult{
        std::move(lex),
        std::move(parse),
        std::move(semantic)};
}

const Diagnostic* findDiagnostic(
    const std::vector<Diagnostic>& diagnostics,
    std::string_view code)
{
    const auto found = std::find_if(
        diagnostics.begin(),
        diagnostics.end(),
        [code](const Diagnostic& diagnostic) {
            return diagnostic.code == code;
        });
    return found == diagnostics.end() ? nullptr : &*found;
}

std::vector<std::string> boundParameterNames(const BoundCall& call)
{
    std::vector<std::string> names;
    names.reserve(call.arguments.size());
    for (const BoundArgument& argument : call.arguments) {
        names.push_back(argument.parameterName);
    }
    return names;
}

TEST(SignalScriptLexerTest, RecognizesTheLanguageVocabularyAndEscapes)
{
    constexpr std::string_view source =
        "# a comment\r\n"
        "load \"a\\n\\t\\\"\\\\b.csv\" as raw;\n"
        "let gain = -1.25e+2;\n"
        "plot raw as \"波形\";";

    Lexer lexer(source);
    const LexResult result = lexer.tokenize();

    ASSERT_FALSE(result.hasErrors());
    const std::vector<TokenKind> expectedKinds{
        TokenKind::Load,
        TokenKind::String,
        TokenKind::As,
        TokenKind::Identifier,
        TokenKind::Semicolon,
        TokenKind::Let,
        TokenKind::Identifier,
        TokenKind::Equal,
        TokenKind::Number,
        TokenKind::Semicolon,
        TokenKind::Plot,
        TokenKind::Identifier,
        TokenKind::As,
        TokenKind::String,
        TokenKind::Semicolon,
        TokenKind::EndOfFile};
    ASSERT_EQ(result.tokens.size(), expectedKinds.size());
    for (std::size_t index = 0; index < expectedKinds.size(); ++index) {
        EXPECT_EQ(result.tokens[index].kind, expectedKinds[index]) << index;
    }
    EXPECT_EQ(
        std::get<std::string>(result.tokens[1].literal),
        "a\n\t\"\\b.csv");
    EXPECT_DOUBLE_EQ(std::get<double>(result.tokens[8].literal), -125.0);
    EXPECT_EQ(
        std::get<std::string>(result.tokens[13].literal),
        "波形");
}

TEST(SignalScriptLexerTest, TracksCrLfAndUtf8ByDisplayedCodePoint)
{
    constexpr std::string_view source =
        "# 中文注释\r\nplot raw as \"波形\";";

    Lexer lexer(source);
    const LexResult result = lexer.tokenize();

    ASSERT_FALSE(result.hasErrors());
    ASSERT_GE(result.tokens.size(), 5U);
    EXPECT_EQ(result.tokens[0].span.begin, (SourceLocation{16U, 2U, 1U}));
    EXPECT_EQ(result.tokens[3].kind, TokenKind::String);
    EXPECT_EQ(result.tokens[3].span.begin.line, 2U);
    EXPECT_EQ(result.tokens[3].span.begin.column, 13U);
    EXPECT_EQ(result.tokens[3].span.end.column, 17U);
}

TEST(SignalScriptLexerTest, ReportsIllegalCharacterAtExactLocation)
{
    constexpr std::string_view source = "let x = @;";

    Lexer lexer(source);
    const LexResult result = lexer.tokenize();

    const Diagnostic* diagnostic = findDiagnostic(result.diagnostics, "LEX001");
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->stage, DiagnosticStage::Lexer);
    EXPECT_EQ(diagnostic->span.begin, (SourceLocation{8U, 1U, 9U}));
    EXPECT_EQ(diagnostic->span.end, (SourceLocation{9U, 1U, 10U}));
}

TEST(SignalScriptLexerTest, AcceptsFractionsWithoutALeadingZero)
{
    constexpr std::string_view source =
        "let negative = -.5; let positive = .25;";

    Lexer lexer(source);
    const LexResult result = lexer.tokenize();

    ASSERT_FALSE(result.hasErrors());
    ASSERT_GT(result.tokens.size(), 8U);
    EXPECT_DOUBLE_EQ(std::get<double>(result.tokens[3].literal), -0.5);
    EXPECT_DOUBLE_EQ(std::get<double>(result.tokens[8].literal), 0.25);
}

TEST(SignalScriptLexerTest, ReportsInvalidUtf8WithoutLosingFollowingTokens)
{
    std::string source = "let x = \"";
    source.push_back(static_cast<char>(0xFF));
    source += "\"; let y = 2;";

    Lexer lexer(source);
    const LexResult result = lexer.tokenize();

    const Diagnostic* diagnostic = findDiagnostic(result.diagnostics, "LEX005");
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->span.begin.line, 1U);
    EXPECT_EQ(diagnostic->span.begin.column, 10U);
    EXPECT_EQ(diagnostic->span.end.column, 11U);
    ASSERT_GE(result.tokens.size(), 11U);
    EXPECT_EQ(result.tokens[result.tokens.size() - 2U].kind, TokenKind::Semicolon);
}

TEST(SignalScriptLexerTest, ReportsUnclosedStringAndInvalidExponent)
{
    constexpr std::string_view source =
        "let value = 1e+;\r\nload \"unterminated";

    Lexer lexer(source);
    const LexResult result = lexer.tokenize();

    const Diagnostic* number = findDiagnostic(result.diagnostics, "LEX004");
    const Diagnostic* string = findDiagnostic(result.diagnostics, "LEX002");
    ASSERT_NE(number, nullptr);
    ASSERT_NE(string, nullptr);
    EXPECT_EQ(number->span.begin, (SourceLocation{12U, 1U, 13U}));
    EXPECT_EQ(number->span.end.column, 16U);
    EXPECT_EQ(string->span.begin.line, 2U);
    EXPECT_EQ(string->span.begin.column, 6U);
    EXPECT_EQ(string->span.end.line, 2U);
    EXPECT_EQ(string->span.end.column, 19U);
}

TEST(SignalScriptParserTest, ParsesTheReferenceAnalysisScript)
{
    constexpr std::string_view source =
        "# import and analyze\n"
        "load \"data/samples/motor_01.csv\" as raw;\n"
        "clean = raw |> normalize() |> lowpass(cutoff=1000, order=63);\n"
        "spec = clean |> fft(window=\"hann\");\n"
        "result = features(spec, fundamental=200);\n"
        "plot clean as \"filtered\";\n"
        "plot spec as \"spectrum\";\n"
        "save result to \"outputs/features.json\";";

    Lexer lexer(source);
    LexResult lex = lexer.tokenize();
    ASSERT_FALSE(lex.hasErrors());
    Parser parser(lex.tokens);
    const ParseResult result = parser.parse();

    ASSERT_FALSE(result.hasErrors());
    EXPECT_EQ(result.program.statements.size(), 7U);
    EXPECT_EQ(
        formatAst(result.program),
        "(program "
        "(load \"data/samples/motor_01.csv\" as raw) "
        "(assign clean (pipeline (pipeline (variable raw) "
        "(call normalize)) (call lowpass (arg cutoff (number 1000)) "
        "(arg order (number 63))))) "
        "(assign spec (pipeline (variable clean) "
        "(call fft (arg window (string \"hann\"))))) "
        "(assign result (call features (arg (variable spec)) "
        "(arg fundamental (number 200)))) "
        "(plot (variable clean) as \"filtered\") "
        "(plot (variable spec) as \"spectrum\") "
        "(save (variable result) to \"outputs/features.json\"))");
}

TEST(SignalScriptParserTest, ParsesLiteralsGroupingCallsAndNamedArguments)
{
    constexpr std::string_view source =
        "let enabled = true;\n"
        "let label = \"demo\";\n"
        "let result = (features(input=fft(lowpass(raw, 100, 63), "
        "window=\"hamming\"), fundamental=50));";

    Lexer lexer(source);
    LexResult lex = lexer.tokenize();
    ASSERT_FALSE(lex.hasErrors());
    Parser parser(lex.tokens);
    const ParseResult result = parser.parse();

    ASSERT_FALSE(result.hasErrors());
    EXPECT_EQ(result.program.statements.size(), 3U);
    const std::string formatted = formatAst(result.program);
    EXPECT_NE(formatted.find("(boolean true)"), std::string::npos);
    EXPECT_NE(formatted.find("(group (call features"), std::string::npos);
    EXPECT_NE(formatted.find("(arg input (call fft"), std::string::npos);
    EXPECT_NE(formatted.find("(arg fundamental (number 50))"), std::string::npos);
}

TEST(SignalScriptParserTest, RecoversAtSemicolonAndParsesLaterStatements)
{
    constexpr std::string_view source =
        "let broken = fft(raw;\r\n"
        "let good = 42;\r\n"
        "good;";

    Lexer lexer(source);
    LexResult lex = lexer.tokenize();
    ASSERT_FALSE(lex.hasErrors());
    Parser parser(lex.tokens);
    const ParseResult result = parser.parse();

    const Diagnostic* diagnostic = findDiagnostic(result.diagnostics, "PAR021");
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->span.begin.line, 1U);
    EXPECT_EQ(diagnostic->span.begin.column, 21U);
    ASSERT_EQ(result.program.statements.size(), 2U);
    EXPECT_EQ(
        formatAst(result.program),
        "(program (let good (number 42)) (expression (variable good)))");
}

TEST(SignalScriptParserTest, RequiresACallAfterPipelineOperator)
{
    constexpr std::string_view source =
        "raw |> fft;\n"
        "let recovered = 1;";

    Lexer lexer(source);
    LexResult lex = lexer.tokenize();
    Parser parser(lex.tokens);
    const ParseResult result = parser.parse();

    const Diagnostic* diagnostic = findDiagnostic(result.diagnostics, "PAR016");
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->span.begin, (SourceLocation{7U, 1U, 8U}));
    ASSERT_EQ(result.program.statements.size(), 1U);
    EXPECT_EQ(
        formatAst(result.program),
        "(program (let recovered (number 1)))");
}

TEST(SignalScriptParserTest, ReportsMissingClosingParenthesisAtEof)
{
    constexpr std::string_view source = "let x = fft(raw";

    Lexer lexer(source);
    LexResult lex = lexer.tokenize();
    Parser parser(lex.tokens);
    const ParseResult result = parser.parse();

    const Diagnostic* diagnostic = findDiagnostic(result.diagnostics, "PAR021");
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->span.begin.line, 1U);
    EXPECT_EQ(diagnostic->span.begin.column, 16U);
    EXPECT_TRUE(result.program.statements.empty());
}

TEST(SignalScriptParserTest, RejectsExcessiveExpressionNestingAndRecovers)
{
    std::string source = "let too_deep = ";
    source.append(kMaximumExpressionNestingDepth + 32U, '(');
    source += '1';
    source.append(kMaximumExpressionNestingDepth + 32U, ')');
    source += ";\nlet recovered = 2;";

    Lexer lexer(source);
    LexResult lex = lexer.tokenize();
    ASSERT_FALSE(lex.hasErrors());
    Parser parser(lex.tokens);
    const ParseResult result = parser.parse();

    EXPECT_NE(findDiagnostic(result.diagnostics, "PAR022"), nullptr);
    ASSERT_EQ(result.program.statements.size(), 1U);
    EXPECT_EQ(
        formatAst(result.program),
        "(program (let recovered (number 2)))");
}

TEST(SignalScriptParserTest, RejectsExcessivePipelineLengthAndRecovers)
{
    std::string source = "load \"signal.csv\" as raw;\nresult = raw";
    for (std::size_t index = 0;
         index < kMaximumPipelineStageCount + 32U;
         ++index) {
        source += " |> normalize()";
    }
    source += ";\nlet recovered = 2;";

    Lexer lexer(source);
    LexResult lex = lexer.tokenize();
    ASSERT_FALSE(lex.hasErrors());
    Parser parser(lex.tokens);
    const ParseResult result = parser.parse();

    EXPECT_NE(findDiagnostic(result.diagnostics, "PAR023"), nullptr);
    ASSERT_EQ(result.program.statements.size(), 2U);
    EXPECT_EQ(
        formatAst(result.program),
        "(program (load \"signal.csv\" as raw) "
        "(let recovered (number 2)))");
}

TEST(SignalScriptSemanticTest, AcceptsReferenceScriptAndBindsEveryBuiltin)
{
    constexpr std::string_view source =
        "load \"motor.csv\" as raw;\n"
        "clean = raw |> normalize() |> lowpass(cutoff=1000, order=63);\n"
        "spec = clean |> fft(window=\"hann\");\n"
        "result = features(input=spec, fundamental=200);\n"
        "plot clean as \"filtered\";\n"
        "save result to \"result.json\";";

    const FrontendResult result = runFrontend(source);

    ASSERT_FALSE(result.lex.hasErrors());
    ASSERT_FALSE(result.parse.hasErrors());
    ASSERT_FALSE(result.semantic.hasErrors());
    EXPECT_EQ(result.semantic.model.finalSymbols.at("raw"), ScriptType::Signal);
    EXPECT_EQ(result.semantic.model.finalSymbols.at("clean"), ScriptType::Signal);
    EXPECT_EQ(result.semantic.model.finalSymbols.at("spec"), ScriptType::Spectrum);
    EXPECT_EQ(
        result.semantic.model.finalSymbols.at("result"),
        ScriptType::FeatureSet);

    std::vector<BuiltinId> builtins;
    for (const auto& [nodeId, call] : result.semantic.model.calls) {
        static_cast<void>(nodeId);
        builtins.push_back(call.builtin);
    }
    EXPECT_EQ(
        std::count(builtins.begin(), builtins.end(), BuiltinId::Normalize),
        1);
    EXPECT_EQ(
        std::count(builtins.begin(), builtins.end(), BuiltinId::Lowpass),
        1);
    EXPECT_EQ(
        std::count(builtins.begin(), builtins.end(), BuiltinId::Fft),
        1);
    EXPECT_EQ(
        std::count(builtins.begin(), builtins.end(), BuiltinId::Features),
        1);
}

TEST(SignalScriptSemanticTest, FeaturesAcceptsSignalSpectrumAndPipelineInput)
{
    constexpr std::string_view source =
        "load \"signal.csv\" as raw;\n"
        "from_signal = features(raw);\n"
        "from_pipe = raw |> features();\n"
        "spectrum = raw |> fft();\n"
        "from_spectrum = features(input=spectrum, fundamental=50);";

    const FrontendResult result = runFrontend(source);

    ASSERT_FALSE(result.lex.hasErrors());
    ASSERT_FALSE(result.parse.hasErrors());
    ASSERT_FALSE(result.semantic.hasErrors());
    EXPECT_EQ(
        result.semantic.model.finalSymbols.at("from_signal"),
        ScriptType::FeatureSet);
    EXPECT_EQ(
        result.semantic.model.finalSymbols.at("from_pipe"),
        ScriptType::FeatureSet);
    EXPECT_EQ(
        result.semantic.model.finalSymbols.at("from_spectrum"),
        ScriptType::FeatureSet);

    for (const auto& [nodeId, call] : result.semantic.model.calls) {
        static_cast<void>(nodeId);
        if (call.builtin == BuiltinId::Features) {
            const auto names = boundParameterNames(call);
            ASSERT_FALSE(names.empty());
            EXPECT_EQ(names.front(), "input");
        }
    }
}

TEST(SignalScriptSemanticTest, ReportsUnknownVariableAtItsIdentifier)
{
    constexpr std::string_view source =
        "let result = missing |> fft();";

    const FrontendResult result = runFrontend(source);

    const Diagnostic* diagnostic = findDiagnostic(
        result.semantic.diagnostics,
        "SEM008");
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->span.begin, (SourceLocation{13U, 1U, 14U}));
    EXPECT_EQ(diagnostic->span.end.column, 21U);
}

TEST(SignalScriptSemanticTest, ValidatesLowpassCutoffAndOrder)
{
    constexpr std::string_view source =
        "load \"signal.csv\" as raw;\n"
        "bad = raw |> lowpass(cutoff=0, order=4);";

    const FrontendResult result = runFrontend(source);

    EXPECT_NE(findDiagnostic(result.semantic.diagnostics, "SEM017"), nullptr);
    EXPECT_NE(findDiagnostic(result.semantic.diagnostics, "SEM018"), nullptr);
}

TEST(SignalScriptSemanticTest, RejectsResourceExhaustingLowpassOrder)
{
    constexpr std::string_view source =
        "load \"signal.csv\" as raw;\n"
        "bad = raw |> lowpass(cutoff=100, order=9007199254740991);";

    const FrontendResult result = runFrontend(source);

    const Diagnostic* diagnostic = findDiagnostic(
        result.semantic.diagnostics,
        "SEM018");
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_NE(
        diagnostic->message.find(std::to_string(kMaximumLowpassOrder)),
        std::string::npos);
}

TEST(SignalScriptSemanticTest, ValidatesFftWindowAndFeatureFundamental)
{
    constexpr std::string_view source =
        "load \"signal.csv\" as raw;\n"
        "spec = raw |> fft(window=\"blackman\");\n"
        "values = features(spec, fundamental=-20);";

    const FrontendResult result = runFrontend(source);

    EXPECT_NE(findDiagnostic(result.semantic.diagnostics, "SEM020"), nullptr);
    EXPECT_NE(findDiagnostic(result.semantic.diagnostics, "SEM021"), nullptr);
}

TEST(SignalScriptSemanticTest, RejectsFundamentalForBasicSignalFeatures)
{
    constexpr std::string_view source =
        "load \"signal.csv\" as raw;\n"
        "values = features(raw, fundamental=50);";

    const FrontendResult result = runFrontend(source);

    const Diagnostic* diagnostic = findDiagnostic(
        result.semantic.diagnostics,
        "SEM022");
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->span.begin.line, 2U);
    EXPECT_EQ(diagnostic->span.begin.column, 24U);
}

TEST(SignalScriptSemanticTest, RejectsDuplicateUnknownAndMisorderedArguments)
{
    constexpr std::string_view source =
        "load \"signal.csv\" as raw;\n"
        "a = lowpass(raw, cutoff=100, 63);\n"
        "b = raw |> lowpass(cutoff=100, cutoff=200);\n"
        "c = raw |> fft(size=1024);\n"
        "d = system(\"dir\");\n"
        "e = http_get(\"https://example.com\");\n"
        "f = read_file(\"outside.txt\");";

    const FrontendResult result = runFrontend(source);

    EXPECT_NE(findDiagnostic(result.semantic.diagnostics, "SEM012"), nullptr);
    EXPECT_NE(findDiagnostic(result.semantic.diagnostics, "SEM014"), nullptr);
    EXPECT_NE(findDiagnostic(result.semantic.diagnostics, "SEM011"), nullptr);

    std::vector<const Diagnostic*> unknownCalls;
    for (const Diagnostic& diagnostic : result.semantic.diagnostics) {
        if (diagnostic.code == "SEM010") {
            unknownCalls.push_back(&diagnostic);
        }
    }
    ASSERT_EQ(unknownCalls.size(), 3U);
    for (std::size_t index = 0; index < unknownCalls.size(); ++index) {
        EXPECT_EQ(unknownCalls[index]->span.begin.line, 5U + index);
        EXPECT_EQ(unknownCalls[index]->span.begin.column, 5U);
    }
    EXPECT_NE(
        unknownCalls[0]->message.find("system"),
        std::string::npos);
    EXPECT_NE(
        unknownCalls[1]->message.find("http_get"),
        std::string::npos);
    EXPECT_NE(
        unknownCalls[2]->message.find("read_file"),
        std::string::npos);
}

TEST(SignalScriptSemanticTest, EnforcesStatementAndAssignmentTypes)
{
    constexpr std::string_view source =
        "let number = 1;\n"
        "plot number;\n"
        "save true to \"value.json\";\n"
        "number = \"changed\";";

    const FrontendResult result = runFrontend(source);

    EXPECT_NE(findDiagnostic(result.semantic.diagnostics, "SEM005"), nullptr);
    EXPECT_NE(findDiagnostic(result.semantic.diagnostics, "SEM003"), nullptr);
    EXPECT_NE(findDiagnostic(result.semantic.diagnostics, "SEM007"), nullptr);
}

TEST(SignalScriptSemanticTest, LocatesPipelineTypeErrorsAtTheInput)
{
    constexpr std::string_view source = "bad = 1 |> fft();";

    const FrontendResult result = runFrontend(source);

    const Diagnostic* diagnostic = findDiagnostic(
        result.semantic.diagnostics,
        "SEM016");
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->span.begin, (SourceLocation{6U, 1U, 7U}));
    EXPECT_EQ(diagnostic->span.end, (SourceLocation{7U, 1U, 8U}));
    EXPECT_NE(diagnostic->message.find("expects Signal"), std::string::npos);
}

TEST(SignalScriptSemanticTest, DoesNotEmitRangeErrorsForWrongValueTypes)
{
    constexpr std::string_view source =
        "load \"signal.csv\" as raw;\n"
        "filtered = raw |> lowpass(cutoff=\"fast\", order=3);\n"
        "values = features(input=true, fundamental=\"base\");";

    const FrontendResult result = runFrontend(source);

    EXPECT_EQ(
        std::count_if(
            result.semantic.diagnostics.begin(),
            result.semantic.diagnostics.end(),
            [](const Diagnostic& diagnostic) {
                return diagnostic.code == "SEM016";
            }),
        3);
    EXPECT_EQ(findDiagnostic(result.semantic.diagnostics, "SEM017"), nullptr);
    EXPECT_EQ(findDiagnostic(result.semantic.diagnostics, "SEM021"), nullptr);
}

TEST(SignalScriptSemanticTest, RejectsEmptyPathsAndDuplicateDeclarations)
{
    constexpr std::string_view source =
        "load \"\" as raw;\n"
        "let raw = 1;\n"
        "save raw to \"\";";

    const FrontendResult result = runFrontend(source);

    EXPECT_NE(findDiagnostic(result.semantic.diagnostics, "SEM001"), nullptr);
    EXPECT_NE(findDiagnostic(result.semantic.diagnostics, "SEM006"), nullptr);
    EXPECT_NE(findDiagnostic(result.semantic.diagnostics, "SEM004"), nullptr);
}

}  // namespace
}  // namespace signallab::parser
