#pragma once

#include <string>

namespace signallab::core {

enum class ErrorCode {
    InvalidSampleRate,
    EmptySamples,
    NonFiniteSample,
    InvalidFrequency,
    InvalidAmplitude,
    InvalidDuration,
    InvalidParameter
};

struct Error {
    ErrorCode code;
    std::string message;
};

}  // namespace signallab::core
