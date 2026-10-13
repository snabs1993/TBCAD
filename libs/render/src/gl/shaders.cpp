#include "gl/shaders.h"

namespace tbcad::render::gl {

const char* const kStrokeThinVs = R"GLSL(#version 330 core
layout(location = 0) in vec4 aPos;  // hi.xy, lo.xy (mm, relative to the tile origin)
layout(location = 1) in float aD;   // distance along the polyline modulo the dash period
uniform vec4 uOff;                  // tile origin - camera centre: hi.xy, lo.xy
uniform vec2 uNdc;                  // 2 * pixelsPerMm / viewport
out float vD;
void main() {
    vec2 p = (aPos.xy + uOff.xy) + (aPos.zw + uOff.zw);
    gl_Position = vec4(p * uNdc, 0.0, 1.0);
    vD = aD;
}
)GLSL";

// One instance per segment, four vertices per instance (triangle strip). The segment is
// expanded to a screen-space quad `uWidthPx` wide, extended by half a width at both ends so that
// consecutive segments overlap at joins.
const char* const kStrokeThickVs = R"GLSL(#version 330 core
layout(location = 0) in vec4 aA;
layout(location = 1) in float aDa;
layout(location = 2) in vec4 aB;
layout(location = 3) in float aDb;
uniform vec4 uOff;
uniform vec2 uViewport;  // pixels
uniform float uPpmm;
uniform float uWidthPx;
out float vD;
out float vSide;
out float vHalf;
void main() {
    vec2 a = ((aA.xy + uOff.xy) + (aA.zw + uOff.zw)) * uPpmm;
    vec2 b = ((aB.xy + uOff.xy) + (aB.zw + uOff.zw)) * uPpmm;
    vec2 dir = b - a;
    float len = length(dir);
    dir = len > 1e-6 ? dir / len : vec2(1.0, 0.0);
    vec2 n = vec2(-dir.y, dir.x);
    float half_ = 0.5 * uWidthPx;
    float ext = half_ + 0.5;
    float u = float(gl_VertexID & 1);
    float side = float((gl_VertexID >> 1) & 1) * 2.0 - 1.0;
    vec2 p = mix(a, b, u) + dir * (u * 2.0 - 1.0) * ext + n * side * ext;
    gl_Position = vec4(p * 2.0 / uViewport, 0.0, 1.0);
    vD = mix(aDa, aDb, u) + (u * 2.0 - 1.0) * ext / uPpmm;
    vSide = side * ext;
    vHalf = half_;
}
)GLSL";

const char* const kStrokeFs = R"GLSL(#version 330 core
in float vD;
in float vSide;
in float vHalf;
uniform vec4 uColor;
uniform int uPatN;         // 0 = continuous
uniform float uPat[32];    // + dash, - gap, 0 dot (mm)
uniform float uPatP;       // sum of |uPat|
uniform float uPixelMm;    // size of one pixel in mm
uniform int uSoft;         // 1 = antialias across the line (thick path)
out vec4 oColor;

bool inDash(float d) {
    if (uPatN == 0) return true;
    float s = mod(d, uPatP);
    float acc = 0.0;
    for (int i = 0; i < uPatN; ++i) {
        float e = uPat[i];
        if (e == 0.0) {
            if (s >= acc && s < acc + uPixelMm) return true;
            continue;
        }
        float len = abs(e);
        if (s < acc + len) return e > 0.0;
        acc += len;
    }
    return false;
}

void main() {
    if (!inDash(vD)) discard;
    float cover = 1.0;
    if (uSoft == 1) cover = clamp(vHalf + 0.5 - abs(vSide), 0.0, 1.0);
    oColor = vec4(uColor.rgb, uColor.a * cover);
}
)GLSL";

const char* const kFillVs = kStrokeThinVs;

const char* const kFillFs = R"GLSL(#version 330 core
uniform vec4 uColor;
out vec4 oColor;
void main() { oColor = uColor; }
)GLSL";

const char* const kOverlayVs = R"GLSL(#version 330 core
layout(location = 0) in vec2 aPos;   // pixels, origin top-left
layout(location = 1) in vec4 aColor;
uniform vec2 uViewport;
out vec4 vColor;
void main() {
    gl_Position = vec4(aPos.x / uViewport.x * 2.0 - 1.0, 1.0 - aPos.y / uViewport.y * 2.0, 0.0, 1.0);
    vColor = aColor;
}
)GLSL";

const char* const kOverlayFs = R"GLSL(#version 330 core
in vec4 vColor;
out vec4 oColor;
void main() { oColor = vColor; }
)GLSL";

const char* const kImageVs = R"GLSL(#version 330 core
layout(location = 0) in vec2 aPos;  // pixels, origin top-left
layout(location = 1) in vec2 aUv;
uniform vec2 uViewport;
out vec2 vUv;
void main() {
    gl_Position = vec4(aPos.x / uViewport.x * 2.0 - 1.0, 1.0 - aPos.y / uViewport.y * 2.0, 0.0, 1.0);
    vUv = aUv;
}
)GLSL";

const char* const kImageFs = R"GLSL(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
uniform float uOpacity;
out vec4 oColor;
void main() {
    vec4 c = texture(uTex, vUv);
    oColor = vec4(c.rgb, c.a * uOpacity);
}
)GLSL";

}  // namespace tbcad::render::gl
