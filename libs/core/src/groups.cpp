#include "tbcad/core/groups.h"

#include <algorithm>
#include <charconv>
#include <sstream>
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

std::vector<EntityId> expandGroups(const Document& doc, std::span<const EntityId> ids) {
    std::vector<EntityId> out(ids.begin(), ids.end());
    if (doc.header().custom.find(kKey) == doc.header().custom.end())
        return out;
    std::unordered_set<EntityId> seen(out.begin(), out.end());
    std::vector<Group> groups = readGroups(doc);
    for (EntityId id : ids) {
        for (const Group& g : groups) {
            if (!g.selectable ||
                std::find(g.members.begin(), g.members.end(), id) == g.members.end())
                continue;
            for (EntityId m : g.members) {
                if (seen.insert(m).second)
                    out.push_back(m);
            }
        }
    }
    return out;
}

}  // namespace tbcad
