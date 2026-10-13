// tbcad-dump: prints a .tbcad file as readable text.
//   tbcad-dump file.tbcad            chunk list, report and canonical dump of the document
//   tbcad-dump --chunks file.tbcad   chunk list only

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"

int main(int argc, char** argv) {
    bool chunksOnly = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--chunks") == 0) {
            chunksOnly = true;
        } else if (!path) {
            path = argv[i];
        } else {
            path = nullptr;
            break;
        }
    }
    if (!path) {
        std::cerr << "usage: tbcad-dump [--chunks] file.tbcad\n";
        return 2;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "tbcad-dump: cannot open " << path << "\n";
        return 2;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());

    auto chunks = tbcad::io::listChunks(bytes);
    if (!chunks) {
        std::cerr << "tbcad-dump: " << chunks.error().message << "\n";
        return 1;
    }
    for (const auto& c : chunks.value()) {
        std::printf("chunk %s flags=%u stored=%llu crc=%s\n", c.type.c_str(), c.flags,
                    static_cast<unsigned long long>(c.storedSize), c.crcOk ? "ok" : "BAD");
    }
    if (chunksOnly)
        return 0;

    tbcad::io::IoReport report;
    auto doc = tbcad::io::readTbcad(bytes, report);
    if (!doc) {
        std::cerr << "tbcad-dump: " << doc.error().message << "\n";
        return 1;
    }
    std::printf("format %u.%u entities=%zu unknownChunks=%zu unknownEntityKinds=%zu\n",
                report.formatMajor, report.formatMinor, report.entityCount, report.unknownChunks,
                report.unknownEntityKinds);
    for (const auto& w : report.warnings)
        std::printf("warning: %s\n", w.c_str());
    std::fputs(tbcad::canonicalDump(*doc.value()).c_str(), stdout);
    return 0;
}
