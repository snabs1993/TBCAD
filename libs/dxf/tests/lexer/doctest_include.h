#pragma once

// MSVC needs the complete stream and string_view types before doctest.h declares its
// forward declarations of std::basic_ostream, so always include doctest through this header.
#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <string_view>
