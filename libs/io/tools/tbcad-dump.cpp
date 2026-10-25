// tbcad-dump: prints a .tbcad file as readable text.
//   tbcad-dump file.tbcad            chunk list, report and canonical dump of the document
//   tbcad-dump --chunks file.tbcad   chunk list only
//   tbcad-dump --salvage file.tbcad  keep what can be read of a damaged file, list the losses

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"

int main(int argc, char** argv) {
    bool chunksOnly = false;
    bool salvage = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--chunks") == 0) {
            chunksOnly = true;
        } else if (std::strcmp(argv[i], "--salvage") == 0) {
            salvage = true;
        } else if (!path) {
            path = argv[i];
        } else {
            path = nullptr;
            break;
        }
    }
    if (!path) {
        std::cerr << "usage: tbcad-dump [--chunks] [--salvage] file.tbcad\n";
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
    if (!chunks && !salvage) {
        std::cerr << "tbcad-dump: " << chunks.error().message << "\n";
        return 1;
    }
    if (!chunks)
        std::printf("warning: chunk list unreadable (%s)\n", chunks.error().message.c_str());
    for (const auto& c : chunks ? chunks.value() : std::vector<tbcad::io::ChunkInfo>{}) {
        std::printf("chunk %s flags=%u stored=%llu crc=%s\n", c.type.c_str(), c.flags,
                    static_cast<unsigned long long>(c.storedSize), c.crcOk ? "ok" : "BAD");
    }
    if (chunksOnly)
        return 0;

    tbcad::io::IoReport report;
    tbcad::io::ReadOptions opt;
    opt.salvage = salvage;
    auto doc = tbcad::io::readTbcad(bytes, report, opt);
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
