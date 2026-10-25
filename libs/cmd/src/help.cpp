#include "tbcad/cmd/help.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace tbcad::cmd {

namespace {

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trimmed(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return std::string(s.substr(b, e - b));
}

bool startsWith(const std::string& s, std::string_view prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

void append(std::string& field, const std::string& text) {
    if (!field.empty())
        field += ' ';
    field += text;
}

}  // namespace

void HelpCatalog::parse(std::string_view text) {
    std::istringstream in{std::string(text)};
    std::string line;
    HelpEntry cur;
    std::string* field = nullptr;
    auto flush = [&] {
        if (cur.name.empty())
            return;
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const HelpEntry& e) { return e.name == cur.name; });
        if (it != entries_.end())
            *it = cur;
        else
            entries_.push_back(cur);
    };
    while (std::getline(in, line)) {
        line = trimmed(line);
        if (startsWith(line, "## ")) {
            flush();
            cur = HelpEntry{};
            cur.name = upper(trimmed(line.substr(3)));
            field = &cur.summary;
        } else if (cur.name.empty() || line.empty()) {
            continue;
        } else if (startsWith(line, "Usage:")) {
            field = &cur.usage;
            append(*field, trimmed(line.substr(6)));
        } else if (startsWith(line, "Options:")) {
            field = &cur.options;
            append(*field, trimmed(line.substr(8)));
        } else if (startsWith(line, "Example:")) {
            field = &cur.example;
            append(*field, trimmed(line.substr(8)));
        } else if (field) {
            append(*field, line);
        }
    }
    flush();
    std::sort(entries_.begin(), entries_.end(),
              [](const HelpEntry& a, const HelpEntry& b) { return a.name < b.name; });
}

const HelpEntry* HelpCatalog::find(std::string_view name) const {
    const std::string up = upper(trimmed(name));
    for (const HelpEntry& e : entries_)
        if (e.name == up)
            return &e;
    return nullptr;
}

std::vector<const HelpEntry*> HelpCatalog::search(std::string_view query) const {
    std::vector<std::string> words;
    std::istringstream in{lower(query)};
    for (std::string w; in >> w;)
        words.push_back(w);
    std::vector<const HelpEntry*> byName, byText;
    for (const HelpEntry& e : entries_) {
        const std::string name = lower(e.name);
        const std::string all =
            name + ' ' + lower(e.summary + ' ' + e.usage + ' ' + e.options + ' ' + e.example);
        bool nameHit = false, allHit = true;
        for (const std::string& w : words) {
            if (name.find(w) != std::string::npos)
                nameHit = true;
            if (all.find(w) == std::string::npos)
                allHit = false;
        }
        if (words.empty() || (allHit && nameHit))
            byName.push_back(&e);
        else if (allHit)
            byText.push_back(&e);
    }
    byName.insert(byName.end(), byText.begin(), byText.end());
    return byName;
}

std::vector<std::string> helpLines(const HelpEntry& e) {
    std::vector<std::string> out;
    out.push_back(e.name + (e.summary.empty() ? "" : ": " + e.summary));
    if (!e.usage.empty())
        out.push_back("  Usage: " + e.usage);
    if (!e.options.empty())
        out.push_back("  Options: " + e.options);
    if (!e.example.empty())
        out.push_back("  Example: " + e.example);
    return out;
}

}  // namespace tbcad::cmd
