#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "tbcad/core/document.h"

namespace tbcad::sysvars {

// The drawing variables SETVAR knows, by name (case-insensitive) as text; nullopt when unknown.
std::optional<std::string> variableValue(const Document& doc, std::string_view name);
// Changes one inside `tx` (the caller owns the undo step). Empty string = done, else the message.
std::string setVariable(Transaction& tx, const Document& doc, std::string_view name,
                        std::string_view text);

}  // namespace tbcad::sysvars
