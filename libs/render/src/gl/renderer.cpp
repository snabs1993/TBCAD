#include "tbcad/render/renderer.h"

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "gl/shaders.h"
#include "tbcad/render/colors.h"

namespace tbcad::render {
namespace {

constexpr int kMaxPatternElements = 32;
constexpr double kThickLinePx = 2.0;  // wider than this uses instanced quads, else 1 px lines
constexpr float kPointSizePx = 4.0f;
constexpr GLsizei kVertexStride = static_cast<GLsizei>(sizeof(GpuVertex));
constexpr GLsizei kOverlayStride = static_cast<GLsizei>(sizeof(OverlayVertex));

struct Program {
    GLuint id = 0;
    GLint off = -1, ndc = -1, viewport = -1, ppmm = -1, widthPx = -1, color = -1;
    GLint patN = -1, pat = -1, patP = -1, pixelMm = -1, soft = -1;
};

struct Texture {
    GLuint id = 0;
    std::uint64_t lastFrame = 0;
};

struct Buffer {
    GLuint vbo = 0;
    GLuint vaoThin = 0;   // vertices as GL_LINES / GL_TRIANGLES / GL_POINTS input
    GLuint vaoThick = 0;  // segments as instances
    std::uint64_t version = ~0ull;
    std::size_t vertexCount = 0;
};

std::string compileLog(GLuint obj, bool program) {
    GLint len = 0;
    if (program)
        glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &len);
    else
        glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
    if (program)
        glGetProgramInfoLog(obj, len, nullptr, log.data());
    else
        glGetShaderInfoLog(obj, len, nullptr, log.data());
    return log;
}

GLuint compile(GLenum type, const char* src, std::string& error) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        error = "shader compile failed: " + compileLog(s, false);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool link(Program& p, const char* vs, const char* fs, std::string& error) {
    GLuint v = compile(GL_VERTEX_SHADER, vs, error);
    if (!v)
        return false;
    GLuint f = compile(GL_FRAGMENT_SHADER, fs, error);
    if (!f) {
        glDeleteShader(v);
        return false;
    }
    p.id = glCreateProgram();
    glAttachShader(p.id, v);
    glAttachShader(p.id, f);
    glLinkProgram(p.id);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
    if (!ok) {
        error = "program link failed: " + compileLog(p.id, true);
        glDeleteProgram(p.id);
        p.id = 0;
        return false;
    }
    auto u = [&](const char* n) { return glGetUniformLocation(p.id, n); };
    p.off = u("uOff");
    p.ndc = u("uNdc");
    p.viewport = u("uViewport");
    p.ppmm = u("uPpmm");
    p.widthPx = u("uWidthPx");
    p.color = u("uColor");
    p.patN = u("uPatN");
    p.pat = u("uPat[0]");
    p.patP = u("uPatP");
    p.pixelMm = u("uPixelMm");
    p.soft = u("uSoft");
    return true;
}

void setupThinLayout(const Buffer& b) {
    glBindVertexArray(b.vaoThin);
    glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, kVertexStride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, kVertexStride,
                          reinterpret_cast<void*>(4 * sizeof(float)));
}

void setupThickLayout(const Buffer& b) {
    glBindVertexArray(b.vaoThick);
    glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
    const GLsizei stride = 2 * kVertexStride;
    const std::size_t v = sizeof(GpuVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glVertexAttribDivisor(0, 1);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(4 * sizeof(float)));
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(v));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(v + 4 * sizeof(float)));
    glVertexAttribDivisor(3, 1);
}

void freeBuffer(Buffer& b) {
    if (b.vaoThin)
        glDeleteVertexArrays(1, &b.vaoThin);
    if (b.vaoThick)
        glDeleteVertexArrays(1, &b.vaoThick);
    if (b.vbo)
        glDeleteBuffers(1, &b.vbo);
    b = {};
}

}  // namespace

struct Renderer::Impl {
    bool ok = false;
    Camera2D camera{};
    Rgba background{33, 40, 48, 255};
    bool lineweightDisplay = true;
    HighlightStyle highlight{{51, 153, 255, 110}, 5.0};
    HighlightStyle selection{{255, 170, 40, 120}, 5.0};

