// CAL / QUICKCALC: a calculator on the command line.
#include "extra.h"
#include "tbcad/expr/expr.h"

namespace tbcad::inquiry {

namespace {

class CalCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CAL"; }
    std::vector<std::string> defaultAliases() const override { return {"QUICKCALC", "QC"}; }
    // 'CAL inside another command: the result answers that command's prompt.
    bool isTransparent() const override { return true; }
    std::string_view description() const override {
        return "Calculate an expression: + - * / ^, sqrt, sin, cos, round, pi; ans is the last "
               "result";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        if (ctx.transparentRun()) {
            // One expression; a point is two (or a distance and an angle) joined by "," or "<",
            // with an optional "@": 'CAL then 1200/4,300*2 at "Specify next point".
            cmd::StringOptions so;
            auto s = co_await ctx.getString(">>Expression", so);
            if (!s.ok() || cmd::trim(s.text).empty())
                co_return;
            std::string text = cmd::trim(s.text);
            std::string result;
            if (text[0] == '@') {
                result = "@";
                text.erase(0, 1);
            }
            const char sep = text.find('<') != std::string::npos ? '<' : ',';
            for (std::size_t from = 0; from <= text.size();) {
                const std::size_t at = std::min(text.find(sep, from), text.size());
                auto parsed = expr::Expression::parse(cmd::trim(text.substr(from, at - from)));
                auto value = parsed ? parsed.value().eval([](std::string_view) {
                    return std::optional<expr::Value>{};
                })
                                    : Result<expr::Value>(parsed.error());
                if (!value) {
                    ctx.message(value.error().message);
                    co_return;
                }
                if (from > 0)
                    result += sep;
                result += value.value().toString();
                from = at + 1;
            }
            ctx.message("= " + result);
            ctx.answerParent(result);
            co_return;
        }
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
