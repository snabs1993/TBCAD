#include "gui_harness.h"

#include <GLFW/glfw3.h>
#include <doctest/doctest.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>

#include "gui.h"
#include "tbcad/text/text.h"

using tbcad::app::applyTheme;
using tbcad::app::buildFonts;
using tbcad::app::Gui;
using tbcad::app::ViewportRect;
#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/registry.h"
#include "tbcad/mcp/png.h"
#include "tbcad/platform/paths.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"
#include "tbcad/underlay/image.h"

namespace fs = std::filesystem;

namespace tbcad::guitest {

namespace {

std::vector<std::string>& errorLog() {
    static std::vector<std::string> e;
    return e;
}
void onImGuiError(ImGuiContext*, void*, const char* msg) {
    errorLog().emplace_back(msg);
}

// The text a person sees on a widget: its label up to "##".
std::string visibleText(const std::string& label) {
    std::string s = label.substr(0, label.find("##"));
    const auto a = s.find_first_not_of(' ');  // tree rows are indented with spaces
    if (a == std::string::npos)
        return {};
    return s.substr(a, s.find_last_not_of(' ') - a + 1);
}

// Combos and a few other widgets report no label. Their id is the hash of the label they were
// created with, so a query like "##filters" is recognised by hashing it with the same seed.
bool labelMatches(const imgui_items::Item& it, const std::string& want) {
    if (!it.label.empty() && (it.label == want || visibleText(it.label) == want))
        return true;
    return it.seed != 0 && ImHashStr(want.c_str(), 0, it.seed) == it.id;
}

// ---- software rasteriser -------------------------------------------------------------------

struct Atlas {
    const unsigned char* alpha = nullptr;
    int w = 0, h = 0;
    ImTextureID id = 0;
};

inline float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void fillTriangle(Image& img, const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c,
                  const ImVec4& clip, bool useAtlas, const Atlas& atlas) {
    const float area = edge(a.pos.x, a.pos.y, b.pos.x, b.pos.y, c.pos.x, c.pos.y);
    if (area == 0)
        return;
    const float minX = std::max({0.0f, clip.x, std::floor(std::min({a.pos.x, b.pos.x, c.pos.x}))});
    const float maxX =
        std::min({float(img.width), clip.z, std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x}))});
    const float minY = std::max({0.0f, clip.y, std::floor(std::min({a.pos.y, b.pos.y, c.pos.y}))});
    const float maxY =
        std::min({float(img.height), clip.w, std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y}))});
    const float inv = 1.0f / area;
    // Top-left rule: a pixel centre exactly on an edge belongs to one of the two triangles only.
    auto owns = [&](const ImVec2& p, const ImVec2& q) {
        const float dx = (q.x - p.x) * (area > 0 ? 1.f : -1.f);
        const float dy = (q.y - p.y) * (area > 0 ? 1.f : -1.f);
        return dy < 0 || (dy == 0 && dx > 0);
    };
    const bool o0 = owns(b.pos, c.pos), o1 = owns(c.pos, a.pos), o2 = owns(a.pos, b.pos);
    for (int y = int(minY); y < int(maxY); ++y) {
        for (int x = int(minX); x < int(maxX); ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            float w0 = edge(b.pos.x, b.pos.y, c.pos.x, c.pos.y, px, py) * inv;
            float w1 = edge(c.pos.x, c.pos.y, a.pos.x, a.pos.y, px, py) * inv;
            float w2 = edge(a.pos.x, a.pos.y, b.pos.x, b.pos.y, px, py) * inv;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            if ((w0 == 0 && !o0) || (w1 == 0 && !o1) || (w2 == 0 && !o2))
                continue;
            auto ch = [&](int shift) {
                return w0 * float((a.col >> shift) & 0xff) + w1 * float((b.col >> shift) & 0xff) +
                       w2 * float((c.col >> shift) & 0xff);
            };
            float r = ch(IM_COL32_R_SHIFT), g = ch(IM_COL32_G_SHIFT), bl = ch(IM_COL32_B_SHIFT);
            float al = ch(IM_COL32_A_SHIFT) / 255.0f;
            if (useAtlas && atlas.alpha) {
                const float u = w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x;
                const float v = w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y;
                const int tx = std::clamp(int(u * atlas.w), 0, atlas.w - 1);
                const int ty = std::clamp(int(v * atlas.h), 0, atlas.h - 1);
                al *= atlas.alpha[ty * atlas.w + tx] / 255.0f;
            }
            if (al <= 0)
                continue;
            std::uint8_t* px4 = &img.rgba[(std::size_t(y) * img.width + x) * 4];
            px4[0] = std::uint8_t(r * al + px4[0] * (1 - al) + 0.5f);
            px4[1] = std::uint8_t(g * al + px4[1] * (1 - al) + 0.5f);
            px4[2] = std::uint8_t(bl * al + px4[2] * (1 - al) + 0.5f);
            px4[3] = 255;
        }
    }
}

