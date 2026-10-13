// The class, header and handle sections of R2004 and later files (ODA spec sections 9, 10, 23),
// read from the section buffers that readSections() returns.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "container/r2000.h"
#include "format.h"

namespace tbcad::dwg::detail {

std::vector<ClassInfo> parseClasses(Bytes section, const Format& fmt, int maintenance,
                                    std::vector<std::string>& notes);

HeaderVars parseHeader(Bytes section, const Format& fmt, int maintenance,
                       std::vector<std::string>& notes);

}  // namespace tbcad::dwg::detail
