#pragma once

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "tbcad/core/result.h"

namespace tbcad::platform {

// Small JSON document model for settings and keybinding files. Numbers are doubles; objects keep
// their keys sorted, so written files are deterministic.
class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) {}                      // NOLINT: implicit by design
    Json(bool b) : v_(b) {}                      // NOLINT
    Json(double d) : v_(d) {}                    // NOLINT
    Json(int i) : v_(static_cast<double>(i)) {}  // NOLINT
    Json(const char* s) : v_(std::string(s)) {}  // NOLINT
    Json(std::string s) : v_(std::move(s)) {}    // NOLINT
    Json(Array a) : v_(std::move(a)) {}          // NOLINT
    Json(Object o) : v_(std::move(o)) {}         // NOLINT

    bool isNull() const { return v_.index() == 0; }
    bool isBool() const { return std::holds_alternative<bool>(v_); }
    bool isNumber() const { return std::holds_alternative<double>(v_); }
    bool isString() const { return std::holds_alternative<std::string>(v_); }
    bool isArray() const { return std::holds_alternative<Array>(v_); }
    bool isObject() const { return std::holds_alternative<Object>(v_); }

    // Typed access; the fallback is returned when the value has another type.
    bool asBool(bool fallback = false) const;
    double asNumber(double fallback = 0) const;
    const std::string& asString() const;  // empty string when not a string
    const Array& asArray() const;         // empty array when not an array
    const Object& asObject() const;       // empty object when not an object
    Array& array();                       // converts to an empty array first if needed
    Object& object();                     // converts to an empty object first if needed

    // Object member or null.
    const Json* find(const std::string& key) const;

    friend bool operator==(const Json&, const Json&) = default;

private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v_{};
};

Result<Json> parseJson(std::string_view text);
// Two-space indentation when pretty; one line otherwise. Always ends with '\n' when pretty.
std::string toJson(const Json& value, bool pretty = true);

}  // namespace tbcad::platform
