#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "uwb/protocol/errors.hpp"

namespace uwb::protocol {

// Small allocation-light result carrier used by every protocol codec.
// Exceptions are not used anywhere in the protocol library.
template <typename T>
class [[nodiscard]] Result {
public:

    static Result ok(T value) {
        Result r{std::move(value), ProtocolError{}};
        return r;
    }

    static Result error(ProtocolErrorCode code, std::uint32_t detail = 0) {
        return Result{std::nullopt, ProtocolError{code, detail}};
    }

    static Result error(ProtocolError error) {
        return Result{std::nullopt, error};
    }

    [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
    [[nodiscard]] bool failed() const noexcept { return !value_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const T &value() const & { return *value_; }
    [[nodiscard]] T &value() & { return *value_; }
    [[nodiscard]] T &&value() && { return std::move(*value_); }

    [[nodiscard]] const T &operator*() const & { return *value_; }
    [[nodiscard]] T &operator*() & { return *value_; }
    [[nodiscard]] T *operator->() { return &*value_; }
    [[nodiscard]] const T *operator->() const { return &*value_; }

    [[nodiscard]] ProtocolError error() const noexcept { return error_; }
    [[nodiscard]] ProtocolErrorCode code() const noexcept { return error_.code; }

private:
    Result(std::optional<T> value, ProtocolError error) noexcept
        : value_(std::move(value)), error_(error) {}

    std::optional<T> value_;
    ProtocolError error_;
};

} // namespace uwb::protocol

#define UWB_PROTOCOL_RETURN_OK(expr) return ::uwb::protocol::Result<decltype(expr)>::ok(expr)