    Program stroke;       // thin lines
    Program strokeThick;  // instanced quads
    Program fill;
    Program overlay;
    std::unordered_map<std::uint64_t, Buffer> buffers;
    std::vector<Buffer> highlightBuffers, selectionBuffers;
    std::uint64_t highlightVersion = ~0ull, selectionVersion = ~0ull;
    GLuint overlayVao = 0, overlayVbo = 0;
    Program image;
    GLint imageTex = -1, imageOpacity = -1;
    GLuint imageVao = 0, imageVbo = 0;
    std::unordered_map<std::uint64_t, Texture> textures;
    std::uint64_t frameNo = 0;

    RenderStats stats;
    std::chrono::steady_clock::time_point frameStart;

    void upload(Buffer& b, const void* data, std::size_t vertexCount, std::uint64_t version) {
        if (!b.vbo) {
            glGenBuffers(1, &b.vbo);
            glGenVertexArrays(1, &b.vaoThin);
            setupThinLayout(b);
        }
        if (b.version == version)
            return;
        glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertexCount * sizeof(GpuVertex)),
                     data, GL_DYNAMIC_DRAW);
        b.version = version;
        b.vertexCount = vertexCount;
        stats.uploadedBytes += vertexCount * sizeof(GpuVertex);
    }

    void ensureThickVao(Buffer& b) {
        if (!b.vaoThick) {
            glGenVertexArrays(1, &b.vaoThick);
            setupThickLayout(b);
        }
    }

    void setCommon(const Program& p, const Vec2& tileOrigin) {
        const TileOffset o = tileOffset(tileOrigin, camera.center);
        glUniform4f(p.off, o.hiX, o.hiY, o.loX, o.loY);
        if (p.ndc >= 0)
            glUniform2f(p.ndc, static_cast<float>(2.0 * camera.pixelsPerMm / camera.viewportW),
                        static_cast<float>(2.0 * camera.pixelsPerMm / camera.viewportH));
        if (p.viewport >= 0)
            glUniform2f(p.viewport, static_cast<float>(camera.viewportW),
                        static_cast<float>(camera.viewportH));
        if (p.ppmm >= 0)
            glUniform1f(p.ppmm, static_cast<float>(camera.pixelsPerMm));
    }

    void setColor(const Program& p, Rgba c, bool adapt) {
        if (isBackgroundColor(c))
            c = Rgba{background.r, background.g, background.b, 255};
        else if (adapt)
            c = adaptToBackground(c, background);
        glUniform4f(p.color, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
    }

    void setPattern(const Program& p, const DashPattern& pat) {
        if (pat.solidAt(camera.pixelsPerMm) || pat.elements.size() > kMaxPatternElements) {
            glUniform1i(p.patN, 0);
            return;
        }
        std::array<float, kMaxPatternElements> e{};
        for (std::size_t i = 0; i < pat.elements.size(); ++i)
            e[i] = static_cast<float>(pat.elements[i]);
        glUniform1i(p.patN, static_cast<GLint>(pat.elements.size()));
        glUniform1fv(p.pat, kMaxPatternElements, e.data());
        glUniform1f(p.patP, static_cast<float>(pat.period()));
        glUniform1f(p.pixelMm, static_cast<float>(camera.pixelSizeMm()));
    }

    void drawStroke(const Tile& tile, const Batch& batch, const DashPattern& pat) {
        Buffer& buf = buffers[batch.id];
        upload(buf, batch.verts.data(), batch.verts.size(), batch.version);
        const double lw = batch.key.lineweightUm / 1000.0;
        const double widthPx = lineweightDisplay ? lw * camera.pixelsPerMm : 0.0;
        const Rgba col{static_cast<std::uint8_t>(batch.key.rgba & 0xFF),
                       static_cast<std::uint8_t>((batch.key.rgba >> 8) & 0xFF),
                       static_cast<std::uint8_t>((batch.key.rgba >> 16) & 0xFF),
                       static_cast<std::uint8_t>(batch.key.rgba >> 24)};
        const std::size_t segments = batch.verts.size() / 2;
        if (widthPx >= kThickLinePx) {
            ensureThickVao(buf);
            glUseProgram(strokeThick.id);
            setCommon(strokeThick, tile.origin);
            glUniform1f(strokeThick.widthPx, static_cast<float>(widthPx));
            glUniform1i(strokeThick.soft, 1);
            setColor(strokeThick, col, true);
            setPattern(strokeThick, pat);
            glBindVertexArray(buf.vaoThick);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(segments));
            stats.vertices += segments * 2;
        } else {
            glUseProgram(stroke.id);
            setCommon(stroke, tile.origin);
            glUniform1i(stroke.soft, 0);
            setColor(stroke, col, true);
            setPattern(stroke, pat);
            glBindVertexArray(buf.vaoThin);
            glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(batch.verts.size()));
            stats.vertices += batch.verts.size();
        }
        ++stats.drawCalls;
    }

    void drawFillOrPoints(const Tile& tile, const Batch& batch) {
        Buffer& buf = buffers[batch.id];
        upload(buf, batch.verts.data(), batch.verts.size(), batch.version);
        const Rgba col{static_cast<std::uint8_t>(batch.key.rgba & 0xFF),
                       static_cast<std::uint8_t>((batch.key.rgba >> 8) & 0xFF),
                       static_cast<std::uint8_t>((batch.key.rgba >> 16) & 0xFF),
                       static_cast<std::uint8_t>(batch.key.rgba >> 24)};
        glUseProgram(fill.id);
        setCommon(fill, tile.origin);
        setColor(fill, col, true);
        glBindVertexArray(buf.vaoThin);
        glDrawArrays(batch.key.kind == BatchKind::Point ? GL_POINTS : GL_TRIANGLES, 0,
                     static_cast<GLsizei>(batch.verts.size()));
        ++stats.drawCalls;
        stats.vertices += batch.verts.size();
    }

    void drawGlow(const SelectionGeometry& geo, std::vector<Buffer>& bufs, std::uint64_t& seen,
                  const HighlightStyle& style) {
        if (seen != geo.version) {
            for (Buffer& b : bufs)
                freeBuffer(b);
            bufs.assign(geo.tiles.size(), {});
        }
        if (bufs.size() != geo.tiles.size())
            bufs.resize(geo.tiles.size());
        for (std::size_t i = 0; i < geo.tiles.size(); ++i) {
            const SelectionTile& st = geo.tiles[i];
            if (st.verts.empty())
                continue;
            Buffer& buf = bufs[i];
            upload(buf, st.verts.data(), st.verts.size(), geo.version);
            ensureThickVao(buf);
            glUseProgram(strokeThick.id);
            setCommon(strokeThick, st.origin);
            const double w = (lineweightDisplay ? st.lineweightMm * camera.pixelsPerMm : 0.0) +
                             style.extraWidthPx;
            glUniform1f(strokeThick.widthPx, static_cast<float>(std::max(w, kThickLinePx)));
            glUniform1i(strokeThick.soft, 1);
            glUniform1i(strokeThick.patN, 0);
            setColor(strokeThick, style.color, false);
            glBindVertexArray(buf.vaoThick);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                                  static_cast<GLsizei>(st.verts.size() / 2));
            ++stats.drawCalls;
            stats.vertices += st.verts.size();
        }
        seen = geo.version;
    }
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

