// importDxfGroups reads groups from memory; it must import exactly what importDxf imports from
// the same groups as text. Every corpus file is lexed, its groups put into a GroupBuffer, and
// both imports are compared.
#include <filesystem>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "tbcad/core/dump.h"
#include "tbcad/dxf/reader.h"

namespace tbcad::dxf {
namespace {

bool toBuffer(std::span<const std::byte> bytes, GroupBuffer& out) {
    Report scratch;
    auto opened = Lexer::open(bytes, scratch);
    if (!opened || opened.value().isBinary())
        return false;
    Lexer& lx = opened.value();
    while (auto g = lx.next()) {
        switch (g->type()) {
            case ValueType::Double:
                out.add(g->code, g->number());
                break;
            case ValueType::Int16:
            case ValueType::Int32:
            case ValueType::Int64:
            case ValueType::Bool:
                out.addInt(g->code, g->integer());
                break;
            case ValueType::Handle:
                out.add(g->code, g->str());
                break;
            case ValueType::String:
                out.add(g->code, g->str());
                break;
            case ValueType::Binary:
                return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("importDxfGroups gives the same drawing as importDxf of the same groups") {
    int compared = 0;
    for (const char* dir : {"reader", "synthetic"}) {
        const std::filesystem::path root = testing::corpusPath(dir);
        if (!std::filesystem::exists(root))
            continue;
        for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
            if (e.path().extension() != ".dxf")
                continue;
            const auto bytes = testing::readFile(e.path().string());
            GroupBuffer groups;
            if (!toBuffer(bytes, groups))
                continue;
            // The text the buffer stands for (what importDxf gets).
            const std::string text = groups.str();
            Report a, b;
            auto viaText =
                importDxf(std::span<const std::byte>(
                              reinterpret_cast<const std::byte*>(text.data()), text.size()),
                          ImportOptions{}, a);
            auto viaGroups = importDxfGroups(groups, ImportOptions{}, b);
            INFO(e.path().string());
            REQUIRE(viaText.ok() == viaGroups.ok());
            if (!viaText.ok())
                continue;
            CHECK(canonicalDump(*viaText.value().document) ==
                  canonicalDump(*viaGroups.value().document));
            CHECK(a.diagnostics().size() == b.diagnostics().size());
            ++compared;
        }
    }
    CHECK(compared > 10);
}

}  // namespace tbcad::dxf
