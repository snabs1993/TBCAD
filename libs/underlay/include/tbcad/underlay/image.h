// Raster underlays: PNG, JPEG, BMP, GIF, TGA and TIFF pictures drawn behind the drawing. An image
// is attached as an external reference (libs/xref) whose content is a frame of the picture in pixel
// units; the pixels themselves are not stored in the drawing, only the path. How it is shown (fade,
// clip) is kept per block in the header table "tbcad.underlays". See docs/underlay.md.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::underlay {

// The layer the frame of an image goes on.
inline constexpr const char* kImageLayerName = "IMAGE";
// Header table key of the display parameters.
inline constexpr const char* kImageParamsKey = "tbcad.underlays";

bool isImagePath(
    std::string_view stored);  // by extension: .png .jpg .jpeg .bmp .tif .tiff .gif .tga

struct ImageInfo {
    int width = 0, height = 0;
};
// Size of the picture without decoding it.
Result<ImageInfo> imageInfo(const std::filesystem::path& file);
// True when every pixel is pure black or pure white.
bool isBitonal(const ImageData& img);
// Decodes the picture to RGBA.
Result<std::shared_ptr<const ImageData>> loadImage(const std::filesystem::path& file);
// A Document holding the frame (a closed polyline on kImageLayerName) of a width x height picture,
// in pixel units with the origin in the bottom-left corner and y up.
Result<std::unique_ptr<Document>> imageFrameDocument(int width, int height);

// How an image is shown. `fade` is the percentage of transparency (0 = opaque, 95 = nearly
// invisible); the clip is a rectangle in fractions of the picture (u from the left, v from the
// top), the default being the whole picture. `clip` is a polygon in the same fractions: when it
// has three or more points only the inside shows, and u0..v1 are its bounding box.
// `transparent` only has an effect on bitonal pictures.
// `brightness` and `contrast` are 0..100 with 50 = the picture as it is. `frame` says whether
// the outline of the picture is drawn: 0 no, 1 yes and it is plotted, 2 yes but not plotted.
struct ImageParams {
    double fade = 0;
    double u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    int brightness = 50, contrast = 50;
    int frame = 2;
    // Bitonal (black and white only) pictures: the white background does not show (DXF 70 bit 8).
    bool transparent = false;
    bool monochrome = false;  // shown in grey tones (by luminance)
    std::vector<Vec2> clip{};
    bool clipped() const { return u0 > 0 || v0 > 0 || u1 < 1 || v1 < 1 || clip.size() >= 3; }
    bool adjusted() const { return brightness != 50 || contrast != 50 || monochrome; }
    bool derived() const { return adjusted() || transparent || clip.size() >= 3; }
    friend bool operator==(const ImageParams&, const ImageParams&) = default;
};
inline constexpr double kDefaultImageFade = 30;
inline constexpr double kMaxImageFade = 95;

// The table, keyed by block name. Missing blocks have default parameters.
using ImageParamTable = std::map<std::string, ImageParams>;
ImageParamTable readImageParams(const Header& header);
void writeImageParams(Header& header, const ImageParamTable& table);
ImageParams imageParams(const Header& header, std::string_view blockName);
// Sets the parameters of one block (erases the entry for default parameters).
Result<void> setImageParams(Transaction& tx, std::string_view blockName, const ImageParams& params);

// One image to draw. Corners are bottom-left, bottom-right, top-right, top-left of the visible
// part, in world mm. u/v select the visible part of the picture (v from the top).
struct ImageInstance {
    std::uint64_t key = 0;
    std::shared_ptr<const ImageData> data{};
    std::array<Vec2, 4> corners{};
    double uMin = 0, uMax = 1, vMin = 0, vMax = 1;
    float opacity = 1;
    EntityId insert{};  // the Insert entity
};

// Decoded pictures by file and time stamp, so a picture is read once.
class ImageCache {
public:
    // Null when the file cannot be decoded (the failure is remembered until the stamp changes).
    std::shared_ptr<const ImageData> get(const std::filesystem::path& file, std::int64_t stamp,
                                         std::uint64_t* key = nullptr);
    // The picture `key` with the brightness, contrast and polygon clip of `params` applied (the
    // polygon becomes transparency). Same pixels and key when there is nothing to apply.
    std::shared_ptr<const ImageData> adjusted(std::uint64_t key,
                                              const std::shared_ptr<const ImageData>& src,
                                              const ImageParams& params,
                                              std::uint64_t* outKey = nullptr);
    // Forgets pictures that were not asked for since the last call.
    void sweep();
    std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::shared_ptr<const ImageData> data{};
        std::uint64_t key = 0;
        bool used = false;
    };
    std::unordered_map<std::string, Entry> entries_{};
    std::unordered_map<std::string, Entry> derived_{};
    std::unordered_map<std::uint64_t, bool> bitonal_{};
    std::uint64_t nextKey_ = 1;
};

// The picture of one insert of a picture block (null when it is not one or the file cannot be
// read). `params` is readImageParams(doc.header()).
std::optional<ImageInstance> imageOfInsert(
    const Document& doc, const Entity& insert, const ImageParamTable& params, ImageCache& cache,
    const std::function<std::filesystem::path(std::string_view)>& resolve);

// The visible images of the drawing (model-space inserts of image blocks, whose entity and layer
// are shown). `resolve` turns a stored path into a file. Drawn in the order of the model space.
std::vector<ImageInstance> collectImages(
    const Document& doc, ImageCache& cache,
    const std::function<std::filesystem::path(std::string_view)>& resolve);

}  // namespace tbcad::underlay
