#include "tbcad/blocks/attributes.h"

#include <cmath>
#include <unordered_set>

#include "tbcad/blocks/blocks.h"

namespace tbcad::blocks {

std::string normalizeTag(std::string_view tag) {
    std::string s(tag);
    for (char& c : s)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return s;
}

Result<void> validateTag(std::string_view tag) {
    if (tag.empty())
        return makeError(ErrorCode::InvalidArgument, "An attribute needs a tag");
    if (tag.size() > 255)
        return makeError(ErrorCode::InvalidArgument,
                         "An attribute tag can have at most 255 characters");
    for (char c : tag) {
        auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f)
            return makeError(ErrorCode::InvalidArgument,
                             "An attribute tag cannot contain spaces or control characters");
    }
    return {};
}

std::vector<const Entity*> attributeDefinitions(const Document& doc, BlockId block) {
    std::vector<const Entity*> out;
    const BlockDef* def = doc.block(block);
    if (!def)
        return out;
    for (EntityId id : def->entities)
        if (const Entity* e = doc.find(id))
            if (const auto* t = entityAs<TextData>(*e); t && t->attdef)
                out.push_back(e);
    return out;
}

bool hasEditableAttributes(const Document& doc, BlockId block) {
    for (const Entity* e : attributeDefinitions(doc, block))
        if (!entityAs<TextData>(*e)->attdef->constant)
            return true;
    return false;
}

std::string duplicateTag(const std::vector<const Entity*>& definitions) {
    std::unordered_set<std::string> seen;
    for (const Entity* e : definitions) {
        const std::string& tag = entityAs<TextData>(*e)->attdef->tag;
        if (!seen.insert(tag).second)
            return tag;
    }
    return {};
}

std::string attributeValue(const AttDefInfo& def, const InsertData& insert) {
    if (def.constant)
        return def.defaultValue;
    for (const AttribValue& a : insert.attribs)
        if (a.tag == def.tag)
            return a.value;
    return def.defaultValue;
}

std::vector<AttribValue> defaultAttribs(const Document& doc, BlockId block) {
    std::vector<AttribValue> out;
    for (const Entity* e : attributeDefinitions(doc, block)) {
        const AttDefInfo& a = *entityAs<TextData>(*e)->attdef;
        if (!a.constant)
            out.push_back({a.tag, a.defaultValue, {}});
    }
    return out;
}

std::vector<AttribValue> syncedAttribs(const Document& doc, const InsertData& insert) {
    std::vector<AttribValue> out;
    for (const Entity* e : attributeDefinitions(doc, insert.block)) {
        const AttDefInfo& a = *entityAs<TextData>(*e)->attdef;
        if (a.constant)
            continue;
        bool found = false;
        for (const AttribValue& v : insert.attribs)
            if (v.tag == a.tag) {
                out.push_back(v);
                found = true;
                break;
            }
        if (!found)
            out.push_back({a.tag, a.defaultValue});
    }
    return out;
}

std::size_t syncInserts(Transaction& tx, const Document& doc, BlockId block,
                        const std::vector<std::pair<std::string, std::string>>& renames) {
    std::vector<EntityId> ids;
    doc.forEachEntity([&](const Entity& e) {
        if (const auto* ins = entityAs<InsertData>(e); ins && ins->block == block)
            ids.push_back(e.common.id);
    });
    std::size_t changed = 0;
    for (EntityId id : ids) {
        const auto* ins = entityAs<InsertData>(*doc.find(id));
        InsertData renamed;
        const InsertData* source = ins;
        if (!renames.empty()) {
            renamed = *ins;
            for (const auto& [from, to] : renames)
                for (AttribValue& v : renamed.attribs)
                    if (v.tag == from)
                        v.tag = to;
            source = &renamed;
        }
        auto synced = syncedAttribs(doc, *source);
        bool same = synced.size() == ins->attribs.size();
        for (std::size_t i = 0; same && i < synced.size(); ++i)
            same = synced[i].tag == ins->attribs[i].tag &&
                   synced[i].value == ins->attribs[i].value &&
                   synced[i].offset == ins->attribs[i].offset;
        if (same)
            continue;
        if (tx.modify(id, [&](Entity& e) {
                  entityAs<InsertData>(e)->attribs = std::move(synced);
              }).ok())
            ++changed;
    }
    return changed;
}

