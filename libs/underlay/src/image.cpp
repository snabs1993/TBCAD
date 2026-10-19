#include "tbcad/underlay/image.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

#include "stb_image.h"

namespace tbcad::underlay {

namespace {

std::string lowerExt(std::string_view s) {
    const auto dot = s.find_last_of('.');
    if (dot == std::string_view::npos)
        return {};
    std::string e(s.substr(dot));
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

Result<std::vector<unsigned char>> readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return makeError(ErrorCode::NotFound, "Cannot open the file " + file.string());
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

constexpr long long kMaxPixels = 100'000'000;  // 400 MB of RGBA

}  // namespace

bool isImagePath(std::string_view stored) {
    const std::string e = lowerExt(stored);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp";
}

Result<ImageInfo> imageInfo(const std::filesystem::path& file) {
    auto raw = readFile(file);
    if (!raw)
        return raw.error();
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(raw.value().data(), static_cast<int>(raw.value().size()), &w, &h,
                               &comp) ||
        w <= 0 || h <= 0)
        return makeError(ErrorCode::Parse, "Not a PNG, JPEG or BMP picture: " + file.string());
    return ImageInfo{w, h};
}

Result<std::shared_ptr<const ImageData>> loadImage(const std::filesystem::path& file) {
    auto raw = readFile(file);
    if (!raw)
        return raw.error();
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(raw.value().data(), static_cast<int>(raw.value().size()), &w, &h,
                               &comp) ||
        w <= 0 || h <= 0)
        return makeError(ErrorCode::Parse, "Not a PNG, JPEG or BMP picture: " + file.string());
    if (static_cast<long long>(w) * h > kMaxPixels)
        return makeError(ErrorCode::Parse, "The picture is too large: " + file.string());
    unsigned char* px = stbi_load_from_memory(
        raw.value().data(), static_cast<int>(raw.value().size()), &w, &h, &comp, 4);
    if (!px)
        return makeError(ErrorCode::Parse, "Cannot decode the picture " + file.string());
    auto data = std::make_shared<ImageData>();
    data->width = w;
    data->height = h;
    data->rgba.assign(px, px + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(px);
    return std::shared_ptr<const ImageData>(std::move(data));
}

Result<std::unique_ptr<Document>> imageFrameDocument(int width, int height) {
    if (width <= 0 || height <= 0)
        return makeError(ErrorCode::InvalidArgument, "The picture has no size");
    auto doc = std::make_unique<Document>();
    auto tx = doc->begin("Image frame");
    Layer layer;
    layer.name = kImageLayerName;
    auto lid = tx.addLayer(layer);
    if (!lid)
        return lid.error();
    PolylineData pl;
    pl.closed = true;
    for (Vec2 p : {Vec2{0, 0}, Vec2{double(width), 0}, Vec2{double(width), double(height)},
                   Vec2{0, double(height)}})
        pl.v.push_back({p, 0, 0, 0});
    Entity e;
    e.common.layer = lid.value();
    e.data = std::move(pl);
    auto added = tx.add(std::move(e));
    if (!added)
        return added.error();
    tx.commit();
    return doc;
}

// ---- parameters --------------------------------------------------------------------------------

ImageParamTable readImageParams(const Header& header) {
    ImageParamTable table;
    auto it = header.custom.find(kImageParamsKey);
    if (it == header.custom.end())
        return table;
    std::istringstream in(it->second);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::size_t start = 0;
        for (;;) {
            const auto tab = line.find('\t', start);
            f.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
            if (tab == std::string::npos)
                break;
            start = tab + 1;
        }
        if (f.size() < 6 || f[0].empty())
            continue;
        ImageParams p;
        try {
            p.fade = std::clamp(std::stod(f[1]), 0.0, kMaxImageFade);
            p.u0 = std::clamp(std::stod(f[2]), 0.0, 1.0);
            p.v0 = std::clamp(std::stod(f[3]), 0.0, 1.0);
            p.u1 = std::clamp(std::stod(f[4]), 0.0, 1.0);
            p.v1 = std::clamp(std::stod(f[5]), 0.0, 1.0);
            if (f.size() >= 9) {  // brightness, contrast, frame
                p.brightness = std::clamp(std::stoi(f[6]), 0, 100);
                p.contrast = std::clamp(std::stoi(f[7]), 0, 100);
                p.frame = std::clamp(std::stoi(f[8]), 0, 2);
            }
            if (f.size() >= 11)
                p.transparent = f[10] == "1";
            if (f.size() >= 12)
                p.monochrome = f[11] == "1";
            if (f.size() >= 10 && !f[9].empty()) {  // polygon: u,v;u,v;...
                std::istringstream ps(f[9]);
                std::string item;
                while (std::getline(ps, item, ';')) {
                    const auto comma = item.find(',');
                    if (comma == std::string::npos)
                        throw std::invalid_argument("clip");
                    p.clip.push_back({std::clamp(std::stod(item.substr(0, comma)), 0.0, 1.0),
                                      std::clamp(std::stod(item.substr(comma + 1)), 0.0, 1.0)});
                }
                if (p.clip.size() < 3 || p.clip.size() > 4096)
                    p.clip.clear();
            }
        } catch (...) {
            continue;
        }
        if (p.u1 <= p.u0 || p.v1 <= p.v0) {
            p.u0 = p.v0 = 0, p.u1 = p.v1 = 1;
            p.clip.clear();
        }
        table[f[0]] = p;
    }
    return table;
}

