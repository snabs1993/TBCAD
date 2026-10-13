#include "tbcad/font/font_library.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include FT_SFNT_NAMES_H

#include <hb-ot.h>
#include <hb.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>

namespace tbcad::font {

namespace {

constexpr std::uintmax_t kMaxFontFileBytes = 256ull * 1024 * 1024;

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

struct Cp {
    char32_t cp;
    std::uint32_t offset;  // byte offset in the input
};

// Decodes UTF-8; every invalid byte becomes U+FFFD so offsets stay monotonic.
std::vector<Cp> decodeUtf8(std::string_view s) {
    std::vector<Cp> out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        char32_t cp = 0xFFFD;
        std::size_t len = 1;
        if (b0 < 0x80) {
            cp = b0;
        } else if (b0 >= 0xC2 && b0 <= 0xF4) {
            const std::size_t need = b0 >= 0xF0 ? 3 : b0 >= 0xE0 ? 2 : 1;
            if (i + need < s.size()) {
                char32_t v = b0 & (0x3F >> need);
                bool good = true;
                for (std::size_t k = 1; k <= need; ++k) {
                    const auto b = static_cast<unsigned char>(s[i + k]);
                    if ((b & 0xC0) != 0x80) {
                        good = false;
                        break;
                    }
                    v = (v << 6) | (b & 0x3F);
                }
                const char32_t minimum = need == 1 ? 0x80 : need == 2 ? 0x800 : 0x10000;
                if (good && v >= minimum && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF)) {
                    cp = v;
                    len = need + 1;
                }
            }
        }
        out.push_back({cp, static_cast<std::uint32_t>(i)});
        i += len;
    }
    return out;
}

// Marks that must stay with the base character's font when possible.
bool isExtender(char32_t c) {
    return (c >= 0x0300 && c <= 0x036F) || c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E3A) ||
           (c >= 0x0E47 && c <= 0x0E4E) || c == 0x200C || c == 0x200D ||
           (c >= 0xFE00 && c <= 0xFE0F);
}

struct OutlineKey {
    std::uint32_t glyph;
    std::uint64_t tolerance;
    bool operator==(const OutlineKey& o) const {
        return glyph == o.glyph && tolerance == o.tolerance;
    }
};
struct OutlineKeyHash {
    std::size_t operator()(const OutlineKey& k) const noexcept {
        return std::hash<std::uint64_t>()(k.tolerance * 1000003ull + k.glyph);
    }
};

// --- outline flattening ----------------------------------------------------

struct Flattener {
    double scale;  // font units -> em
    double tol;
    GlyphOutline* out;
    FPoint cur{0, 0};

    FPoint conv(const FT_Vector* v) const {
        return {static_cast<double>(v->x) * scale, static_cast<double>(v->y) * scale};
    }
    void add(FPoint p) {
        out->contours.back().points.push_back(p);
        cur = p;
    }
    void quad(FPoint p0, FPoint c, FPoint p2, int depth) {
        const double dx = p0.x - 2 * c.x + p2.x;
        const double dy = p0.y - 2 * c.y + p2.y;
        if (depth >= 16 || std::hypot(dx, dy) * 0.25 <= tol) {
            add(p2);
            return;
        }
        const FPoint a{(p0.x + c.x) / 2, (p0.y + c.y) / 2};
        const FPoint b{(c.x + p2.x) / 2, (c.y + p2.y) / 2};
        const FPoint m{(a.x + b.x) / 2, (a.y + b.y) / 2};
        quad(p0, a, m, depth + 1);
        quad(m, b, p2, depth + 1);
    }
    void cubic(FPoint p0, FPoint c1, FPoint c2, FPoint p3, int depth) {
        const double d1 = std::hypot(p0.x - 2 * c1.x + c2.x, p0.y - 2 * c1.y + c2.y);
        const double d2 = std::hypot(c1.x - 2 * c2.x + p3.x, c1.y - 2 * c2.y + p3.y);
        if (depth >= 16 || std::max(d1, d2) * 0.75 <= tol) {
            add(p3);
            return;
        }
        const FPoint a{(p0.x + c1.x) / 2, (p0.y + c1.y) / 2};
        const FPoint b{(c1.x + c2.x) / 2, (c1.y + c2.y) / 2};
        const FPoint c{(c2.x + p3.x) / 2, (c2.y + p3.y) / 2};
        const FPoint ab{(a.x + b.x) / 2, (a.y + b.y) / 2};
        const FPoint bc{(b.x + c.x) / 2, (b.y + c.y) / 2};
        const FPoint m{(ab.x + bc.x) / 2, (ab.y + bc.y) / 2};
        cubic(p0, a, ab, m, depth + 1);
        cubic(m, bc, c, p3, depth + 1);
    }
    void closeContour() {
        auto& pts = out->contours.back().points;
        if (pts.size() > 1 && pts.front().x == pts.back().x && pts.front().y == pts.back().y) {
            pts.pop_back();
        }
    }
};

