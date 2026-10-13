#pragma once

#include <string_view>

#include "tbcad/text/text.h"

namespace tbcad::text::detail {

std::size_t decodeAt(std::string_view s, std::size_t i, char32_t& cp);
bool isExtend(char32_t c);
bool isPrepend(char32_t c);

}  // namespace tbcad::text::detail
