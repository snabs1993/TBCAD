#include <algorithm>
#include <sstream>
#include <unordered_set>

#include "common.h"

namespace tbcad::select {

namespace {
constexpr const char* kPrefix = "filter.";
}  // namespace

std::optional<std::vector<EntityId>> filterEntities(const Document& doc,
                                                    const std::vector<Query>& queries) {
    std::vector<EntityId> out;
    std::unordered_set<EntityId> taken;
    std::optional<std::vector<EntityId>> group;  // what the current AND group still matches
    auto close = [&] {
        if (group)
            for (EntityId id : *group)
                if (taken.insert(id).second)
                    out.push_back(id);
        group.reset();
    };
    for (const Query& q : queries) {
        auto r = queryEntities(doc, q);
        if (!r)
            return std::nullopt;
        if (q.orPrevious)
            close();
        if (!group) {
            group = std::move(r);
            continue;
        }
        const std::unordered_set<EntityId> in(r->begin(), r->end());
        std::vector<EntityId> keep;
        for (EntityId id : *group)
            if (in.count(id))
                keep.push_back(id);
        group = std::move(keep);
    }
    close();
    return out;
}

// One condition per line: '|' when it starts an OR group, property index, '!' when negated (or
// the comparison), then the value.
std::string encodeFilter(const std::vector<Query>& queries) {
    std::string s;
    for (const Query& q : queries)
        s += std::string(q.orPrevious ? "|" : "") +
             std::string(1, static_cast<char>(static_cast<int>(q.property) < 10
                                                  ? '0' + static_cast<int>(q.property)
                                                  : 'a' + static_cast<int>(q.property) - 10)) +
             (q.negate                         ? "!"
              : q.compare == Compare::Greater  ? ">"
              : q.compare == Compare::Less     ? "<"
              : q.compare == Compare::Wildcard ? "~"
                                               : "=") +
             q.value + "\n";
    return s;
}

std::vector<Query> decodeFilter(const std::string& text) {
    std::vector<Query> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const bool orPrevious = !line.empty() && line[0] == '|';
        if (orPrevious)
            line.erase(0, 1);
        if (line.size() < 2)
            continue;
        const int idx = line[0] >= '0' && line[0] <= '9'   ? line[0] - '0'
                        : line[0] >= 'a' && line[0] <= 'z' ? line[0] - 'a' + 10
                                                           : -1;
        if (idx < 0 || idx >= detail::kPropCount)
            continue;
        Query q;
        q.property = static_cast<Property>(idx);
        q.negate = line[1] == '!';
        q.compare = line[1] == '>'   ? Compare::Greater
                    : line[1] == '<' ? Compare::Less
                    : line[1] == '~' ? Compare::Wildcard
                                     : Compare::Equals;
        q.value = line.substr(2);
        q.orPrevious = orPrevious;
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
    return std::string(kPropNames[static_cast<int>(q.property)]) +
           (q.negate                         ? " <> "
            : q.compare == Compare::Greater  ? " > "
            : q.compare == Compare::Less     ? " < "
            : q.compare == Compare::Wildcard ? " like "
                                             : " = ") +
           q.value;
}

// FILTER [FI]: build a list of conditions, select what matches, and optionally save the list
// under a name for later. Add joins a condition with AND, Or starts a new group joined with OR
// (AutoCAD's "** Begin OR"). Apply selects the matches in the whole drawing; SElect asks for the
// objects to look in first, as AutoCAD's Apply does.
class Filter final : public cmd::Command {
public:
    std::string_view name() const override { return "-FILTER"; }
    std::string_view description() const override {
        return "Select objects that satisfy a list of conditions (command line)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        std::vector<Query> conds;
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Add", "Or", "Remove", "Apply", "SElect", "Save", "Use"};
            const std::string def = conds.empty() ? "Add" : "Apply";
            ko.defaultKeyword = def;
            std::string head = conds.empty() ? "No conditions." : "";
            for (std::size_t i = 0; i < conds.size(); ++i)
                head += (i == 0                ? ""
                         : conds[i].orPrevious ? " OR "
                                               : " AND ") +
                        describeQuery(conds[i]);
            auto k = co_await ctx.getKeyword(
                head + " [Add/Or/Remove/Apply/SElect/Save/Use] <" + def + ">", std::move(ko));
            if (!k.ok())
                co_return;
            if (k.keyword == "Add" || k.keyword == "Or") {
                const std::size_t before = conds.size();
                co_await add(ctx, conds);
                if (k.keyword == "Or" && conds.size() > before && before > 0)
                    conds.back().orPrevious = true;
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
            } else {  // Apply, Select
                if (conds.empty()) {
                    ctx.message("Add a condition first.");
                    continue;
                }
                auto ids = filterEntities(ctx.doc(), conds);
                if (!ids) {
                    ctx.message("A condition has a value that cannot be read.");
                    continue;
                }
                if (k.keyword == "SElect") {
                    cmd::SelectionOptions so;
                    so.allowPreselection = false;
                    auto sel = co_await ctx.getSelection("Select objects to filter", so);
                    if (!sel.ok())
                        co_return;
                    const std::unordered_set<EntityId> within(sel.ids.begin(), sel.ids.end());
                    std::erase_if(*ids, [&](EntityId id) { return !within.count(id); });
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
        po.keywords.assign(kPropNames, kPropNames + kPropCount);
        po.defaultKeyword = "Type";
        auto p = co_await ctx.getKeyword(propertyPrompt(), std::move(po));
        if (!p.ok())
            co_return;
        Query q;
        for (int i = 0; i < kPropCount; ++i)
            if (p.keyword == kPropNames[i])
                q.property = static_cast<Property>(i);
        auto v = co_await ctx.getString("Value");
        if (!v.ok())
            co_return;
        q.value = v.text;
        bool opOk = false;
        co_await askOperator(ctx, q, "Objects that are", opOk);
        if (!opOk)
            co_return;
        if (!queryEntities(ctx.doc(), q)) {
            ctx.message("\"" + q.value + "\" is not a valid " + p.keyword + " value.");
            co_return;
        }
        conds.push_back(std::move(q));
    }
};

// FILTER [FI]: the Object Selection Filters dialog.
class FilterDialog final : public cmd::Command {
public:
    std::string_view name() const override { return "FILTER"; }
    std::vector<std::string> defaultAliases() const override { return {"FI"}; }
    std::string_view description() const override {
        return "Object Selection Filters: build, save and apply lists of conditions";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.host().openPanel("filter");
        co_return;
    }
};

// _FILTERSAVE (hidden): the dialog's Save As and Delete. Setting tmp.filter.save holds the name,
// a newline and the encoded conditions; no conditions deletes the named filter. One undo step.
class FilterSave final : public cmd::Command {
public:
    std::string_view name() const override { return "_FILTERSAVE"; }
    std::string_view description() const override { return {}; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const std::string spec = ctx.host().setting("tmp.filter.save").value_or("");
        ctx.host().setSetting("tmp.filter.save", "");
        const auto nl = spec.find('\n');
        const std::string name = cmd::toUpperAscii(cmd::trim(spec.substr(0, nl)));
        const std::string enc = nl == std::string::npos ? std::string() : spec.substr(nl + 1);
        if (name.empty())
            co_return;
        const std::string key = "filter." + name;
        const bool exists = ctx.doc().header().custom.count(key) != 0;
        if (enc.empty() && !exists)
            co_return;
        ctx.setUndoLabel("FILTER");
        (void)ctx.tx().modifyHeader([&](Header& h) {
            if (enc.empty())
                h.custom.erase(key);
            else
                h.custom[key] = enc;
        });
        ctx.message(enc.empty() ? "Filter " + name + " deleted." : "Filter " + name + " saved.");
        co_return;
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeFilter() {
    return std::make_unique<Filter>();
}

std::unique_ptr<cmd::Command> makeFilterDialog() {
    return std::make_unique<FilterDialog>();
}

std::unique_ptr<cmd::Command> makeFilterSave() {
    return std::make_unique<FilterSave>();
}

}  // namespace detail
}  // namespace tbcad::select
