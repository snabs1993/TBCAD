#include "tbcad/mcp/schema.h"

#include <cmath>

namespace tbcad::mcp {

namespace {

bool typeMatches(const std::string& type, const Json& v) {
    if (type == "object")
        return v.isObject();
    if (type == "array")
        return v.isArray();
    if (type == "string")
        return v.isString();
    if (type == "boolean")
        return v.isBool();
    if (type == "number")
        return v.isNumber() && std::isfinite(v.asNumber());
    if (type == "integer")
        return v.isNumber() && std::isfinite(v.asNumber()) && v.asNumber() == std::floor(v.asNumber());
    if (type == "null")
        return v.isNull();
    return true;
}

const char* describe(const Json& v) {
    if (v.isNull())
        return "null";
    if (v.isBool())
        return "boolean";
    if (v.isNumber())
        return "number";
    if (v.isString())
        return "string";
    if (v.isArray())
        return "array";
    return "object";
}

}  // namespace

std::string validateSchema(const Json& schema, const Json& value, const std::string& path) {
    if (!schema.isObject())
        return {};
    if (const Json* oneOf = schema.find("oneOf")) {
        for (const auto& alt : oneOf->asArray()) {
            if (validateSchema(alt, value, path).empty())
                return {};
        }
        return path + ": does not match any of the allowed forms";
    }
    if (const Json* type = schema.find("type")) {
        bool ok = false;
        std::string expected;
        auto check = [&](const std::string& t) {
            if (!expected.empty())
                expected += " or ";
            expected += t;
            ok = ok || typeMatches(t, value);
        };
        if (type->isString()) {
            check(type->asString());
        } else {
            for (const auto& t : type->asArray())
                check(t.asString());
        }
        if (!ok)
            return path + ": expected " + expected + ", got " + describe(value);
    }
    if (const Json* en = schema.find("enum")) {
        bool found = false;
        for (const auto& e : en->asArray())
            found = found || e == value;
        if (!found) {
            std::string list;
            for (const auto& e : en->asArray()) {
                if (!list.empty())
                    list += ", ";
                list += e.isString() ? e.asString() : dump(e);
            }
            return path + ": must be one of " + list;
        }
    }
    if (value.isNumber()) {
        if (const Json* mn = schema.find("minimum"); mn && value.asNumber() < mn->asNumber())
            return path + ": must be >= " + dump(*mn);
        if (const Json* mx = schema.find("maximum"); mx && value.asNumber() > mx->asNumber())
            return path + ": must be <= " + dump(*mx);
    }
    if (value.isString()) {
        if (const Json* ml = schema.find("minLength");
            ml && static_cast<double>(value.asString().size()) < ml->asNumber())
            return path + ": must not be empty";
    }
    if (value.isArray()) {
        const auto& arr = value.asArray();
        if (const Json* mn = schema.find("minItems"); mn && arr.size() < mn->asNumber())
            return path + ": needs at least " + dump(*mn) + " items";
        if (const Json* mx = schema.find("maxItems"); mx && arr.size() > mx->asNumber())
            return path + ": allows at most " + dump(*mx) + " items";
        if (const Json* items = schema.find("items")) {
            for (std::size_t i = 0; i < arr.size(); ++i) {
                auto err = validateSchema(*items, arr[i], path + "[" + std::to_string(i) + "]");
                if (!err.empty())
                    return err;
            }
        }
    }
    if (value.isObject()) {
        if (const Json* req = schema.find("required")) {
            for (const auto& r : req->asArray()) {
                if (!value.find(r.asString()))
                    return path + ": missing required property \"" + r.asString() + "\"";
            }
        }
        if (const Json* props = schema.find("properties")) {
            for (const auto& [name, sub] : props->asObject()) {
                if (const Json* v = value.find(name)) {
                    auto err = validateSchema(sub, *v, path + "." + name);
                    if (!err.empty())
                        return err;
                }
            }
        }
    }
    return {};
}

}  // namespace tbcad::mcp