int ftMoveTo(const FT_Vector* to, void* user) {
    auto* f = static_cast<Flattener*>(user);
    if (!f->out->contours.empty()) {
        f->closeContour();
    }
    f->out->contours.emplace_back();
    f->add(f->conv(to));
    return 0;
}
int ftLineTo(const FT_Vector* to, void* user) {
    auto* f = static_cast<Flattener*>(user);
    f->add(f->conv(to));
    return 0;
}
int ftConicTo(const FT_Vector* c, const FT_Vector* to, void* user) {
    auto* f = static_cast<Flattener*>(user);
    f->quad(f->cur, f->conv(c), f->conv(to), 0);
    return 0;
}
int ftCubicTo(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user) {
    auto* f = static_cast<Flattener*>(user);
    f->cubic(f->cur, f->conv(c1), f->conv(c2), f->conv(to), 0);
    return 0;
}

struct SystemEntry {
    std::filesystem::path path;
    int index;
    std::string family;  // lower-case
    bool bold;
    bool italic;
};

}  // namespace

struct FontLibrary::Impl {
    struct Face {
        std::vector<unsigned char> data;
        FT_Face ft = nullptr;
        hb_blob_t* blob = nullptr;
        hb_face_t* hbFace = nullptr;
        hb_font_t* hbFont = nullptr;
        int upem = 1000;
        FontMetrics metrics;
        std::string family;
        std::unordered_map<OutlineKey, GlyphOutline, OutlineKeyHash> outlines;

        ~Face() {
            if (hbFont) hb_font_destroy(hbFont);
            if (hbFace) hb_face_destroy(hbFace);
            if (blob) hb_blob_destroy(blob);
            if (ft) FT_Done_Face(ft);
        }
    };

    std::mutex mutex;
    FT_Library ftLib = nullptr;
    std::vector<std::unique_ptr<Face>> faces;
    std::map<std::pair<std::string, int>, FontId> byPath;
    std::filesystem::path bundledDir;
    std::optional<FontId> thai, latin;
    bool thaiTried = false, latinTried = false;
    std::vector<SystemEntry> systemFonts;
    bool systemScanned = false;

    Impl() {
        if (FT_Init_FreeType(&ftLib) != 0) {
            ftLib = nullptr;
        }
#ifdef TBCAD_BUNDLED_FONT_DIR
        bundledDir = TBCAD_BUNDLED_FONT_DIR;
#endif
    }
    ~Impl() {
        faces.clear();  // faces must go before the library
        if (ftLib) FT_Done_FreeType(ftLib);
    }

    Face* get(FontId id) {
        return id.value < faces.size() ? faces[id.value].get() : nullptr;
    }

