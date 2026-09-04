//
// Created by ASUS on 2026/9/2.
//

#ifndef SIGNALLAB_ERROR_H
#define SIGNALLAB_ERROR_H
using namespace std ;
enum class ErrorCode;
struct Error ;
enum class ErrorCode {
    InvalidSampleRate,
    EmptySamples,
    NonFiniteSample,
    InvalidParameter
};
struct Error {
    ErrorCode code;
    std::string message;
};
#endif //SIGNALLAB_ERROR_H
