// -DWGPROPS: title, subject, author, keywords and comments of the drawing, and custom properties,
// kept in the file header. DWGPROPS opens the Drawing Properties dialog.
#include <cctype>

#include "extra.h"

namespace tbcad::inquiry {

namespace {

constexpr const char* kCustomPrefix = "tbcad.prop.custom.";
constexpr const char* kFields[] = {"Title", "Subject", "Author", "Keywords", "Comments"};

std::string keyOf(const std::string& field) {
    std::string k = "tbcad.prop.";
    for (char c : field)
        k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}

class DwgPropsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-DWGPROPS"; }
    std::string_view description() const override {
        return "Show or set the title, subject, author, keywords, comments and custom properties";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        for (;;) {
            for (const char* f : kFields) {
                auto it = ctx.doc().header().custom.find(keyOf(f));
                ctx.message(
                    std::string(f) + ": " +
                    (it == ctx.doc().header().custom.end() ? std::string("(none)") : it->second));
            }
            for (const auto& [name, value] : customProperties(ctx.doc().header()))
                ctx.message("Custom " + name + ": " + value);
            cmd::KeywordOptions ko;
            ko.keywords = {"Title", "Subject", "Author", "Keywords", "Comments", "Custom"};
            auto k = co_await ctx.getKeyword(
                "Property to change [Title/Subject/Author/Keywords/"
                "Comments/Custom] or Enter to finish",
                ko);
            if (!k.ok())
                co_return;
            if (k.keyword == "Custom") {
                auto n = co_await ctx.getString("Custom property name");
                if (!n.ok() || cmd::trim(n.text).empty())
                    continue;
                const std::string name = cmd::trim(n.text);
                cmd::StringOptions vo;
                vo.allowEmpty = true;
                auto v = co_await ctx.getString("Value of " + name + " (empty removes it)", vo);
                if (v.status == cmd::Status::Cancel)
                    co_return;
                const std::string value = v.ok() ? cmd::trim(v.text) : std::string();
                ctx.setUndoLabel("DWGPROPS");
                (void)ctx.tx().modifyHeader([&](Header& h) {
                    if (value.empty())
                        h.custom.erase(kCustomPrefix + name);
                    else
                        h.custom[kCustomPrefix + name] = value;
                });
                continue;
            }
            cmd::StringOptions so;
            so.allowEmpty = true;
            auto v = co_await ctx.getString("New " + k.keyword + " (empty clears it)", so);
            if (v.status == cmd::Status::Cancel)
                co_return;
            const std::string value = v.ok() ? cmd::trim(v.text) : std::string();
            const std::string key = keyOf(k.keyword);
            ctx.setUndoLabel("DWGPROPS");
            (void)ctx.tx().modifyHeader([&](Header& h) {
                if (value.empty())
                    h.custom.erase(key);
                else
                    h.custom[key] = value;
            });
        }
    }
};

// DWGPROPS: the Drawing Properties dialog (general, summary, statistics, custom).
class DwgPropsDialog final : public cmd::Command {
public:
    std::string_view name() const override { return "DWGPROPS"; }
    std::string_view description() const override {
        return "Open the Drawing Properties dialog: file, summary, statistics and custom "
               "properties";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("dwgprops");
        co_return;
    }
};

}  // namespace

std::vector<std::pair<std::string, std::string>> customProperties(const Header& h) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto it = h.custom.lower_bound(kCustomPrefix);
         it != h.custom.end() && it->first.rfind(kCustomPrefix, 0) == 0; ++it)
        out.emplace_back(it->first.substr(std::string_view(kCustomPrefix).size()), it->second);
    return out;
}

std::unique_ptr<cmd::Command> makeDwgProps() {
    return std::make_unique<DwgPropsCommand>();
}

std::unique_ptr<cmd::Command> makeDwgPropsDialog() {
    return std::make_unique<DwgPropsDialog>();
}

}  // namespace tbcad::inquiry
