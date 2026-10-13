#include <algorithm>
#include <sstream>

#include "common.h"

namespace tbcad::select {

namespace {
constexpr const char* kPrefix = "filter.";
const char* const kProps[] = {"Type", "Layer", "Color", "Linetype", "Lineweight", "Level"};
}  // namespace

std::optional<std::vector<EntityId>> filterEntities(const Document& doc,
                                                    const std::vector<Query>& queries) {
    std::optional<std::vector<EntityId>> acc;
    for (const Query& q : queries) {
        auto r = queryEntities(doc, q);
        if (!r)
            return std::nullopt;
        if (!acc) {
            acc = std::move(r);
            continue;
        }
        std::vector<EntityId> keep;
        for (EntityId id : *acc)
            if (std::find(r->begin(), r->end(), id) != r->end())
                keep.push_back(id);
        acc = std::move(keep);
    }
    return acc ? acc : std::vector<EntityId>{};
}

// One condition per line: property index, '!' when negated, then the value.
std::string encodeFilter(const std::vector<Query>& queries) {
    std::string s;
    for (const Query& q : queries)
        s += std::to_string(static_cast<int>(q.property)) + (q.negate ? "!" : "=") + q.value + "\n";
    return s;
}

std::vector<Query> decodeFilter(const std::string& text) {
    std::vector<Query> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2 || line[0] < '0' || line[0] > '5')
            continue;
        Query q;
        q.property = static_cast<Property>(line[0] - '0');
        q.negate = line[1] == '!';
        q.value = line.substr(2);
        out.push_back(std::move(q));
    }
    return out;
}

std::vector<std::string> savedFilters(const Document& doc) {
    std::vector<std::string> names;
    const std::string pre = kPrefix;
    for (const auto& [k, v] : doc.header().custom)
        if (k.compare(0, pre.size(), pre) == 0)
            names.push_back(k.substr(pre.size()));
    return names;
}

namespace detail {
namespace {

std::string describeQuery(const Query& q) {
    return std::string(kProps[static_cast<int>(q.property)]) + (q.negate ? " <> " : " = ") +
           q.value;
}

// FILTER [FI]: build a list of conditions (all must hold), select what matches, and optionally
// save the list under a name for later.
class Filter final : public cmd::Command {
public:
    std::string_view name() const override { return "FILTER"; }
    std::vector<std::string> defaultAliases() const override { return {"FI"}; }
    std::string_view description() const override {
        return "Select objects that satisfy a list of conditions";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        std::vector<Query> conds;
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Add", "Remove", "Apply", "Save", "Use"};
            const std::string def = conds.empty() ? "Add" : "Apply";
            ko.defaultKeyword = def;
            std::string head = conds.empty() ? "No conditions." : "";
            for (std::size_t i = 0; i < conds.size(); ++i)
                head += (i ? " AND " : "") + describeQuery(conds[i]);
            auto k = co_await ctx.getKeyword(head + " [Add/Remove/Apply/Save/Use] <" + def + ">",
                                             std::move(ko));
            if (!k.ok())
                co_return;
            if (k.keyword == "Add") {
                co_await add(ctx, conds);
            } else if (k.keyword == "Remove") {
                if (!conds.empty())
                    conds.pop_back();
            } else if (k.keyword == "Save") {
                auto n = co_await ctx.getString("Filter name");
                if (!n.ok() || n.text.empty())
                    continue;
                if (conds.empty()) {
                    ctx.message("Add a condition first.");
                    continue;
                }
                const std::string key = "filter." + cmd::toUpperAscii(n.text),
                                  enc = encodeFilter(conds);
                (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[key] = enc; });
                ctx.message("Filter " + cmd::toUpperAscii(n.text) + " saved.");
            } else if (k.keyword == "Use") {
                auto names = savedFilters(ctx.doc());
                if (names.empty()) {
                    ctx.message("There are no saved filters.");
                    continue;
                }
                std::string list;
                for (const auto& n : names)
                    list += (list.empty() ? "" : ", ") + n;
                auto n = co_await ctx.getString("Filter name (" + list + ")");
                if (!n.ok())
                    continue;
                const auto it =
                    ctx.doc().header().custom.find("filter." + cmd::toUpperAscii(n.text));
                if (it == ctx.doc().header().custom.end()) {
                    ctx.message("There is no filter named " + n.text + ".");
                    continue;
                }
                conds = decodeFilter(it->second);
            } else {  // Apply
                if (conds.empty()) {
                    ctx.message("Add a condition first.");
                    continue;
                }
                auto ids = filterEntities(ctx.doc(), conds);
                if (!ids) {
                    ctx.message("A condition has a value that cannot be read.");
                    continue;
                }
                ctx.host().setSelection(*ids);
                ctx.message(std::to_string(ids->size()) + " object(s) found.");
                co_return;
            }
        }
    }

private:
    static cmd::Task add(cmd::CommandContext& ctx, std::vector<Query>& conds) {
        cmd::KeywordOptions po;
        po.keywords = {"Type", "Layer", "Color", "Linetype", "Lineweight", "Level"};
        po.defaultKeyword = "Type";
        auto p = co_await ctx.getKeyword(
            "Property [Type/Layer/Color/Linetype/Lineweight/Level] <Type>", std::move(po));
        if (!p.ok())
            co_return;
        Query q;
        for (int i = 0; i < 6; ++i)
            if (p.keyword == kProps[i])
                q.property = static_cast<Property>(i);
        auto v = co_await ctx.getString("Value");
        if (!v.ok())
            co_return;
        q.value = v.text;
        cmd::KeywordOptions oo;
        oo.keywords = {"Equals", "Not"};
        oo.defaultKeyword = "Equals";
        auto o = co_await ctx.getKeyword("Objects that are [Equals/Not] <Equals>", std::move(oo));
        if (!o.ok())
            co_return;
        q.negate = o.keyword == "Not";
        if (!queryEntities(ctx.doc(), q)) {
            ctx.message("\"" + q.value + "\" is not a valid " + p.keyword + " value.");
            co_return;
        }
        conds.push_back(std::move(q));
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeFilter() {
    return std::make_unique<Filter>();
}

}  // namespace detail
}  // namespace tbcad::select
