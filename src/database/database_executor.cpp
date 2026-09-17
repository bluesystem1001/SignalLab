#include "signallab/database/database_executor.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "signallab/core/error.hpp"
#include "signallab/database/database_connection.hpp"
#include "signallab/database/migration_runner.hpp"
#include "signallab/database/sqlite_script_repository.hpp"
#include "signallab/database/sqlite_signal_repository.hpp"
#include "signallab/database/sqlite_task_repository.hpp"

namespace signallab::database {

class DatabaseExecutor::Impl {
public:
    Impl(
        std::filesystem::path databasePath,
        std::filesystem::path migrationsDirectory)
        : databasePath_(std::move(databasePath)),
          migrationsDirectory_(std::move(migrationsDirectory))
    {
    }

    ~Impl()
    {
        const auto ignored = shutdown();
        (void)ignored;
    }

    core::Result<bool> start()
    {
        std::lock_guard lifecycleLock(lifecycleMutex_);

        {
            std::lock_guard queueLock(queueMutex_);
            if (state_ == State::Running) {
                return core::Result<bool>::success(true);
            }
            startupError_.reset();
            shutdownError_.reset();
            commands_.clear();
            state_ = State::Starting;
        }

        try {
            worker_ = std::jthread([this]() { run(); });
        } catch (const std::exception& exception) {
            std::lock_guard queueLock(queueMutex_);
            state_ = State::Stopped;
            return core::Result<bool>::failure(
                core::ErrorCode::DatabaseOpenFailed,
                "Cannot start the database thread: "
                    + std::string(exception.what()));
        }

        std::unique_lock queueLock(queueMutex_);
        stateChanged_.wait(queueLock, [this]() {
            return state_ != State::Starting;
        });
        if (state_ == State::Running) {
            return core::Result<bool>::success(true);
        }

        const core::Error error = startupError_.value_or(core::Error{
            core::ErrorCode::DatabaseOpenFailed,
            "The database thread stopped during startup."});
        queueLock.unlock();
        if (worker_.joinable()) {
            worker_.join();
        }
        return core::Result<bool>::failure(error);
    }

    core::Result<bool> shutdown()
    {
        std::lock_guard lifecycleLock(lifecycleMutex_);

        {
            std::lock_guard queueLock(queueMutex_);
            if (state_ == State::Running) {
                // No command may enter the queue after this transition. The
                // worker still processes every command already in FIFO order.
                state_ = State::Stopping;
            }
        }
        commandsReady_.notify_all();

        if (worker_.joinable()) {
            worker_.join();
        }

        std::lock_guard queueLock(queueMutex_);
        if (shutdownError_.has_value()) {
            return core::Result<bool>::failure(*shutdownError_);
        }
        return core::Result<bool>::success(true);
    }

    bool isRunning() const noexcept
    {
        std::lock_guard lock(queueMutex_);
        return state_ == State::Running;
    }

    struct RepositorySet {
        explicit RepositorySet(DatabaseConnection& connection)
            : signal(connection), task(connection), script(connection)
        {
        }

        SqliteSignalRepository signal;
        SqliteTaskRepository task;
        SqliteScriptRepository script;
    };

    template<typename T, typename Function>
    core::Result<T> execute(
        Function operation,
        std::stop_token stopToken = {})
    {
        if (stopToken.stop_requested()) {
            return core::Result<T>::failure(
                core::ErrorCode::Cancelled,
                "The database command was cancelled.");
        }

        auto promise = std::make_shared<std::promise<core::Result<T>>>();
        auto future = promise->get_future();

        {
            std::lock_guard lock(queueMutex_);
            if (state_ != State::Running) {
                return core::Result<T>::failure(
                    core::ErrorCode::DatabaseOpenFailed,
                    "The database executor is not running.");
            }
            if (std::this_thread::get_id() == workerThreadId_) {
                return core::Result<T>::failure(
                    core::ErrorCode::DatabaseQueryFailed,
                    "A database command cannot wait on its own thread.");
            }

            commands_.push_back(
                [promise, operation = std::move(operation), stopToken](
                    RepositorySet& repositories) mutable {
                    if (stopToken.stop_requested()) {
                        promise->set_value(core::Result<T>::failure(
                            core::ErrorCode::Cancelled,
                            "The database command was cancelled."));
                        return;
                    }
                    try {
                        promise->set_value(operation(repositories));
                    } catch (const std::exception& exception) {
                        promise->set_value(core::Result<T>::failure(
                            core::ErrorCode::DatabaseQueryFailed,
                            "A database command threw an exception: "
                                + std::string(exception.what())));
                    } catch (...) {
                        promise->set_value(core::Result<T>::failure(
                            core::ErrorCode::DatabaseQueryFailed,
                            "A database command threw an unknown exception."));
                    }
                });
        }
        commandsReady_.notify_one();
        // Once a command has entered the queue, do not report cancellation
        // until the database thread has observed the token and finished the
        // operation. Otherwise the caller can begin shutdown while an
        // apparently completed command is still copying or hashing a BLOB.
        future.wait();
        return future.get();
    }

private:
    enum class State {
        Stopped,
        Starting,
        Running,
        Stopping
    };

