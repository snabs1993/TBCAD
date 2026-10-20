// libFuzzer target: formulas are typed by users and stored in drawings (parameters, fields,
// assemblies). Parsing and evaluating any text must give a value or an error, never a crash,
// stack overflow or hang.
// Build: cmake -DTBCAD_FORMATS_FUZZ=ON (clang), run tbcad_expr_fuzz <corpus dir>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "tbcad/expr/expr.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace tbcad::expr;
    auto e = Expression::parse(std::string_view(reinterpret_cast<const char*>(data), size));
    if (!e.ok())
        return 0;
    const Env env = [](std::string_view name) -> std::optional<Value> {
        if (name == "Length")
            return Value::ofNumber(1200);
        if (name == "Name")
            return Value::ofText("ผนัง");
        return std::nullopt;
    };
    (void)e.value().eval(env);
    (void)e.value().names();
    return 0;
}
