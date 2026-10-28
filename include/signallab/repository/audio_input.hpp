#pragma once

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "signallab/core/result.hpp"

namespace signallab::repository {

struct AudioInputDevice {
    std::string id;
    std::string name;
};

struct AudioInputFormat {
    unsigned int sampleRate{48000};
};

class AudioInput {
public:
    virtual ~AudioInput() = default;

    [[nodiscard]] virtual core::Result<std::vector<AudioInputDevice>> devices() const = 0;

    [[nodiscard]] virtual core::Result<bool> start(
        const std::string& deviceId,
        AudioInputFormat format,
        std::function<void(std::span<const double>)> onSamples,
        std::function<void(std::string)> onError) = 0;

    virtual void stop() = 0;
};

}