Image rasterize(const ImDrawData* dd, int w, int h, const Atlas& atlas) {
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.assign(std::size_t(w) * h * 4, 0);
    for (std::size_t i = 3; i < img.rgba.size(); i += 4)
        img.rgba[i] = 255;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* list = dd->CmdLists[n];
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback)
                continue;
            const bool useAtlas = cmd.GetTexID() == atlas.id;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawIdx* idx = list->IdxBuffer.Data + cmd.IdxOffset + i;
                const ImDrawVert* vtx = list->VtxBuffer.Data + cmd.VtxOffset;
                fillTriangle(img, vtx[idx[0]], vtx[idx[1]], vtx[idx[2]], cmd.ClipRect, useAtlas,
                             atlas);
            }
        }
    }
    return img;
}

fs::path goldenDir() {
    return fs::path(TBCAD_GUI_GOLDEN_DIR);
}
fs::path failureDir() {
    return fs::path(TBCAD_GUI_FAILURE_DIR);
}

bool writePng(const fs::path& file, const Image& img) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const auto bytes = mcp::encodePng(img.rgba, img.width, img.height);
    if (bytes.empty())
        return false;
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(out);
}

std::optional<Image> readPng(const fs::path& file) {
    auto r = underlay::loadImage(file);
    if (!r)
        return std::nullopt;
    Image img;
    img.width = r.value()->width;
    img.height = r.value()->height;
    img.rgba = r.value()->rgba;
    return img;
}

}  // namespace

Image crop(const Image& img, int x, int y, int w, int h) {
    x = std::clamp(x, 0, img.width);
    y = std::clamp(y, 0, img.height);
    w = std::clamp(w, 0, img.width - x);
    h = std::clamp(h, 0, img.height - y);
    Image out;
    out.width = w;
    out.height = h;
    out.rgba.resize(std::size_t(w) * h * 4);
    for (int row = 0; row < h; ++row)
        std::copy_n(&img.rgba[(std::size_t(y + row) * img.width + x) * 4], std::size_t(w) * 4,
                    &out.rgba[std::size_t(row) * w * 4]);
    return out;
}

