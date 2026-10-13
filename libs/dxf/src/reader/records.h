// Groups of a DXF file collected into records (everything from one "0" group to the next).
#pragma once

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "tbcad/dxf/lexer.h"

namespace tbcad::dxf::detail {

struct Record {
    std::string_view type{};  // value of the leading "0" group (raw bytes)
    int line = 0;             // line (or byte offset) of the leading group
    std::vector<Group> g{};   // the groups after the leading "0" group

    void clear() {
        type = {};
        line = 0;
        g.clear();
    }
    // First group with this code, or nullptr.
    const Group* find(int code) const {
        for (const Group& x : g) {
            if (x.code == code)
                return &x;
        }
        return nullptr;
    }
    bool has(int code) const { return find(code) != nullptr; }
    double num(int code, double fallback = 0) const {
        const Group* x = find(code);
        return x ? x->number() : fallback;
    }
    std::int64_t integer(int code, std::int64_t fallback = 0) const {
        const Group* x = find(code);
        return x ? x->integer() : fallback;
    }
    std::string_view raw(int code) const {
        const Group* x = find(code);
        return x ? x->str() : std::string_view{};
    }
};

// Reads records from a lexer; supports pushing one record back.
class RecordReader {
public:
    explicit RecordReader(Lexer& lexer) : lexer_(lexer) {}

    // False at the end of the input. The record's type is "EOF" for the final "0 EOF".
    bool next(Record& out) {
        if (held_) {
            out = std::move(*held_);
            held_.reset();
            return true;
        }
        out.clear();
        if (!pending_) {
            // Skip stray groups before the first "0".
            while (true) {
                std::optional<Group> g = lexer_.next();
                if (!g)
                    return false;
                if (g->code == 0) {
                    pending_ = std::move(g);
                    break;
                }
            }
        }
        out.type = pending_->str();
        out.line = pending_->line;
        pending_.reset();
        while (true) {
            std::optional<Group> g = lexer_.next();
            if (!g)
                break;
            if (g->code == 0) {
                pending_ = std::move(g);
                break;
            }
            out.g.push_back(std::move(*g));
        }
        return true;
    }

    // The next call to next() returns `r` again.
    void pushBack(Record&& r) { held_ = std::move(r); }

private:
    Lexer& lexer_;
    std::optional<Group> pending_{};
    std::optional<Record> held_{};
};

}  // namespace tbcad::dxf::detail