    using Command = std::function<void(RepositorySet&)>;

    void reportStartupFailure(core::Error error)
    {
        std::lock_guard lock(queueMutex_);
        startupError_ = std::move(error);
        workerThreadId_ = {};
        state_ = State::Stopped;
        stateChanged_.notify_all();
    }

    void run()
    {
        DatabaseConnection connection(databasePath_);
        const auto openResult = connection.open();
        if (!openResult) {
            reportStartupFailure(openResult.error());
            return;
        }

        MigrationRunner migrationRunner(connection);
        const auto migrationResult = migrationRunner.run(
            migrationsDirectory_);
        if (!migrationResult) {
            reportStartupFailure(migrationResult.error());
            return;
        }

        {
            RepositorySet repositories(connection);
            {
                std::lock_guard lock(queueMutex_);
                workerThreadId_ = std::this_thread::get_id();
                state_ = State::Running;
                stateChanged_.notify_all();
            }

            while (true) {
                Command command;
                {
                    std::unique_lock lock(queueMutex_);
                    commandsReady_.wait(lock, [this]() {
                        return state_ == State::Stopping
                            || !commands_.empty();
                    });

                    if (commands_.empty() && state_ == State::Stopping) {
                        break;
                    }

                    command = std::move(commands_.front());
                    commands_.pop_front();
                }

                // Commands run without holding the queue mutex. They may use
                // transactions, but never call back into this proxy.
                command(repositories);
            }
        }

        const auto closeResult = connection.close();
        {
            std::lock_guard lock(queueMutex_);
            if (!closeResult) {
                shutdownError_ = closeResult.error();
            }
            workerThreadId_ = {};
            state_ = State::Stopped;
            stateChanged_.notify_all();
        }
    }

    std::filesystem::path databasePath_;
    std::filesystem::path migrationsDirectory_;

