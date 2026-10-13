#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tbcad/cmd/cmd.h"
#include "tbcad/core/core.h"

namespace tbcad::dimcmd {

std::unique_ptr<cmd::Command> makeDimStyleCommand();

// Applies one DIMSTYLE property. `key` is one of propertyKeywords() (matched case-insensitively);
// `value` is the text the user typed ('_' stands for a space in prefix and suffix). Returns an
// empty string on success, else the reason.
std::string applyProperty(const Document& doc, DimStyle& style, std::string_view key,
                          std::string_view value);

const std::vector<std::string>& propertyKeywords();

}  // namespace tbcad::dimcmd
