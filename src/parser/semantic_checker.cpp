#include "signallab/parser/semantic_checker.hpp"

#include "signallab/parser/parser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace signallab::parser {
namespace {

template<class... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};

template<class... Visitors>
Overloaded(Visitors...) -> Overloaded<Visitors...>;

struct ParameterSpec {
    std::string_view name;
    ScriptType type;
    bool required;
};

struct BuiltinSpec {
    BuiltinId id;
    std::string_view name;
    const ParameterSpec* parameters;
    std::size_t parameterCount;
    ScriptType resultType;
};

constexpr std::array lowpassParameters{
    ParameterSpec{"signal", ScriptType::Signal, true},
    ParameterSpec{"cutoff", ScriptType::Number, true},
    ParameterSpec{"order", ScriptType::Number, false}};

constexpr std::array normalizeParameters{
    ParameterSpec{"signal", ScriptType::Signal, true},
    ParameterSpec{"target", ScriptType::Number, false}};

constexpr std::array fftParameters{
    ParameterSpec{"signal", ScriptType::Signal, true},
    ParameterSpec{"window", ScriptType::String, false}};

constexpr std::array featuresParameters{
    ParameterSpec{"input", ScriptType::Signal, true},
    ParameterSpec{"fundamental", ScriptType::Number, false}};

constexpr std::array builtinSpecs{
    BuiltinSpec{
        BuiltinId::Lowpass,
        "lowpass",
        lowpassParameters.data(),
        lowpassParameters.size(),
        ScriptType::Signal},
    BuiltinSpec{
        BuiltinId::Normalize,
        "normalize",
        normalizeParameters.data(),
        normalizeParameters.size(),
        ScriptType::Signal},
    BuiltinSpec{
        BuiltinId::Fft,
        "fft",
        fftParameters.data(),
        fftParameters.size(),
        ScriptType::Spectrum},
    BuiltinSpec{
        BuiltinId::Features,
        "features",
        featuresParameters.data(),
        featuresParameters.size(),
        ScriptType::FeatureSet}};

const BuiltinSpec* findBuiltin(std::string_view name) noexcept
{
    const auto found = std::find_if(
        builtinSpecs.begin(),
        builtinSpecs.end(),
        [name](const BuiltinSpec& spec) {
            return spec.name == name;
        });
    return found == builtinSpecs.end() ? nullptr : &*found;
}

std::optional<std::size_t> findParameter(
    const BuiltinSpec& spec,
    std::string_view name) noexcept
{
    for (std::size_t index = 0; index < spec.parameterCount; ++index) {
        if (spec.parameters[index].name == name) {
            return index;
        }
    }
    return std::nullopt;
}

bool canPlot(ScriptType type) noexcept
{
    return type == ScriptType::Signal || type == ScriptType::Spectrum;
}

bool canSave(ScriptType type) noexcept
{
    return type == ScriptType::Signal || type == ScriptType::Spectrum
        || type == ScriptType::FeatureSet;
}

bool acceptsParameterType(
    const BuiltinSpec& spec,
    std::size_t parameterIndex,
    ScriptType actualType) noexcept
{
    if (spec.id == BuiltinId::Features && parameterIndex == 0U) {
        return actualType == ScriptType::Signal
            || actualType == ScriptType::Spectrum;
    }
    return actualType == spec.parameters[parameterIndex].type;
}

std::string expectedParameterType(
    const BuiltinSpec& spec,
    std::size_t parameterIndex)
{
    if (spec.id == BuiltinId::Features && parameterIndex == 0U) {
        return "Signal or Spectrum";
    }
    return scriptTypeName(spec.parameters[parameterIndex].type);
}

