// aircraft_icons.cpp - see aircraft_icons.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/aircraft_icons.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#endif
#include <GL/gl.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include "gui/aircraft_icon_pixels.hpp"
#include "gui/track_silhouette.hpp"

namespace cascade::gui {

namespace {

constexpr double kPi = 3.14159265358979323846;

static_assert(aircraft_pixels::kCount == kAircraftIconCount,
              "aircraft_icon_pixels.hpp and AircraftIcon disagree - re-run "
              "tools/make-aircraft-icons.py");

// The shadow: how dark, and how soft. Soft enough to read as a shadow on the
// ground rather than a second, black aircraft.
constexpr float kShadowOpacity = 0.55f;
constexpr int kShadowBlurPasses = 3;
constexpr int kShadowBlurRadius = 3;  // texels at 128 px, per pass

// One icon's two textures. Made on first draw, when the GL context is
// certainly current; `tried` stops a failed upload being retried every frame.
struct IconTextures {
    unsigned int icon = 0u;
    unsigned int shadow = 0u;
};
IconTextures gTex[kAircraftIconCount];
bool gTried = false;
bool gOk = false;

// Uploads `rgba` (side x side) and every mipmap below it down to 1 x 1. The
// chain is built here rather than by glGenerateMipmap, which the GL 1.1
// headers this file compiles against on Windows do not declare - and without
// mipmaps a 128 px texture drawn at 20 px shimmers as it turns.
unsigned int uploadWithMips(std::vector<std::uint8_t> level, int side) {
    unsigned int tex = 0u;
    glGenTextures(1, &tex);
    if (tex == 0u) { return 0u; }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    int mip = 0;
    for (;;) {
        glTexImage2D(GL_TEXTURE_2D, mip, GL_RGBA, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     level.data());
        if (side == 1) { break; }
        level = halveRgba(level.data(), side);
        side /= 2;
        ++mip;
    }
    return tex;
}

bool ensureTextures() {
    if (gTried) { return gOk; }
    gTried = true;
    const int side = aircraft_pixels::kSide;
    const std::size_t bytes = static_cast<std::size_t>(side) * side * 4u;
    for (int i = 0; i < kAircraftIconCount; ++i) {
        const std::uint8_t* px = aircraft_pixels::kRgba[i];
        gTex[i].icon = uploadWithMips(std::vector<std::uint8_t>(px, px + bytes), side);
        gTex[i].shadow = uploadWithMips(shadowFromAlpha(px, side, kShadowOpacity), side);
        if (gTex[i].icon == 0u || gTex[i].shadow == 0u) {
            releaseAircraftIconTextures();
            gTried = true;  // release clears it; one failure is enough
            gOk = false;
            return false;
        }
    }
    gOk = true;
    return true;
}

void addQuad(ImDrawList* dl, unsigned int tex, const IconQuad& q, ImU32 tint) {
    dl->AddImageQuad(static_cast<ImTextureID>(tex), q.p[0], q.p[1], q.p[2], q.p[3],
                     ImVec2(0.0f, 0.0f), ImVec2(1.0f, 0.0f), ImVec2(1.0f, 1.0f),
                     ImVec2(0.0f, 1.0f), tint);
}

}  // namespace

AircraftIcon aircraftIconForCategory(std::uint32_t category) {
    switch (category) {
        case CASCADE_AIRCRAFT_LIGHT: return AircraftIcon::Light;
        case CASCADE_AIRCRAFT_MEDIUM1:
        case CASCADE_AIRCRAFT_MEDIUM2:
        case CASCADE_AIRCRAFT_HIGH_VORTEX:
        case CASCADE_AIRCRAFT_HIGH_PERF:
        case CASCADE_AIRCRAFT_SPACE: return AircraftIcon::Airliner;
        case CASCADE_AIRCRAFT_HEAVY: return AircraftIcon::Heavy;
        case CASCADE_AIRCRAFT_ROTORCRAFT: return AircraftIcon::Helicopter;
        case CASCADE_AIRCRAFT_GLIDER: return AircraftIcon::Glider;
        case CASCADE_AIRCRAFT_LIGHTER_THAN_AIR:
        case CASCADE_AIRCRAFT_PARACHUTIST: return AircraftIcon::Balloon;
        case CASCADE_AIRCRAFT_ULTRALIGHT: return AircraftIcon::Microlight;
        case CASCADE_AIRCRAFT_UAV: return AircraftIcon::Uav;
        case CASCADE_AIRCRAFT_SURFACE_EMERGENCY:
        case CASCADE_AIRCRAFT_SURFACE_SERVICE:
        case CASCADE_AIRCRAFT_GROUND_OBSTRUCTION: return AircraftIcon::Surface;
        case CASCADE_AIRCRAFT_NONE:
        default: return AircraftIcon::Unknown;
    }
}

bool isSingleEngineHelicopterType(const char* icaoType) {
    if (icaoType == nullptr || icaoType[0] == '\0') { return false; }
    // Doc 8643 description H1P / H1T, checked 2026-09-23 - see the header.
    static constexpr const char* kSingle[] = {"R22",  "R44",  "R66",  "B06",
                                              "B407", "B505", "AS50", "EC20",
                                              "EC30", "H500", "H269", "EN28",
                                              "EN48", "G2CA", "A119", "B47G"};
    // Designators are upper case; a registry that answered in lower case, or
    // padded the field, is still the same aircraft.
    char norm[8] = {};
    std::size_t n = 0;
    for (const char* p = icaoType; *p != '\0'; ++p) {
        if (*p == ' ') { continue; }
        if (n + 1 >= sizeof(norm)) { return false; }
        char c = *p;
        if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
        norm[n++] = c;
    }
    for (const char* t : kSingle) {
        if (std::strcmp(norm, t) == 0) { return true; }
    }
    return false;
}

AircraftIcon aircraftIconFor(std::uint32_t category, const char* icaoType) {
    const AircraftIcon base = aircraftIconForCategory(category);
    if (base == AircraftIcon::Helicopter && isSingleEngineHelicopterType(icaoType)) {
        return AircraftIcon::HelicopterLight;
    }
    return base;
}

IconQuad aircraftIconQuad(const ImVec2& centre, double courseDeg, float sizePx) {
    const double a = (std::isnan(courseDeg) ? 0.0 : courseDeg) * kPi / 180.0;
    const float ca = static_cast<float>(std::cos(a));
    const float sa = static_cast<float>(std::sin(a));
    const float h = sizePx * 0.5f;
    // The same rotation the flat silhouettes used: screen y points down, so a
    // positive course turns the nose clockwise.
    const auto put = [&](float x, float y) {
        return ImVec2(centre.x + x * ca - y * sa, centre.y + x * sa + y * ca);
    };
    IconQuad q;
    q.p[0] = put(-h, -h);
    q.p[1] = put(h, -h);
    q.p[2] = put(h, h);
    q.p[3] = put(-h, h);
    return q;
}

ImVec2 aircraftShadowOffset(float sizePx) { return ImVec2(sizePx * 0.06f, sizePx * 0.08f); }

std::vector<std::uint8_t> halveRgba(const std::uint8_t* rgba, int side) {
    const int half = side / 2;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(half) * half * 4u);
    for (int y = 0; y < half; ++y) {
        for (int x = 0; x < half; ++x) {
            unsigned aSum = 0u;
            unsigned wr = 0u, wg = 0u, wb = 0u;  // alpha-weighted
            unsigned pr = 0u, pg = 0u, pb = 0u;  // plain
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const std::uint8_t* p =
                        rgba + (static_cast<std::size_t>(2 * y + dy) * side + (2 * x + dx)) * 4u;
                    aSum += p[3];
                    wr += p[0] * p[3];
                    wg += p[1] * p[3];
                    wb += p[2] * p[3];
                    pr += p[0];
                    pg += p[1];
                    pb += p[2];
                }
            }
            std::uint8_t* o = out.data() + (static_cast<std::size_t>(y) * half + x) * 4u;
            if (aSum == 0u) {
                o[0] = static_cast<std::uint8_t>((pr + 2u) / 4u);
                o[1] = static_cast<std::uint8_t>((pg + 2u) / 4u);
                o[2] = static_cast<std::uint8_t>((pb + 2u) / 4u);
            } else {
                o[0] = static_cast<std::uint8_t>((wr + aSum / 2u) / aSum);
                o[1] = static_cast<std::uint8_t>((wg + aSum / 2u) / aSum);
                o[2] = static_cast<std::uint8_t>((wb + aSum / 2u) / aSum);
            }
            o[3] = static_cast<std::uint8_t>((aSum + 2u) / 4u);
        }
    }
    return out;
}

