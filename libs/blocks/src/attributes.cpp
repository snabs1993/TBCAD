#include "tbcad/blocks/attributes.h"

#include <unordered_set>

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
            out.push_back({a.tag, a.defaultValue});
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

std::size_t syncInserts(Transaction& tx, const Document& doc, BlockId block) {
    std::vector<EntityId> ids;
    doc.forEachEntity([&](const Entity& e) {
        if (const auto* ins = entityAs<InsertData>(e); ins && ins->block == block)
            ids.push_back(e.common.id);
    });
    std::size_t changed = 0;
    for (EntityId id : ids) {
        const auto* ins = entityAs<InsertData>(*doc.find(id));
        auto synced = syncedAttribs(doc, *ins);
        bool same = synced.size() == ins->attribs.size();
        for (std::size_t i = 0; same && i < synced.size(); ++i)
            same = synced[i].tag == ins->attribs[i].tag && synced[i].value == ins->attribs[i].value;
        if (same)
            continue;
        if (tx.modify(id, [&](Entity& e) {
                  entityAs<InsertData>(e)->attribs = std::move(synced);
              }).ok())
            ++changed;
    }
    return changed;
}

std::optional<Entity> attributeText(const Entity& definition, const InsertData& insert) {
    const auto* t = entityAs<TextData>(definition);
    if (!t || !t->attdef || t->attdef->invisible)
        return std::nullopt;
    Entity e = definition;
    auto* td = entityAs<TextData>(e);
    td->utf8 = attributeValue(*t->attdef, insert);
    td->attdef.reset();
    return e;
}

}  // namespace tbcad::blocks