bool isInvalidPositiveNumber(const LiteralValue& value) noexcept
{
    const auto* number = std::get_if<double>(&value);
    return number != nullptr
        && (!std::isfinite(*number) || *number <= 0.0);
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

SemanticResult SemanticChecker::check(const Program& program)
{
    symbols_.clear();
    expressionDepth_ = 0;
    result_ = {};
    for (const Statement& statement : program.statements) {
        checkStatement(statement);
    }
    for (const auto& [name, symbol] : symbols_) {
        result_.model.finalSymbols.emplace(name, symbol.type);
    }
    return std::move(result_);
}

void SemanticChecker::checkStatement(const Statement& statement)
{
    std::visit(
        Overloaded{
            [this](const LoadStatement& load) {
                if (load.path.empty()) {
                    report(
                        "SEM001",
                        "The load path must not be empty.",
                        load.pathSpan);
                }
                if (!load.alias.has_value()) {
                    return;
                }
                const auto existing = symbols_.find(*load.alias);
                if (existing != symbols_.end()) {
                    report(
                        "SEM002",
                        "The load alias '" + *load.alias
                            + "' is already declared.",
                        *load.aliasSpan);
                    return;
                }
                symbols_.emplace(
                    *load.alias,
                    Symbol{
                        ScriptType::Signal,
                        std::nullopt,
                        *load.aliasSpan});
            },
            [this](const SaveStatement& save) {
                const TypedValue value = checkExpression(*save.value);
                if (value.type != ScriptType::Invalid
                    && !canSave(value.type)) {
                    report(
                        "SEM003",
                        std::string("Cannot save a value of type ")
                            + scriptTypeName(value.type)
                            + "; expected Signal, Spectrum, or FeatureSet.",
                        save.value->span);
                }
                if (save.path.empty()) {
                    report(
                        "SEM004",
                        "The save path must not be empty.",
                        save.pathSpan);
                }
            },
            [this](const PlotStatement& plot) {
                const TypedValue value = checkExpression(*plot.value);
                if (value.type != ScriptType::Invalid
                    && !canPlot(value.type)) {
                    report(
                        "SEM005",
                        std::string("Cannot plot a value of type ")
                            + scriptTypeName(value.type)
                            + "; expected Signal or Spectrum.",
                        plot.value->span);
                }
            },
            [this](const AssignmentStatement& assignment) {
                const TypedValue value = checkExpression(*assignment.value);
                const auto existing = symbols_.find(assignment.name);

                if (assignment.declaration && existing != symbols_.end()) {
                    report(
                        "SEM006",
                        "The variable '" + assignment.name
                            + "' is already declared.",
                        assignment.nameSpan);
                    return;
                }

                if (existing != symbols_.end()) {
                    if (value.type != ScriptType::Invalid
                        && existing->second.type != ScriptType::Invalid
                        && value.type != existing->second.type) {
                        report(
                            "SEM007",
                            "Cannot change variable '" + assignment.name
                                + "' from "
                                + scriptTypeName(existing->second.type)
                                + " to " + scriptTypeName(value.type) + ".",
                            assignment.nameSpan);
                        return;
                    }
                    if (value.type != ScriptType::Invalid) {
                        existing->second.type = value.type;
                        existing->second.constant = value.constant;
                    }
                    return;
                }

                symbols_.emplace(
                    assignment.name,
                    Symbol{
                        value.type,
                        value.constant,
                        assignment.nameSpan});
            },
            [this](const ExpressionStatement& expression) {
                static_cast<void>(checkExpression(*expression.value));
            }},
        statement.node);
}

SemanticChecker::TypedValue SemanticChecker::checkExpression(
    const Expression& expression)
{
    if (expressionDepth_ >= kMaximumExpressionNestingDepth) {
        report(
            "SEM023",
            "Expression nesting exceeds the supported depth.",
            expression.span);
        recordType(expression.id, ScriptType::Invalid);
        return {};
    }
    const ExpressionDepthGuard depthGuard(expressionDepth_);

    const TypedValue value = std::visit(
        Overloaded{
            [](const LiteralExpression& literal) -> TypedValue {
                return std::visit(
                    Overloaded{
                        [&literal](double) {
                            return TypedValue{
                                ScriptType::Number,
                                literal.value};
                        },
                        [&literal](const std::string&) {
                            return TypedValue{
                                ScriptType::String,
                                literal.value};
                        },
                        [&literal](bool) {
                            return TypedValue{
                                ScriptType::Boolean,
                                literal.value};
                        }},
                    literal.value);
            },
            [this](const VariableExpression& variable) -> TypedValue {
                const auto symbol = symbols_.find(variable.name);
                if (symbol == symbols_.end()) {
                    report(
                        "SEM008",
                        "Unknown variable '" + variable.name + "'.",
                        variable.nameSpan);
                    return {};
                }
                return TypedValue{
                    symbol->second.type,
                    symbol->second.constant};
            },
            [this, &expression](const CallExpression& call) {
                return checkCall(expression, call, std::nullopt);
            },
            [this, &expression](const PipelineExpression& pipeline) {
                const TypedValue input = checkExpression(*pipeline.input);
                const auto* call = std::get_if<CallExpression>(
                    &pipeline.stage->node);
                if (call == nullptr) {
                    report(
                        "SEM009",
                        "A pipeline stage must be a function call.",
                        pipeline.stage->span);
                    return TypedValue{};
                }
                return checkCall(
                    *pipeline.stage,
                    *call,
                    PipelineInput{
                        pipeline.input->id,
                        input,
                        pipeline.input->span});
            },
            [this](const GroupingExpression& grouping) {
                return checkExpression(*grouping.expression);
            }},
        expression.node);

    recordType(expression.id, value.type);
    return value;
}

SemanticChecker::TypedValue SemanticChecker::checkCall(
    const Expression& expression,
    const CallExpression& call,
    std::optional<PipelineInput> pipelineInput)
{
    const BuiltinSpec* spec = findBuiltin(call.callee);
    if (spec == nullptr) {
        for (const CallArgument& argument : call.arguments) {
            static_cast<void>(checkExpression(*argument.value));
        }
        report(
            "SEM010",
            "Unknown function '" + call.callee + "'.",
            call.calleeSpan);
        recordType(expression.id, ScriptType::Invalid);
        return {};
    }

    struct CheckedArgument {
        NodeId expressionId;
        TypedValue value;
        SourceSpan span;
        bool pipelineInput;
    };

    std::vector<std::optional<CheckedArgument>> arguments(
        spec->parameterCount);
    if (pipelineInput.has_value()) {
        arguments[0].emplace(CheckedArgument{
            pipelineInput->expressionId,
            pipelineInput->value,
            pipelineInput->span,
            true});
    }

    bool sawNamedArgument = false;
    std::size_t nextPositional = pipelineInput.has_value() ? 1U : 0U;
    for (const CallArgument& argument : call.arguments) {
        const TypedValue value = checkExpression(*argument.value);
        std::optional<std::size_t> parameterIndex;

        if (argument.name.has_value()) {
            sawNamedArgument = true;
            parameterIndex = findParameter(*spec, *argument.name);
            if (!parameterIndex.has_value()) {
                report(
                    "SEM011",
                    "Function '" + call.callee
                        + "' has no parameter named '" + *argument.name
                        + "'.",
                    argument.nameSpan.value_or(argument.span));
                continue;
            }
        } else {
            if (sawNamedArgument) {
                report(
                    "SEM012",
                    "A positional argument cannot follow a named argument.",
                    argument.span);
            }
            while (nextPositional < arguments.size()
                   && arguments[nextPositional].has_value()) {
                ++nextPositional;
            }
            if (nextPositional < arguments.size()) {
                parameterIndex = nextPositional;
                ++nextPositional;
            }
        }

        if (!parameterIndex.has_value()) {
            report(
                "SEM013",
                "Function '" + call.callee + "' received too many arguments.",
                argument.span);
            continue;
        }
        if (arguments[*parameterIndex].has_value()) {
            report(
                "SEM014",
                "Parameter '"
                    + std::string(spec->parameters[*parameterIndex].name)
                    + "' is specified more than once.",
                argument.nameSpan.value_or(argument.span));
            continue;
        }
        arguments[*parameterIndex].emplace(CheckedArgument{
            argument.value->id,
            value,
            argument.span,
            false});
    }

    BoundCall boundCall;
    boundCall.builtin = spec->id;
    boundCall.resultType = spec->resultType;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const ParameterSpec& parameter = spec->parameters[index];
        if (!arguments[index].has_value()) {
            if (parameter.required) {
                report(
                    "SEM015",
                    "Function '" + call.callee
                        + "' requires parameter '"
                        + std::string(parameter.name) + "'.",
                    call.calleeSpan);
            }
            continue;
        }

        const CheckedArgument& checked = *arguments[index];
        if (checked.value.type != ScriptType::Invalid
            && !acceptsParameterType(*spec, index, checked.value.type)) {
            report(
                "SEM016",
                "Parameter '" + std::string(parameter.name) + "' of '"
                    + call.callee + "' expects "
                    + expectedParameterType(*spec, index)
                    + ", but received "
                    + scriptTypeName(checked.value.type) + ".",
                checked.span);
        }
        boundCall.arguments.push_back(BoundArgument{
            std::string(parameter.name),
            checked.expressionId,
            checked.pipelineInput});
    }

    auto constantFor = [&arguments](std::size_t index)
        -> const LiteralValue* {
        if (index >= arguments.size() || !arguments[index].has_value()
            || !arguments[index]->value.constant.has_value()) {
            return nullptr;
        }
        return &*arguments[index]->value.constant;
    };
    auto spanFor = [&arguments, &call](std::size_t index) {
        return index < arguments.size() && arguments[index].has_value()
            ? arguments[index]->span
            : call.calleeSpan;
    };

    switch (spec->id) {
    case BuiltinId::Lowpass:
        if (const LiteralValue* cutoff = constantFor(1);
            cutoff != nullptr && isInvalidPositiveNumber(*cutoff)) {
            report(
                "SEM017",
                "The lowpass cutoff must be a finite positive number.",
                spanFor(1));
        }
        if (const LiteralValue* orderValue = constantFor(2);
            orderValue != nullptr) {
            const auto* order = std::get_if<double>(orderValue);
            if (order != nullptr
                && (!std::isfinite(*order) || *order < 3.0
                    || std::floor(*order) != *order
                    || std::fmod(*order, 2.0) == 0.0
                    || *order > static_cast<double>(
                        kMaximumLowpassOrder))) {
                report(
                    "SEM018",
                    "The lowpass order must be an odd integer from 3 through "
                        + std::to_string(kMaximumLowpassOrder) + ".",
                    spanFor(2));
            }
        }
        break;
    case BuiltinId::Normalize:
        if (const LiteralValue* target = constantFor(1);
            target != nullptr && isInvalidPositiveNumber(*target)) {
            report(
                "SEM019",
                "The normalization target must be a finite positive number.",
                spanFor(1));
        }
        break;
    case BuiltinId::Fft:
        if (const LiteralValue* windowValue = constantFor(1);
            windowValue != nullptr) {
            const auto* window = std::get_if<std::string>(windowValue);
            if (window != nullptr && *window != "rectangular"
                && *window != "hann" && *window != "hamming") {
                report(
                    "SEM020",
                    "The FFT window must be 'rectangular', 'hann', or "
                    "'hamming'.",
                    spanFor(1));
            }
        }
        break;
    case BuiltinId::Features:
        if (const LiteralValue* fundamental = constantFor(1);
            fundamental != nullptr
            && isInvalidPositiveNumber(*fundamental)) {
            report(
                "SEM021",
                "The feature fundamental must be a finite positive number.",
                spanFor(1));
        }
        if (arguments[0].has_value()
            && arguments[0]->value.type == ScriptType::Signal
            && arguments[1].has_value()
            && arguments[1]->value.type == ScriptType::Number) {
            report(
                "SEM022",
                "The feature fundamental is only valid when the input is "
                "a Spectrum.",
                spanFor(1));
        }
        break;
    }

    result_.model.calls.insert_or_assign(expression.id, std::move(boundCall));
    recordType(expression.id, spec->resultType);
    return TypedValue{spec->resultType, std::nullopt};
}

void SemanticChecker::report(
    std::string code,
    std::string message,
    SourceSpan span)
{
    result_.diagnostics.push_back(Diagnostic{
        DiagnosticStage::Semantic,
        DiagnosticSeverity::Error,
        std::move(code),
        std::move(message),
        span});
}

void SemanticChecker::recordType(NodeId id, ScriptType type)
{
    result_.model.expressionTypes.insert_or_assign(id, type);
}

}  // namespace signallab::parser
