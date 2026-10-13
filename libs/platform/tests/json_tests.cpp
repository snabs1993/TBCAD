#include <doctest/doctest.h>

#include "tbcad/platform/json.h"

using namespace tbcad::platform;

TEST_CASE("json parses scalars, arrays and objects") {
    auto r = parseJson(R"({"a": 1.5, "b": [true, null, "x\n\u00e9"], "c": {"d": false}})");
    REQUIRE(r.ok());
    const Json& j = r.value();
    CHECK(j.find("a")->asNumber() == 1.5);
    const auto& arr = j.find("b")->asArray();
    REQUIRE(arr.size() == 3);
    CHECK(arr[0].asBool());
    CHECK(arr[1].isNull());
    CHECK(arr[2].asString() == "x\n\xC3\xA9");
    CHECK(j.find("c")->find("d")->asBool(true) == false);
    CHECK(j.find("zzz") == nullptr);
}

TEST_CASE("json round trips and is deterministic") {
    Json::Object o;
    o["name"] = "Thai: \xE0\xB8\x81";
    o["n"] = 10;
    o["pi"] = 3.14159;
    o["list"] = Json(Json::Array{Json(1), Json("two"), Json(true)});
    Json doc{std::move(o)};
    std::string text = toJson(doc);
    auto back = parseJson(text);
    REQUIRE(back.ok());
    CHECK(back.value() == doc);
    CHECK(toJson(back.value()) == text);
    CHECK(toJson(Json(Json::Object{}), false) == "{}");
}

TEST_CASE("json rejects malformed text without crashing") {
    for (const char* bad : {"", "{", "[1,", "{\"a\" 1}", "{\"a\":}", "tru", "\"abc", "[1] x",
                            "\"\\ud800\"", "{\"a\":1,}", "01x"}) {
        CAPTURE(bad);
        CHECK_FALSE(parseJson(bad).ok());
    }
    std::string deep(200, '[');
    CHECK_FALSE(parseJson(deep).ok());
}

TEST_CASE("json skips a byte order mark") {
    CHECK(parseJson("\xEF\xBB\xBF{\"a\":1}").ok());
}