void writeImageParams(Header& header, const ImageParamTable& table) {
    std::string text;
    for (const auto& [name, p] : table) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "\t%.6g\t%.9g\t%.9g\t%.9g\t%.9g\t%d\t%d\t%d\t", p.fade, p.u0,
                      p.v0, p.u1, p.v1, p.brightness, p.contrast, p.frame);
        text += name;
        text += buf;
        for (std::size_t i = 0; i < p.clip.size(); ++i) {
            char pt[64];
            std::snprintf(pt, sizeof pt, "%s%.9g,%.9g", i ? ";" : "", p.clip[i].x, p.clip[i].y);
            text += pt;
        }
        text += p.transparent ? "\t1" : "\t0";
        text += p.monochrome ? "\t1\n" : "\n";
    }
    if (text.empty())
        header.custom.erase(kImageParamsKey);
    else
        header.custom[kImageParamsKey] = std::move(text);
}

ImageParams imageParams(const Header& header, std::string_view blockName) {
    const ImageParamTable t = readImageParams(header);
    auto it = t.find(std::string(blockName));
    return it == t.end() ? ImageParams{} : it->second;
}

Result<void> setImageParams(Transaction& tx, std::string_view blockName,
                            const ImageParams& params) {
    return tx.modifyHeader([&](Header& h) {
        ImageParamTable t = readImageParams(h);
        if (params == ImageParams{})
            t.erase(std::string(blockName));
        else
            t[std::string(blockName)] = params;
        writeImageParams(h, t);
    });
}

// ---- cache -------------------------------------------------------------------------------------

std::shared_ptr<const ImageData> ImageCache::get(const std::filesystem::path& file,
                                                 std::int64_t stamp, std::uint64_t* key) {
    const std::string id = file.generic_string() + "|" + std::to_string(stamp);
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        Entry e;
        e.key = nextKey_++;
        auto img = loadImage(file);
        if (img)
            e.data = img.value();
        it = entries_.emplace(id, std::move(e)).first;
    }
    it->second.used = true;
    if (key)
        *key = it->second.key;
    return it->second.data;
}

bool isBitonal(const ImageData& img) {
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        const std::uint8_t r = img.rgba[i];
        if ((r != 0 && r != 255) || img.rgba[i + 1] != r || img.rgba[i + 2] != r)
            return false;
    }
    return !img.rgba.empty();
}

std::shared_ptr<const ImageData> ImageCache::adjusted(std::uint64_t key,
                                                      const std::shared_ptr<const ImageData>& src,
                                                      const ImageParams& params,
                                                      std::uint64_t* outKey) {
    const bool poly = params.clip.size() >= 3;
    bool clear = false;
    if (src && params.transparent) {  // looking at every pixel once per picture is enough
        auto b = bitonal_.find(key);
        if (b == bitonal_.end())
            b = bitonal_.emplace(key, isBitonal(*src)).first;
        clear = b->second;
    }
    if (!src || (!poly && !params.adjusted() && !clear)) {
        if (outKey)
            *outKey = key;
        return src;
    }
    std::string id = std::to_string(key) + "|" + std::to_string(params.brightness) + "|" +
                     std::to_string(params.contrast) + (clear ? "|t" : "") +
                     (params.monochrome ? "|m" : "");
    if (poly)
        for (const Vec2& p : params.clip) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "|%.9g,%.9g", p.x, p.y);
            id += buf;
        }
    auto it = derived_.find(id);
    if (it == derived_.end()) {
        auto img = std::make_shared<ImageData>(*src);
        if (params.adjusted()) {
            const double gain = params.contrast <= 50 ? params.contrast / 50.0
                                                      : 1.0 + (params.contrast - 50) / 50.0 * 3.0;
            const double off = (params.brightness - 50) / 50.0 * 255.0;
            std::uint8_t lut[256];
            for (int i = 0; i < 256; ++i)
                lut[i] = static_cast<std::uint8_t>(
                    std::clamp((i - 128.0) * gain + 128.0 + off, 0.0, 255.0) + 0.5);
            for (std::size_t i = 0; i + 3 < img->rgba.size(); i += 4) {
                if (params.monochrome) {
                    const auto g = static_cast<std::uint8_t>(
                        std::lround(0.2126 * img->rgba[i] + 0.7152 * img->rgba[i + 1] +
                                    0.0722 * img->rgba[i + 2]));
                    img->rgba[i] = img->rgba[i + 1] = img->rgba[i + 2] = g;
                }
                for (int c = 0; c < 3; ++c)
                    img->rgba[i + c] = lut[img->rgba[i + c]];
            }
        }
        if (clear)  // the white background of a bitonal picture
            for (std::size_t i = 0; i + 3 < img->rgba.size(); i += 4)
                if (img->rgba[i] > 127)
                    img->rgba[i + 3] = 0;
        if (poly) {
            const auto& pts = params.clip;
            const int w = img->width, h = img->height;
            for (int y = 0; y < h; ++y) {
                const double v = (y + 0.5) / h;
                for (int x = 0; x < w; ++x) {
                    const double u = (x + 0.5) / w;
                    bool in = false;
                    for (std::size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
                        const Vec2 &a = pts[i], &b = pts[j];
                        if ((a.y > v) != (b.y > v) &&
                            u < (b.x - a.x) * (v - a.y) / (b.y - a.y) + a.x)
                            in = !in;
                    }
                    if (!in)
                        img->rgba[(static_cast<std::size_t>(y) * w + x) * 4 + 3] = 0;
                }
            }
        }
        Entry e;
        e.key = nextKey_++;
        e.data = std::move(img);
        it = derived_.emplace(id, std::move(e)).first;
    }
    it->second.used = true;
    if (outKey)
        *outKey = it->second.key;
    return it->second.data;
}

