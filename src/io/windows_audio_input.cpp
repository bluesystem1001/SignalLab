#include "signallab/io/windows_audio_input.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace signallab::io {

#ifdef _WIN32
namespace {

constexpr std::size_t bufferCount = 8;

std::string toUtf8(const wchar_t* text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::string audioError(const std::string& operation, MMRESULT result)
{
    std::array<wchar_t, MAXERRORLENGTH> message{};
    if (waveInGetErrorTextW(result, message.data(), MAXERRORLENGTH) == MMSYSERR_NOERROR) {
        return operation + ": " + toUtf8(message.data());
    }
    return operation + " (Windows audio error " + std::to_string(result) + ")";
}

struct AudioBuffer {
    WAVEHDR header{};
    std::vector<std::int16_t> samples;
    bool prepared{false};
};

struct CaptureSession {
    HWAVEIN input{nullptr};
    HANDLE dataEvent{nullptr};
    HANDLE stopEvent{nullptr};
    std::array<AudioBuffer, bufferCount> buffers;
    std::vector<double> normalized;
    std::function<void(std::span<const double>)> onSamples;
    std::function<void(std::string)> onError;
    std::size_t nextBuffer{0};

    bool release(bool releaseEvents = true)
    {
        if (input) {
            waveInReset(input);
            for (auto& buffer : buffers) {
                if (buffer.prepared) {
                    const MMRESULT result = waveInUnprepareHeader(input, &buffer.header, sizeof(WAVEHDR));
                    if (result == MMSYSERR_NOERROR) {
                        buffer.prepared = false;
                    }
                }
            }
            if (waveInClose(input) != MMSYSERR_NOERROR) {
                return false;
            }
            input = nullptr;
        }
        if (releaseEvents && dataEvent) {
            CloseHandle(dataEvent);
            dataEvent = nullptr;
        }
        if (releaseEvents && stopEvent) {
            CloseHandle(stopEvent);
            stopEvent = nullptr;
        }
        return true;
    }

    void reportError(std::string message) noexcept
    {
        try {
            if (onError) {
                onError(std::move(message));
            }
        } catch (...) {
        }
    }

    bool deliver(AudioBuffer& buffer, bool requeue)
    {
        const DWORD byteCount = buffer.header.dwBytesRecorded;
        if (byteCount > buffer.header.dwBufferLength || byteCount % sizeof(std::int16_t) != 0) {
            reportError("The microphone returned an invalid audio buffer; recording was stopped.");
            return false;
        }
        const auto sampleCount = static_cast<std::size_t>(byteCount) / sizeof(std::int16_t);
        for (std::size_t index = 0; index < sampleCount; ++index) {
            normalized[index] = static_cast<double>(buffer.samples[index]) / 32768.0;
        }
        if (requeue) {
            buffer.header.dwBytesRecorded = 0;
            const MMRESULT result = waveInAddBuffer(input, &buffer.header, sizeof(WAVEHDR));
            if (result != MMSYSERR_NOERROR) {
                reportError(audioError("Cannot queue microphone audio", result));
                return false;
            }
        }
        if (sampleCount != 0) {
            onSamples(std::span<const double>(normalized.data(), sampleCount));
        }
        return true;
    }

    void run() noexcept
    {
        bool healthy = true;
        try {
            const std::array<HANDLE, 2> events{stopEvent, dataEvent};
            auto lastSamples = std::chrono::steady_clock::now();
            while (healthy) {
                const DWORD waitResult = WaitForMultipleObjects(
                    static_cast<DWORD>(events.size()), events.data(), FALSE, 1000);
                if (waitResult == WAIT_OBJECT_0) {
                    break;
                }
                if (waitResult == WAIT_FAILED) {
                    reportError("Cannot wait for microphone audio (Windows error "
                        + std::to_string(GetLastError()) + ").");
                    healthy = false;
                    break;
                }
                for (std::size_t processed = 0; processed < bufferCount; ++processed) {
                    auto& buffer = buffers[nextBuffer];
                    if ((buffer.header.dwFlags & WHDR_DONE) == 0
                        || WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                        break;
                    }
                    std::size_t completed = 0;
                    for (const auto& queuedBuffer : buffers) {
                        if ((queuedBuffer.header.dwFlags & WHDR_DONE) != 0) {
                            ++completed;
                        }
                    }
                    if (completed == bufferCount) {
                        reportError("Microphone audio buffers overflowed; recording was stopped to avoid gaps.");
                        healthy = false;
                        break;
                    }
                    const bool containsSamples = buffer.header.dwBytesRecorded != 0;
                    healthy = deliver(buffer, true);
                    nextBuffer = (nextBuffer + 1) % bufferCount;
                    if (containsSamples) {
                        lastSamples = std::chrono::steady_clock::now();
                    }
                    if (!healthy) {
                        break;
                    }
                }
                if (std::chrono::steady_clock::now() - lastSamples > std::chrono::seconds(3)) {
                    reportError("No microphone audio arrived for 3 seconds; check the device connection and microphone permissions.");
                    healthy = false;
                }
            }
            const MMRESULT resetResult = waveInReset(input);
            if (resetResult != MMSYSERR_NOERROR) {
                reportError(audioError("Cannot stop microphone audio", resetResult));
                healthy = false;
            }
            if (healthy) {
                for (std::size_t processed = 0; processed < bufferCount; ++processed) {
                    auto& buffer = buffers[nextBuffer];
                    nextBuffer = (nextBuffer + 1) % bufferCount;
                    if ((buffer.header.dwFlags & WHDR_DONE) != 0 && !deliver(buffer, false)) {
                        break;
                    }
                }
            }
        } catch (const std::exception& error) {
            reportError(std::string("Microphone processing failed: ") + error.what());
        } catch (...) {
            reportError("Microphone processing failed with an unexpected error.");
        }
        if (!release(false)) {
            reportError("The microphone driver could not release the recording device.");
        }
    }
};

void disposeSession(std::unique_ptr<CaptureSession>& session)
{
    if (!session) {
        return;
    }
    if (!session->release()) {
        session->onSamples = {};
        session->onError = {};
        static_cast<void>(session.release());
    } else {
        session.reset();
    }
}

}
#endif

struct WindowsAudioInput::Impl {
#ifdef _WIN32
    std::mutex mutex;
    std::unique_ptr<CaptureSession> session;
    std::thread worker;
#endif
};

WindowsAudioInput::WindowsAudioInput()
    : impl_(std::make_unique<Impl>())
{
}

WindowsAudioInput::~WindowsAudioInput()
{
    stop();
}

core::Result<std::vector<repository::AudioInputDevice>> WindowsAudioInput::devices() const
{
#ifdef _WIN32
    std::vector<repository::AudioInputDevice> devices;
    const UINT deviceCount = waveInGetNumDevs();
    if (deviceCount != 0) {
        devices.push_back({"", "System default microphone"});
    }
    for (UINT deviceId = 0; deviceId < deviceCount; ++deviceId) {
        WAVEINCAPSW capabilities{};
        const MMRESULT result = waveInGetDevCapsW(deviceId, &capabilities, sizeof(capabilities));
        if (result != MMSYSERR_NOERROR) {
            return core::Result<std::vector<repository::AudioInputDevice>>::failure(
                core::ErrorCode::FileOpenFailed, audioError("Cannot list microphones", result));
        }
        devices.push_back({std::to_string(deviceId), toUtf8(capabilities.szPname)});
    }
    return core::Result<std::vector<repository::AudioInputDevice>>::success(std::move(devices));
#else
    return core::Result<std::vector<repository::AudioInputDevice>>::failure(
        core::ErrorCode::InvalidParameter, "Microphone capture is currently supported on Windows only.");
#endif
}

core::Result<bool> WindowsAudioInput::start(
    const std::string& deviceId,
    repository::AudioInputFormat format,
    std::function<void(std::span<const double>)> onSamples,
    std::function<void(std::string)> onError)
{
#ifdef _WIN32
    std::lock_guard lock(impl_->mutex);
    if (impl_->session) {
        return core::Result<bool>::failure(core::ErrorCode::InvalidParameter,
            "Microphone recording is already started; stop it before starting again.");
    }
    if (!onSamples || !onError) {
        return core::Result<bool>::failure(core::ErrorCode::InvalidParameter,
            "Microphone sample and error callbacks are required.");
    }
    if (format.sampleRate < 8000 || format.sampleRate > 192000) {
        return core::Result<bool>::failure(core::ErrorCode::InvalidSampleRate,
            "Microphone sample rate must be between 8000 and 192000 Hz.");
    }
    UINT selectedDevice = WAVE_MAPPER;
    if (!deviceId.empty()) {
        const auto parsed = std::from_chars(deviceId.data(), deviceId.data() + deviceId.size(), selectedDevice);
        if (parsed.ec != std::errc{} || parsed.ptr != deviceId.data() + deviceId.size()
            || selectedDevice >= waveInGetNumDevs()) {
            return core::Result<bool>::failure(core::ErrorCode::InvalidParameter,
                "The selected microphone is unavailable; refresh the device list.");
        }
    }
    auto session = std::make_unique<CaptureSession>();
    const auto fail = [&session](std::string message) {
        disposeSession(session);
        return core::Result<bool>::failure(core::ErrorCode::FileOpenFailed, std::move(message));
    };
    try {
        session->onSamples = std::move(onSamples);
        session->onError = std::move(onError);
        session->dataEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        session->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!session->dataEvent || !session->stopEvent) {
            return fail("Cannot create microphone events (Windows error "
                + std::to_string(GetLastError()) + ").");
        }
        WAVEFORMATEX waveFormat{};
        waveFormat.wFormatTag = WAVE_FORMAT_PCM;
        waveFormat.nChannels = 1;
        waveFormat.nSamplesPerSec = format.sampleRate;
        waveFormat.wBitsPerSample = 16;
        waveFormat.nBlockAlign = 2;
        waveFormat.nAvgBytesPerSec = format.sampleRate * waveFormat.nBlockAlign;
        MMRESULT result = waveInOpen(&session->input, selectedDevice, &waveFormat,
            reinterpret_cast<DWORD_PTR>(session->dataEvent), 0, CALLBACK_EVENT);
        if (result != MMSYSERR_NOERROR) {
            return fail(audioError("Cannot open microphone", result));
        }
        const std::size_t samplesPerBuffer = format.sampleRate / 50;
        session->normalized.resize(samplesPerBuffer);
        for (auto& buffer : session->buffers) {
            buffer.samples.resize(samplesPerBuffer);
            buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.samples.data());
            buffer.header.dwBufferLength = static_cast<DWORD>(samplesPerBuffer * sizeof(std::int16_t));
            result = waveInPrepareHeader(session->input, &buffer.header, sizeof(WAVEHDR));
            if (result != MMSYSERR_NOERROR) {
                return fail(audioError("Cannot prepare microphone buffers", result));
            }
            buffer.prepared = true;
            result = waveInAddBuffer(session->input, &buffer.header, sizeof(WAVEHDR));
            if (result != MMSYSERR_NOERROR) {
                return fail(audioError("Cannot queue microphone buffers", result));
            }
        }
        result = waveInStart(session->input);
        if (result != MMSYSERR_NOERROR) {
            return fail(audioError("Cannot start microphone", result));
        }
        impl_->worker = std::thread([capture = session.get()] { capture->run(); });
        impl_->session = std::move(session);
    } catch (const std::exception& error) {
        return fail(std::string("Cannot start microphone: ") + error.what());
    }
    return core::Result<bool>::success(true);
#else
    static_cast<void>(deviceId);
    static_cast<void>(format);
    static_cast<void>(onSamples);
    static_cast<void>(onError);
    return core::Result<bool>::failure(core::ErrorCode::InvalidParameter,
        "Microphone capture is currently supported on Windows only.");
#endif
}

void WindowsAudioInput::stop()
{
#ifdef _WIN32
    std::lock_guard lock(impl_->mutex);
    if (!impl_->session) {
        return;
    }
    if (impl_->session->stopEvent) {
        SetEvent(impl_->session->stopEvent);
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    disposeSession(impl_->session);
#endif
}

}