    mutable std::mutex queueMutex_;
    std::mutex lifecycleMutex_;
    std::condition_variable commandsReady_;
    std::condition_variable stateChanged_;
    std::deque<Command> commands_;
    std::jthread worker_;
    std::thread::id workerThreadId_;
    State state_{State::Stopped};
    std::optional<core::Error> startupError_;
    std::optional<core::Error> shutdownError_;
};

DatabaseExecutor::DatabaseExecutor(
    std::filesystem::path databasePath,
    std::filesystem::path migrationsDirectory)
    : implementation_(std::make_unique<Impl>(
          std::move(databasePath),
          std::move(migrationsDirectory)))
{
}

DatabaseExecutor::~DatabaseExecutor()
{
    const auto ignored = shutdown();
    (void)ignored;
}

core::Result<bool> DatabaseExecutor::start()
{
    return implementation_->start();
}

core::Result<bool> DatabaseExecutor::shutdown()
{
    return implementation_->shutdown();
}

bool DatabaseExecutor::isRunning() const noexcept
{
    return implementation_->isRunning();
}

core::Result<repository::StoredSignal> DatabaseExecutor::saveSignal(
    repository::SignalRegistration registration)
{
    return implementation_->execute<repository::StoredSignal>(
        [registration = std::move(registration)](
            Impl::RepositorySet& repositories) mutable {
            return repositories.signal.saveSignal(std::move(registration));
        });
}

core::Result<std::optional<repository::StoredSignal>>
DatabaseExecutor::findBySourceKey(const std::string& sourceKey)
{
    return implementation_->execute<
        std::optional<repository::StoredSignal>>(
        [sourceKey](Impl::RepositorySet& repositories) {
            return repositories.signal.findBySourceKey(sourceKey);
        });
}

core::Result<std::optional<repository::StoredSignal>>
DatabaseExecutor::findByRecordId(
    std::int64_t recordId,
    std::stop_token stopToken)
{
    return implementation_->execute<
        std::optional<repository::StoredSignal>>(
        [recordId, stopToken](Impl::RepositorySet& repositories) {
            return repositories.signal.findByRecordId(recordId, stopToken);
        },
        stopToken);
}

core::Result<std::optional<repository::StoredSignal>>
DatabaseExecutor::findByFileHash(const std::string& fileHash)
{
    return implementation_->execute<
        std::optional<repository::StoredSignal>>(
        [fileHash](Impl::RepositorySet& repositories) {
            return repositories.signal.findByFileHash(fileHash);
        });
}

core::Result<std::vector<repository::StoredSignal>>
DatabaseExecutor::listSignals()
{
    return implementation_->execute<std::vector<repository::StoredSignal>>(
        [](Impl::RepositorySet& repositories) {
            return repositories.signal.listSignals();
        });
}

core::Result<bool> DatabaseExecutor::removeSignal(std::int64_t recordId)
{
    return implementation_->execute<bool>(
        [recordId](Impl::RepositorySet& repositories) {
            return repositories.signal.removeSignal(recordId);
        });
}

core::Result<std::int64_t> DatabaseExecutor::createTask(
    const repository::AnalysisTaskStart& task)
{
    return implementation_->execute<std::int64_t>(
        [task](Impl::RepositorySet& repositories) {
            return repositories.task.createTask(task);
        });
}

core::Result<bool> DatabaseExecutor::startTask(std::int64_t taskId)
{
    return implementation_->execute<bool>(
        [taskId](Impl::RepositorySet& repositories) {
            return repositories.task.startTask(taskId);
        });
}

core::Result<std::string> DatabaseExecutor::completeTask(
    std::int64_t taskId,
    const std::vector<repository::FeatureValue>& features)
{
    return implementation_->execute<std::string>(
        [taskId, features](Impl::RepositorySet& repositories) {
            return repositories.task.completeTask(taskId, features);
        });
}

core::Result<bool> DatabaseExecutor::failTask(
    std::int64_t taskId,
    const std::string& errorMessage)
{
    return implementation_->execute<bool>(
        [taskId, errorMessage](Impl::RepositorySet& repositories) {
            return repositories.task.failTask(taskId, errorMessage);
        });
}

core::Result<bool> DatabaseExecutor::cancelTask(
    std::int64_t taskId,
    const std::string& reason)
{
    return implementation_->execute<bool>(
        [taskId, reason](Impl::RepositorySet& repositories) {
            return repositories.task.cancelTask(taskId, reason);
        });
}

core::Result<std::size_t> DatabaseExecutor::interruptRunningTasks()
{
    return implementation_->execute<std::size_t>(
        [](Impl::RepositorySet& repositories) {
            return repositories.task.interruptRunningTasks();
        });
}

core::Result<std::vector<repository::AnalysisTaskRecord>>
DatabaseExecutor::listTasks(std::size_t maximumCount)
{
    return implementation_->execute<
        std::vector<repository::AnalysisTaskRecord>>(
        [maximumCount](Impl::RepositorySet& repositories) {
            return repositories.task.listTasks(maximumCount);
        });
}

core::Result<std::vector<repository::FeatureResultRecord>>
DatabaseExecutor::listFeatureResults(std::int64_t taskId)
{
    return implementation_->execute<
        std::vector<repository::FeatureResultRecord>>(
        [taskId](Impl::RepositorySet& repositories) {
            return repositories.task.listFeatureResults(taskId);
        });
}

core::Result<std::int64_t> DatabaseExecutor::addScriptHistory(
    const std::string& scriptText,
    const std::string& status,
    const std::string& errorMessage)
{
    return implementation_->execute<std::int64_t>(
        [scriptText, status, errorMessage](Impl::RepositorySet& repositories) {
            return repositories.script.addScriptHistory(
                scriptText, status, errorMessage);
        });
}

core::Result<std::vector<repository::ScriptHistoryRecord>>
DatabaseExecutor::listScriptHistory(std::size_t maximumCount)
{
    return implementation_->execute<
        std::vector<repository::ScriptHistoryRecord>>(
        [maximumCount](Impl::RepositorySet& repositories) {
            return repositories.script.listScriptHistory(maximumCount);
        });
}

}  // namespace signallab::database
