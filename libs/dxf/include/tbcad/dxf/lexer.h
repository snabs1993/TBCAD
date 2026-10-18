// Tolerant DXF tokenizer: turns the bytes of an ASCII or binary DXF file into a stream of
// (group code, value) pairs. It knows nothing about sections or entities.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "tbcad/dxf/report.h"

namespace tbcad::dxf {

enum class ValueType { String, Double, Int16, Int32, Int64, Bool, Handle, Binary };

// Type of the value for a group code, per the DXF group-code type table.
ValueType valueTypeForCode(int code);

// Hexadecimal handle (group codes 5, 105, 320-369, 390-399, 480-481). `text` is the raw
// field; `valid` is false when it is not hexadecimal (some DIMSTYLE groups reuse code 5 for
// names), in which case `value` is 0 and the caller should use `text`.
struct Handle {
    std::uint64_t value = 0;
    std::string_view text;
    bool valid = false;
};

// String values are slices of the input buffer: raw bytes, not yet decoded (see
// decodeText), valid only while the buffer given to Lexer::open is alive.
using Value = std::variant<std::string_view, double, std::int16_t, std::int32_t, std::int64_t, bool,
                           Handle, std::vector<std::byte>>;

struct Group {
    int code = 0;
    Value value;
    // 1-based line of the group code in ASCII DXF; byte offset of the group in binary DXF.
    int line = 0;

    ValueType type() const { return static_cast<ValueType>(value.index()); }
    // Typed accessors; return an empty/zero result when the value has another type.
    std::string_view str() const;
    double number() const;         // Double, or any integer type converted
    std::int64_t integer() const;  // any integer type, Bool, or Double truncated
};

// Groups produced in memory (by the DWG reader) and handed to the importer without being
// written out as DXF text and tokenized again. The values keep the meaning they would have
// as text: str() renders the same two lines per group a DXF file has, and Lexer::fromBuffer
// reads the groups back as if that text had been lexed.
class GroupBuffer {
public:
    enum class Kind : std::uint8_t { Text, Double, Int };

    void reserve(std::size_t groups, std::size_t textBytes) {
        items_.reserve(groups);
        arena_.reserve(textBytes);
    }
    // Text value; line breaks become spaces (a DXF value is one line).
    void add(int code, std::string_view s) {
        const std::size_t at = arena_.size();
        for (char c : s)
            arena_.push_back(c == '\n' || c == '\r' ? ' ' : c);
        items_.push_back({static_cast<std::int16_t>(code), Kind::Text,
                          static_cast<std::uint32_t>(arena_.size() - at),
                          static_cast<std::uint64_t>(at)});
    }
    void add(int code, double v) {
        Item it{static_cast<std::int16_t>(code), Kind::Double, 0, 0};
        std::memcpy(&it.payload, &v, 8);
        items_.push_back(it);
    }
    void addInt(int code, std::int64_t v) {
        items_.push_back(
            {static_cast<std::int16_t>(code), Kind::Int, 0, static_cast<std::uint64_t>(v)});
    }

    std::size_t size() const { return items_.size(); }
    // All the text values, back to back: what a byte-order or codepage check has to look at.
    std::string_view arena() const { return arena_; }
    // DXF text of the groups (as written to a file).
    std::string str() const;

private:
    friend class Lexer;
    struct Item {
        std::int16_t code;
        Kind kind;
        std::uint32_t len;      // Text: length in the arena
        std::uint64_t payload;  // Text: offset in the arena; Double: its bits; Int: the value
    };
    std::vector<Item> items_{};
    std::string arena_{};
};

class Lexer {
public:
    // Fails only when the input cannot be a DXF file at all (empty, or the first line is
    // not a group code). The buffer must outlive the Lexer and every Group it returns.
    static Result<Lexer> open(std::span<const std::byte> bytes, Report& report);
    // A lexer over groups already in memory; the buffer must outlive it and its groups.
    static Lexer fromBuffer(const GroupBuffer& buffer, Report& report);

    // Next group, or nullopt at "0 / EOF" (which is returned itself first) or end of input.
    // Never throws on bad input; problems go to the Report and the lexer recovers.
    std::optional<Group> next();

    bool isBinary() const { return binary_; }
    // Width in bytes of a binary group code (1 for R12, 2 for R13+); 0 for ASCII.
    int binaryCodeWidth() const { return codeWidth_; }
    bool atEnd() const { return done_; }

private:
    Lexer() = default;

    std::optional<Group> nextAscii();
    std::optional<Group> nextBuffered();
    std::string_view keep(std::string&& s);
    std::optional<Group> nextBinary();
    // Returns the next ASCII line (without terminator) and advances; nullopt at end.
    std::optional<std::string_view> readLine();
    void resyncAscii(int badLine);
    void resyncBinary(std::size_t badOffset);
    void finish();

    std::string_view data_;
    std::size_t pos_ = 0;
    int line_ = 1;
    Report* report_ = nullptr;
    bool binary_ = false;
    int codeWidth_ = 0;
    bool done_ = false;
    bool sawEof_ = false;
    const GroupBuffer* buffer_ = nullptr;
    std::size_t next_ = 0;
    std::deque<std::string> scratch_{};  // text of values that were not text in the buffer
};

}  // namespace tbcad::dxf