bool Renderer::init(GLLoadProc loader, std::string* error) {
    std::string err;
    auto fail = [&](const std::string& msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (!loader)
        return fail("no GL loader given");
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(loader)))
        return fail("could not load OpenGL functions");
    if (!GLAD_GL_VERSION_3_3)
        return fail("OpenGL 3.3 or newer is required");

    Impl& d = *impl_;
    namespace s = tbcad::render::gl;
    if (!link(d.stroke, s::kStrokeThinVs, s::kStrokeFs, err) ||
        !link(d.strokeThick, s::kStrokeThickVs, s::kStrokeFs, err) ||
        !link(d.fill, s::kFillVs, s::kFillFs, err) ||
        !link(d.overlay, s::kOverlayVs, s::kOverlayFs, err) ||
        !link(d.image, s::kImageVs, s::kImageFs, err))
        return fail(err);
    d.imageTex = glGetUniformLocation(d.image.id, "uTex");
    d.imageOpacity = glGetUniformLocation(d.image.id, "uOpacity");
    glGenVertexArrays(1, &d.imageVao);
    glGenBuffers(1, &d.imageVbo);
    glBindVertexArray(d.imageVao);
    glBindBuffer(GL_ARRAY_BUFFER, d.imageVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));

    glGenVertexArrays(1, &d.overlayVao);
    glGenBuffers(1, &d.overlayVbo);
    glBindVertexArray(d.overlayVao);
    glBindBuffer(GL_ARRAY_BUFFER, d.overlayVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kOverlayStride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, kOverlayStride,
                          reinterpret_cast<void*>(2 * sizeof(float)));
    d.ok = true;
    return true;
}

