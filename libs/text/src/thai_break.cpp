#include <algorithm>
#include <fstream>
#include <mutex>
#include <unordered_set>

#include "text_internal.h"

namespace tbcad::text {

namespace {

struct Dictionary {
    std::unordered_set<std::string> words;
    std::size_t maxClusters = 0;
    bool loaded = false;
};

std::mutex g_mutex;
Dictionary g_dict;
std::filesystem::path g_dir;
bool g_tried = false;

bool isThaiLetterOrMark(char32_t c) {
    return c >= 0x0E01 && c <= 0x0E4E;
}

void installWords(std::unordered_set<std::string> words) {
    g_dict.words = std::move(words);
    g_dict.maxClusters = 0;
    for (const std::string& w : g_dict.words)
        g_dict.maxClusters = std::max(g_dict.maxClusters, clusterBoundaries(w).size() - 1);
    g_dict.loaded = !g_dict.words.empty();
}

std::size_t loadFileLocked(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return 0;
    std::unordered_set<std::string> words;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.size() >= 3)
            words.insert(line);
    }
    std::size_t n = words.size();
    installWords(std::move(words));
    return n;
}

bool ensureLoadedLocked() {
    if (g_dict.loaded)
        return true;
    if (g_tried)
        return false;
    g_tried = true;
    std::vector<std::filesystem::path> dirs;
    if (!g_dir.empty())
        dirs.push_back(g_dir);
#ifdef TBCAD_TEXT_ASSETS_DIR
    dirs.emplace_back(TBCAD_TEXT_ASSETS_DIR);
#endif
    dirs.emplace_back("assets/dictionaries");
    for (const auto& d : dirs)
        if (loadFileLocked(d / "thai-words.txt") > 0)
            return true;
    return false;
}

}  // namespace

void setDictionaryDirectory(std::filesystem::path dir) {
    std::lock_guard lock(g_mutex);
    g_dir = std::move(dir);
    g_dict = Dictionary{};
    g_tried = false;
}

std::size_t loadThaiWordList(const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    g_tried = true;
    return loadFileLocked(file);
}

bool thaiWordListAvailable() {
    std::lock_guard lock(g_mutex);
    return ensureLoadedLocked();
}

std::vector<std::size_t> thaiWordBreaks(std::string_view s) {
    std::vector<std::size_t> out;
    std::lock_guard lock(g_mutex);
    if (!ensureLoadedLocked())
        return out;

    std::vector<std::size_t> cb = clusterBoundaries(s);
    const std::size_t n = cb.size() - 1;  // clusters
    if (n < 2)
        return out;

    // Which clusters belong to Thai runs? A run is a maximal sequence of clusters whose first
    // character is a Thai letter.
    auto isThaiCluster = [&](std::size_t i) {
        char32_t cp = 0;
        detail::decodeAt(s, cb[i], cp);
        return isThaiLetterOrMark(cp);
    };

    std::size_t i = 0;
    while (i < n) {
        if (!isThaiCluster(i)) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && isThaiCluster(j))
            ++j;
        // Segment clusters [i, j) by dynamic programming over (unknown clusters, words).
        const std::size_t m = j - i;
        struct Cell {
            int unknown = 1 << 28;
            int words = 1 << 28;
            std::size_t from = 0;
            bool known = false;  // the last token is a dictionary word
        };
        std::vector<Cell> dp(m + 1);
        dp[0] = {0, 0, 0, false};
        auto better = [](const Cell& a, const Cell& b) {
            return a.unknown != b.unknown ? a.unknown < b.unknown : a.words < b.words;
        };
        for (std::size_t e = 1; e <= m; ++e) {
            // An unknown cluster extends the previous segmentation.
            {
                Cell c = dp[e - 1];
                c.unknown += 1;
                c.words += 1;
                c.from = e - 1;
                c.known = false;
                dp[e] = c;
            }
            std::size_t lo = e > g_dict.maxClusters ? e - g_dict.maxClusters : 0;
            for (std::size_t b = lo; b + 1 <= e; ++b) {
                std::string_view w = s.substr(cb[i + b], cb[i + e] - cb[i + b]);
                if (e - b < 2 && w.size() < 6)
                    continue;  // single short clusters are not words
                if (!g_dict.words.count(std::string(w)))
                    continue;
                Cell c = dp[b];
                c.words += 1;
                c.from = b;
                c.known = true;
                if (better(c, dp[e]))
                    dp[e] = c;
            }
        }
        // Walk back, emitting a break between tokens when at least one side is a known word.
        std::vector<std::pair<std::size_t, bool>> toks;  // (end cluster, known)
        for (std::size_t e = m; e > 0; e = dp[e].from)
            toks.emplace_back(e, dp[e].known);
        std::reverse(toks.begin(), toks.end());
        for (std::size_t t = 0; t + 1 < toks.size(); ++t) {
            std::size_t end = toks[t].first;
            bool leftKnown = toks[t].second, rightKnown = toks[t + 1].second;
            if (!leftKnown && !rightKnown)
                continue;  // two unknown tokens: keep them together
            // Never break before a repetition mark / abbreviation sign.
            char32_t next = 0;
            detail::decodeAt(s, cb[i + end], next);
            if (next == 0x0E46 || next == 0x0E2F)
                continue;
            out.push_back(cb[i + end]);
        }
        i = j;
    }
    return out;
}

}  // namespace tbcad::text
