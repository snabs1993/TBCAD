// Vector underlays from PDF pages. A page is read into a Document of coloured polylines (strokes
// and filled outlines; text and raster images are counted but not drawn), which libs/xref then
// attaches as an external reference. See docs/underlay.md.
#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::underlay {

struct PdfOptions {
    int page = 1;             // 1-based
    double fadePercent = 50;  // 0 = original colours, 90 = nearly the neutral grey
    int contrast = 50;        // 0..100, 50 = the colours as they are (as for pictures)
    bool monochrome = false;  // every stroke in one grey tone (by luminance), before the fade
    std::size_t maxVertices = 4'000'000;
};

struct PdfReport {
    int pageCount = 0;
    double widthMm = 0, heightMm = 0;  // of the imported page, after rotation
    std::size_t paths = 0;             // polylines created
    std::size_t vertices = 0;
    std::size_t textRuns = 0;      // text objects (not drawn)
    std::size_t images = 0;        // raster images (not drawn)
    std::size_t skippedFills = 0;  // white or page-sized fills
    bool truncated = false;        // hit maxVertices
    // Optional-content layers of the page (PDF layers); each becomes a layer of its own below the
    // imported document, so it can be switched off in the host. Content outside any is on "PDF".
    std::vector<std::string> layers{};
    std::vector<std::string> notes{};  // human readable remarks
};

// The layer all geometry goes on.
inline constexpr const char* kPdfLayerName = "PDF";

Result<std::unique_ptr<Document>> importPdf(std::span<const std::byte> bytes,
                                            const PdfOptions& options = {},
                                            PdfReport* report = nullptr);
Result<std::unique_ptr<Document>> importPdfFile(const std::filesystem::path& file,
                                                const PdfOptions& options = {},
                                                PdfReport* report = nullptr);

// Number of pages (0 on error).
Result<int> pdfPageCount(const std::filesystem::path& file);

// "file.pdf#page=2,fade=60,contrast=70,mono=1": the options part of a stored underlay path. `file` is what comes
// before the '#'. Unknown keys are ignored; a stored path without a valid spec is returned whole.
struct Spec {
    std::string file{};
    PdfOptions pdf{};
    bool hasSpec = false;
};
Spec parseSpec(std::string_view stored);
// The inverse: "file.pdf#page=2,fade=60" (default options are omitted).
std::string makeSpec(std::string_view file, const PdfOptions& options);
bool isPdfPath(std::string_view stored);

}  // namespace tbcad::underlay