    Result<FontId> addFace(std::vector<unsigned char> data, int index, const std::string& key) {
        if (!ftLib) return Error{"FreeType could not be initialised"};
        if (data.empty()) return Error{"font file is empty"};
        auto face = std::make_unique<Face>();
        face->data = std::move(data);
        if (FT_New_Memory_Face(ftLib, face->data.data(), static_cast<FT_Long>(face->data.size()),
                               index, &face->ft) != 0 ||
            !face->ft) {
            face->ft = nullptr;
            return Error{"not a readable font file"};
        }
        if (!FT_IS_SCALABLE(face->ft) || face->ft->units_per_EM < 16 ||
            face->ft->units_per_EM > 16384) {
            return Error{"font is not a scalable outline font"};
        }
        face->upem = face->ft->units_per_EM;
        face->blob = hb_blob_create(reinterpret_cast<const char*>(face->data.data()),
                                    static_cast<unsigned>(face->data.size()),
                                    HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        face->hbFace = hb_face_create(face->blob, static_cast<unsigned>(index));
        if (hb_face_get_glyph_count(face->hbFace) == 0) {
            return Error{"font has no glyphs"};
        }
        face->hbFont = hb_font_create(face->hbFace);
        hb_font_set_scale(face->hbFont, face->upem, face->upem);
        hb_ot_font_set_funcs(face->hbFont);

        FontMetrics& m = face->metrics;
        const double upem = face->upem;
        m.unitsPerEm = face->upem;
        m.ascender = face->ft->ascender / upem;
        m.descender = face->ft->descender / upem;
        if (auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face->ft, FT_SFNT_OS2))) {
            if (os2->version >= 2 && os2->sCapHeight > 0) m.capHeight = os2->sCapHeight / upem;
            if (os2->version >= 2 && os2->sxHeight > 0) m.xHeight = os2->sxHeight / upem;
        }
        face->family = face->ft->family_name ? face->ft->family_name : "";

        const FontId id{static_cast<std::uint32_t>(faces.size())};
        faces.push_back(std::move(face));
        if (!key.empty()) byPath[{key, index}] = id;
        return id;
    }

    Result<FontId> loadFile(const std::filesystem::path& path, int index) {
        std::error_code ec;
        const std::string key = std::filesystem::absolute(path, ec).generic_string();
        if (auto it = byPath.find({key, index}); it != byPath.end()) return it->second;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) return Error{"cannot open font file: " + path.generic_string()};
        if (size == 0 || size > kMaxFontFileBytes) return Error{"unreasonable font file size"};
        std::ifstream in(path, std::ios::binary);
        if (!in) return Error{"cannot open font file: " + path.generic_string()};
        std::vector<unsigned char> data(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!in) return Error{"cannot read font file: " + path.generic_string()};
        return addFace(std::move(data), index, key);
    }

    Result<FontId> loadBundledLocked(std::string_view name) {
        const std::string n = lower(name);
        const char* file = n == "sarabun" ? "Sarabun-Regular.ttf"
                           : n == "lato"  ? "Lato-Regular.ttf"
                                          : nullptr;
        if (!file) return Error{"unknown bundled font: " + std::string(name)};
        std::vector<std::filesystem::path> dirs;
        if (!bundledDir.empty()) dirs.push_back(bundledDir);
        dirs.emplace_back("assets/fonts");
        std::string last = "bundled font not found: ";
        last += file;
        for (const auto& d : dirs) {
            std::error_code ec;
            if (std::filesystem::exists(d / file, ec)) return loadFile(d / file, 0);
        }
        return Error{last};
    }

    std::optional<FontId> thaiFont() {
        if (!thaiTried) {
            thaiTried = true;
            if (auto r = loadBundledLocked("Sarabun"); r.ok()) thai = r.value();
        }
        return thai;
    }
    std::optional<FontId> latinFont() {
        if (!latinTried) {
            latinTried = true;
            if (auto r = loadBundledLocked("Lato"); r.ok()) latin = r.value();
        }
        return latin;
    }

    bool hasGlyphLocked(FontId id, char32_t cp) {
        Face* f = get(id);
        return f && FT_Get_Char_Index(f->ft, static_cast<FT_ULong>(cp)) != 0;
    }

    void scanSystemFonts() {
        if (systemScanned) return;
        systemScanned = true;
        if (!ftLib) return;
        std::vector<std::filesystem::path> roots;
#ifdef _WIN32
        roots.emplace_back("C:\\Windows\\Fonts");
#else
        roots.emplace_back("/usr/share/fonts");
        roots.emplace_back("/usr/local/share/fonts");
#endif
        for (const auto& root : roots) {
            std::error_code ec;
            std::filesystem::recursive_directory_iterator it(
                root, std::filesystem::directory_options::skip_permission_denied, ec);
            for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                std::error_code fec;
                if (!it->is_regular_file(fec)) continue;
                const std::string ext = lower(it->path().extension().string());
                if (ext != ".ttf" && ext != ".otf" && ext != ".ttc" && ext != ".otc") continue;
                FT_Face face = nullptr;
                if (FT_New_Face(ftLib, it->path().string().c_str(), 0, &face) != 0 || !face) {
                    continue;
                }
                const FT_Long count = std::min<FT_Long>(face->num_faces, 16);
                for (FT_Long i = 0; i < count; ++i) {
                    if (i > 0) {
                        FT_Done_Face(face);
                        face = nullptr;
                        if (FT_New_Face(ftLib, it->path().string().c_str(), i, &face) != 0 ||
                            !face) {
                            break;
                        }
                    }
                    if (!face->family_name || !FT_IS_SCALABLE(face)) continue;
                    systemFonts.push_back({it->path(), static_cast<int>(i), lower(face->family_name),
                                           (face->style_flags & FT_STYLE_FLAG_BOLD) != 0,
                                           (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0});
                }
                if (face) FT_Done_Face(face);
            }
        }
    }
};