std::optional<Entity> attributeText(const Entity& definition, const InsertData& insert,
                                    int attDisplay) {
    const auto* t = entityAs<TextData>(definition);
    if (!t || !t->attdef || attDisplay == 2 || (t->attdef->invisible && attDisplay != 1))
        return std::nullopt;
    Entity e = definition;
    auto* td = entityAs<TextData>(e);
    td->utf8 = attributeValue(*t->attdef, insert);
    if (t->attdef->multiLine && td->utf8.find('\n') != std::string::npos) {
        // Several lines show as multiline text anchored where the single line would sit.
        std::string markup;
        for (char c : td->utf8) {
            if (c == '\n')
                markup += "\\P";
            else if (c == '\\' || c == '{' || c == '}')
                markup += std::string("\\") + c;
            else if (c != '\r')
                markup += c;
        }
        const bool aligned = td->h != HAlign::Left || td->v != VAlign::Baseline;
        MTextData m;
        m.position = aligned ? td->alignPoint : td->position;
        if (!aligned)  // the baseline of a single line becomes the top of the first one
            m.position += Vec2{-std::sin(td->rotation), std::cos(td->rotation)} * td->height;
        m.height = td->height;
        m.rotation = td->rotation;
        m.style = td->style;
        m.utf8 = std::move(markup);
        m.attach = td->h == HAlign::Center || td->h == HAlign::Middle ? Attachment::TopCenter
                   : td->h == HAlign::Right                           ? Attachment::TopRight
                                                                      : Attachment::TopLeft;
        if (!t->attdef->constant)
            if (const Vec2 off = attributeOffset(*t->attdef, insert); off != Vec2{})
                m.position += off;
        e.data = Indirect<MTextData>(std::move(m));
        return e;
    }
    if (!t->attdef->constant)
        if (const Vec2 off = attributeOffset(*t->attdef, insert); off != Vec2{}) {
            td->position += off;
            td->alignPoint += off;
        }
    td->attdef.reset();
    return e;
}

Vec2 attributeOffset(const AttDefInfo& def, const InsertData& insert) {
    for (const AttribValue& a : insert.attribs)
        if (a.tag == def.tag)
            return a.offset;
    return {};
}

namespace {
// The shown, movable attributes of an Insert's block, in definition order.
std::vector<const TextData*> movableAttribs(const Document& doc, const InsertData& ins) {
    std::vector<const TextData*> out;
    for (const Entity* e : attributeDefinitions(doc, ins.block)) {
        const auto* t = entityAs<TextData>(*e);
        if (!t->attdef->constant && !t->attdef->invisible)
            out.push_back(t);
    }
    return out;
}
Vec2 anchorOf(const TextData& t) {
    return (t.h == HAlign::Left && t.v == VAlign::Baseline) ? t.position : t.alignPoint;
}
}  // namespace

std::vector<AttribHandle> attribHandles(const Document& doc, const Entity& insert) {
    std::vector<AttribHandle> out;
    const auto* ins = entityAs<InsertData>(insert);
    const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
    if (!def || ins->array)
        return out;
    const Transform2 t = instanceTransform(*ins, def->base);
    int k = 0;
    for (const TextData* td : movableAttribs(doc, *ins))
        out.push_back({k++, t.apply(anchorOf(*td) + attributeOffset(*td->attdef, *ins))});
    return out;
}

Result<Entity> attribDrag(const Document& doc, const Entity& insert, int index, Vec2 world) {
    const auto* ins = entityAs<InsertData>(insert);
    const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
    if (!def || ins->array)
        return makeError(ErrorCode::InvalidArgument, "This object has no movable attributes");
    const auto defs = movableAttribs(doc, *ins);
    if (index < 0 || static_cast<std::size_t>(index) >= defs.size())
        return makeError(ErrorCode::OutOfRange, "There is no such attribute");
    auto inv = instanceTransform(*ins, def->base).inverse();
    if (!inv)
        return makeError(ErrorCode::InvalidArgument, "The block reference is degenerate");
    const TextData& td = *defs[static_cast<std::size_t>(index)];
    Entity out = insert;
    auto& d = *entityAs<InsertData>(out);
    d.attribs = syncedAttribs(doc, *ins);
    for (AttribValue& v : d.attribs)
        if (v.tag == td.attdef->tag)
            v.offset = inv.value().apply(world) - anchorOf(td);
    return out;
}

}  // namespace tbcad::blocks

namespace tbcad::blocks {

std::string breaksToPrompt(std::string_view value) {
    std::string out;
    for (char c : value) {
        if (c == '\n')
            out += "\\P";
        else if (c != '\r')
            out += c;
    }
    return out;
}

std::string breaksFromPrompt(std::string_view typed) {
    std::string out;
    for (std::size_t i = 0; i < typed.size(); ++i) {
        if (typed[i] == '\\' && i + 1 < typed.size() &&
            (typed[i + 1] == 'P' || typed[i + 1] == 'p')) {
            out += '\n';
            ++i;
        } else {
            out += typed[i];
        }
    }
    return out;
}

}  // namespace tbcad::blocks
