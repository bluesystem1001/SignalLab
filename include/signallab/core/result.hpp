#pragma once

#include <string>
#include <utility>
#include <variant>

#include "signallab/core/error.hpp"

namespace signallab::core {

template<typename T>
class Result {
public:
    [[nodiscard]] static Result success(T value)
    {
        Storage storage(std::in_place_index<0>, std::move(value));
        return Result(std::move(storage));
    }

    [[nodiscard]] static Result failure(Error error)
    {
        Storage storage(std::in_place_index<1>, std::move(error));
        return Result(std::move(storage));
    }

    [[nodiscard]] static Result failure(ErrorCode code, std::string message)
    {
        return failure(Error{code, std::move(message)});
    }

    [[nodiscard]] bool hasValue() const noexcept
    {
        return storage_.index() == 0;
    }

    explicit operator bool() const noexcept
    {
        return hasValue();
    }

    [[nodiscard]] const T& value() const &
    {
        return std::get<0>(storage_);
    }

    [[nodiscard]] T&& value() &&
    {
        return std::get<0>(std::move(storage_));
    }

    [[nodiscard]] const Error& error() const &
    {
        return std::get<1>(storage_);
    }

private:
    using Storage = std::variant<T, Error>;

    explicit Result(Storage storage)
        : storage_(std::move(storage))
    {
    }

    Storage storage_;
};

}  // namespace signallab::core
