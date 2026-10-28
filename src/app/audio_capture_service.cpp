#include "signallab/app/audio_capture_service.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

#include "signallab/algorithm/fir_filter.hpp"

namespace signallab::app {

AudioCaptureService::AudioCaptureService(
    std::unique_ptr<repository::AudioInput> input)
    : input_(std::move(input))
{
}

AudioCaptureService::~AudioCaptureService()
{
    stop();
}

core::Result<std::vector<repository::AudioInputDevice>>
AudioCaptureService::devices() const
{
    if (!input_) {
        return core::Result<std::vector<repository::AudioInputDevice>>::failure(
            core::ErrorCode::FileOpenFailed, "Audio input is unavailable.");
    }
    return input_->devices();
}

core::Result<bool> AudioCaptureService::start(const AudioCaptureOptions& options)
{
    {
        const std::lock_guard lock(mutex_);
        if (recording_) {
            return core::Result<bool>::failure(
                core::ErrorCode::InvalidParameter,
                "Stop the current recording before starting another one.");
        }
    }
    if (!input_) {
        return core::Result<bool>::failure(
            core::ErrorCode::FileOpenFailed, "Audio input is unavailable.");
    }
    if ((options.sampleRate != 16000 && options.sampleRate != 44100
         && options.sampleRate != 48000)
        || options.maximumDurationSeconds == 0
        || options.maximumDurationSeconds > 300) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "Choose 16000, 44100 or 48000 Hz and a duration from 1 to 300 seconds.");
    }
    std::optional<algorithm::StreamingFirFilter> newFilter;
    if (options.filterType != AudioFilterType::None) {
        if ((options.filterType != AudioFilterType::LowPass
             && options.filterType != AudioFilterType::HighPass)
            || options.tapCount > 255) {
            return core::Result<bool>::failure(
                core::ErrorCode::InvalidParameter,
                "Real-time FIR filtering supports low/high pass and at most 255 taps.");
        }
        auto coefficients = options.filterType == AudioFilterType::LowPass
            ? algorithm::designLowPassFir(
                options.tapCount, options.cutoffHz, options.sampleRate)
            : algorithm::designHighPassFir(
                options.tapCount, options.cutoffHz, options.sampleRate);
        if (!coefficients) {
            return core::Result<bool>::failure(coefficients.error());
        }
        auto filter = algorithm::StreamingFirFilter::create(
            std::move(coefficients).value());
        if (!filter) {
            return core::Result<bool>::failure(filter.error());
        }
        newFilter.emplace(std::move(filter).value());
    }

    std::vector<double> previousRaw;
    std::vector<double> previousFiltered;
    const auto maximumSamples = static_cast<std::size_t>(options.sampleRate)
        * options.maximumDurationSeconds;
    try {
        previousRaw.reserve(maximumSamples);
        previousFiltered.reserve(maximumSamples);
    } catch (const std::exception&) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidParameter,
            "Insufficient memory for this recording. Choose a shorter duration.");
    }

    input_->stop();
    AudioCaptureOptions previousOptions;
    bool previousLimitReached;
    double previousPeakLevel;
    std::string previousError;
    {
        const std::lock_guard lock(mutex_);
        previousOptions = options_;
        previousLimitReached = limitReached_;
        previousPeakLevel = peakLevel_;
        previousError = std::move(error_);
        rawSamples_.swap(previousRaw);
        filteredSamples_.swap(previousFiltered);
        options_ = options;
        filter_ = std::move(newFilter);
        recording_ = true;
        limitReached_ = false;
        peakLevel_ = 0.0;
        error_.clear();
    }
    auto started = input_->start(
        options.deviceId,
        repository::AudioInputFormat{options.sampleRate},
        [this](std::span<const double> samples) { receiveSamples(samples); },
        [this](std::string message) { receiveError(std::move(message)); });
    if (!started) {
        input_->stop();
        const std::lock_guard lock(mutex_);
        rawSamples_.swap(previousRaw);
        filteredSamples_.swap(previousFiltered);
        options_ = previousOptions;
        recording_ = false;
        limitReached_ = previousLimitReached;
        peakLevel_ = previousPeakLevel;
        error_ = std::move(previousError);
        filter_.reset();
        return started;
    }
    return core::Result<bool>::success(true);
}

void AudioCaptureService::stop()
{
    if (input_) {
        input_->stop();
    }
    const std::lock_guard lock(mutex_);
    recording_ = false;
}

