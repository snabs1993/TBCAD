// SPELL: checks the English words in the selected text and mtext. Words with any non-ASCII letter
// (Thai, accents), digits, or all capitals are skipped, so a Thai drawing is never flagged. Words
// added with the Add keyword live in the drawing (Header::custom "tbcad.spell.words").
#include <cctype>
#include <set>
#include <sstream>

#include "commands.h"

namespace tbcad::textcmd {

namespace {

constexpr const char* kUserWords = "tbcad.spell.words";

// Common English plus the words that turn up on drawings. Lower case.
const std::set<std::string>& builtinWords() {
    static const std::set<std::string> words = [] {
        static const char* list =
            "a about above access add all along also an and any are area as at back bar base bath "
            "bathroom beam bed bedroom below beyond block bolt bottom boundary box bracket break "
            "building by cable ceiling center centre chamfer check circle clear column concrete "
            "connection construction contour cover cross cut dashed data date datum dead depth "
            "design detail diameter dimension door down drain drawing drawn each east edge "
            "elevation "
            "end enlarged entrance equal existing exit exterior fence field file finish fire first "
            "fixed flat floor foot footing for foundation frame from front garage gate general "
            "grade "
            "grid ground hall handrail height hidden hole horizontal house in inch inner "
            "insulation "
            "interior join joint kitchen label ladder landing layer left length level line living "
            "main material maximum minimum mm near new no north not note notes number of off "
            "office "
            "on only opening or outer over paint pipe plan plate plot plumbing point post radius "
            "rail railing ramp reference reinforcement revision rib right road roof room scale "
            "schedule screw second section see sheet side size slab slope south space spacing "
            "specified stair stairs standard steel step stone structure structural support symbol "
            "table tank tee text the then thick thickness this through title to toilet top total "
            "type typical unit up use vertical wall water west wide width window with without wood "
            "work yard zone one two three four five six seven eight nine ten per plus minus is be "
            "if it its we you your our their they them these those that than too very can "
            "will must should may shall have has had do does done made make using used unless "
            "until "
            "when where which while who whose am pm yes been being was were what both either "
            "neither other another such same some more most less least few many much how why into "
            "onto upon within across around before after during between against among via etc "
            "checked approved client project address issue issued "
            "tolerance surface rough smooth thread drill ream bore tap hex nut washer pin "
            "shaft gear spring bearing weld fillet groove seam rivet hardness";
        std::set<std::string> out;
        std::istringstream in(list);
        std::string w;
        while (in >> w)
            out.insert(w);
        return out;
    }();
    return words;
}

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Span {
    std::size_t begin = 0, end = 0;
    std::string word;
};

bool isLetter(unsigned char c) {
    return std::isalpha(c) != 0;
}

// The candidate words of `text`: ASCII letters only (an apostrophe inside a word is kept), at
// least three letters, not all capitals, never glued to a non-ASCII byte, a digit or '_'. MText
// codes such as \fArial; and \P are not words.
std::vector<Span> candidates(const std::string& text, bool markup) {
    std::vector<Span> out;
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (markup && c == '\\' && i + 1 < n) {
            const unsigned char k = static_cast<unsigned char>(text[i + 1]);
            if (isLetter(k) && k != 'P' && k != 'N') {
                while (i < n && text[i] != ';')
                    ++i;
            }
            i += 2;
            continue;
        }
        if (!(c >= 0x80 || std::isalnum(c) || c == '_' || c == '\'')) {
            ++i;
            continue;
        }
        // One token: everything up to a space or an ASCII symbol.
        const std::size_t b = i;
        bool plain = true;
        while (i < n) {
            const unsigned char d = static_cast<unsigned char>(text[i]);
            if (d >= 0x80 || std::isdigit(d) || d == '_')
                plain = false;
            else if (!isLetter(d) && d != '\'')
                break;
            ++i;
        }
        std::size_t s = b, e = i;
        while (e > s && text[e - 1] == '\'')
            --e;
        while (s < e && text[s] == '\'')
            ++s;
        if (!plain || e - s < 3)
            continue;
        bool allCaps = true;
        for (std::size_t k = s; k < e; ++k)
            allCaps = allCaps && !std::islower(static_cast<unsigned char>(text[k]));
        if (allCaps)
            continue;
        out.push_back({s, e, text.substr(s, e - s)});
    }
    return out;
}

std::set<std::string> userWords(const Document& doc) {
    std::set<std::string> out;
    const auto it = doc.header().custom.find(kUserWords);
    if (it != doc.header().custom.end()) {
        std::istringstream in(it->second);
        std::string w;
        while (in >> w)
            out.insert(w);
    }
    return out;
}

bool known(const std::string& word, const std::set<std::string>& user) {
    const std::string w = lower(word);
    if (builtinWords().count(w) || user.count(w))
        return true;
    // Plurals, -ed, -ing and possessives of a known word.
    auto strip = [&](const std::string& suf, const std::string& add) {
        if (w.size() > suf.size() + 1 && w.compare(w.size() - suf.size(), suf.size(), suf) == 0) {
            const std::string root = w.substr(0, w.size() - suf.size()) + add;
            return builtinWords().count(root) > 0 || user.count(root) > 0;
        }
        return false;
    };
    return strip("'s", "") || strip("s", "") || strip("es", "") || strip("ed", "") ||
           strip("ed", "e") || strip("ing", "") || strip("ing", "e") || strip("ies", "y");
}

class SpellCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SPELL"; }
    std::string_view description() const override {
        return "Check the spelling of text and mtext (skips Thai and other non-English text)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [](const Entity& e) {
            const auto* t = entityAs<TextData>(e);
            return entityAs<MTextData>(e) != nullptr || (t && !t->attdef);
        };
        auto sel = co_await ctx.getSelection("Select text to check", so);
        if (!sel.ok())
            co_return;
        ctx.setUndoLabel("SPELL");
        std::set<std::string> user = userWords(ctx.doc());
        std::set<std::string> ignored;
        std::size_t found = 0, changed = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            std::string text;
            bool markup = false;
            if (const auto* t = entityAs<TextData>(*e)) {
                text = t->utf8;
            } else if (const auto* m = entityAs<MTextData>(*e)) {
                text = m->utf8;
                markup = true;
            }
            bool edited = false;
            // From the end, so a replacement does not move the words still to be checked.
            const std::vector<Span> spans = candidates(text, markup);
            for (std::size_t k = spans.size(); k-- > 0;) {
                const Span sp = spans[k];
                const std::string key = lower(sp.word);
                if (known(sp.word, user) || ignored.count(key))
                    continue;
                ++found;
                cmd::KeywordOptions ko;
                ko.keywords = {"Ignore", "Change", "Add", "All"};
                ko.defaultKeyword = "Ignore";
                auto a = co_await ctx.getKeyword(
                    "Not in dictionary: \"" + sp.word + "\" [Ignore/Change/Add/All]", ko);
                if (a.status == cmd::Status::Cancel)
                    co_return;
                if (!a.ok() || a.keyword == "Ignore") {
                    continue;
                } else if (a.keyword == "All") {
                    ignored.insert(key);
                } else if (a.keyword == "Add") {
                    user.insert(key);
                    std::string all;
                    for (const std::string& w : user)
                        all += (all.empty() ? "" : " ") + w;
                    (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[kUserWords] = all; });
                } else {
                    auto repl = co_await ctx.getString("Replace \"" + sp.word + "\" with");
                    if (!repl.ok())
                        co_return;
                    text.replace(sp.begin, sp.end - sp.begin, repl.text);
                    edited = true;
                    ++changed;
                }
            }
            if (edited)
                (void)ctx.tx().modify(id, [&](Entity& en) {
                    if (auto* t = entityAs<TextData>(en))
                        t->utf8 = text;
                    else if (auto* m = entityAs<MTextData>(en))
                        m->utf8 = text;
                });
        }
        ctx.message(found == 0 ? "No spelling mistakes found."
                               : std::to_string(found) + " word(s) not in the dictionary, " +
                                     std::to_string(changed) + " changed.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeSpellCommand() {
    return std::make_unique<SpellCommand>();
}

}  // namespace tbcad::textcmd
