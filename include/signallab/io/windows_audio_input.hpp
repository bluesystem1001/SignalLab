#pragma once

#include <memory>

#include "signallab/repository/audio_input.hpp"

namespace signallab::io {

class WindowsAudioInput final : public repository::AudioInput {
public:
    WindowsAudioInput();
    ~WindowsAudioInput() override;

    [[nodiscard]] core::Result<std::vector<repository::AudioInputDevice>> devices() const override;

    [[nodiscard]] core::Result<bool> start(
        const std::string& deviceId,
        repository::AudioInputFormat format,
        std::function<void(std::span<const double>)> onSamples,
        std::function<void(std::string)> onError) override;

    void stop() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