FontLibrary::FontLibrary() : impl_(std::make_unique<Impl>()) {}
FontLibrary::~FontLibrary() = default;

Result<FontId> FontLibrary::load(const std::filesystem::path& path) {
    std::lock_guard lock(impl_->mutex);
    return impl_->loadFile(path, 0);
}

Result<FontId> FontLibrary::loadFromMemory(std::vector<unsigned char> data, int faceIndex) {
    std::lock_guard lock(impl_->mutex);
    return impl_->addFace(std::move(data), faceIndex, std::string());
}

Result<FontId> FontLibrary::loadBundled(std::string_view name) {
    std::lock_guard lock(impl_->mutex);
    return impl_->loadBundledLocked(name);
}

void FontLibrary::setBundledDirectory(std::filesystem::path dir) {
    std::lock_guard lock(impl_->mutex);
    impl_->bundledDir = std::move(dir);
    impl_->thaiTried = impl_->latinTried = false;
    impl_->thai.reset();
    impl_->latin.reset();
}

std::optional<FontId> FontLibrary::findSystem(std::string_view family, bool bold, bool italic) {
    std::lock_guard lock(impl_->mutex);
    impl_->scanSystemFonts();
    const std::string want = lower(family);
    const SystemEntry* best = nullptr;
    int bestScore = -1;
    for (const auto& e : impl_->systemFonts) {
        if (e.family != want) continue;
        const int score = (e.bold == bold ? 2 : 0) + (e.italic == italic ? 1 : 0);
        if (score > bestScore) {
            best = &e;
            bestScore = score;
        }
    }
    if (!best) return std::nullopt;
    auto r = impl_->loadFile(best->path, best->index);
    if (!r.ok()) return std::nullopt;
    return r.value();
}

