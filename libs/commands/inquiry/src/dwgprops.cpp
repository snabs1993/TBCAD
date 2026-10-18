// DWGPROPS: title, subject, author, keywords and comments of the drawing, kept in the file header.
#include "extra.h"

namespace tbcad::inquiry {

namespace {

constexpr const char* kFields[] = {"Title", "Subject", "Author", "Keywords", "Comments"};

std::string keyOf(const std::string& field) {
    std::string k = "tbcad.prop.";
    for (char c : field)
        k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}

class DwgPropsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DWGPROPS"; }
    std::string_view description() const override {
        return "Show or set the title, subject, author, keywords and comments of the drawing";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        for (;;) {
            for (const char* f : kFields) {
                auto it = ctx.doc().header().custom.find(keyOf(f));
                ctx.message(
                    std::string(f) + ": " +
                    (it == ctx.doc().header().custom.end() ? std::string("(none)") : it->second));
            }
            cmd::KeywordOptions ko;
            ko.keywords = {"Title", "Subject", "Author", "Keywords", "Comments"};
            auto k = co_await ctx.getKeyword(
                "Property to change [Title/Subject/Author/Keywords/"
                "Comments] or Enter to finish",
                ko);
            if (!k.ok())
                co_return;
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

}  // namespace

std::unique_ptr<cmd::Command> makeDwgProps() {
    return std::make_unique<DwgPropsCommand>();
}

}  // namespace tbcad::inquiry
