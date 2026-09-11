#pragma once

#include "signallab/database/database_connection.hpp"
#include "signallab/repository/script_repository.hpp"

namespace signallab::database {

class SqliteScriptRepository final : public repository::ScriptRepository {
public:
    explicit SqliteScriptRepository(DatabaseConnection& connection);

    [[nodiscard]] core::Result<std::int64_t> addScriptHistory(
        const std::string& scriptText,
        const std::string& status,
        const std::string& errorMessage) override;

    [[nodiscard]] core::Result<std::vector<repository::ScriptHistoryRecord>>
    listScriptHistory(std::size_t maximumCount) override;

private:
    DatabaseConnection& connection_;
};

}  // namespace signallab::database