bool matchesGolden(const Image& img, const std::string& name, const Tolerance& tol) {
    const fs::path golden = goldenDir() / (name + ".png");
    if (std::getenv("TBCAD_UPDATE_GOLDEN")) {
        const bool ok = writePng(golden, img);
        MESSAGE("golden picture " << golden.string() << (ok ? " written" : " NOT written"));
        return ok;
    }
    auto want = readPng(golden);
    if (!want) {
        writePng(failureDir() / (name + ".actual.png"), img);
        MESSAGE("no golden picture " << golden.string() << ". Check "
                                     << (failureDir() / (name + ".actual.png")).string()
                                     << ", then run with TBCAD_UPDATE_GOLDEN=1 to create it.");
        return false;
    }
    if (want->width != img.width || want->height != img.height) {
        writePng(failureDir() / (name + ".actual.png"), img);
        MESSAGE("golden " << name << " is " << want->width << "x" << want->height
                          << ", the frame is " << img.width << "x" << img.height);
        return false;
    }
    Image diff = img;
    std::size_t bad = 0;
    int worst = 0;
    for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
        int d = 0;
        for (int c = 0; c < 3; ++c)
            d = std::max(d, std::abs(int(img.rgba[i + c]) - int(want->rgba[i + c])));
        worst = std::max(worst, d);
        if (d > tol.channel) {
            ++bad;
            diff.rgba[i] = 255;
            diff.rgba[i + 1] = 0;
            diff.rgba[i + 2] = 0;
        } else {
            diff.rgba[i] = std::uint8_t(img.rgba[i] / 3);
            diff.rgba[i + 1] = std::uint8_t(img.rgba[i + 1] / 3);
            diff.rgba[i + 2] = std::uint8_t(img.rgba[i + 2] / 3);
        }
    }
    const double fraction = double(bad) / double(img.width * img.height);
    if (fraction <= tol.fraction)
        return true;
    writePng(failureDir() / (name + ".actual.png"), img);
    writePng(failureDir() / (name + ".diff.png"), diff);
    MESSAGE("picture " << name << " differs in " << bad << " pixels (" << fraction * 100
                       << " %, worst channel step " << worst << "). See "
                       << (failureDir() / (name + ".diff.png")).string());
    return false;
}

// ---- Harness -----------------------------------------------------------------------------------

struct Harness::Impl {
    int width = 0, height = 0;
    GLFWwindow* window = nullptr;
    ImGuiContext* ctx = nullptr;
    fs::path dir;
    bool ctrl = false, shift = false;
    std::vector<unsigned char> atlas;
    int atlasW = 0, atlasH = 0;
    Image last;
    bool haveFrame = false;
    float mx = -1000, my = -1000;
};

Harness::Harness(int width, int height) : impl_(std::make_unique<Impl>()) {
    // Text is drawn with the bundled fonts, as in the application.
    tbcad::text::fonts().setBundledDirectory(TBCAD_GUI_FONT_DIR);
    impl_->width = width;
    impl_->height = height;
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);
    REQUIRE_MESSAGE(glfwInit(), "GLFW (null platform) did not start");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    impl_->window = glfwCreateWindow(width, height, "TBCAD test", nullptr, nullptr);
    REQUIRE(impl_->window != nullptr);

    static int counter = 0;
    impl_->dir = fs::temp_directory_path() / ("tbcad_gui_test_" + std::to_string(++counter));
    fs::remove_all(impl_->dir);
    errorLog().clear();

    impl_->ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingWithShift = false;
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(float(width), float(height));
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    impl_->ctx->ErrorCallback = onImGuiError;

    registry_ = std::make_unique<cmd::Registry>();
    registerAllCommands(*registry_);
    uiRegistry_ = std::make_unique<ui::UiRegistry>();
    registerAllUi(*uiRegistry_);
    core_ = std::make_unique<ui::AppCore>(
        *registry_, platform::AppPaths{impl_->dir / "cfg", impl_->dir / "data"});
    core_->registerBuiltinUi(*uiRegistry_);
    core_->applyUiKeybindings(*uiRegistry_);
    core_->setSetting("ui.dockLayout", "v3");

    gui_ = std::make_unique<Gui>(*core_, *uiRegistry_, impl_->window);
    buildFonts(1.0f);
    applyTheme(gui_->theme(), 1.0f);
    // Rasteriser texture: the font atlas.
    unsigned char* px = nullptr;
    io.Fonts->GetTexDataAsAlpha8(&px, &impl_->atlasW, &impl_->atlasH);
    impl_->atlas.assign(px, px + std::size_t(impl_->atlasW) * impl_->atlasH);
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));

    imgui_items::setRecording(impl_->ctx, true);
    frame(3);
}

Harness::~Harness() {
    const bool bad = !errorLog().empty();
    for (const std::string& e : errorLog())
        MESSAGE("ImGui reported: " << e);
    imgui_items::setRecording(impl_->ctx, false);
    gui_.reset();
    core_.reset();
    ImGui::DestroyContext(impl_->ctx);
    glfwDestroyWindow(impl_->window);
    glfwTerminate();
    std::error_code ec;
    fs::remove_all(impl_->dir, ec);
    CHECK_MESSAGE(!bad, "Dear ImGui reported errors during the test");
}

