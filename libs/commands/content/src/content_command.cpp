#include <filesystem>

#include "commands.h"
#include "tbcad/xref/xref.h"

namespace tbcad::content_cmds {

namespace {

std::filesystem::path pathFrom(const std::string& text) {
    const std::string t = cmd::trim(text);
    return std::filesystem::path(std::u8string(t.begin(), t.end()));
}

std::filesystem::path hostFileOf(cmd::CommandContext& ctx) {
    auto f = ctx.host().setting("document.file");
    return f ? pathFrom(*f) : std::filesystem::path{};
}

std::vector<std::string> splitNames(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text + "|") {
        if (c == '|') {
            std::string t = cmd::trim(cur);
            if (!t.empty())
                out.push_back(std::move(t));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    return out;
}

const char* kindLabel(xref::ContentKind k) {
    switch (k) {
        case xref::ContentKind::Block:
            return "Block        ";
        case xref::ContentKind::Layer:
            return "Layer        ";
        case xref::ContentKind::Linetype:
            return "Linetype     ";
        case xref::ContentKind::TextStyle:
            return "Textstyle    ";
        case xref::ContentKind::DimStyle:
            return "Dimstyle     ";
        case xref::ContentKind::MLeaderStyle:
            return "Mleaderstyle ";
        case xref::ContentKind::TableStyle:
            return "Tablestyle   ";
        case xref::ContentKind::MLineStyle:
            return "MLStyle      ";
    }
    return "";
}

xref::ContentKind kindOf(const std::string& keyword) {
    using K = xref::ContentKind;
    for (K k : {K::Block, K::Layer, K::Linetype, K::TextStyle, K::DimStyle, K::MLeaderStyle,
                K::TableStyle, K::MLineStyle}) {
        std::string label = kindLabel(k);
        label.erase(label.find(' '));
        if (cmd::toUpperAscii(label) == cmd::toUpperAscii(keyword))
            return k;
    }
    return K::Block;
}

class ContentImportCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CONTENTIMPORT"; }
    std::vector<std::string> defaultAliases() const override { return {"CIMP"}; }
    std::string_view description() const override {
        return "Copy blocks, layers, linetypes or styles from another drawing file";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::StringOptions fo;
        auto file = co_await ctx.getString("Drawing file (.tbcad, .dxf, .tbt)", fo);
        if (!file.ok())
            co_return;
        auto source = xref::loadSource(xref::resolvePath(cmd::trim(file.text), hostFileOf(ctx)));
        if (!source) {
            ctx.message(source.error().message);
            co_return;
        }
        cmd::KeywordOptions ko;
        ko.keywords = {"Block",        "Layer",      "Linetype", "Textstyle", "Dimstyle",
                       "Mleaderstyle", "TAblestyle", "MLStyle",  "List"};
        ko.defaultKeyword = "Block";
        auto kind = co_await ctx.getKeyword(
            "What to copy "
            "[Block/Layer/Linetype/Textstyle/Dimstyle/Mleaderstyle/TAblestyle/MLStyle/List]",
            ko);
        if (!kind.ok())
            co_return;

        auto all = xref::listContent(*source.value().doc);
        if (kind.keyword == "List") {
            for (const auto& i : all)
                ctx.message(std::string(kindLabel(i.kind)) + i.name);
            if (all.empty())
                ctx.message("The file has nothing to copy.");
            co_return;
        }
        const xref::ContentKind want = kindOf(kind.keyword);
        cmd::StringOptions no;
        auto names = co_await ctx.getString("Names, separated by | (* = all)", no);
        if (!names.ok())
            co_return;
        std::vector<xref::ContentItem> items;
        for (const std::string& n : splitNames(names.text)) {
            if (n == "*") {
                for (const auto& i : all)
                    if (i.kind == want)
                        items.push_back(i);
            } else {
                items.push_back({want, n});
            }
        }
        if (items.empty()) {
            ctx.message("Nothing to copy.");
            co_return;
        }
        ctx.setUndoLabel("Copy from " + cmd::trim(file.text));
        xref::ImportReport report;
        if (auto r = xref::importContent(ctx.tx(), ctx.doc(), source.value(), items, report); !r) {
            ctx.message(r.error().message);
            co_return;
        }
        for (const std::string& s : report.skipped)
            ctx.message("Skipped " + s);
        ctx.message("Copied " + std::to_string(report.added) + " item(s).");
    }
};

class ContentPanelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CONTENT"; }
    // The DesignCenter names drafters know.
    std::vector<std::string> defaultAliases() const override {
        return {"ADC", "ADCENTER", "DC", "DCENTER"};
    }
    std::string_view description() const override {
        return "Open the content browser (blocks, layers and styles from other drawings)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("content");
        co_return;
    }
};

// ADCCLOSE: closes the content browser.
class ContentCloseCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ADCCLOSE"; }
    std::string_view description() const override { return "Close the content browser"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().closePanel("content");
        co_return;
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeContentImportCommand() {
    return std::make_unique<ContentImportCommand>();
}

std::unique_ptr<cmd::Command> makeContentPanelCommand() {
    return std::make_unique<ContentPanelCommand>();
}

std::unique_ptr<cmd::Command> makeContentCloseCommand() {
    return std::make_unique<ContentCloseCommand>();
}

}  // namespace tbcad::content_cmds
