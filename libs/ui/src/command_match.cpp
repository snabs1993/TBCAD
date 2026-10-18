#include "tbcad/ui/command_match.h"

#include <algorithm>
#include <cctype>

namespace tbcad::ui {

namespace {

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

bool isSubsequence(std::string_view query, std::string_view text) {
    std::size_t q = 0;
    for (char c : text) {
        if (q < query.size() && std::toupper(static_cast<unsigned char>(c)) ==
                                    std::toupper(static_cast<unsigned char>(query[q])))
            ++q;
    }
    return q == query.size();
}

std::size_t keywordKeyLength(const std::vector<std::string>& keywords, std::size_t index) {
    if (index >= keywords.size())
        return 1;
    const std::string mine = upper(keywords[index]);
    std::size_t need = 1;
    for (std::size_t i = 0; i < keywords.size(); ++i) {
        if (i == index)
            continue;
        const std::string other = upper(keywords[i]);
        std::size_t common = 0;
        while (common < mine.size() && common < other.size() && mine[common] == other[common])
            ++common;
        need = std::max(need, common + 1);
    }
    return std::min(need, std::max<std::size_t>(mine.size(), 1));
}

std::vector<CommandMatch> matchCommands(const cmd::Registry& registry, std::string_view queryText,
                                        std::size_t limit, const std::vector<std::string>& recent) {
    const std::string query = upper(queryText);
    std::vector<CommandMatch> out;
    for (const cmd::Command* command : registry.list()) {
        const std::string name(command->name());
        const std::vector<std::string> aliases = registry.aliasesOf(name);
        CommandMatch m;
        m.name = name;
        m.description = std::string(command->description());
        m.alias = aliases.empty() ? std::string{} : aliases.front();
        if (query.empty()) {
            m.rank = CommandMatch::Rank::NamePrefix;
            out.push_back(std::move(m));
            continue;
        }
        bool matched = false;
        for (const std::string& a : aliases)
            if (upper(a) == query) {
                m.rank = CommandMatch::Rank::ExactAlias;
                m.alias = a;
                matched = true;
                break;
            }
        if (!matched && name == query) {
            m.rank = CommandMatch::Rank::ExactAlias;
            matched = true;
        }
        if (!matched)
            for (const std::string& a : aliases)
                if (startsWith(upper(a), query)) {
                    m.rank = CommandMatch::Rank::AliasPrefix;
                    m.alias = a;
                    matched = true;
                    break;
                }
        if (!matched && startsWith(name, query)) {
            m.rank = CommandMatch::Rank::NamePrefix;
            matched = true;
        }
        if (!matched && query.size() >= 2 &&
            (isSubsequence(query, name) || upper(m.description).find(query) != std::string::npos)) {
            m.rank = CommandMatch::Rank::Fuzzy;
            matched = true;
        }
        if (matched)
            out.push_back(std::move(m));
    }
    const bool dashQuery = !query.empty() && query[0] == '-';
    auto recency = [&](const std::string& name) {
        const auto it = std::find(recent.begin(), recent.end(), name);
        return static_cast<std::size_t>(it - recent.begin());  // recent.size() = not used lately
    };
    std::stable_sort(out.begin(), out.end(), [&](const CommandMatch& a, const CommandMatch& b) {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        if (!query.empty()) {  // the palette (empty query) keeps its alphabetical order
            if (const auto ra = recency(a.name), rb = recency(b.name); ra != rb)
                return ra < rb;
            const bool da = a.name[0] == '-', db = b.name[0] == '-';
            if (da != db && !dashQuery)
                return !da;
            if (a.rank >= CommandMatch::Rank::NamePrefix && a.name.size() != b.name.size())
                return a.name.size() < b.name.size();
        }
        return false;
    });
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

}  // namespace tbcad::ui