std::vector<ShapedRun> FontLibrary::shape(FontId font, std::string_view utf8, Direction direction) {
    std::lock_guard lock(impl_->mutex);
    std::vector<ShapedRun> runs;
    if (!impl_->get(font) || utf8.empty()) return runs;
    if (utf8.size() > 0x7FFFFFFF) return runs;

    std::vector<FontId> chain{font};
    for (auto extra : {impl_->thaiFont(), impl_->latinFont()}) {
        if (extra && std::find(chain.begin(), chain.end(), *extra) == chain.end()) {
            chain.push_back(*extra);
        }
    }

    // Pick a font for every code point, then group equal neighbours.
    const std::vector<Cp> cps = decodeUtf8(utf8);
    std::vector<std::size_t> pick(cps.size(), 0);
    for (std::size_t i = 0; i < cps.size(); ++i) {
        const char32_t c = cps[i].cp;
        if (i > 0 && (isExtender(c) || c == 0x20 || c == 0xA0) &&
            impl_->hasGlyphLocked(chain[pick[i - 1]], c)) {
            pick[i] = pick[i - 1];
            continue;
        }
        for (std::size_t k = 0; k < chain.size(); ++k) {
            if (impl_->hasGlyphLocked(chain[k], c)) {
                pick[i] = k;
                break;
            }
        }
    }

    hb_buffer_t* buf = hb_buffer_create();
    for (std::size_t i = 0; i < cps.size();) {
        std::size_t j = i + 1;
        while (j < cps.size() && pick[j] == pick[i]) ++j;
        const std::uint32_t begin = cps[i].offset;
        const std::uint32_t end = j < cps.size() ? cps[j].offset
                                                 : static_cast<std::uint32_t>(utf8.size());
        Impl::Face* f = impl_->get(chain[pick[i]]);

        hb_buffer_clear_contents(buf);
        hb_buffer_add_utf8(buf, utf8.data(), static_cast<int>(utf8.size()), begin, end - begin);
        hb_buffer_set_direction(buf, direction == Direction::RTL ? HB_DIRECTION_RTL
                                                                 : HB_DIRECTION_LTR);
        hb_buffer_guess_segment_properties(buf);
        hb_shape(f->hbFont, buf, nullptr, 0);

        unsigned n = 0;
        const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
        const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
        ShapedRun run;
        run.font = chain[pick[i]];
        run.unitsPerEm = f->upem;
        run.glyphs.reserve(n);
        for (unsigned g = 0; g < n; ++g) {
            run.glyphs.push_back({info[g].codepoint, info[g].cluster, pos[g].x_advance,
                                  pos[g].y_advance, pos[g].x_offset, pos[g].y_offset});
        }
        runs.push_back(std::move(run));
        i = j;
    }
    hb_buffer_destroy(buf);
    if (direction == Direction::RTL) std::reverse(runs.begin(), runs.end());
    return runs;
}

const GlyphOutline& FontLibrary::outline(FontId font, std::uint32_t glyphId, double toleranceEm) {
    static const GlyphOutline kEmpty;
    std::lock_guard lock(impl_->mutex);
    Impl::Face* f = impl_->get(font);
    if (!f) return kEmpty;
    if (!(toleranceEm > 1e-6)) toleranceEm = 1e-6;
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(toleranceEm));
    std::memcpy(&bits, &toleranceEm, sizeof(bits));
    const OutlineKey key{glyphId, bits};
    auto [it, inserted] = f->outlines.try_emplace(key);
    if (!inserted) return it->second;

    GlyphOutline& out = it->second;
    if (FT_Load_Glyph(f->ft, glyphId, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) !=
            0 ||
        f->ft->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
        return out;
    }
    Flattener fl{1.0 / f->upem, toleranceEm, &out};
    FT_Outline_Funcs funcs{};
    funcs.move_to = ftMoveTo;
    funcs.line_to = ftLineTo;
    funcs.conic_to = ftConicTo;
    funcs.cubic_to = ftCubicTo;
    if (FT_Outline_Decompose(&f->ft->glyph->outline, &funcs, &fl) != 0) {
        out.contours.clear();
        return out;
    }
    if (!out.contours.empty()) fl.closeContour();
    return out;
}

FontMetrics FontLibrary::metrics(FontId font) {
    std::lock_guard lock(impl_->mutex);
    Impl::Face* f = impl_->get(font);
    return f ? f->metrics : FontMetrics{};
}

bool FontLibrary::hasGlyph(FontId font, char32_t codePoint) {
    std::lock_guard lock(impl_->mutex);
    return impl_->hasGlyphLocked(font, codePoint);
}

std::string FontLibrary::familyName(FontId font) {
    std::lock_guard lock(impl_->mutex);
    Impl::Face* f = impl_->get(font);
    return f ? f->family : std::string();
}

}  // namespace tbcad::font