const std::vector<std::string>& Harness::imguiErrors() const {
    return errorLog();
}

void Harness::frame(int n) {
    for (int i = 0; i < n; ++i) {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        imgui_items::clear();
        core_->updateFrame();
        ImGui::NewFrame();
        ViewportRect rect = gui_->frame();
        gui_->showViewport(7, rect);
        ImGui::Render();
        impl_->haveFrame = true;
        impl_->last = Image{};
    }
}

void Harness::settle(int limit) {
    std::size_t before = imgui_items::items().size();
    int calm = 0;
    for (int i = 0; i < limit && calm < 3; ++i) {
        frame();
        const std::size_t now = imgui_items::items().size();
        calm = now == before ? calm + 1 : 0;
        before = now;
    }
}

const imgui_items::Item* Harness::find(const std::string& label, const std::string& window,
                                       int nth) const {
    std::vector<const imgui_items::Item*> hits;
    for (const imgui_items::Item& it : imgui_items::items()) {
        if (!labelMatches(it, label))
            continue;
        if (!window.empty() && it.window.find(window) == std::string::npos)
            continue;
        hits.push_back(&it);
    }
    std::sort(hits.begin(), hits.end(), [](const auto* a, const auto* b) {
        return a->min.y != b->min.y ? a->min.y < b->min.y : a->min.x < b->min.x;
    });
    // The same widget is reported once for each of its parts; keep one per id.
    hits.erase(std::unique(hits.begin(), hits.end(),
                           [](const auto* a, const auto* b) { return a->id == b->id; }),
               hits.end());
    return nth >= 0 && nth < int(hits.size()) ? hits[std::size_t(nth)] : nullptr;
}

std::vector<std::string> Harness::labels(const std::string& window) const {
    std::vector<std::string> out;
    for (const imgui_items::Item& it : imgui_items::items())
        if (!it.label.empty() && (window.empty() || it.window.find(window) != std::string::npos))
            out.push_back(it.label);
    return out;
}

bool Harness::windowOpen(const std::string& windowPart) const {
    for (const imgui_items::Item& it : imgui_items::items())
        if (it.window.find(windowPart) != std::string::npos)
            return true;
    return false;
}

void Harness::moveMouse(float x, float y) {
    ImGui::GetIO().AddMousePosEvent(x, y);
    impl_->mx = x;
    impl_->my = y;
    frame();
}

void Harness::click(float x, float y, int button) {
    ImGuiIO& io = ImGui::GetIO();
    moveMouse(x, y);
    frame();  // hover is seen before the press
    io.AddMouseButtonEvent(button, true);
    frame();
    io.AddMouseButtonEvent(button, false);
    frame(2);
}

void Harness::doubleClick(float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    moveMouse(x, y);
    frame();
    for (int i = 0; i < 2; ++i) {
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
    }
    frame();
}

