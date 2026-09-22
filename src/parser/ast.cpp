#include "signallab/parser/ast.hpp"

#include <charconv>
#include <limits>
#include <string>
#include <type_traits>
#include <variant>

namespace signallab::parser {
namespace {

template<class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};

template<class... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

std::string escapeString(const std::string& value)
{
    std::string result;
    result.push_back('"');
    for (const char character : value) {
        switch (character) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result.push_back(character);
            break;
        }
    }
    result.push_back('"');
    return result;
}

std::string formatNumber(double value)
{
    char buffer[64]{};
    const auto conversion = std::to_chars(
        buffer,
        buffer + sizeof(buffer),
        value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    return std::string(buffer, conversion.ptr);
}

std::string formatExpression(const Expression& expression);

std::string formatArgument(const CallArgument& argument)
{
    std::string result = "(arg";
    if (argument.name.has_value()) {
        result += " " + *argument.name;
    }
    result += " " + formatExpression(*argument.value) + ")";
    return result;
}

std::string formatExpression(const Expression& expression)
{
    return std::visit(
        Overloaded{
            [](const LiteralExpression& literal) {
                return std::visit(
                    Overloaded{
                        [](double value) {
                            return "(number " + formatNumber(value) + ")";
                        },
                        [](const std::string& value) {
                            return "(string " + escapeString(value) + ")";
                        },
                        [](bool value) {
                            return std::string("(boolean ")
                                + (value ? "true)" : "false)");
                        }},
                    literal.value);
            },
            [](const VariableExpression& variable) {
                return "(variable " + variable.name + ")";
            },
            [](const CallExpression& call) {
                std::string result = "(call " + call.callee;
                for (const auto& argument : call.arguments) {
                    result += " " + formatArgument(argument);
                }
                result += ")";
                return result;
            },
            [](const PipelineExpression& pipeline) {
                return "(pipeline " + formatExpression(*pipeline.input)
                    + " " + formatExpression(*pipeline.stage) + ")";
            },
            [](const GroupingExpression& grouping) {
                return "(group " + formatExpression(*grouping.expression)
                    + ")";
            }},
        expression.node);
}

std::string formatStatement(const Statement& statement)
{
    return std::visit(
        Overloaded{
            [](const LoadStatement& load) {
                std::string result = "(load " + escapeString(load.path);
                if (load.alias.has_value()) {
                    result += " as " + *load.alias;
                }
                result += ")";
                return result;
            },
            [](const SaveStatement& save) {
                return "(save " + formatExpression(*save.value) + " to "
                    + escapeString(save.path) + ")";
            },
            [](const PlotStatement& plot) {
                std::string result = "(plot "
                    + formatExpression(*plot.value);
                if (plot.title.has_value()) {
                    result += " as " + escapeString(*plot.title);
                }
                result += ")";
                return result;
            },
            [](const AssignmentStatement& assignment) {
                return std::string(assignment.declaration ? "(let " : "(assign ")
                    + assignment.name + " "
                    + formatExpression(*assignment.value) + ")";
            },
            [](const ExpressionStatement& expression) {
                return "(expression "
                    + formatExpression(*expression.value) + ")";
            }},
        statement.node);
}

}  // namespace

std::string formatAst(const Program& program)
{
    std::string result = "(program";
    for (const auto& statement : program.statements) {
        result += " " + formatStatement(statement);
    }
    result += ")";
    return result;
}

}  // namespace signallab::parser
