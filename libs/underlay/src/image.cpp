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
        if (f.size() != 6 || f[0].empty())
            continue;
        ImageParams p;
        try {
            p.fade = std::clamp(std::stod(f[1]), 0.0, kMaxImageFade);
            p.u0 = std::clamp(std::stod(f[2]), 0.0, 1.0);
            p.v0 = std::clamp(std::stod(f[3]), 0.0, 1.0);
            p.u1 = std::clamp(std::stod(f[4]), 0.0, 1.0);
            p.v1 = std::clamp(std::stod(f[5]), 0.0, 1.0);
        } catch (...) {
            continue;
        }
        if (p.u1 <= p.u0 || p.v1 <= p.v0)
            p.u0 = p.v0 = 0, p.u1 = p.v1 = 1;
        table[f[0]] = p;
    }
    return table;
}

void writeImageParams(Header& header, const ImageParamTable& table) {
    std::string text;
    for (const auto& [name, p] : table) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "\t%.6g\t%.9g\t%.9g\t%.9g\t%.9g\n", p.fade, p.u0, p.v0, p.u1,
                      p.v1);
        text += name;
        text += buf;
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

void ImageCache::sweep() {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!it->second.used)
            it = entries_.erase(it);
        else {
            it->second.used = false;
            ++it;
        }
    }
}

// ---- collecting --------------------------------------------------------------------------------

std::vector<ImageInstance> collectImages(
    const Document& doc, ImageCache& cache,
    const std::function<std::filesystem::path(std::string_view)>& resolve) {
    std::vector<ImageInstance> out;
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
        std::uint64_t key = 0;
        auto data = cache.get(resolve(def->xrefPath), def->xrefStamp, &key);
        if (!data)
            continue;
        ImageParams p;
        if (auto it = params.find(def->name); it != params.end())
            p = it->second;

        ImageInstance im;
        im.key = key;
        im.data = data;
        im.insert = id;
        im.uMin = p.u0;
        im.uMax = p.u1;
        im.vMin = p.v0;
        im.vMax = p.v1;
        im.opacity = static_cast<float>(1.0 - p.fade / 100.0);
        // Pixel units, origin bottom-left, y up; the visible part is the clip rectangle.
        const double w = data->width, h = data->height;
        const double x0 = p.u0 * w, x1 = p.u1 * w;
        const double y0 = (1 - p.v1) * h, y1 = (1 - p.v0) * h;
        const double c = std::cos(ins->rotation), s = std::sin(ins->rotation);
        const Vec2 local[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        for (int i = 0; i < 4; ++i) {
            const double lx = (local[i].x - def->base.x) * ins->scale.x;
            const double ly = (local[i].y - def->base.y) * ins->scale.y;
            im.corners[i] = {ins->position.x + c * lx - s * ly, ins->position.y + s * lx + c * ly};
        }
        out.push_back(std::move(im));
    }
    cache.sweep();
    return out;
}

}  // namespace tbcad::underlay
