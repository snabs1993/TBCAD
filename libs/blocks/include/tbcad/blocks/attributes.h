#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::blocks {

// Attributes: a Text inside a block definition with TextData::attdef set is an attribute
// definition. Each Insert carries its own value per tag in InsertData::attribs. The definition's
// text (position, height, style, rotation, alignment) says where and how the value is drawn.

// Upper-cases ASCII letters; leaves everything else (Thai included) untouched.
std::string normalizeTag(std::string_view tag);
// Non-empty, no spaces or control characters, at most 255 bytes. Expects a normalised tag.
Result<void> validateTag(std::string_view tag);

// The attribute definitions of a block, in definition order.
std::vector<const Entity*> attributeDefinitions(const Document& doc, BlockId block);
// True when at least one definition is not constant (the Insert has something to fill in).
bool hasEditableAttributes(const Document& doc, BlockId block);

// The first tag that two of the given definitions share; empty when all are unique.
std::string duplicateTag(const std::vector<const Entity*>& definitions);

// What an Insert shows for a definition: the fixed value of a constant one, else the Insert's
// value for the tag, else the definition's default.
std::string attributeValue(const AttDefInfo& def, const InsertData& insert);

// Values for a fresh Insert: one per non-constant definition, set to its default.
std::vector<AttribValue> defaultAttribs(const Document& doc, BlockId block);
// The Insert's attribs aligned with the block's current definitions: values of tags that still
// exist are kept, new tags get their default, removed and constant ones are dropped. Order
// follows the definitions.
std::vector<AttribValue> syncedAttribs(const Document& doc, const InsertData& insert);

// Sets the attribs of every Insert of `block` (model space and nested in other blocks) to
// syncedAttribs. `renames` (old tag, new tag, applied in order) first moves the values of tags that
// were renamed in the definition, so they are not lost. Returns how many Inserts changed.
std::size_t syncInserts(Transaction& tx, const Document& doc, BlockId block,
                        const std::vector<std::pair<std::string, std::string>>& renames = {});

// The text entity an attribute definition turns into for one Insert: the value in place of the
// tag, attdef cleared. Null when the attribute is invisible.
std::optional<Entity> attributeText(const Entity& definition, const InsertData& insert);

}  // namespace tbcad::blocks
