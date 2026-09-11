#pragma once

#include "signallab/database/database_connection.hpp"
#include "signallab/repository/signal_repository.hpp"

namespace signallab::database {

class SqliteSignalRepository final : public repository::SignalRepository {
public:
    explicit SqliteSignalRepository(DatabaseConnection& connection);

    [[nodiscard]] core::Result<repository::StoredSignal> saveSignal(
        repository::SignalRegistration registration) override;

    [[nodiscard]] core::Result<std::optional<repository::StoredSignal>>
    findBySourceKey(const std::string& sourceKey) override;

    [[nodiscard]] core::Result<std::optional<repository::StoredSignal>>
    findByRecordId(std::int64_t recordId) override;

    [[nodiscard]] core::Result<std::optional<repository::StoredSignal>>
    findByFileHash(const std::string& fileHash) override;

    [[nodiscard]] core::Result<std::vector<repository::StoredSignal>>
    listSignals() override;

    [[nodiscard]] core::Result<bool> removeSignal(
        std::int64_t recordId) override;

private:
    DatabaseConnection& connection_;
};

}  // namespace signallab::database
