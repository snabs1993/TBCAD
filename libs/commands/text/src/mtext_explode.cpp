#include "commands.h"
#include "tbcad/blocks/explode.h"
#include "tbcad/text/text.h"

namespace tbcad::textcmd {

namespace {

// EXPLODE on multiline text: one single-line Text per line, where the line sits now.
Result<blocks::ExplodeResult> explodeMText(const Document& doc, const Entity& e) {
    const auto* m = entityAs<MTextData>(e);
    if (!m)
        return Error{ErrorCode::InvalidArgument, "Not multiline text."};
    const text::TextLayout lay = text::layoutMText(doc, *m);
    blocks::ExplodeResult out;
    for (const text::LayoutLine& ln : lay.lines) {
        if (ln.text.empty())
            continue;
        TextData t;
        t.position = lay.toWorld.apply({ln.x0, ln.baseline});
        t.height = ln.height > 0 ? ln.height : m->height;
        t.rotation = m->rotation;
        t.style = m->style;
        t.utf8 = ln.text;
        Entity part;
        part.common = e.common;
        part.common.id = {};
        part.data = std::move(t);
        out.entities.push_back(std::move(part));
    }
    if (out.entities.empty())
        return Error{ErrorCode::InvalidArgument, "The text is empty."};
    if (m->utf8.find('{') != std::string::npos || m->utf8.find("\\S") != std::string::npos ||
        m->utf8.find("\\L") != std::string::npos || m->utf8.find("\\O") != std::string::npos)
        out.messages.push_back(
            "Bold, italic, colour, underline and stacked fractions were dropped.");
    return out;
}

}  // namespace

void registerMTextExplode() {
    blocks::ExplodeRegistry::set(EntityKind::MText, explodeMText);
}

}  // namespace tbcad::textcmd
