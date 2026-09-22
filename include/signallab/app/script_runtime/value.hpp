#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "signallab/app/analysis_service.hpp"
#include "signallab/core/signal.hpp"

namespace signallab::app::script_runtime {

using SignalValue = std::shared_ptr<const core::Signal>;

struct SpectrumValue {
    SignalValue source;
    std::shared_ptr<const AnalysisResult> analysis;
};

using SpectrumValuePtr = std::shared_ptr<const SpectrumValue>;

struct FeatureValue {
    std::string name;
    double value{0.0};
    std::string unit;
};

struct FeatureSetValue {
    std::vector<FeatureValue> values;
};

using FeatureSetValuePtr = std::shared_ptr<const FeatureSetValue>;

using ScriptValue = std::variant<
    std::monostate,
    double,
    std::string,
    bool,
    SignalValue,
    SpectrumValuePtr,
    FeatureSetValuePtr>;

struct PlotRequest {
    std::variant<SignalValue, SpectrumValuePtr> data;
    std::string title;
};

struct EvaluationOutput {
    std::map<std::string, ScriptValue> variables;
    std::vector<PlotRequest> plots;
    std::vector<std::filesystem::path> outputs;
};

}  // namespace signallab::app::script_runtime