void Harness::drag(float x0, float y0, float x1, float y1, int button) {
    ImGuiIO& io = ImGui::GetIO();
    moveMouse(x0, y0);
    frame();
    io.AddMouseButtonEvent(button, true);
    frame();
    const int steps = 8;
    for (int i = 1; i <= steps; ++i) {
        io.AddMousePosEvent(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
        frame();
    }
    io.AddMouseButtonEvent(button, false);
    frame(2);
    impl_->mx = x1;
    impl_->my = y1;
}

void Harness::scroll(float x, float y, float dy) {
    moveMouse(x, y);
    frame();
    ImGui::GetIO().AddMouseWheelEvent(0, dy);
    frame(2);
}

bool Harness::doubleClickItem(const std::string& label, const std::string& window, int nth) {
    const imgui_items::Item* it = find(label, window, nth);
    if (!it) {
        MESSAGE("no widget \"" << label << "\"" << (window.empty() ? "" : " in " + window));
        return false;
    }
    const imgui_items::Item copy = *it;
    const float width = copy.max.x - copy.min.x;
    const float x = width > 220.0f ? copy.min.x + 24.0f : (copy.min.x + copy.max.x) * 0.5f;
    doubleClick(x, (copy.min.y + copy.max.y) * 0.5f);
    return true;
}

bool Harness::clickAt(const imgui_items::Item& item, int button) {
    // Wide items (a table row spanning every column) have column dividers in the middle that a
    // press would grab, so those are clicked near their left end; the rest in the centre.
    const float width = item.max.x - item.min.x;
    const float x = width > 220.0f ? item.min.x + 24.0f : (item.min.x + item.max.x) * 0.5f;
    click(x, (item.min.y + item.max.y) * 0.5f, button);
    return true;
}

bool Harness::click(const std::string& label, const std::string& window, int nth) {
    const imgui_items::Item* it = find(label, window, nth);
    if (!it) {
        MESSAGE("no widget \"" << label << "\"" << (window.empty() ? "" : " in " + window));
        return false;
    }
    const imgui_items::Item copy = *it;  // the list is rebuilt by the frames of the click
    return clickAt(copy, 0);
}

bool Harness::rightClick(const std::string& label, const std::string& window, int nth) {
    const imgui_items::Item* it = find(label, window, nth);
    if (!it) {
        MESSAGE("no widget \"" << label << "\"" << (window.empty() ? "" : " in " + window));
        return false;
    }
    const imgui_items::Item copy = *it;
    return clickAt(copy, 1);
}

void Harness::holdCtrl(bool on) {
    impl_->ctrl = on;
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, on);
    frame();
}

void Harness::holdShift(bool on) {
    impl_->shift = on;
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, on);
    frame();
}

void Harness::type(const std::string& utf8) {
    ImGui::GetIO().AddInputCharactersUTF8(utf8.c_str());
    frame(2);
}

void Harness::key(ImGuiKey k, bool ctrl, bool shift, bool alt) {
    ImGuiIO& io = ImGui::GetIO();
    if (ctrl)
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
    if (shift)
        io.AddKeyEvent(ImGuiMod_Shift, true);
    if (alt)
        io.AddKeyEvent(ImGuiMod_Alt, true);
    frame();
    io.AddKeyEvent(k, true);
    frame();
    io.AddKeyEvent(k, false);
    frame();
    if (ctrl && !impl_->ctrl)
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
    if (shift && !impl_->shift)
        io.AddKeyEvent(ImGuiMod_Shift, false);
    if (alt)
        io.AddKeyEvent(ImGuiMod_Alt, false);
    frame(2);
}

bool Harness::enterText(const std::string& label, const std::string& text,
                        const std::string& window, int nth) {
    if (!click(label, window, nth))
        return false;
    key(ImGuiKey_A, /*ctrl=*/true);  // select what is there
    type(text);
    key(ImGuiKey_Enter);
    return true;
}

void Harness::command(const std::string& line) {
    // The command window keeps keyboard focus for typing; clicking its input makes sure.
    if (const imgui_items::Item* in = find("##command")) {
        const imgui_items::Item copy = *in;
        clickAt(copy, 0);
    }
    type(line);
    key(ImGuiKey_Enter);
}

Image Harness::screenshot() {
    if (!impl_->haveFrame)
        frame();
    Atlas atlas;
    atlas.alpha = impl_->atlas.data();
    atlas.w = impl_->atlasW;
    atlas.h = impl_->atlasH;
    atlas.id = ImGui::GetIO().Fonts->TexID;
    return rasterize(ImGui::GetDrawData(), impl_->width, impl_->height, atlas);
}

Image Harness::windowShot(const std::string& windowPart) {
    ImGuiContext& g = *impl_->ctx;
    for (ImGuiWindow* w : g.Windows) {
        if (w->Active && std::string(w->Name).find(windowPart) != std::string::npos) {
            Image full = screenshot();
            return crop(full, int(w->Pos.x) - 2, int(w->Pos.y) - 2, int(w->Size.x) + 4,
                        int(w->Size.y) + 4);
        }
    }
    MESSAGE("no window \"" << windowPart << "\"");
    return Image{};
}

}  // namespace tbcad::guitest
