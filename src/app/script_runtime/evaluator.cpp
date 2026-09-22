#include "signallab/app/script_runtime/evaluator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "signallab/algorithm/features.hpp"
#include "signallab/algorithm/fir_filter.hpp"
#include "signallab/algorithm/preprocessing.hpp"
#include "signallab/algorithm/quality_metrics.hpp"
#include "signallab/algorithm/window.hpp"
#include "signallab/core/error.hpp"
#include "signallab/parser/parser.hpp"
#include "signallab/repository/file_repository.hpp"

namespace signallab::app::script_runtime {
namespace {

RuntimeIssue makeIssue(
    std::string code,
    std::string message,
    parser::SourceSpan span)
{
    return RuntimeIssue{
        std::move(code),
        std::move(message),
        span};
}

RuntimeIssue makeIssue(
    std::string code,
    const core::Error& error,
    parser::SourceSpan span)
{
    return makeIssue(std::move(code), error.message, span);
}

std::string lowerAscii(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            if (character >= 'A' && character <= 'Z') {
                return static_cast<char>(character - 'A' + 'a');
            }
            return static_cast<char>(character);
        });
    return value;
}

const SignalValue* signalArgument(
    const std::map<std::string, ScriptValue>& arguments)
{
    auto found = arguments.find("signal");
    if (found == arguments.end()) {
        found = arguments.find("input");
    }
    if (found == arguments.end()) {
        return nullptr;
    }
    return std::get_if<SignalValue>(&found->second);
}

const double* numberArgument(
    const std::map<std::string, ScriptValue>& arguments,
    std::string_view name)
{
    const auto found = arguments.find(std::string(name));
    if (found == arguments.end()) {
        return nullptr;
    }
    return std::get_if<double>(&found->second);
}

const std::string* stringArgument(
    const std::map<std::string, ScriptValue>& arguments,
    std::string_view name)
{
    const auto found = arguments.find(std::string(name));
    if (found == arguments.end()) {
        return nullptr;
    }
    return std::get_if<std::string>(&found->second);
}

std::string jsonEscape(std::string_view value)
{
    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (character < 0x20U) {
                escaped += "\\u00";
                escaped += hexadecimal[(character >> 4U) & 0x0fU];
                escaped += hexadecimal[character & 0x0fU];
            } else {
                escaped.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return escaped;
}

void configureNumericStream(std::ostringstream& stream)
{
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17);
}

