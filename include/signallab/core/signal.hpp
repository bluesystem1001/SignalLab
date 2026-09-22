#pragma once

#include <cstddef>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "signallab/core/result.hpp"

namespace signallab::core {

struct SignalMetadata {
    std::string name;
    std::string unit;
    std::string source;
};

class Signal {
public:
    [[nodiscard]] static Result<Signal> create(
        std::vector<double> samples,
        double sample_rate,
        SignalMetadata metadata,
        std::stop_token stopToken = {});

    [[nodiscard]] std::span<const double> samples() const noexcept;
    [[nodiscard]] double sampleRate() const noexcept;
    [[nodiscard]] std::size_t sampleCount() const noexcept;
    [[nodiscard]] double durationSeconds() const noexcept;
    [[nodiscard]] double sampleTimeSeconds(std::size_t index) const;
    [[nodiscard]] std::pair<double, double> sampleRange() const noexcept;
    [[nodiscard]] const SignalMetadata& metadata() const noexcept;

private:
    Signal(
        std::vector<double> samples,
        double sample_rate,
        SignalMetadata metadata);

    std::vector<double> samples_;
    double sample_rate_;
    SignalMetadata metadata_;
};

}  // namespace signallab::core