bool Renderer::initialized() const {
    return impl_->ok;
}

void Renderer::shutdown() {
    Impl& d = *impl_;
    if (!d.ok)
        return;
    for (auto& [id, b] : d.buffers)
        freeBuffer(b);
    d.buffers.clear();
    for (Buffer& b : d.highlightBuffers)
        freeBuffer(b);
    for (Buffer& b : d.selectionBuffers)
        freeBuffer(b);
    d.highlightBuffers.clear();
    d.selectionBuffers.clear();
    d.highlightVersion = d.selectionVersion = ~0ull;
    for (auto& [key, t] : d.textures)
        glDeleteTextures(1, &t.id);
    d.textures.clear();
    glDeleteBuffers(1, &d.imageVbo);
    glDeleteVertexArrays(1, &d.imageVao);
    for (Program* p : {&d.stroke, &d.strokeThick, &d.fill, &d.overlay, &d.image})
        if (p->id)
            glDeleteProgram(p->id);
    d.stroke = d.strokeThick = d.fill = d.overlay = d.image = {};
    glDeleteBuffers(1, &d.overlayVbo);
    glDeleteVertexArrays(1, &d.overlayVao);
    d.ok = false;
}

void Renderer::resize(int w, int h) {
    impl_->camera.viewportW = std::max(w, 1);
    impl_->camera.viewportH = std::max(h, 1);
}

void Renderer::setCamera(const Camera2D& c) {
    impl_->camera = c;
    impl_->camera.viewportW = std::max(c.viewportW, 1);
    impl_->camera.viewportH = std::max(c.viewportH, 1);
}

const Camera2D& Renderer::camera() const {
    return impl_->camera;
}
void Renderer::setBackground(Rgba c) {
    impl_->background = c;
}
void Renderer::setLineweightDisplay(bool on) {
    impl_->lineweightDisplay = on;
}
void Renderer::setHighlightStyle(const HighlightStyle& s) {
    impl_->highlight = s;
}
void Renderer::setSelectionStyle(const HighlightStyle& s) {
    impl_->selection = s;
}

