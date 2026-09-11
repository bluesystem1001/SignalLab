#pragma once

#include <QSqlDatabase>

#include <filesystem>

#include "signallab/core/result.hpp"

class QThread;

namespace signallab::database {

// Owns one named Qt SQL connection. Create, use and destroy this object in
// the same thread; Qt database connections must not cross thread boundaries.
class DatabaseConnection {
public:
    explicit DatabaseConnection(std::filesystem::path databasePath);
    ~DatabaseConnection();

    DatabaseConnection(const DatabaseConnection&) = delete;
    DatabaseConnection& operator=(const DatabaseConnection&) = delete;
    DatabaseConnection(DatabaseConnection&&) = delete;
    DatabaseConnection& operator=(DatabaseConnection&&) = delete;

    [[nodiscard]] core::Result<bool> open();
    [[nodiscard]] core::Result<bool> close();
    [[nodiscard]] core::Result<bool> checkThread() const;

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] QSqlDatabase database() const;
    [[nodiscard]] const std::filesystem::path& databasePath() const noexcept;
    [[nodiscard]] QString connectionName() const;

private:
    void closeWithoutResult() noexcept;

    std::filesystem::path databasePath_;
    QString connectionName_;
    QSqlDatabase database_;
    QThread* ownerThread_{nullptr};
};

}  // namespace signallab::database
