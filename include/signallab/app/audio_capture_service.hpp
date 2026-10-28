#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "signallab/algorithm/streaming_fir_filter.hpp"
#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"
#include "signallab/repository/audio_input.hpp"

namespace signallab::app {

enum class AudioFilterType { None, LowPass, HighPass };

struct AudioCaptureOptions {
    std::string deviceId;
    unsigned int sampleRate{48000};
    AudioFilterType filterType{AudioFilterType::LowPass};
    double cutoffHz{3000.0};
    std::size_t tapCount{63};
    unsigned int maximumDurationSeconds{120};
};

struct AudioPreviewPoint {
    double timeSeconds;
    double raw;
    double filtered;
};

struct AudioCaptureSnapshot {
    bool recording{false};
    bool limitReached{false};
    std::size_t sampleCount{0};
    unsigned int sampleRate{48000};
    double peakLevel{0.0};
    std::string error;
    std::vector<AudioPreviewPoint> preview;
};

class AudioCaptureService {
public:
    explicit AudioCaptureService(std::unique_ptr<repository::AudioInput> input);
    ~AudioCaptureService();

    AudioCaptureService(const AudioCaptureService&) = delete;
    AudioCaptureService& operator=(const AudioCaptureService&) = delete;

    [[nodiscard]] core::Result<std::vector<repository::AudioInputDevice>>
        devices() const;
    [[nodiscard]] core::Result<bool> start(const AudioCaptureOptions& options);
    void stop();
    [[nodiscard]] AudioCaptureSnapshot snapshot(
        std::size_t maximumPoints = 2000) const;
    [[nodiscard]] core::Result<core::Signal> recordedSignal(bool filtered) const;

private:
    void receiveSamples(std::span<const double> samples);
    void receiveError(std::string message);

    std::unique_ptr<repository::AudioInput> input_;
    mutable std::mutex mutex_;
    AudioCaptureOptions options_;
    std::optional<algorithm::StreamingFirFilter> filter_;
    std::vector<double> rawSamples_;
    std::vector<double> filteredSamples_;
    bool recording_{false};
    bool limitReached_{false};
    double peakLevel_{0.0};
    std::string error_;
};

}