void Renderer::beginFrame() {
    Impl& d = *impl_;
    d.stats = {};
    d.frameStart = std::chrono::steady_clock::now();
    glViewport(0, 0, d.camera.viewportW, d.camera.viewportH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(d.background.r / 255.0f, d.background.g / 255.0f, d.background.b / 255.0f,
                 d.background.a / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glPointSize(kPointSizePx);
}

void Renderer::draw(std::span<const ImageDraw> images) {
    Impl& d = *impl_;
    for (const ImageDraw& im : images) {
        if (!im.data || im.data->width <= 0 || im.data->height <= 0 || im.opacity <= 0 ||
            im.data->rgba.size() < static_cast<std::size_t>(im.data->width) * im.data->height * 4)
            continue;
        Texture& t = d.textures[im.key];
        if (!t.id) {
            glGenTextures(1, &t.id);
            glBindTexture(GL_TEXTURE_2D, t.id);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, im.data->width, im.data->height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, im.data->rgba.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            d.stats.uploadedBytes += im.data->rgba.size();
        }
        t.lastFrame = d.frameNo;
        // Corners to pixels (double precision on the CPU), two triangles.
        std::array<Vec2, 4> px;
        for (int i = 0; i < 4; ++i)
            px[i] = d.camera.worldToScreen(im.corners[i]);
        const float u0 = static_cast<float>(im.uMin), u1 = static_cast<float>(im.uMax);
        const float v0 = static_cast<float>(im.vMin), v1 = static_cast<float>(im.vMax);
        // BL, BR, TR, TL
        const float uv[4][2] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};
        float vtx[6][4];
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; ++i) {
            vtx[i][0] = static_cast<float>(px[order[i]].x);
            vtx[i][1] = static_cast<float>(px[order[i]].y);
            vtx[i][2] = uv[order[i]][0];
            vtx[i][3] = uv[order[i]][1];
        }
        glUseProgram(d.image.id);
        glUniform2f(d.image.viewport, static_cast<float>(d.camera.viewportW),
                    static_cast<float>(d.camera.viewportH));
        glUniform1i(d.imageTex, 0);
        glUniform1f(d.imageOpacity, std::min(im.opacity, 1.0f));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glBindVertexArray(d.imageVao);
        glBindBuffer(GL_ARRAY_BUFFER, d.imageVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof vtx, vtx, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        ++d.stats.drawCalls;
        d.stats.vertices += 6;
    }
}

void Renderer::draw(Scene& scene) {
    Impl& d = *impl_;
    for (std::uint64_t id : scene.takeDeletedBatches()) {
        auto it = d.buffers.find(id);
        if (it != d.buffers.end()) {
            freeBuffer(it->second);
            d.buffers.erase(it);
        }
    }
    std::unordered_set<const Tile*> tiles;
    for (const VisibleBatch& vb : scene.visibleBatches(d.camera)) {
        tiles.insert(vb.tile);
        if (vb.batch->key.kind == BatchKind::Stroke)
            d.drawStroke(*vb.tile, *vb.batch, scene.pattern(vb.batch->key.pattern));
        else
            d.drawFillOrPoints(*vb.tile, *vb.batch);
    }
    d.stats.tilesDrawn += static_cast<std::uint32_t>(tiles.size());
    d.drawGlow(scene.highlightGeometry(), d.highlightBuffers, d.highlightVersion, d.highlight);
    d.drawGlow(scene.selectedGeometry(), d.selectionBuffers, d.selectionVersion, d.selection);
}

void Renderer::draw(const Overlay& overlay) {
    Impl& d = *impl_;
    OverlayMesh mesh = overlay.build(d.camera);
    if (mesh.vertices.empty())
        return;
    glUseProgram(d.overlay.id);
    glUniform2f(d.overlay.viewport, static_cast<float>(d.camera.viewportW),
                static_cast<float>(d.camera.viewportH));
    glBindVertexArray(d.overlayVao);
    glBindBuffer(GL_ARRAY_BUFFER, d.overlayVbo);
    const std::size_t bytes = mesh.vertices.size() * sizeof(OverlayVertex);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), mesh.vertices.data(),
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(mesh.vertices.size()));
    ++d.stats.drawCalls;
    d.stats.vertices += mesh.vertices.size();
    d.stats.uploadedBytes += bytes;
}

RenderStats Renderer::endFrame() {
    Impl& d = *impl_;
    d.stats.cpuMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - d.frameStart)
            .count();
    // Drop textures of images that were not drawn for a while.
    ++d.frameNo;
    for (auto it = d.textures.begin(); it != d.textures.end();) {
        if (d.frameNo - it->second.lastFrame > 300) {
            glDeleteTextures(1, &it->second.id);
            it = d.textures.erase(it);
        } else {
            ++it;
        }
    }
    glBindVertexArray(0);
    glUseProgram(0);
    return d.stats;
}

const RenderStats& Renderer::stats() const {
    return impl_->stats;
}

}  // namespace tbcad::render
