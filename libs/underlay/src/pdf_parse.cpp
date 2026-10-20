#include "pdf_parse.h"

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace tbcad::underlay::pdf {

namespace {

constexpr int kMaxDepth = 64;
constexpr std::size_t kMaxDecoded = 256u * 1024 * 1024;

bool isSpace(char c) {
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}
bool isDelim(char c) {
    switch (c) {
        case '(':
        case ')':
        case '<':
        case '>':
        case '[':
        case ']':
        case '{':
        case '}':
        case '/':
        case '%':
            return true;
        default:
            return false;
    }
}
bool isDigit(char c) {
    return c >= '0' && c <= '9';
}
int hexVal(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

const Obj kNull{};

}  // namespace

const Obj* Obj::get(std::string_view key) const {
    if (!dict)
        return nullptr;
    auto it = dict->find(std::string(key));
    return it == dict->end() ? nullptr : &it->second;
}

void Lexer::skipSpace() {
    while (p_ < d_.size()) {
        char c = d_[p_];
        if (isSpace(c)) {
            ++p_;
        } else if (c == '%') {
            while (p_ < d_.size() && d_[p_] != '\n' && d_[p_] != '\r')
                ++p_;
        } else {
            break;
        }
    }
}

bool Lexer::atEnd() {
    skipSpace();
    return p_ >= d_.size();
}

bool Lexer::next(Obj& out, bool& keyword, bool allowRefs, int depth) {
    keyword = false;
    out = Obj{};
    if (depth > kMaxDepth)
        return false;
    for (;;) {
        skipSpace();
        if (p_ >= d_.size())
            return false;
        char c = d_[p_];
        if (c == '/') {
            ++p_;
            std::string name;
            while (p_ < d_.size() && !isSpace(d_[p_]) && !isDelim(d_[p_])) {
                char ch = d_[p_++];
                if (ch == '#' && p_ + 1 < d_.size() && hexVal(d_[p_]) >= 0 &&
                    hexVal(d_[p_ + 1]) >= 0) {
                    ch = static_cast<char>(hexVal(d_[p_]) * 16 + hexVal(d_[p_ + 1]));
                    p_ += 2;
                }
                name.push_back(ch);
            }
            out.type = Obj::Type::Name;
            out.str = std::move(name);
            return true;
        }
        if (c == '(') {
            ++p_;
            std::string s;
            int level = 1;
            while (p_ < d_.size()) {
                char ch = d_[p_++];
                if (ch == '\\' && p_ < d_.size()) {
                    char e = d_[p_++];
                    switch (e) {
                        case 'n':
                            s.push_back('\n');
                            break;
                        case 'r':
                            s.push_back('\r');
                            break;
                        case 't':
                            s.push_back('\t');
                            break;
                        case 'b':
                            s.push_back('\b');
                            break;
                        case 'f':
                            s.push_back('\f');
                            break;
                        case '\r':
                            if (p_ < d_.size() && d_[p_] == '\n')
                                ++p_;
                            break;
                        case '\n':
                            break;
                        default:
                            if (e >= '0' && e <= '7') {
                                int v = e - '0';
                                for (int k = 0;
                                     k < 2 && p_ < d_.size() && d_[p_] >= '0' && d_[p_] <= '7'; ++k)
                                    v = v * 8 + (d_[p_++] - '0');
                                s.push_back(static_cast<char>(v & 0xFF));
                            } else {
                                s.push_back(e);
                            }
                    }
                } else if (ch == '(') {
                    ++level;
                    s.push_back(ch);
                } else if (ch == ')') {
                    if (--level == 0)
                        break;
                    s.push_back(ch);
                } else {
                    s.push_back(ch);
                }
            }
            out.type = Obj::Type::Str;
            out.str = std::move(s);
            return true;
        }
        if (c == '<') {
            if (p_ + 1 < d_.size() && d_[p_ + 1] == '<') {
                p_ += 2;
                out.type = Obj::Type::Dict;
                out.dict = std::make_shared<Dict>();
                for (;;) {
                    skipSpace();
                    if (p_ >= d_.size())
                        return true;  // truncated dictionary: keep what we have
                    if (d_[p_] == '>' && p_ + 1 < d_.size() && d_[p_ + 1] == '>') {
                        p_ += 2;
                        return true;
                    }
                    Obj key, val;
                    bool kw = false;
                    if (!next(key, kw, allowRefs, depth + 1))
                        return true;
                    if (kw || key.type != Obj::Type::Name)
                        continue;  // junk between entries
                    if (!next(val, kw, allowRefs, depth + 1))
                        return true;
                    if (kw && val.type == Obj::Type::Name)
                        continue;  // a stray operator word (true, false and null are values)
                    (*out.dict)[key.str] = std::move(val);
                }
            }
            ++p_;
            std::string s;
            int hi = -1;
            while (p_ < d_.size() && d_[p_] != '>') {
                int h = hexVal(d_[p_++]);
                if (h < 0)
                    continue;
                if (hi < 0) {
                    hi = h;
                } else {
                    s.push_back(static_cast<char>(hi * 16 + h));
                    hi = -1;
                }
            }
            if (hi >= 0)
                s.push_back(static_cast<char>(hi * 16));
            if (p_ < d_.size())
                ++p_;
            out.type = Obj::Type::Str;
            out.str = std::move(s);
            return true;
        }
        if (c == '[') {
            ++p_;
            out.type = Obj::Type::Arr;
            for (;;) {
                skipSpace();
                if (p_ >= d_.size())
                    return true;
                if (d_[p_] == ']') {
                    ++p_;
                    return true;
                }
                Obj el;
                bool kw = false;
                if (!next(el, kw, allowRefs, depth + 1))
                    return true;
                if (kw && el.type == Obj::Type::Name)
                    continue;
                out.arr.push_back(std::move(el));
            }
        }
        if (c == ']' || c == '>' || c == ')' || c == '{' || c == '}') {
            ++p_;  // stray delimiter
            continue;
        }
        // number or keyword
        std::size_t start = p_;
        while (p_ < d_.size() && !isSpace(d_[p_]) && !isDelim(d_[p_]))
            ++p_;
        std::string_view tok = d_.substr(start, p_ - start);
        bool numeric = !tok.empty();
        bool seenDot = false, seenDigit = false;
        for (std::size_t i = 0; i < tok.size() && numeric; ++i) {
            char ch = tok[i];
            if (isDigit(ch))
                seenDigit = true;
            else if ((ch == '+' || ch == '-') && i == 0)
                continue;
            else if (ch == '.' && !seenDot)
                seenDot = true;
            else
                numeric = false;
        }
        if (numeric && seenDigit) {
            out.type = Obj::Type::Num;
            out.num = std::strtod(std::string(tok).c_str(), nullptr);
            if (allowRefs && !seenDot && out.num >= 0 && out.num < 1e9) {
                std::size_t save = p_;
                skipSpace();
                std::size_t g0 = p_;
                while (p_ < d_.size() && isDigit(d_[p_]))
                    ++p_;
                if (p_ > g0 && p_ - g0 < 6) {
                    skipSpace();
                    if (p_ < d_.size() && d_[p_] == 'R' &&
                        (p_ + 1 >= d_.size() || isSpace(d_[p_ + 1]) || isDelim(d_[p_ + 1]))) {
                        ++p_;
                        out.type = Obj::Type::Ref;
                        out.refNum = static_cast<int>(out.num);
                        out.num = 0;
                        return true;
                    }
                }
                p_ = save;
            }
            return true;
        }
        if (tok == "true" || tok == "false") {
            out.type = Obj::Type::Bool;
            out.boolean = tok == "true";
            keyword = true;
            out.str = std::string(tok);
            return true;
        }
        if (tok == "null") {
            keyword = true;
            out.str = "null";
            return true;
        }
        out.type = Obj::Type::Name;
        out.str = std::string(tok);
        keyword = true;
        return true;
    }
}

// ---- File -----------------------------------------------------------------------------------

namespace {

std::size_t findToken(std::string_view d, std::string_view tok, std::size_t from) {
    return d.find(tok, from);
}

}  // namespace

bool File::open(std::string_view bytes, std::string& error) {
    bytes_ = bytes;
    objs_.clear();
    pages_.clear();
    pageNodes_.clear();
    encrypted_ = false;
    if (bytes.size() < 8 || bytes.find("%PDF-") == std::string_view::npos) {
        error = "This is not a PDF file";
        return false;
    }
    Obj trailerRoot;
    bool haveRoot = false;

    // 1. Indirect objects.
    std::size_t i = 0;
    while (i < bytes.size()) {
        std::size_t k = findToken(bytes, "obj", i);
        if (k == std::string_view::npos)
            break;
        i = k + 3;
        if (k >= 1 && bytes[k - 1] == 'd')
            continue;  // endobj
        if (k + 3 < bytes.size() && !isSpace(bytes[k + 3]) && !isDelim(bytes[k + 3]) &&
            bytes[k + 3] != '<')
            continue;
        // "<num> <gen> obj" before k
        std::size_t q = k;
        while (q > 0 && isSpace(bytes[q - 1]))
            --q;
        std::size_t genEnd = q;
        while (q > 0 && isDigit(bytes[q - 1]))
            --q;
        if (q == genEnd)
            continue;
        std::size_t genStart = q;
        (void)genStart;
        while (q > 0 && isSpace(bytes[q - 1]))
            --q;
        std::size_t numEnd = q;
        while (q > 0 && isDigit(bytes[q - 1]))
            --q;
        if (q == numEnd || numEnd - q > 9)
            continue;
        if (q > 0 && !isSpace(bytes[q - 1]) && !isDelim(bytes[q - 1]))
            continue;
        int num = std::atoi(std::string(bytes.substr(q, numEnd - q)).c_str());
        Lexer lx(bytes, k + 3);
        Obj obj;
        bool kw = false;
        if (!lx.next(obj, kw, true))
            continue;
        if (kw)
            continue;
        if (obj.type == Obj::Type::Dict) {
            std::size_t save = lx.pos();
            Obj next;
            bool kw2 = false;
            if (lx.next(next, kw2, true) && kw2 && next.str == "stream") {
                std::size_t s = lx.pos();
                if (s < bytes.size() && bytes[s] == '\r')
                    ++s;
                if (s < bytes.size() && bytes[s] == '\n')
                    ++s;
                std::size_t len = 0;
                bool ok = false;
                if (const Obj* l = obj.get("Length"); l && l->isNum() && l->num >= 0) {
                    len = static_cast<std::size_t>(l->num);
                    if (s + len <= bytes.size()) {
                        std::size_t e = s + len;
                        while (e < bytes.size() && isSpace(bytes[e]))
                            ++e;
                        ok = bytes.compare(e, 9, "endstream") == 0;
                    }
                }
                if (!ok) {
                    std::size_t e = findToken(bytes, "endstream", s);
                    if (e == std::string_view::npos)
                        e = bytes.size();
                    len = e - s;
                    if (len > 0 && bytes[s + len - 1] == '\n')
                        --len;
                    if (len > 0 && bytes[s + len - 1] == '\r')
                        --len;
                }
                obj.type = Obj::Type::Stream;
                obj.streamOff = s;
                obj.streamLen = len;
                i = std::max(i, s + len);
            } else {
                lx.seek(save);
            }
        }
        objs_[num] = std::move(obj);
    }

    // 2. Trailers.
    i = 0;
    while (i < bytes.size()) {
        std::size_t k = findToken(bytes, "trailer", i);
        if (k == std::string_view::npos)
            break;
        i = k + 7;
        Lexer lx(bytes, i);
        Obj d;
        bool kw = false;
        if (lx.next(d, kw, true) && d.type == Obj::Type::Dict) {
            if (d.get("Encrypt"))
                encrypted_ = true;
            if (const Obj* r = d.get("Root")) {
                trailerRoot = *r;
                haveRoot = true;
            }
        }
    }
    for (const auto& [n, o] : objs_) {
        (void)n;
        if (o.type != Obj::Type::Stream)
            continue;
        const Obj* t = o.get("Type");
        if (t && t->isName("XRef")) {
            if (o.get("Encrypt"))
                encrypted_ = true;
            if (const Obj* r = o.get("Root")) {
                trailerRoot = *r;
                haveRoot = true;
            }
        }
    }
    if (encrypted_) {
        error = "This PDF is encrypted";
        return false;
    }

    // 3. Compressed objects.
    readObjectStreams();

    if (haveRoot)
        root_ = resolve(trailerRoot);
    if (!root_.isDict()) {
        for (const auto& [n, o] : objs_) {
            (void)n;
            const Obj* t = o.get("Type");
            if (o.isDict() && t && t->isName("Catalog")) {
                root_ = o;
                break;
            }
        }
    }

    // 4. Pages.
    if (root_.isDict()) {
        if (const Obj* p = find(root_, "Pages"))
            collectPages(*p, Obj{}, 0);
    }
    if (pages_.empty()) {
        for (const auto& [n, o] : objs_) {
            (void)n;
            const Obj* t = o.get("Type");
            if (o.type == Obj::Type::Dict && t && t->isName("Page"))
                pages_.push_back({o});
        }
    }
    if (pages_.empty()) {
        error = "No pages found in this PDF";
        return false;
    }
    return true;
}

void File::readObjectStreams() {
    std::vector<int> streams;
    for (const auto& [n, o] : objs_) {
        const Obj* t = o.get("Type");
        if (o.type == Obj::Type::Stream && t && t->isName("ObjStm"))
            streams.push_back(n);
    }
    for (int sn : streams) {
        const Obj& so = objs_[sn];
        std::vector<std::uint8_t> bytes = decode(so);
        if (bytes.empty())
            continue;
        const Obj* nObj = so.get("N");
        const Obj* firstObj = so.get("First");
        if (!nObj || !firstObj || !nObj->isNum() || !firstObj->isNum())
            continue;
        const int n = static_cast<int>(nObj->num);
        const std::size_t first = static_cast<std::size_t>(firstObj->num);
        std::string_view sv(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        Lexer head(sv, 0);
        std::vector<std::pair<int, std::size_t>> table;
        for (int k = 0; k < n && k < 1000000; ++k) {
            Obj a, b;
            bool kw = false;
            if (!head.next(a, kw, false) || kw || !head.next(b, kw, false) || kw)
                break;
            table.emplace_back(static_cast<int>(a.num), static_cast<std::size_t>(b.num));
        }
        for (const auto& [num, off] : table) {
            if (first + off >= sv.size())
                continue;
            Lexer lx(sv, first + off);
            Obj o;
            bool kw = false;
            if (!lx.next(o, kw, true) || kw)
                continue;
            if (!objs_.count(num))
                objs_[num] = std::move(o);
        }
    }
}

const Obj& File::resolve(const Obj& o) const {
    const Obj* cur = &o;
    for (int hops = 0; cur->type == Obj::Type::Ref && hops < 32; ++hops) {
        auto it = objs_.find(cur->refNum);
        if (it == objs_.end())
            return kNull;
        cur = &it->second;
    }
    return cur->type == Obj::Type::Ref ? kNull : *cur;
}

const Obj* File::find(const Obj& dict, std::string_view key) const {
    const Obj& d = resolve(dict);
    const Obj* v = d.get(key);
    if (!v)
        return nullptr;
    const Obj& r = resolve(*v);
    return r.type == Obj::Type::Null ? nullptr : &r;
}

void File::collectPages(const Obj& nodeIn, const Obj& inherited, int depth) {
    if (depth > 32)
        return;
    const Obj& node = resolve(nodeIn);
    if (!node.isDict())
        return;
    // A page tree is a tree: a node listed twice (or a kid that is its own ancestor) is taken
    // once, else a damaged file can make the walk exponential.
    if (!pageNodes_.insert(&node).second || pages_.size() >= 100000)
        return;
    Obj inh;
    inh.type = Obj::Type::Dict;
    inh.dict = std::make_shared<Dict>(inherited.dict ? *inherited.dict : Dict{});
    for (const char* key : {"MediaBox", "CropBox", "Resources", "Rotate", "UserUnit"})
        if (const Obj* v = node.get(key))
            (*inh.dict)[key] = *v;
    const Obj* kids = find(node, "Kids");
    if (kids && kids->isArr()) {
        for (const Obj& k : kids->arr)
            collectPages(k, inh, depth + 1);
        return;
    }
    Page pg;
    pg.dict.type = Obj::Type::Dict;
    pg.dict.dict = std::make_shared<Dict>(*node.dict);
    for (const auto& [k, v] : *inh.dict)
        if (!pg.dict.dict->count(k))
            (*pg.dict.dict)[k] = v;
    pages_.push_back(std::move(pg));
}

namespace {

std::vector<std::uint8_t> inflate(const std::uint8_t* in, std::size_t n, bool& ok) {
    ok = false;
    std::vector<std::uint8_t> out(std::max<std::size_t>(n * 4, 4096));
    tinfl_decompressor d;
    tinfl_init(&d);
    std::size_t inPos = 0, outPos = 0;
    for (;;) {
        std::size_t inN = n - inPos;
        std::size_t outN = out.size() - outPos;
        tinfl_status st = tinfl_decompress(
            &d, in + inPos, &inN, out.data(), out.data() + outPos, &outN,
            TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        inPos += inN;
        outPos += outN;
        if (st == TINFL_STATUS_DONE) {
            ok = true;
            break;
        }
        if (st == TINFL_STATUS_HAS_MORE_OUTPUT) {
            // Deflate expands at most 1032:1, so a damaged stream that keeps asking for room
            // without making progress stops here instead of growing the buffer to the limit.
            if (out.size() >= kMaxDecoded || out.size() > n * 1032 + 4096 ||
                (inN == 0 && outN == 0))
                break;
            out.resize(out.size() * 2);
            continue;
        }
        break;  // truncated or damaged: keep what was produced
    }
    out.resize(outPos);
    ok = ok || outPos > 0;
    return out;
}

std::vector<std::uint8_t> asciiHex(const std::vector<std::uint8_t>& in) {
    std::vector<std::uint8_t> out;
    int hi = -1;
    for (std::uint8_t c : in) {
        if (c == '>')
            break;
        int h = hexVal(static_cast<char>(c));
        if (h < 0)
            continue;
        if (hi < 0) {
            hi = h;
        } else {
            out.push_back(static_cast<std::uint8_t>(hi * 16 + h));
            hi = -1;
        }
    }
    if (hi >= 0)
        out.push_back(static_cast<std::uint8_t>(hi * 16));
    return out;
}

std::vector<std::uint8_t> ascii85(const std::vector<std::uint8_t>& in) {
    std::vector<std::uint8_t> out;
    std::uint32_t group[5];
    int n = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        std::uint8_t c = in[i];
        if (c == '~')
            break;
        if (isSpace(static_cast<char>(c)))
            continue;
        if (c == 'z' && n == 0) {
            out.insert(out.end(), 4, 0);
            continue;
        }
        if (c < '!' || c > 'u')
            continue;
        group[n++] = c - '!';
        if (n == 5) {
            std::uint32_t v = 0;
            for (int k = 0; k < 5; ++k)
                v = v * 85 + group[k];
            for (int k = 3; k >= 0; --k)
                out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
            n = 0;
        }
    }
    if (n > 1) {
        for (int k = n; k < 5; ++k)
            group[k] = 84;
        std::uint32_t v = 0;
        for (int k = 0; k < 5; ++k)
            v = v * 85 + group[k];
        for (int k = 3; k > 3 - (n - 1); --k)
            out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    }
    return out;
}

}  // namespace

std::vector<std::uint8_t> File::decode(const Obj& streamIn, bool* unsupported) const {
    const Obj& stream = resolve(streamIn);
    if (unsupported)
        *unsupported = false;
    if (stream.type != Obj::Type::Stream || stream.streamOff + stream.streamLen > bytes_.size())
        return {};
    std::vector<std::uint8_t> data(
        reinterpret_cast<const std::uint8_t*>(bytes_.data()) + stream.streamOff,
        reinterpret_cast<const std::uint8_t*>(bytes_.data()) + stream.streamOff + stream.streamLen);

    std::vector<std::string> filters;
    if (const Obj* f = find(stream, "Filter")) {
        if (f->isName())
            filters.push_back(f->str);
        else if (f->isArr())
            for (const Obj& e : f->arr) {
                const Obj& r = resolve(e);
                if (r.isName())
                    filters.push_back(r.str);
            }
    }
    const Obj* parms = find(stream, "DecodeParms");
    if (!parms)
        parms = find(stream, "DP");
    for (std::size_t fi = 0; fi < filters.size(); ++fi) {
        const std::string& f = filters[fi];
        const Obj* pd = nullptr;
        if (parms) {
            if (parms->isDict())
                pd = parms;
            else if (parms->isArr() && fi < parms->arr.size())
                pd = &resolve(parms->arr[fi]);
        }
        if (f == "FlateDecode" || f == "Fl") {
            if (pd) {
                if (const Obj* pr = find(*pd, "Predictor"); pr && pr->isNum() && pr->num > 1) {
                    if (unsupported)
                        *unsupported = true;
                    return {};
                }
            }
            bool ok = false;
            data = inflate(data.data(), data.size(), ok);
            if (!ok) {
                if (unsupported)
                    *unsupported = true;
                return {};
            }
        } else if (f == "ASCIIHexDecode" || f == "AHx") {
            data = asciiHex(data);
        } else if (f == "ASCII85Decode" || f == "A85") {
            data = ascii85(data);
        } else {
            if (unsupported)
                *unsupported = true;
            return {};
        }
    }
    return data;
}

}  // namespace tbcad::underlay::pdf