std::vector<std::uint8_t> shadowFromAlpha(const std::uint8_t* rgba, int side, float opacity) {
    const std::size_t n = static_cast<std::size_t>(side) * side;
    std::vector<float> a(n), tmp(n);
    for (std::size_t i = 0; i < n; ++i) { a[i] = rgba[i * 4u + 3u] / 255.0f; }
    // Repeated box blurs approximate a Gaussian; separable, so each pass is a
    // horizontal then a vertical run. Edges clamp - an icon never touches the
    // border of its own texture closely enough for that to matter.
    const int r = kShadowBlurRadius;
    const float norm = 1.0f / static_cast<float>(2 * r + 1);
    for (int pass = 0; pass < kShadowBlurPasses; ++pass) {
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                float s = 0.0f;
                for (int k = -r; k <= r; ++k) {
                    const int xx = std::clamp(x + k, 0, side - 1);
                    s += a[static_cast<std::size_t>(y) * side + xx];
                }
                tmp[static_cast<std::size_t>(y) * side + x] = s * norm;
            }
        }
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                float s = 0.0f;
                for (int k = -r; k <= r; ++k) {
                    const int yy = std::clamp(y + k, 0, side - 1);
                    s += tmp[static_cast<std::size_t>(yy) * side + x];
                }
                a[static_cast<std::size_t>(y) * side + x] = s * norm;
            }
        }
    }
    std::vector<std::uint8_t> out(n * 4u, 0u);
    const float op = std::clamp(opacity, 0.0f, 1.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const float v = std::clamp(a[i] * op, 0.0f, 1.0f);
        out[i * 4u + 3u] = static_cast<std::uint8_t>(std::lround(v * 255.0f));
    }
    return out;
}

