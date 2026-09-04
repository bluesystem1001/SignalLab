//
// Created by ASUS on 2026/9/2.
//

#ifndef SIGNALLAB_SIGNAL_H
#define SIGNALLAB_SIGNAL_H
struct SignalMetadata;
class Signal;
struct SignalMetadata {
    std::string name;
    std::string unit;
    std::string source;
};
class Signal {
public:
    static Result<Signal> create(
        std::vector<double> samples,
        double sampleRate,
        SignalMetadata metadata);

    std::span<const double> samples() const noexcept;
    double sampleRate() const noexcept;
    std::size_t sampleCount() const noexcept;
    double durationSeconds() const noexcept;
    const SignalMetadata& metadata() const noexcept;
};
#endif //SIGNALLAB_SIGNAL_H
