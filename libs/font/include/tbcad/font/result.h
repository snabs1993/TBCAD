#pragma once

#include <string>
#include <utility>
#include <variant>

namespace tbcad::font {

// Minimal error carrier. libs/font does not depend on libs/core, so it has its
// own tiny Result; callers that use tbcad::Result convert at the boundary.
struct Error {
    std::string message;
};

template <class T>
class Result {
public:
    Result(T value) : v_(std::move(value)) {}
    Result(Error error) : v_(std::move(error)) {}

    bool ok() const { return std::holds_alternative<T>(v_); }
    explicit operator bool() const { return ok(); }
    const T& value() const { return std::get<T>(v_); }
    const Error& error() const { return std::get<Error>(v_); }

private:
    std::variant<T, Error> v_;
};

}  // namespace tbcad::font