void AudioCaptureService::receiveSamples(std::span<const double> samples)
{
    const std::lock_guard lock(mutex_);
    if (!recording_ || limitReached_ || !error_.empty()) {
        return;
    }
    if (std::any_of(samples.begin(), samples.end(), [](double value) {
            return !std::isfinite(value);
        })) {
        error_ = "The audio device supplied invalid samples. Recording stopped.";
        return;
    }
    const auto maximumSamples = static_cast<std::size_t>(options_.sampleRate)
        * options_.maximumDurationSeconds;
    const auto count = std::min(samples.size(), maximumSamples - rawSamples_.size());
    if (count > 0) {
        peakLevel_ = 0.0;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const double sample = samples[index];
        rawSamples_.push_back(sample);
        filteredSamples_.push_back(filter_ ? filter_->processSample(sample) : sample);
        peakLevel_ = std::max(peakLevel_, std::abs(sample));
    }
    limitReached_ = rawSamples_.size() == maximumSamples;
}

void AudioCaptureService::receiveError(std::string message)
{
    const std::lock_guard lock(mutex_);
    if (recording_ && error_.empty()) {
        error_ = message.empty() ? "The audio input stopped unexpectedly." : std::move(message);
    }
}

AudioCaptureSnapshot AudioCaptureService::snapshot(std::size_t maximumPoints) const
{
    const std::lock_guard lock(mutex_);
    AudioCaptureSnapshot result;
    result.recording = recording_;
    result.limitReached = limitReached_;
    result.sampleCount = rawSamples_.size();
    result.sampleRate = options_.sampleRate;
    result.peakLevel = peakLevel_;
    result.error = error_;
    maximumPoints = std::min(maximumPoints, std::size_t{4000});
    if (maximumPoints == 0 || rawSamples_.empty()) {
        return result;
    }
    const auto previewCount = std::min(
        rawSamples_.size(), static_cast<std::size_t>(options_.sampleRate) * 2);
    const auto startIndex = rawSamples_.size() - previewCount;
    const auto pointCount = std::min(previewCount, maximumPoints);
    result.preview.reserve(pointCount);
    const auto appendPoint = [&](std::size_t sampleIndex) {
        result.preview.push_back({
            static_cast<double>(sampleIndex) / options_.sampleRate,
            rawSamples_[sampleIndex], filteredSamples_[sampleIndex]});
    };
    if (pointCount == 1) {
        appendPoint(rawSamples_.size() - 1);
    } else if (previewCount <= maximumPoints) {
        for (std::size_t index = startIndex; index < rawSamples_.size(); ++index) {
            appendPoint(index);
        }
    } else {
        const auto bucketCount = pointCount / 2;
        for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
            const auto begin = startIndex + bucket * previewCount / bucketCount;
            const auto end = startIndex + (bucket + 1) * previewCount / bucketCount;
            auto minimumIndex = begin;
            auto maximumIndex = begin;
            for (auto index = begin + 1; index < end; ++index) {
                if (std::min(rawSamples_[index], filteredSamples_[index])
                    < std::min(rawSamples_[minimumIndex], filteredSamples_[minimumIndex])) {
                    minimumIndex = index;
                }
                if (std::max(rawSamples_[index], filteredSamples_[index])
                    > std::max(rawSamples_[maximumIndex], filteredSamples_[maximumIndex])) {
                    maximumIndex = index;
                }
            }
            if (minimumIndex == maximumIndex) {
                maximumIndex = end - 1;
            }
            appendPoint(std::min(minimumIndex, maximumIndex));
            if (minimumIndex != maximumIndex) {
                appendPoint(std::max(minimumIndex, maximumIndex));
            }
        }
    }
    return result;
}

core::Result<core::Signal> AudioCaptureService::recordedSignal(bool filtered) const
{
    const std::lock_guard lock(mutex_);
    if (recording_) {
        return core::Result<core::Signal>::failure(
            core::ErrorCode::InvalidParameter, "Stop recording before saving audio.");
    }
    if (rawSamples_.empty()) {
        return core::Result<core::Signal>::failure(
            core::ErrorCode::EmptySamples, "There are no recorded audio samples.");
    }
    const std::string filterName = options_.filterType == AudioFilterType::None
        ? "none" : options_.filterType == AudioFilterType::LowPass ? "lowpass" : "highpass";
    core::SignalMetadata metadata{
        filtered ? "Microphone (filtered)" : "Microphone (raw)",
        "FS",
        "microphone:" + options_.deviceId + ";filter=" + (filtered ? filterName : "none")
            + ";cutoff_hz=" + std::to_string(options_.cutoffHz)
            + ";taps=" + std::to_string(options_.tapCount)};
    try {
        return core::Signal::create(
            filtered ? filteredSamples_ : rawSamples_, options_.sampleRate, std::move(metadata));
    } catch (const std::exception&) {
        return core::Result<core::Signal>::failure(
            core::ErrorCode::FileWriteFailed,
            "Insufficient memory to save this recording. The captured audio is retained.");
    }
}

}
