#pragma once

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "tbcad/core/document.h"
#include "tbcad/io/tbcad.h"

// Fails the test case with the error message when a Result is an error.
#define TB_REQUIRE_OK(res)               \
    do {                                 \
        const auto& tb_r_ = (res);       \
        if (!tb_r_.ok())                 \
            FAIL(tb_r_.error().message); \
    } while (0)

namespace tbcad::testing {

// A document that uses every entity kind and every table, Thai strings, far coordinates, nested
// blocks, a sub-layer tree, draw-order overrides, hidden entities and gaps in all id sequences.
std::unique_ptr<Document> makeRichDocument();

// Writes then reads; fails the current test case on any error.
std::vector<std::uint8_t> writeOrFail(const Document& doc, const io::WriteOptions& o = {});
std::unique_ptr<Document> readOrFail(const std::vector<std::uint8_t>& bytes,
                                     io::IoReport* report = nullptr);

// Inserts an extra chunk (with a correct CRC) just before the END chunk.
std::vector<std::uint8_t> spliceChunkBeforeEnd(const std::vector<std::uint8_t>& file,
                                               const char (&type)[5], std::uint32_t flags,
                                               const std::vector<std::uint8_t>& payload);

// A fresh empty directory under the system temp directory, removed on destruction.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace tbcad::testing
