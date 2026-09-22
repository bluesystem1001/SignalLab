#pragma once

#include "signallab/database/database_connection.hpp"
#include "signallab/repository/script_repository.hpp"

namespace signallab::database {

class SqliteScriptRepository final : public repository::ScriptRepository {
public:
    explicit SqliteScriptRepository(DatabaseConnection& connection);

    [[nodiscard]] core::Result<std::int64_t> createScriptHistory(
        std::string scriptText) override;

    [[nodiscard]] core::Result<bool> finishScriptHistory(
        std::int64_t historyId,
        repository::ScriptHistoryFinish finish) override;

    [[nodiscard]] core::Result<std::size_t>
    interruptRunningScripts() override;

    [[nodiscard]] core::Result<std::vector<repository::ScriptHistoryRecord>>
    listScriptHistory(std::size_t maximumCount) override;

private:
    DatabaseConnection& connection_;
};

}  // namespace signallab::database