core::Result<std::string> cancelledSerialization()
{
    return core::Result<std::string>::failure(
        core::ErrorCode::Cancelled,
        "SignalScript output serialization was cancelled.");
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

std::string squaredUnit(std::string_view unit)
{
    return unit.empty() ? std::string{} : std::string(unit) + "^2";
}

core::Result<std::string> serializeFeatures(
    const FeatureSetValue& features,
    std::stop_token stopToken)
{
    std::ostringstream stream;
    configureNumericStream(stream);
    stream << "{\n  \"features\": [";
    for (std::size_t index = 0; index < features.values.size(); ++index) {
        if ((index % 1'024U) == 0U && stopToken.stop_requested()) {
            return cancelledSerialization();
        }
        const auto& feature = features.values[index];
        stream << (index == 0U ? "\n" : ",\n")
               << "    {\"name\": \"" << jsonEscape(feature.name)
               << "\", \"value\": ";
        if (std::isfinite(feature.value)) {
            stream << feature.value;
        } else {
            // JSON has no NaN or infinity literal. Preserve a valid export;
            // the in-memory result still retains the computed value.
            stream << "null";
        }
        stream << ", \"unit\": \"" << jsonEscape(feature.unit)
               << "\"}";
    }
    if (!features.values.empty()) {
        stream << '\n';
    }
    stream << "  ]\n}\n";
    if (stopToken.stop_requested()) {
        return cancelledSerialization();
    }
    return core::Result<std::string>::success(stream.str());
}

core::Result<std::string> serializeSignal(
    const core::Signal& signal,
    std::stop_token stopToken)
{
    std::ostringstream stream;
    configureNumericStream(stream);
    stream << "time_seconds,value\n";
    const auto samples = signal.samples();
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if ((index % 1'024U) == 0U && stopToken.stop_requested()) {
            return cancelledSerialization();
        }
        stream << static_cast<double>(index) / signal.sampleRate()
               << ',' << samples[index] << '\n';
    }
    if (stopToken.stop_requested()) {
        return cancelledSerialization();
    }
    return core::Result<std::string>::success(stream.str());
}

core::Result<std::string> serializeSpectrum(
    const algorithm::Spectrum& spectrum,
    std::stop_token stopToken)
{
    std::ostringstream stream;
    configureNumericStream(stream);
    stream << "frequency_hz,amplitude,power\n";
    for (std::size_t index = 0;
         index < spectrum.frequenciesHz.size();
         ++index) {
        if ((index % 1'024U) == 0U && stopToken.stop_requested()) {
            return cancelledSerialization();
        }
        stream << spectrum.frequenciesHz[index]
               << ',' << spectrum.amplitudes[index]
               << ',' << spectrum.powers[index] << '\n';
    }
    if (stopToken.stop_requested()) {
        return cancelledSerialization();
    }
    return core::Result<std::string>::success(stream.str());
}

}  // namespace

Evaluator::Evaluator(
    ImportService& importService,
    repository::ScriptFileGateway& fileGateway)
    : importService_(importService),
      fileGateway_(fileGateway)
{
}

EvaluationResult Evaluator::evaluate(
    const parser::Program& program,
    const parser::SemanticModel& semanticModel,
    std::stop_token stopToken)
{
    semanticModel_ = &semanticModel;
    stopToken_ = stopToken;
    output_ = {};
    loadedSignals_.clear();
    expressionDepth_ = 0;
    cancelled_ = false;

    for (const auto& statement : program.statements) {
        if (stopToken_.stop_requested()) {
            cancelled_ = true;
            return EvaluationResult{
                EvaluationStatus::Cancelled,
                std::move(output_),
                makeIssue(
                    "RUN001",
                    "SignalScript execution was cancelled.",
                    statement.span)};
        }

        auto issue = evaluateStatement(statement);
        if (issue.has_value()) {
            return EvaluationResult{
                cancelled_ ? EvaluationStatus::Cancelled
                           : EvaluationStatus::Failed,
                std::move(output_),
                std::move(issue)};
        }
    }

    return EvaluationResult{
        EvaluationStatus::Succeeded,
        std::move(output_),
        std::nullopt};
}

Evaluator::ValueResult Evaluator::evaluateExpression(
    const parser::Expression& expression,
    std::optional<ScriptValue> pipelineInput)
{
    if (stopToken_.stop_requested()) {
        cancelled_ = true;
        return ValueResult{
            std::nullopt,
            makeIssue(
                "RUN001",
                "SignalScript execution was cancelled.",
                expression.span),
            true};
    }

    if (expressionDepth_ >= parser::kMaximumExpressionNestingDepth) {
        return ValueResult{
            std::nullopt,
            makeIssue(
                "RUN028",
                "Expression nesting exceeds the supported depth.",
                expression.span),
            false};
    }
    const ExpressionDepthGuard depthGuard(expressionDepth_);

    if (const auto* literal = std::get_if<parser::LiteralExpression>(
            &expression.node)) {
        return std::visit(
            [](const auto& value) {
                return ValueResult{ScriptValue{value}, std::nullopt, false};
            },
            literal->value);
    }

    if (const auto* variable = std::get_if<parser::VariableExpression>(
            &expression.node)) {
        const auto found = output_.variables.find(variable->name);
        if (found == output_.variables.end()) {
            return ValueResult{
                std::nullopt,
                makeIssue(
                    "RUN002",
                    "Unknown SignalScript variable '" + variable->name
                        + "'.",
                    variable->nameSpan),
                false};
        }
        return ValueResult{found->second, std::nullopt, false};
    }

    if (const auto* call = std::get_if<parser::CallExpression>(
            &expression.node)) {
        return evaluateCall(expression, *call, std::move(pipelineInput));
    }

    if (const auto* pipeline = std::get_if<parser::PipelineExpression>(
            &expression.node)) {
        auto input = evaluateExpression(*pipeline->input);
        if (!input.value.has_value()) {
            return input;
        }
        return evaluateExpression(
            *pipeline->stage,
            std::move(*input.value));
    }

    if (const auto* grouping = std::get_if<parser::GroupingExpression>(
            &expression.node)) {
        return evaluateExpression(*grouping->expression);
    }

    return ValueResult{
        std::nullopt,
        makeIssue(
            "RUN003",
            "The expression node is not supported by the evaluator.",
            expression.span),
        false};
}

Evaluator::ValueResult Evaluator::evaluateCall(
    const parser::Expression& expression,
    const parser::CallExpression& call,
    std::optional<ScriptValue> pipelineInput)
{
    const auto binding = semanticModel_->calls.find(expression.id);
    if (binding == semanticModel_->calls.end()) {
        return ValueResult{
            std::nullopt,
            makeIssue(
                "RUN004",
                "The checked call has no semantic binding.",
                expression.span),
            false};
    }

    std::map<std::string, ScriptValue> arguments;
    for (const auto& boundArgument : binding->second.arguments) {
        if (boundArgument.pipelineInput) {
            if (!pipelineInput.has_value()) {
                return ValueResult{
                    std::nullopt,
                    makeIssue(
                        "RUN005",
                        "The call requires a pipeline input.",
                        expression.span),
                    false};
            }
            arguments.emplace(
                boundArgument.parameterName,
                *pipelineInput);
            continue;
        }

        const auto sourceArgument = std::find_if(
            call.arguments.begin(),
            call.arguments.end(),
            [&boundArgument](const parser::CallArgument& candidate) {
                return candidate.value
                    && candidate.value->id == boundArgument.expressionId;
            });
        if (sourceArgument == call.arguments.end()) {
            return ValueResult{
                std::nullopt,
                makeIssue(
                    "RUN006",
                    "A checked call argument could not be resolved.",
                    expression.span),
                false};
        }

        auto value = evaluateExpression(*sourceArgument->value);
        if (!value.value.has_value()) {
            return value;
        }
        arguments.emplace(
            boundArgument.parameterName,
            std::move(*value.value));
    }

    switch (binding->second.builtin) {
    case parser::BuiltinId::Lowpass:
        return callLowpass(arguments, expression.span);
    case parser::BuiltinId::Normalize:
        return callNormalize(arguments, expression.span);
    case parser::BuiltinId::Fft:
        return callFft(arguments, expression.span);
    case parser::BuiltinId::Features:
        return callFeatures(arguments, expression.span);
    }

    return ValueResult{
        std::nullopt,
        makeIssue(
            "RUN007",
            "The selected built-in is not implemented.",
            expression.span),
        false};
}

std::optional<RuntimeIssue> Evaluator::evaluateStatement(
    const parser::Statement& statement)
{
    if (const auto* load = std::get_if<parser::LoadStatement>(
            &statement.node)) {
        auto loaded = loadSignal(*load, statement.span);
        if (!loaded.value.has_value()) {
            cancelled_ = loaded.cancelled;
            return std::move(loaded.issue);
        }
        if (load->alias.has_value()) {
            output_.variables.insert_or_assign(
                *load->alias,
                std::move(*loaded.value));
        }
        return std::nullopt;
    }

    if (const auto* assignment = std::get_if<parser::AssignmentStatement>(
            &statement.node)) {
        auto value = evaluateExpression(*assignment->value);
        if (!value.value.has_value()) {
            cancelled_ = value.cancelled;
            return std::move(value.issue);
        }
        output_.variables.insert_or_assign(
            assignment->name,
            std::move(*value.value));
        return std::nullopt;
    }

    if (const auto* expression = std::get_if<parser::ExpressionStatement>(
            &statement.node)) {
        auto value = evaluateExpression(*expression->value);
        if (!value.value.has_value()) {
            cancelled_ = value.cancelled;
            return std::move(value.issue);
        }
        return std::nullopt;
    }

    if (const auto* plot = std::get_if<parser::PlotStatement>(
            &statement.node)) {
        auto value = evaluateExpression(*plot->value);
        if (!value.value.has_value()) {
            cancelled_ = value.cancelled;
            return std::move(value.issue);
        }
        if (const auto* signal = std::get_if<SignalValue>(&*value.value)) {
            output_.plots.push_back(PlotRequest{
                *signal,
                plot->title.value_or("Signal")});
            return std::nullopt;
        }
        if (const auto* spectrum = std::get_if<SpectrumValuePtr>(
                &*value.value)) {
            output_.plots.push_back(PlotRequest{
                *spectrum,
                plot->title.value_or("Spectrum")});
            return std::nullopt;
        }
        return makeIssue(
            "RUN008",
            "plot requires a Signal or Spectrum value.",
            statement.span);
    }

    if (const auto* save = std::get_if<parser::SaveStatement>(
            &statement.node)) {
        auto value = evaluateExpression(*save->value);
        if (!value.value.has_value()) {
            cancelled_ = value.cancelled;
            return std::move(value.issue);
        }
        return saveValue(*value.value, save->path, statement.span);
    }

    return makeIssue(
        "RUN009",
        "The statement node is not supported by the evaluator.",
        statement.span);
}

Evaluator::ValueResult Evaluator::loadSignal(
    const parser::LoadStatement& statement,
    parser::SourceSpan statementSpan)
{
    auto pathResult = fileGateway_.resolveInput(statement.path);
    if (!pathResult) {
        return ValueResult{
            std::nullopt,
            makeIssue("RUN010", pathResult.error(), statement.pathSpan),
            false};
    }
    const std::filesystem::path resolvedPath =
        std::move(pathResult).value();

    const auto cached = loadedSignals_.find(resolvedPath);
    if (cached != loadedSignals_.end()) {
        return ValueResult{cached->second, std::nullopt, false};
    }

    const std::string extension = lowerAscii(
        resolvedPath.extension().string());
    core::Result<SignalEntry> entryResult = [&]() {
        if (extension == ".wav") {
            repository::WavImportOptions options;
            options.metadata.name = resolvedPath.filename().string();
            options.metadata.source = statement.path;
            return importService_.openWav(
                resolvedPath,
                options,
                stopToken_);
        }
        if (extension == ".csv" || extension == ".tsv") {
            repository::CsvImportOptions options;
            if (extension == ".tsv") {
                options.delimiter = repository::CsvDelimiter::Tab;
            }
            options.metadata.name = resolvedPath.filename().string();
            options.metadata.source = statement.path;
            return importService_.openCsv(
                resolvedPath,
                options,
                stopToken_);
        }
        return core::Result<SignalEntry>::failure(
            core::ErrorCode::InvalidParameter,
            "SignalScript load supports only .csv, .tsv, and .wav files.");
    }();

    if (!entryResult) {
        const bool wasCancelled =
            entryResult.error().code == core::ErrorCode::Cancelled;
        cancelled_ = wasCancelled;
        return ValueResult{
            std::nullopt,
            makeIssue(
                wasCancelled ? "RUN001" : "RUN011",
                entryResult.error(),
                statementSpan),
            wasCancelled};
    }

    SignalEntry entry = std::move(entryResult).value();
    auto signal = std::make_shared<const core::Signal>(
        std::move(entry.signal));
    loadedSignals_.emplace(resolvedPath, signal);
    return ValueResult{std::move(signal), std::nullopt, false};
}

Evaluator::ValueResult Evaluator::callLowpass(
    const std::map<std::string, ScriptValue>& arguments,
    parser::SourceSpan span)
{
    const SignalValue* signal = signalArgument(arguments);
    const double* cutoff = numberArgument(arguments, "cutoff");
    const double* order = numberArgument(arguments, "order");
    if (order == nullptr) {
        order = numberArgument(arguments, "taps");
    }
    const double orderValue = order == nullptr ? 63.0 : *order;
    if (signal == nullptr || !*signal || cutoff == nullptr
        || !std::isfinite(orderValue) || orderValue < 3.0
        || std::trunc(orderValue) != orderValue
        || std::fmod(orderValue, 2.0) == 0.0
        || orderValue > static_cast<double>(
            parser::kMaximumLowpassOrder)) {
        return ValueResult{
            std::nullopt,
            makeIssue(
                "RUN012",
                "lowpass requires a Signal, cutoff, and an odd integer "
                "order from 3 through "
                    + std::to_string(parser::kMaximumLowpassOrder) + ".",
                span),
            false};
    }

    auto coefficients = algorithm::designLowPassFir(
        static_cast<std::size_t>(orderValue),
        *cutoff,
        (*signal)->sampleRate());
    if (!coefficients) {
        return ValueResult{
            std::nullopt,
            makeIssue("RUN013", coefficients.error(), span),
            false};
    }

    auto filtered = algorithm::applyFir(
        **signal,
        coefficients.value(),
        algorithm::FirBoundaryStrategy::ZeroPadding,
        stopToken_);
    if (!filtered) {
        const bool wasCancelled =
            filtered.error().code == core::ErrorCode::Cancelled;
        cancelled_ = wasCancelled;
        return ValueResult{
            std::nullopt,
            makeIssue(
                wasCancelled ? "RUN001" : "RUN014",
                filtered.error(),
                span),
            wasCancelled};
    }
    return ValueResult{
        std::make_shared<const core::Signal>(
            std::move(filtered).value()),
        std::nullopt,
        false};
}

Evaluator::ValueResult Evaluator::callNormalize(
    const std::map<std::string, ScriptValue>& arguments,
    parser::SourceSpan span)
{
    const SignalValue* signal = signalArgument(arguments);
    const double* target = numberArgument(arguments, "target");
    const double targetValue = target == nullptr ? 1.0 : *target;
    if (signal == nullptr || !*signal) {
        return ValueResult{
            std::nullopt,
            makeIssue("RUN015", "normalize requires a Signal.", span),
            false};
    }
    auto normalized = algorithm::normalizePeak(
        **signal,
        targetValue,
        stopToken_);
    if (!normalized) {
        const bool wasCancelled =
            normalized.error().code == core::ErrorCode::Cancelled;
        cancelled_ = wasCancelled;
        return ValueResult{
            std::nullopt,
            makeIssue(
                wasCancelled ? "RUN001" : "RUN016",
                normalized.error(),
                span),
            wasCancelled};
    }
    return ValueResult{
        std::make_shared<const core::Signal>(
            std::move(normalized).value()),
        std::nullopt,
        false};
}

Evaluator::ValueResult Evaluator::callFft(
    const std::map<std::string, ScriptValue>& arguments,
    parser::SourceSpan span)
{
    const SignalValue* signal = signalArgument(arguments);
    if (signal == nullptr || !*signal) {
        return ValueResult{
            std::nullopt,
            makeIssue("RUN017", "fft requires a Signal.", span),
            false};
    }

    AnalysisOptions options;
    if (const std::string* window = stringArgument(arguments, "window")) {
        const std::string name = lowerAscii(*window);
        if (name == "hann" || name == "hanning") {
            options.windowType = algorithm::WindowType::Hann;
        } else if (name == "hamming") {
            options.windowType = algorithm::WindowType::Hamming;
        } else if (name == "rectangular" || name == "rect") {
            options.windowType = algorithm::WindowType::Rectangular;
        } else {
            return ValueResult{
                std::nullopt,
                makeIssue(
                    "RUN018",
                    "fft window must be 'hann', 'hamming', or "
                    "'rectangular'.",
                    span),
                false};
        }
    }

    auto analysis = analyzeSignal(**signal, options, stopToken_);
    if (!analysis) {
        const bool wasCancelled =
            analysis.error().code == core::ErrorCode::Cancelled;
        cancelled_ = wasCancelled;
        return ValueResult{
            std::nullopt,
            makeIssue(
                wasCancelled ? "RUN001" : "RUN019",
                analysis.error(),
                span),
            wasCancelled};
    }

    auto value = std::make_shared<const SpectrumValue>(SpectrumValue{
        *signal,
        std::make_shared<const AnalysisResult>(
            std::move(analysis).value())});
    return ValueResult{std::move(value), std::nullopt, false};
}

Evaluator::ValueResult Evaluator::callFeatures(
    const std::map<std::string, ScriptValue>& arguments,
    parser::SourceSpan span)
{
    const auto input = arguments.find("input");
    const auto signalNamed = arguments.find("signal");
    const auto spectrumNamed = arguments.find("spectrum");
    const ScriptValue* source = input != arguments.end()
        ? &input->second
        : (signalNamed != arguments.end()
            ? &signalNamed->second
            : (spectrumNamed != arguments.end()
                ? &spectrumNamed->second
                : nullptr));
    if (source == nullptr) {
        return ValueResult{
            std::nullopt,
            makeIssue(
                "RUN020",
                "features requires a Signal or Spectrum.",
                span),
            false};
    }

    auto featureSet = std::make_shared<FeatureSetValue>();
    const double* fundamental = numberArgument(arguments, "fundamental");
    if (const auto* signal = std::get_if<SignalValue>(source)) {
        if (!*signal) {
            return ValueResult{
                std::nullopt,
                makeIssue("RUN020", "features received an empty Signal.", span),
                false};
        }
        if (fundamental != nullptr) {
            return ValueResult{
                std::nullopt,
                makeIssue(
                    "RUN026",
                    "The feature fundamental is only valid for a Spectrum.",
                    span),
                false};
        }
        auto basics = algorithm::calculateBasicFeatures(
            **signal,
            stopToken_);
        if (!basics) {
            const bool wasCancelled =
                basics.error().code == core::ErrorCode::Cancelled;
            cancelled_ = wasCancelled;
            return ValueResult{
                std::nullopt,
                makeIssue(
                    wasCancelled ? "RUN001" : "RUN021",
                    basics.error(),
                    span),
                wasCancelled};
        }
        const std::string& signalUnit = (*signal)->metadata().unit;
        featureSet->values = {
            {"mean", basics.value().mean, signalUnit},
            {"variance", basics.value().variance, squaredUnit(signalUnit)},
            {"rms", basics.value().rms, signalUnit},
            {"peak_to_peak", basics.value().peakToPeak, signalUnit}};
    } else if (const auto* spectrum = std::get_if<SpectrumValuePtr>(source)) {
        if (!*spectrum || !(*spectrum)->analysis || !(*spectrum)->source) {
            return ValueResult{
                std::nullopt,
                makeIssue(
                    "RUN020",
                    "features received an empty Spectrum.",
                    span),
                false};
        }
        const auto& analysis = *(*spectrum)->analysis;
        const std::string& signalUnit = (*spectrum)->source->metadata().unit;
        featureSet->values = {
            {"mean", analysis.features.mean, signalUnit},
            {"variance", analysis.features.variance,
             squaredUnit(signalUnit)},
            {"rms", analysis.features.rms, signalUnit},
            {"peak_to_peak", analysis.features.peakToPeak, signalUnit}};
        if (analysis.dominantPeak.has_value()) {
            featureSet->values.push_back({
                "dominant_frequency",
                analysis.dominantPeak->frequencyHz,
                "Hz"});
            featureSet->values.push_back({
                "amplitude",
                analysis.dominantPeak->amplitude,
                signalUnit});
        }

        std::optional<double> snr = analysis.snrDb;
        std::optional<double> thd = analysis.thdPercent;
        if (fundamental != nullptr) {
            auto snrResult = algorithm::calculateSnrDb(
                analysis.spectrum,
                *fundamental,
                1U,
                stopToken_);
            if (!snrResult) {
                const bool wasCancelled =
                    snrResult.error().code == core::ErrorCode::Cancelled;
                cancelled_ = wasCancelled;
                return ValueResult{
                    std::nullopt,
                    makeIssue(
                        wasCancelled ? "RUN001" : "RUN022",
                        snrResult.error(),
                        span),
                    wasCancelled};
            }
            auto thdResult = algorithm::calculateThdPercent(
                analysis.spectrum,
                *fundamental,
                5U,
                1U,
                stopToken_);
            if (!thdResult) {
                const bool wasCancelled =
                    thdResult.error().code == core::ErrorCode::Cancelled;
                cancelled_ = wasCancelled;
                return ValueResult{
                    std::nullopt,
                    makeIssue(
                        wasCancelled ? "RUN001" : "RUN023",
                        thdResult.error(),
                        span),
                    wasCancelled};
            }
            snr = snrResult.value();
            thd = thdResult.value();
        }
        if (snr.has_value()) {
            featureSet->values.push_back({"snr", *snr, "dB"});
        }
        if (thd.has_value()) {
            featureSet->values.push_back({"thd", *thd, "%"});
        }
        featureSet->values.push_back({
            "fft_size",
            static_cast<double>(analysis.spectrum.fftSize),
            "samples"});
    } else {
        return ValueResult{
            std::nullopt,
            makeIssue(
                "RUN020",
                "features requires a Signal or Spectrum.",
                span),
            false};
    }

    FeatureSetValuePtr immutable = featureSet;
    return ValueResult{std::move(immutable), std::nullopt, false};
}

std::optional<RuntimeIssue> Evaluator::saveValue(
    const ScriptValue& value,
    const std::string& path,
    parser::SourceSpan span)
{
    if (stopToken_.stop_requested()) {
        cancelled_ = true;
        return makeIssue(
            "RUN001",
            "SignalScript execution was cancelled.",
            span);
    }

    core::Result<std::string> serialized =
        core::Result<std::string>::failure(
            core::ErrorCode::InvalidParameter,
            "Unsupported SignalScript output value.");
    if (const auto* signal = std::get_if<SignalValue>(&value);
        signal != nullptr && *signal) {
        serialized = serializeSignal(**signal, stopToken_);
    } else if (const auto* spectrum = std::get_if<SpectrumValuePtr>(&value);
               spectrum != nullptr && *spectrum && (*spectrum)->analysis) {
        serialized = serializeSpectrum(
            (*spectrum)->analysis->spectrum,
            stopToken_);
    } else if (const auto* features = std::get_if<FeatureSetValuePtr>(&value);
               features != nullptr && *features) {
        serialized = serializeFeatures(**features, stopToken_);
    } else {
        return makeIssue(
            "RUN024",
            "save requires a Signal, Spectrum, or FeatureSet value.",
            span);
    }

    if (!serialized) {
        const bool wasCancelled =
            serialized.error().code == core::ErrorCode::Cancelled;
        cancelled_ = wasCancelled;
        return makeIssue(
            wasCancelled ? "RUN001" : "RUN024",
            serialized.error(),
            span);
    }

    if (stopToken_.stop_requested()) {
        cancelled_ = true;
        return makeIssue(
            "RUN001",
            "SignalScript execution was cancelled.",
            span);
    }

    auto writeResult = fileGateway_.writeOutput(
        path,
        serialized.value(),
        stopToken_);
    if (!writeResult) {
        const bool wasCancelled =
            writeResult.error().code == core::ErrorCode::Cancelled;
        cancelled_ = wasCancelled;
        return makeIssue(
            wasCancelled ? "RUN001" : "RUN025",
            writeResult.error(),
            span);
    }
    output_.outputs.push_back(std::move(writeResult).value());
    return std::nullopt;
}

}  // namespace signallab::app::script_runtime
