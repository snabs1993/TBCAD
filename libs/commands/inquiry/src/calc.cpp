// CAL / QUICKCALC: a calculator on the command line.
#include "extra.h"
#include "tbcad/expr/expr.h"

namespace tbcad::inquiry {

namespace {

class CalCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CAL"; }
    std::vector<std::string> defaultAliases() const override { return {"QUICKCALC", "QC"}; }
    std::string_view description() const override {
        return "Calculate an expression: + - * / ^, sqrt, sin, cos, round, pi; ans is the last "
               "result";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        std::optional<expr::Value> last;
        for (;;) {
            cmd::StringOptions so;
            auto s = co_await ctx.getString("Expression (Enter to finish)", so);
            if (!s.ok() || cmd::trim(s.text).empty())
                co_return;
            auto parsed = expr::Expression::parse(cmd::trim(s.text));
            if (!parsed) {
                ctx.message(parsed.error().message);
                continue;
            }
            auto value = parsed.value().eval([&](std::string_view n) -> std::optional<expr::Value> {
                if (n == "ans" && last)
                    return last;
                return std::nullopt;
            });
            if (!value) {
                ctx.message(value.error().message);
                continue;
            }
            last = value.value();
            ctx.message("= " + last->toString());
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeCal() {
    return std::make_unique<CalCommand>();
}

}  // namespace tbcad::inquiry
