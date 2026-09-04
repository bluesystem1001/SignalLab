#pragma once

#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"

namespace signallab::core {

Result<Signal> generateSineWave(
    double frequencyHz,
    double amplitude,
    double sampleRate,
    double durationSeconds,
    SignalMetadata metadata);

Result<Signal> generateDualTone(
    double firstFrequencyHz,
    double firstAmplitude,
    double secondFrequencyHz,
    double secondAmplitude,
    double sampleRate,
    double durationSeconds,
    SignalMetadata metadata);

}  // namespace signallab::core
