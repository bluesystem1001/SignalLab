#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "signallab/core/result.hpp"
#include "signallab/core/signal.hpp"

namespace signallab::repository {

enum class SignalSourceType {
    Generated,
    Csv,
    Wav
};

struct FileVersionRegistration {
    std::string contentHash;
    std::filesystem::path storedRelativePath;
    std::string originalName;
    std::uint64_t byteSize;
};

struct SignalRegistration {
    SignalSourceType sourceType;
    std::string sourceKey;
    std::filesystem::path sourcePath;
    std::string parametersJson;
    std::optional<FileVersionRegistration> fileVersion;
    core::Signal signal;
};

struct StoredSignal {
    std::int64_t recordId;
    std::int64_t sourceId;
    SignalSourceType sourceType;
    std::string sourceKey;
    std::filesystem::path sourcePath;
    std::filesystem::path storedRelativePath;
    std::string fileHash;
    std::string sampleHash;
    std::string parametersJson;
    core::Signal signal;
};

// Deleting the last logical signal backed by an imported file also removes
// its file_version row. The physical archive lives outside SQLite, so the
// repository returns that exact cleanup candidate to the application layer.
struct SignalRemoval {
    bool removed{false};
    std::optional<std::filesystem::path>
        unreferencedStoredRelativePath;
};

class SignalRepository {
public:
    virtual ~SignalRepository() = default;

    [[nodiscard]] virtual core::Result<StoredSignal> saveSignal(
        SignalRegistration registration) = 0;

    [[nodiscard]] virtual core::Result<std::optional<StoredSignal>>
    findBySourceKey(const std::string& sourceKey) = 0;

    [[nodiscard]] virtual core::Result<std::optional<StoredSignal>>
    findByRecordId(
        std::int64_t recordId,
        std::stop_token stopToken = {}) = 0;

    [[nodiscard]] virtual core::Result<std::optional<StoredSignal>>
    findByFileHash(const std::string& fileHash) = 0;

    [[nodiscard]] virtual core::Result<std::vector<StoredSignal>>
    listSignals() = 0;

    [[nodiscard]] virtual core::Result<SignalRemoval> removeSignal(
        std::int64_t recordId) = 0;
};

}  // namespace signallab::repository
