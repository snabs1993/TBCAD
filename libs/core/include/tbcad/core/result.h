#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace tbcad {

enum class ErrorCode {
    Generic,
    InvalidArgument,
    NotFound,
    OutOfRange,
    Conflict,
    Parse,
    Io,
    Unsupported,
};

// Expected failure (bad file, bad user input, rejected edit). Programmer errors throw instead.
struct Error {
    ErrorCode code = ErrorCode::Generic;
    std::string message;
};

template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}  // NOLINT: implicit by design
    Result(Error error) : v_(std::in_place_index<1>, std::move(error)) {}  // NOLINT

    bool ok() const { return v_.index() == 0; }
    explicit operator bool() const { return ok(); }

    T& value() & { return get(); }
    const T& value() const& { return get(); }
    T&& value() && { return std::move(get()); }

    const Error& error() const {
        if (ok())
            throw std::logic_error("Result::error() called on a success value");
        return std::get<1>(v_);
    }

    T valueOr(T fallback) const& { return ok() ? std::get<0>(v_) : std::move(fallback); }

private:
    T& get() {
        if (!ok())
            throw std::logic_error("Result::value() called on error: " + error().message);
        return std::get<0>(v_);
    }
    const T& get() const {
        if (!ok())
            throw std::logic_error("Result::value() called on error: " + error().message);
        return std::get<0>(v_);
    }
    std::variant<T, Error> v_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), ok_(false) {}  // NOLINT

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }
    const Error& error() const {
        if (ok_)
            throw std::logic_error("Result::error() called on a success value");
        return error_;
    }

private:
    Error error_;
    bool ok_ = true;
};

inline Error makeError(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

}  // namespace tbcad