bool drawAircraftIcon(ImDrawList* dl, const ImVec2& centre, double courseDeg, float sizePx,
                      AircraftIcon icon, float alpha) {
    const int i = static_cast<int>(icon);
    if (dl == nullptr || i < 0 || i >= kAircraftIconCount) { return false; }
    if (!ensureTextures()) { return false; }
    const float al = std::clamp(alpha, 0.0f, 1.0f);
    const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(std::lround(al * 255.0f)));
    const ImVec2 off = aircraftShadowOffset(sizePx);
    addQuad(dl, gTex[i].shadow,
            aircraftIconQuad(ImVec2(centre.x + off.x, centre.y + off.y), courseDeg, sizePx), tint);
    addQuad(dl, gTex[i].icon, aircraftIconQuad(centre, courseDeg, sizePx), tint);
    return true;
}

float aircraftMarkerRadius(float sizePx) { return std::max(14.0f, sizePx * 0.6f); }

void drawAircraftMarker(ImDrawList* dl, const ImVec2& centre, double courseDeg, float sizePx,
                        std::uint32_t category, ImU32 altitudeCol, bool altKnown, bool picked,
                        const char* icaoType) {
    if (dl == nullptr) { return; }
    const unsigned a8 = (altitudeCol >> IM_COL32_A_SHIFT) & 0xFFu;
    const auto withAlpha = [&](float f) {
        return (altitudeCol & ~IM_COL32_A_MASK) |
               (static_cast<ImU32>(std::lround(static_cast<float>(a8) * f)) << IM_COL32_A_SHIFT);
    };
    const ImU32 rim = IM_COL32(0, 0, 0, a8);
    const float haloR = sizePx * 0.42f;
    if (altKnown) { dl->AddCircleFilled(centre, haloR, withAlpha(0.38f)); }
    dl->AddCircle(centre, haloR, withAlpha(0.9f), 0, 1.5f);
    if (picked) {
        const float r = aircraftMarkerRadius(sizePx);
        dl->AddCircle(centre, r + 1.5f, rim, 0, 1.5f);
        dl->AddCircle(centre, r, altitudeCol, 0, 3.0f);
    }
    if (drawAircraftIcon(dl, centre, courseDeg, sizePx, aircraftIconFor(category, icaoType),
                         static_cast<float>(a8) / 255.0f)) {
        return;
    }
    // No textures: the flat silhouette, at the size the setting asks for.
    addTrackSymbol(dl, centre, courseDeg, sizePx * 0.3f, altitudeCol, altKnown, category);
}

void releaseAircraftIconTextures() {
    for (IconTextures& t : gTex) {
        if (t.icon != 0u) { glDeleteTextures(1, &t.icon); }
        if (t.shadow != 0u) { glDeleteTextures(1, &t.shadow); }
        t = IconTextures{};
    }
    gTried = false;
    gOk = false;
}

}  // namespace cascade::gui
