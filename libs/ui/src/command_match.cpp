#include "tbcad/ui/command_match.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include "tbcad/ui/thai_words.h"

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

// Words people type for a command they do not know the name of (AutoCAD's synonym list).
struct Synonym {
    const char* word;
    const char* command;
};
constexpr Synonym kSynonyms[] = {
    {"ROUND", "FILLET"},        {"CORNER", "FILLET"},   {"BEVEL", "CHAMFER"},
    {"SYMBOL", "INSERT"},       {"DELETE", "ERASE"},    {"REMOVE", "ERASE"},
    {"DUPLICATE", "COPY"},      {"TURN", "ROTATE"},     {"SPIN", "ROTATE"},
    {"FLIP", "MIRROR"},         {"PARALLEL", "OFFSET"}, {"CUT", "TRIM"},
    {"RESIZE", "SCALE"},        {"ENLARGE", "SCALE"},   {"ELONGATE", "LENGTHEN"},
    {"NOTE", "MTEXT"},          {"LABEL", "MTEXT"},     {"WRITE", "MTEXT"},
    {"SQUARE", "RECTANG"},      {"BOX", "RECTANG"},     {"HEXAGON", "POLYGON"},
    {"PATTERN", "HATCH"},       {"RULER", "DIST"},      {"PRINT", "PLOT"},
    {"PICTURE", "IMAGEATTACH"}, {"CALCULATOR", "CAL"},  {"SHIFT", "MOVE"},
    {"GROUPS", "GROUP"},        {"COUNT", "QSELECT"},
};

bool hasNonAscii(std::string_view s) {
    return std::any_of(s.begin(), s.end(),
                       [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
}

// Optimal string alignment distance (Levenshtein plus swapping two neighbours), case already equal.
std::size_t typoDistance(const std::string& a, const std::string& b) {
    const std::size_t n = a.size(), m = b.size();
    std::vector<std::vector<std::size_t>> d(n + 1, std::vector<std::size_t>(m + 1));
    for (std::size_t i = 0; i <= n; ++i)
        d[i][0] = i;
    for (std::size_t j = 0; j <= m; ++j)
        d[0][j] = j;
    for (std::size_t i = 1; i <= n; ++i)
        for (std::size_t j = 1; j <= m; ++j) {
            const std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
        }
    return d[n][m];
}

bool isTypoOf(const std::string& query, const std::string& word) {
    if (query.size() < 3 || word.size() < 4)
        return false;
    const std::size_t allowed = word.size() >= 6 ? 2 : 1;
    const std::size_t gap = query.size() > word.size() ? query.size() - word.size()
                                                       : word.size() - query.size();
    return gap <= allowed && typoDistance(query, word) <= allowed;
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
    const bool thai = hasNonAscii(queryText);
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
        if (thai) {  // a Thai word: found through the Thai word list only
            for (const ThaiWord& w : thaiWords())
                if (name == w.command &&
                    std::string_view(w.word).find(queryText) != std::string_view::npos) {
                    m.rank = CommandMatch::Rank::Synonym;
                    m.synonym = w.word;
                    out.push_back(std::move(m));
                    break;
                }
            continue;
        }
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
        if (!matched && query.size() >= 3)
            for (const Synonym& syn : kSynonyms)
                if (name == syn.command && startsWith(syn.word, query)) {
                    m.rank = CommandMatch::Rank::Synonym;
                    m.synonym = syn.word;
                    matched = true;
                    break;
                }
        if (!matched && query.size() >= 2 && name.find(query) != std::string::npos) {
            m.rank = CommandMatch::Rank::MidString;
            matched = true;
        }
        if (!matched && isTypoOf(query, name)) {
            m.rank = CommandMatch::Rank::Typo;
            matched = true;
        }
        if (!matched)
            for (const std::string& a : aliases)
                if (isTypoOf(query, upper(a))) {
                    m.rank = CommandMatch::Rank::Typo;
                    m.alias = a;
                    matched = true;
                    break;
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
