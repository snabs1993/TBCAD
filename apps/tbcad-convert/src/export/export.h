// .tbcad -> DXF half of tbcad-convert (W4-6). The command line itself is parsed in cli.cpp.
#pragma once

#include <iosfwd>
#include <string>

#include "tbcad/dxf/writer.h"

namespace tbcad::convert {

struct ExportArgs {
    std::string input{};
    std::string output{};
    std::string reportPath{};
    dxf::ExportOptions options{};
    bool quiet = false;
    bool timing = false;
};

// Exit codes as in cli.h: 0 written, 1 written with warnings, 2 failed.
int runExport(const ExportArgs& args, std::ostream& out, std::ostream& err);

}  // namespace tbcad::convert
