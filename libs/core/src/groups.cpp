#include "tbcad/core/groups.h"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace tbcad {

namespace {
constexpr const char* kKey = "tbcad.groups";

std::string clean(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    }
    return s;
}
}  // namespace

std::vector<Group> readGroups(const Document& doc) {
    std::vector<Group> out;
    auto it = doc.header().custom.find(kKey);
    if (it == doc.header().custom.end())
        return out;
    std::istringstream in(it->second);
    std::string line;
    while (std::getline(in, line)) {
        // name \t selectable \t id,id,id
        auto t1 = line.find('\t');
        auto t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos)
            continue;
        Group g;
        g.name = line.substr(0, t1);
        g.selectable = line.substr(t1 + 1, t2 - t1 - 1) != "0";
        std::string ids = line.substr(t2 + 1);
        std::size_t pos = 0;
        while (pos < ids.size()) {
            std::size_t end = ids.find(',', pos);
            if (end == std::string::npos)
                end = ids.size();
            std::uint64_t v = 0;
            auto r = std::from_chars(ids.data() + pos, ids.data() + end, v);
            if (r.ec == std::errc() && doc.contains(EntityId{v}))
                g.members.push_back(EntityId{v});
            pos = end + 1;
        }
        if (!g.members.empty())
            out.push_back(std::move(g));
    }
    return out;
}

Result<void> writeGroups(Transaction& tx, const std::vector<Group>& groups) {
    std::string text;
    for (const Group& g : groups) {
        if (g.members.empty())
            continue;
        text += clean(g.name) + '\t' + (g.selectable ? '1' : '0') + '\t';
        for (std::size_t i = 0; i < g.members.size(); ++i) {
            if (i)
                text += ',';
            text += std::to_string(g.members[i].value);
        }
        text += '\n';
    }
    return tx.modifyHeader([&](Header& h) {
        if (text.empty())
            h.custom.erase(kKey);
        else
            h.custom[kKey] = text;
    });
}

int pickStyle(const Header& h) {
    const auto it = h.custom.find(kPickStyleKey);
    if (it == h.custom.end() || it->second.size() != 1 || it->second[0] < '0' ||
        it->second[0] > '3')
        return 1;
    return it->second[0] - '0';
}

std::vector<EntityId> expandGroups(const Document& doc, std::span<const EntityId> ids) {
    std::vector<EntityId> out(ids.begin(), ids.end());
    const int style = pickStyle(doc.header());
    const bool groupsOn =
        (style & 1) != 0 && doc.header().custom.find(kKey) != doc.header().custom.end();
    const bool hatchesOn = (style & 2) != 0;
    if (!groupsOn && !hatchesOn)
        return out;
    std::unordered_set<EntityId> seen(out.begin(), out.end());
    if (hatchesOn) {
        for (EntityId id : ids) {
            const Entity* e = doc.find(id);
            const auto* h = e ? entityAs<HatchData>(*e) : nullptr;
            if (!h)
                continue;
            for (EntityId b : h->sourceBoundary) {
                if (doc.contains(b) && seen.insert(b).second)
                    out.push_back(b);
            }
        }
    }
    if (!groupsOn)
        return out;
    std::vector<Group> groups = readGroups(doc);
    // member -> the selectable groups it belongs to, so a big selection does not search every
    // group for every object.
    std::unordered_map<EntityId, std::vector<std::size_t>> groupsOf;
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        if (!groups[gi].selectable)
            continue;
        for (EntityId m : groups[gi].members)
            groupsOf[m].push_back(gi);
    }
    std::vector<bool> expanded(groups.size(), false);
    // (the hatch boundaries added above bring their groups too)
    const std::vector<EntityId> picked = out;
    for (EntityId id : picked) {
        const auto it = groupsOf.find(id);
        if (it == groupsOf.end())
            continue;
        for (std::size_t gi : it->second) {
            if (expanded[gi])
                continue;
            expanded[gi] = true;
            for (EntityId m : groups[gi].members) {
                if (seen.insert(m).second)
                    out.push_back(m);
            }
        }
    }
    return out;
}

}  // namespace tbcad