void ImageCache::sweep() {
    for (auto* m : {&entries_, &derived_})
        for (auto it = m->begin(); it != m->end();) {
            if (!it->second.used)
                it = m->erase(it);
            else {
                it->second.used = false;
                ++it;
            }
        }
}

// ---- collecting --------------------------------------------------------------------------------

std::optional<ImageInstance> imageOfInsert(
    const Document& doc, const Entity& e, const ImageParamTable& params, ImageCache& cache,
    const std::function<std::filesystem::path(std::string_view)>& resolve) {
    const InsertData* ins = entityAs<InsertData>(e);
    if (!ins)
        return std::nullopt;
    const BlockDef* def = doc.block(ins->block);
    if (!def || def->xrefPath.empty() || !isImagePath(def->xrefPath))
        return std::nullopt;
    std::uint64_t key = 0;
    auto data = cache.get(resolve(def->xrefPath), def->xrefStamp, &key);
    if (!data)
        return std::nullopt;
    ImageParams p;
    if (auto it = params.find(def->name); it != params.end())
        p = it->second;

    ImageInstance im;
    im.data = cache.adjusted(key, data, p, &im.key);
    im.insert = e.common.id;
    im.uMin = p.u0;
    im.uMax = p.u1;
    im.vMin = p.v0;
    im.vMax = p.v1;
    im.opacity = static_cast<float>(1.0 - p.fade / 100.0);
    // Pixel units, origin bottom-left, y up; the visible part is the clip rectangle.
    const double w = im.data->width, h = im.data->height;
    const double x0 = p.u0 * w, x1 = p.u1 * w;
    const double y0 = (1 - p.v1) * h, y1 = (1 - p.v0) * h;
    const double c = std::cos(ins->rotation), s = std::sin(ins->rotation);
    const Vec2 local[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; ++i) {
        const double lx = (local[i].x - def->base.x) * ins->scale.x;
        const double ly = (local[i].y - def->base.y) * ins->scale.y;
        im.corners[i] = {ins->position.x + c * lx - s * ly, ins->position.y + s * lx + c * ly};
    }
    return im;
}

std::vector<ImageInstance> collectImages(
    const Document& doc, ImageCache& cache,
    const std::function<std::filesystem::path(std::string_view)>& resolve) {
    std::vector<ImageInstance> out;
    // Called every frame: a drawing without image blocks (almost all of them) must not pay for a
    // walk over every object.
    bool anyImageBlock = false;
    for (const auto& [id, def] : doc.blocks())
        if (!def.xrefPath.empty() && isImagePath(def.xrefPath)) {
            anyImageBlock = true;
            break;
        }
    if (!anyImageBlock) {
        cache.sweep();
        return out;
    }
    const ImageParamTable params = readImageParams(doc.header());
    for (EntityId id : doc.modelSpace()) {
        const Entity* e = doc.find(id);
        if (!e || !e->common.visible)
            continue;
        const InsertData* ins = entityAs<InsertData>(*e);
        if (!ins)
            continue;
        const BlockDef* def = doc.block(ins->block);
        if (!def || def->xrefPath.empty() || !isImagePath(def->xrefPath))
            continue;
        if (!doc.effective(e->common.layer).visible())
            continue;
        if (const Layer* xl = doc.layerByName(def->name); xl && !doc.effective(xl->id).visible())
            continue;
        if (auto im = imageOfInsert(doc, *e, params, cache, resolve))
            out.push_back(std::move(*im));
    }
    cache.sweep();
    return out;
}

}  // namespace tbcad::underlay
