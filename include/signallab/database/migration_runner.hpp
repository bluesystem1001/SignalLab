#pragma once

#include <cstddef>
#include <filesystem>

#include "signallab/core/result.hpp"
#include "signallab/database/database_connection.hpp"

namespace signallab::database {

class MigrationRunner {
public:
    explicit MigrationRunner(DatabaseConnection& connection);

    // Returns how many new migration files were applied. Files ending in .sql
    // are applied once, in lexicographic filename order.
    [[nodiscard]] core::Result<std::size_t> run(
        const std::filesystem::path& migrationsDirectory);

private:
    DatabaseConnection& connection_;
};

}  // namespace signallab::database
