// A small, forgiving PDF reader: it finds objects by scanning the file (so a damaged or
// incrementally updated cross-reference table does not matter), reads object streams and
// decodes Flate / ASCIIHex / ASCII85 streams. Only what the vector underlay needs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace tbcad::underlay::pdf {

struct Obj;
using Dict = std::map<std::string, Obj>;

struct Obj {
    enum class Type : std::uint8_t { Null, Bool, Num, Name, Str, Arr, Dict, Stream, Ref };
    Type type = Type::Null;
    bool boolean = false;
    double num = 0;
    std::string str{};  // name (without the slash) or string bytes
    std::vector<Obj> arr{};
    std::shared_ptr<Dict> dict{};
    std::size_t streamOff = 0, streamLen = 0;  // into the file bytes (streams of the file only)
    std::shared_ptr<std::vector<std::uint8_t>>
        data{};  // decoded-ahead stream bytes (object streams)
    int refNum = 0;

    bool isNum() const { return type == Type::Num; }
    bool isName() const { return type == Type::Name; }
    bool isName(std::string_view n) const { return type == Type::Name && str == n; }
    bool isArr() const { return type == Type::Arr; }
    bool isDict() const { return type == Type::Dict || type == Type::Stream; }
    const Obj* get(std::string_view key) const;  // dictionary entry, unresolved
};

// Parses one object from `data` at `pos` (advanced). Inside content streams `allowRefs` is off, so
// "1 0 R" stays three numbers. Returns false at the end of input or on an unexpected delimiter;
// bare keywords (operators) come back as Type::Name with `isKeyword` set.
class Lexer {
public:
    explicit Lexer(std::string_view data, std::size_t pos = 0) : d_(data), p_(pos) {}
    std::size_t pos() const { return p_; }
    void seek(std::size_t p) { p_ = p; }
    bool atEnd();
    void skipSpace();
    // `keyword` is set when the token was a bare word such as "BT" or "stream" (returned as Name).
    bool next(Obj& out, bool& keyword, bool allowRefs, int depth = 0);

private:
    std::string_view d_;
    std::size_t p_ = 0;
};

class File {
public:
    // Reads the object table by scanning. False with `error` when nothing usable is found.
    bool open(std::string_view bytes, std::string& error);

    bool encrypted() const { return encrypted_; }
    // Follows references.
    const Obj& resolve(const Obj& o) const;
    const Obj* find(const Obj& dict, std::string_view key) const;  // resolved dictionary entry
    // Decoded stream bytes; empty for unsupported filters (sets `unsupported`).
    std::vector<std::uint8_t> decode(const Obj& stream, bool* unsupported = nullptr) const;

    struct Page {
        Obj dict{};  // with inherited MediaBox / CropBox / Resources / Rotate filled in
    };
    const std::vector<Page>& pages() const { return pages_; }
    std::string_view bytes() const { return bytes_; }
    const Obj& root() const { return root_; }

private:
    void readObjectStreams();
    void collectPages(const Obj& node, const Obj& inherited, int depth);

    std::string_view bytes_{};
    std::map<int, Obj> objs_{};
    Obj root_{};
    bool encrypted_ = false;
    std::vector<Page> pages_{};
    std::set<const Obj*> pageNodes_{};  // page tree nodes already visited
};

}  // namespace tbcad::underlay::pdf
