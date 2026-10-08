// plugin_store_view.cpp - the PLUGIN STORE window, and the page body it shares
// with the FITTED MODULES window.
//
// Read the header first: it carries the division of labour with the FITTED
// MODULES window and, more importantly, the one claim this file refuses to draw.
//
// THE LOOK. A shop window in the application's own theme: phosphor outlines on the
// well, ivory names, muted summaries, small engraved capitals for headings and
// keys, the category glyphs drawn with the draw list. Nothing here is a bitmap
// except the catalogue's own pictures. Every colour is a theme:: name, so all six
// themes carry it, and every size comes from fonts::, so the interface scale does.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/plugin_store_view.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "core/i18n.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_repo.hpp"
#include "core/png_decode.hpp"
#include "core/utf8_text.hpp"
#include "gui/fonts.hpp"
#include "gui/scope_face.hpp"
#include "gui/text_fit.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"
#include "gui/ui_scale.hpp"
#include "imgui.h"

// GL last: <GL/gl.h> on Windows needs <windows.h> first, and the macros it brings
// are not wanted by anything above.
#if defined(_WIN32)
#include <windows.h>
#endif
#include <GL/gl.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace cascade::gui {
using cascade::i18n::tr;
using cascade::i18n::trId;
namespace {

// --- measurement --------------------------------------------------------------

float S() { return uiscale::factor(); }

float textW(ImFont* f, float px, const char* s) {
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}

float faceH(ImFont* f, float px) { return f->CalcTextSizeA(px, FLT_MAX, 0.0f, "Ag").y; }

float wrapH(ImFont* f, float px, float wrapWidth, const char* s) {
    if (s == nullptr || s[0] == '\0') { return 0.0f; }
    if (wrapWidth < 16.0f) { return faceH(f, px); }
    return f->CalcTextSizeA(px, FLT_MAX, wrapWidth, s).y;
}

std::string lowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return out;
}

// THE ONE SIZE EVERY SENTENCE ON A PAGE IS SET IN, through the public accessor so a
// test and the drawing cannot disagree about it.
float prose() { return storeProsePx(); }

bool figureLike(const char* s) {
    if (s == nullptr || s[0] == '\0') { return false; }
    for (const char* p = s; *p != '\0'; ++p) {
        const bool ok = (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == ':' ||
                        *p == ' ';
        if (!ok) { return false; }
    }
    return true;
}

std::string bytesText(std::uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ull * 1024ull) {
        std::snprintf(buf, sizeof buf, "%.2f MB", static_cast<double>(bytes) / 1.0e6);
    } else if (bytes >= 1000ull) {
        std::snprintf(buf, sizeof buf, "%.0f kB", static_cast<double>(bytes) / 1.0e3);
    } else {
        // EXACT UNDER A KILOBYTE, because rounding gets to "0 kB" - the one figure
        // that must never be printed for something that is there.
        std::snprintf(buf, sizeof buf, "%llu bytes", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

// The census helpers: names are built only when the census is on.
void censusRect(const std::string& name, float x0, float y0, float x1, float y1) {
    if (census::enabled()) { census::rect(name, x0, y0, x1, y1); }
}

// --- text cut to a width -------------------------------------------------------

std::vector<const char*> charStarts(const char* text) {
    std::vector<const char*> b;
    const char* p = text;
    while (*p != '\0') {
        b.push_back(p);
        p = cascade::core::utf8Next(p);
    }
    b.push_back(p);  // the end
    return b;
}

// --- the flattening of the glyphs' paths ---------------------------------------

struct Affine {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    ImVec2 apply(float x, float y) const { return ImVec2(a * x + c * y + e, b * x + d * y + f); }
};

Affine mul(const Affine& l, const Affine& r) {  // l * r (r applied first)
    Affine o;
    o.a = l.a * r.a + l.c * r.b;
    o.b = l.b * r.a + l.d * r.b;
    o.c = l.a * r.c + l.c * r.d;
    o.d = l.b * r.c + l.d * r.d;
    o.e = l.a * r.e + l.c * r.f + l.e;
    o.f = l.b * r.e + l.d * r.f + l.f;
    return o;
}
Affine translate(float x, float y) {
    Affine t;
    t.e = x;
    t.f = y;
    return t;
}
Affine scaleBy(float s) {
    Affine t;
    t.a = s;
    t.d = s;
    return t;
}
Affine rotateDeg(float deg) {
    const float r = deg * 3.14159265358979f / 180.0f;
    Affine t;
    t.a = std::cos(r);
    t.b = std::sin(r);
    t.c = -std::sin(r);
    t.d = std::cos(r);
    return t;
}

struct Glyph {
    std::vector<std::vector<ImVec2>> lines;  // in a 56 x 56 box
    std::vector<bool> closed;
    std::vector<ImVec2> dots;
};

void addLine(Glyph& g, std::vector<ImVec2> pts, bool closed) {
    if (pts.size() >= 2) {
        g.lines.push_back(std::move(pts));
        g.closed.push_back(closed);
    }
}

// A small SVG path reader: M L H V C Q T A Z, absolute and relative. Enough for the
// six shapes below, which are the mock-up's own paths.
void addPath(Glyph& g, const char* d, const Affine& tf) {
    const char* p = d;
    const auto skip = [&] {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n') { ++p; }
    };
    const auto number = [&](float& out) {
        skip();
        const char* q = p;
        if (*q == '-' || *q == '+') { ++q; }
        bool any = false;
        while (*q >= '0' && *q <= '9') { ++q; any = true; }
        if (*q == '.') {
            ++q;
            while (*q >= '0' && *q <= '9') { ++q; any = true; }
        }
        if (!any) { return false; }
        out = static_cast<float>(std::strtod(std::string(p, q).c_str(), nullptr));
        p = q;
        return true;
    };
    float cx = 0, cy = 0, sx = 0, sy = 0, lastCx = 0, lastCy = 0;
    char lastCmd = 0;
    std::vector<ImVec2> cur;
    const auto emit = [&](bool closed) {
        if (cur.size() == 2 && std::fabs(cur[0].x - cur[1].x) < 0.5f &&
            std::fabs(cur[0].y - cur[1].y) < 0.5f) {
            g.dots.push_back(cur[0]);  // a zero-length stroke with round caps is a dot
        } else {
            addLine(g, cur, closed);
        }
        cur.clear();
    };
    const auto pt = [&](float x, float y) { cur.push_back(tf.apply(x, y)); };
    char cmd = 0;
    while (true) {
        skip();
        if (*p == '\0') { break; }
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            cmd = *p++;
        } else if (cmd == 'M') {
            cmd = 'L';
        } else if (cmd == 'm') {
            cmd = 'l';
        }
        const bool rel = cmd >= 'a' && cmd <= 'z';
        const char C = static_cast<char>(rel ? cmd - 32 : cmd);
        float v[7] = {0, 0, 0, 0, 0, 0, 0};
        const auto args = [&](int n) {
            for (int i = 0; i < n; ++i) {
                if (!number(v[i])) { return false; }
            }
            return true;
        };
        switch (C) {
            case 'Z':
                if (!cur.empty()) { emit(true); }
                cx = sx;
                cy = sy;
                lastCmd = 'Z';
                continue;
            case 'M':
                if (!args(2)) { return; }
                if (!cur.empty()) { emit(false); }
                cx = rel ? cx + v[0] : v[0];
                cy = rel ? cy + v[1] : v[1];
                sx = cx;
                sy = cy;
                pt(cx, cy);
                break;
            case 'L':
                if (!args(2)) { return; }
                if (cur.empty()) { pt(cx, cy); }
                cx = rel ? cx + v[0] : v[0];
                cy = rel ? cy + v[1] : v[1];
                pt(cx, cy);
                break;
            case 'H':
                if (!args(1)) { return; }
                if (cur.empty()) { pt(cx, cy); }
                cx = rel ? cx + v[0] : v[0];
                pt(cx, cy);
                break;
            case 'V':
                if (!args(1)) { return; }
                if (cur.empty()) { pt(cx, cy); }
                cy = rel ? cy + v[0] : v[0];
                pt(cx, cy);
                break;
            case 'C': {
                if (!args(6)) { return; }
                if (cur.empty()) { pt(cx, cy); }
                const float x1 = rel ? cx + v[0] : v[0], y1 = rel ? cy + v[1] : v[1];
                const float x2 = rel ? cx + v[2] : v[2], y2 = rel ? cy + v[3] : v[3];
                const float x3 = rel ? cx + v[4] : v[4], y3 = rel ? cy + v[5] : v[5];
                for (int i = 1; i <= 14; ++i) {
                    const float t = static_cast<float>(i) / 14.0f, u = 1.0f - t;
                    pt(u * u * u * cx + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                       u * u * u * cy + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
                }
                lastCx = x2;
                lastCy = y2;
                cx = x3;
                cy = y3;
                break;
            }
            case 'Q':
            case 'T': {
                float x1, y1, x2, y2;
                if (C == 'Q') {
                    if (!args(4)) { return; }
                    x1 = rel ? cx + v[0] : v[0];
                    y1 = rel ? cy + v[1] : v[1];
                    x2 = rel ? cx + v[2] : v[2];
                    y2 = rel ? cy + v[3] : v[3];
                } else {
                    if (!args(2)) { return; }
                    // the reflection of the previous quadratic control about this point
                    x1 = (lastCmd == 'Q' || lastCmd == 'T') ? 2 * cx - lastCx : cx;
                    y1 = (lastCmd == 'Q' || lastCmd == 'T') ? 2 * cy - lastCy : cy;
                    x2 = rel ? cx + v[0] : v[0];
                    y2 = rel ? cy + v[1] : v[1];
                }
                if (cur.empty()) { pt(cx, cy); }
                for (int i = 1; i <= 10; ++i) {
                    const float t = static_cast<float>(i) / 10.0f, u = 1.0f - t;
                    pt(u * u * cx + 2 * u * t * x1 + t * t * x2,
                       u * u * cy + 2 * u * t * y1 + t * t * y2);
                }
                lastCx = x1;
                lastCy = y1;
                cx = x2;
                cy = y2;
                break;
            }
            case 'A': {
                // v: rx ry rotation large-arc sweep x y. Circular arcs only (rx == ry).
                if (!args(7)) { return; }
                const float r = v[0];
                const bool large = v[3] != 0.0f, sweep = v[4] != 0.0f;
                const float x2 = rel ? cx + v[5] : v[5], y2 = rel ? cy + v[6] : v[6];
                if (cur.empty()) { pt(cx, cy); }
                const float dx = (cx - x2) * 0.5f, dy = (cy - y2) * 0.5f;
                const float d2 = dx * dx + dy * dy;
                if (d2 > 0.0f && r > 0.0f) {
                    float rr = r;
                    if (d2 > rr * rr) { rr = std::sqrt(d2); }
                    const float k = std::sqrt(std::max(0.0f, (rr * rr - d2) / d2));
                    const float sgn = (large == sweep) ? -1.0f : 1.0f;
                    const float ccx = sgn * k * dy + (cx + x2) * 0.5f;
                    const float ccy = -sgn * k * dx + (cy + y2) * 0.5f;
                    const float a0 = std::atan2(cy - ccy, cx - ccx);
                    float a1 = std::atan2(y2 - ccy, x2 - ccx);
                    float da = a1 - a0;
                    if (sweep && da < 0) { da += 6.28318530718f; }
                    if (!sweep && da > 0) { da -= 6.28318530718f; }
                    const int n = std::max(4, static_cast<int>(std::fabs(da) * 6.0f));
                    for (int i = 1; i <= n; ++i) {
                        const float a = a0 + da * static_cast<float>(i) / static_cast<float>(n);
                        pt(ccx + rr * std::cos(a), ccy + rr * std::sin(a));
                    }
                } else {
                    pt(x2, y2);
                }
                cx = x2;
                cy = y2;
                break;
            }
            default:
                return;
        }
        lastCmd = C;
    }
    if (!cur.empty()) { emit(false); }
}

void addCircle(Glyph& g, float cx, float cy, float r, const Affine& tf) {
    std::vector<ImVec2> pts;
    for (int i = 0; i < 28; ++i) {
        const float a = 6.28318530718f * static_cast<float>(i) / 28.0f;
        pts.push_back(tf.apply(cx + r * std::cos(a), cy + r * std::sin(a)));
    }
    addLine(g, std::move(pts), true);
}

void addRect(Glyph& g, float x, float y, float w, float h, const Affine& tf) {
    addLine(g, {tf.apply(x, y), tf.apply(x + w, y), tf.apply(x + w, y + h), tf.apply(x, y + h)},
            true);
}

const Glyph& glyphFor(const std::string& category) {
    static const std::array<Glyph, 6> glyphs = [] {
        std::array<Glyph, 6> g;
        const Affine id;
        // aircraft: the mock-up's group transform, applied to its one path
        {
            const Affine tf = mul(translate(28, 28),
                                  mul(rotateDeg(40), mul(scaleBy(0.82f), translate(-28, -28))));
            addPath(g[0],
                    "M28 5C30.2 5 31.5 8 31.5 12V22L51 35V39L31.5 33.5V43L37 47.5V51L28 48.5L19 "
                    "51V47.5L24.5 43V33.5L5 39V35L24.5 22V12C24.5 8 25.8 5 28 5Z",
                    tf);
        }
        // marine
        addPath(g[1], "M5 33H51L45 44H12Z", id);
        addPath(g[1], "M15 33V26H37V33", id);
        addPath(g[1], "M21 26V20H31V26", id);
        addPath(g[1], "M25 20V13H30V20", id);
        addPath(g[1], "M20 29.5h.01M25 29.5h.01M30 29.5h.01M35 29.5h.01", id);
        addPath(g[1], "M4 49q5-3.5 10 0t10 0t10 0t10 0t10 0", id);
        // satellites and weather: rotated -45 degrees about the middle
        {
            const Affine tf = mul(translate(28, 28), mul(rotateDeg(-45), translate(-28, -28)));
            addRect(g[2], 22, 22, 12, 12, tf);
            addPath(g[2], "M22 28H20M34 28H36", tf);
            addRect(g[2], 5, 22.5f, 15, 11, tf);
            addRect(g[2], 36, 22.5f, 15, 11, tf);
            addPath(g[2], "M10 22.5V33.5M15 22.5V33.5M41 22.5V33.5M46 22.5V33.5", tf);
            addPath(g[2], "M28 34V38M23.5 43Q28 36 32.5 43", tf);
        }
        // meters and paging: a dial
        addCircle(g[3], 28, 30, 22, id);
        addPath(g[3],
                "M16.74 23.5L13.28 21.5M19.64 20.04L17.07 16.98M23.55 17.78L22.19 14.02M28 "
                "17V13M32.45 17.78L33.81 14.02M36.36 20.04L38.93 16.98M39.26 23.5L42.72 21.5",
                id);
        addPath(g[3], "M28 30L37 17", id);
        addCircle(g[3], 28, 30, 2.5f, id);
        addPath(g[3], "M19 42H37", id);
        // voice and data: a mast with its waves
        addCircle(g[4], 28, 12, 2, id);
        addPath(g[4],
                "M28 14L20 50M28 14L36 50M18 50H38M25.8 24H30.2M24 32H32M22.2 40H33.8M24 "
                "32L33.8 40M32 32L22.2 40",
                id);
        addPath(g[4], "M21 8A8 8 0 0 0 21 16M17 5A13 13 0 0 0 17 19M35 8A8 8 0 0 1 35 16M39 "
                      "5A13 13 0 0 1 39 19",
                id);
        // maps and tools: a pin
        addPath(g[5], "M28 50C28 50 12 33 12 22A16 16 0 0 1 44 22C44 33 28 50 28 50Z", id);
        addCircle(g[5], 28, 22, 6, id);
        addPath(g[5], "M17 53H39", id);
        return g;
    }();
    // The dial is the glyph of anything with no category of its own.
    if (category == "aircraft") { return glyphs[0]; }
    if (category == "marine") { return glyphs[1]; }
    if (category == "satellites-weather") { return glyphs[2]; }
    if (category == "broadcast") { return glyphs[4]; }
    if (category == "maps-tools") { return glyphs[5]; }
    return glyphs[3];
}

// --- the keys --------------------------------------------------------------------

float keyPx() { return fonts::tinyPx(); }
float keyTracking() { return keyPx() * 0.10f; }

// A SMALL CAPS WORD AT THE KEY'S SIZE, tracked, fitted into [x0, x1] and centred.
void drawKeyWord(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* label,
                 ImU32 ink, float yNudge = 0.0f) {
    ImFont* f = fonts::legend();
    float px = keyPx();
    const float room = (br.x - tl.x) - 8.0f;
    px = fitTrackedPx(f, px, label, 0.10f, room, fitFloorFor(px));
    const float w = trackedWidth(f, px, label, px * 0.10f);
    const float h = faceH(f, px);
    const ImVec2 at((tl.x + br.x) * 0.5f - w * 0.5f, (tl.y + br.y) * 0.5f - h * 0.5f + yNudge);
    addTrackedText(dl, f, px, at, ink, label, px * 0.10f, br.x - 2.0f);
}

void drawLoupe(ImDrawList* dl, const ImVec2& c, float r, ImU32 col) {
    dl->AddCircle(c, r, col, 16, 1.6f);
    dl->AddLine(ImVec2(c.x + r * 0.72f, c.y + r * 0.72f), ImVec2(c.x + r * 1.7f, c.y + r * 1.7f),
                col, 1.8f);
}

// A tab of the top bar: proud metal, the active one lit and carrying the phosphor
// underline that says where you are.
bool drawTab(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* label, bool active,
             const char* id) {
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    const bool pressed = ImGui::InvisibleButton("##tab", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    ImGui::PopID();
    const float r = theme::kKeyRounding;
    const ImU32 top = active ? theme::kIvory : (hovered ? theme::kBrassBright : theme::kBrassMid);
    const ImU32 bot = active ? theme::kCream : theme::kBrassDark;
    dl->AddRectFilled(tl, br, bot, r);
    if (br.x - tl.x > r * 2.0f) {
        dl->AddRectFilledMultiColor(ImVec2(tl.x + r, tl.y), ImVec2(br.x - r, br.y), top, top, bot,
                                    bot);
    }
    addBenchBevel(dl, tl, br, r, true);
    drawKeyWord(dl, tl, br, label, active ? theme::kEnamel : theme::kCream);
    if (active) {
        // The underline: phosphor, with a soft glow, under the key.
        const float y = br.y + 7.0f * S();
        dl->AddRectFilled(ImVec2(tl.x, y - 1.0f), ImVec2(br.x, y + 3.0f),
                          theme::withAlpha(theme::kPhosphor, 0.25f));
        dl->AddRectFilled(ImVec2(tl.x, y), ImVec2(br.x, y + 2.0f), theme::kPhosphor);
    }
    if (focused) {
        dl->AddRect(ImVec2(tl.x - 2.0f, tl.y - 2.0f), ImVec2(br.x + 2.0f, br.y + 2.0f),
                    theme::kBrassBright, r + 1.0f, 0, theme::kHairline);
    }
    return pressed;
}

// A small amber, muted or phosphor outline badge: EXPERIMENTAL, WINDOWS ONLY.
float badgeWidth(const char* label) {
    ImFont* f = fonts::legend();
    const float px = std::max(10.0f, keyPx() * 0.78f);
    return trackedWidth(f, px, label, px * 0.08f) + 10.0f * S();
}
float badgeHeight() {
    return faceH(fonts::legend(), std::max(10.0f, keyPx() * 0.78f)) + 4.0f * S();
}
void drawBadge(ImDrawList* dl, const ImVec2& tl, const char* label, ImU32 ink) {
    ImFont* f = fonts::legend();
    const float px = std::max(10.0f, keyPx() * 0.78f);
    const float w = badgeWidth(label);
    const float h = badgeHeight();
    dl->AddRect(tl, ImVec2(tl.x + w, tl.y + h), ink, 1.0f, 0, 1.0f);
    addTrackedText(dl, f, px, ImVec2(tl.x + 5.0f * S(), tl.y + 2.0f * S()), ink, label,
                   px * 0.08f);
}

// A phosphor underline sweeping along a key while a transfer with no known length
// runs, or filling to `frac` when there is one.
void drawFittingLine(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, float frac) {
    const float y1 = br.y - 1.0f;
    const float y0 = y1 - 2.0f;
    dl->AddRectFilled(ImVec2(tl.x + 1.0f, y0), ImVec2(br.x - 1.0f, y1),
                      theme::withAlpha(theme::kPhosphorDim, 0.35f));
    const float w = br.x - tl.x - 2.0f;
    if (frac > 0.0f) {
        dl->AddRectFilled(ImVec2(tl.x + 1.0f, y0), ImVec2(tl.x + 1.0f + w * std::min(1.0f, frac), y1),
                          theme::kPhosphor);
    } else {
        const float t = static_cast<float>(std::fmod(ImGui::GetTime() * 0.9, 1.0));
        const float seg = w * 0.3f;
        const float x0 = tl.x + 1.0f + (w + seg) * t - seg;
        dl->AddRectFilled(ImVec2(std::max(tl.x + 1.0f, x0), y0),
                          ImVec2(std::min(br.x - 1.0f, x0 + seg), y1), theme::kPhosphor);
    }
}

// --- the words -------------------------------------------------------------------

const char* const kNoticeReason = FOX_TR_NOOP("the legal notice must be acknowledged first");
const char* const kTransferReason = FOX_TR_NOOP("a transfer is already in progress");

bool localTm(std::int64_t t, std::tm& out) {
    const std::time_t tt = static_cast<std::time_t>(t);
#if defined(_WIN32)
    return localtime_s(&out, &tt) == 0;
#else
    return localtime_r(&tt, &out) != nullptr;
#endif
}

std::string dateText(std::int64_t t) {
    std::tm tm{};
    if (t <= 0 || !localTm(t, tm)) { return tr("an unknown date"); }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

// "HH:MM" for a read made today, "YYYY-MM-DD HH:MM" for an earlier day.
std::string readTimeText(std::int64_t t, std::int64_t now) {
    std::tm tm{};
    if (t <= 0 || !localTm(t, tm)) { return tr("an unknown date"); }
    std::tm nowTm{};
    const bool haveNow = localTm(now, nowTm);
    const bool today = haveNow && nowTm.tm_year == tm.tm_year && nowTm.tm_yday == tm.tm_yday;
    char buf[48];
    if (today) {
        std::snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900,
                      tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    }
    return buf;
}

// What a catalogue with no rows says, and why. FOUR answers, not two: nobody has
// asked, it was asked and failed, it was asked and listed nothing, or it was read.
enum class CatalogueState { NeverAsked, Failed, ReadEmpty, Read };
CatalogueState catalogueState(const PluginStoreModel& m) {
    if (m.haveCatalogue) { return CatalogueState::Read; }
    if (!m.sourceStatus.empty()) { return CatalogueState::ReadEmpty; }
    if (!m.sourceError.empty()) { return CatalogueState::Failed; }
    return CatalogueState::NeverAsked;
}

}  // namespace

// ===========================================================================
// THE WORDS AND DECISIONS - pure
// ===========================================================================

StoreCategory storeCategoryFor(const std::string& id) {
    if (id == "aircraft") { return StoreCategory::Aircraft; }
    if (id == "marine") { return StoreCategory::Marine; }
    if (id == "satellites-weather") { return StoreCategory::SatellitesWeather; }
    if (id == "meters-paging") { return StoreCategory::MetersPaging; }
    if (id == "broadcast") { return StoreCategory::VoiceData; }
    if (id == "maps-tools") { return StoreCategory::MapsTools; }
    return StoreCategory::Other;
}

const char* storeCategoryHeading(StoreCategory c) {
    switch (c) {
        case StoreCategory::Aircraft: return tr("AIRCRAFT");
        case StoreCategory::Marine: return tr("MARINE");
        case StoreCategory::SatellitesWeather: return tr("SATELLITES AND WEATHER");
        case StoreCategory::MetersPaging: return tr("METERS AND PAGING");
        case StoreCategory::VoiceData: return tr("VOICE AND DATA");
        case StoreCategory::MapsTools: return tr("MAPS AND TOOLS");
        case StoreCategory::Other: return tr("OTHER");
    }
    return tr("OTHER");
}

const char* storeCategoryCensusName(StoreCategory c) {
    switch (c) {
        case StoreCategory::Aircraft: return "aircraft";
        case StoreCategory::Marine: return "marine";
        case StoreCategory::SatellitesWeather: return "satellites";
        case StoreCategory::MetersPaging: return "meters";
        case StoreCategory::VoiceData: return "voice";
        case StoreCategory::MapsTools: return "maps";
        case StoreCategory::Other: return "other";
    }
    return "other";
}

StoreKey storeKeyFor(const StoreModule& sm, const StoreKeyIn& in) {
    StoreKey k;
    // This plugin's own transfer: FITTING..., not a key to press.
    if (!in.busyId.empty() && in.busyId == sm.id) {
        k.kind = StoreKeyKind::Fitting;
        return k;
    }
    switch (sm.install) {
        case StoreInstallKind::Installed:
        case StoreInstallKind::NewerInstalled:
            k.kind = StoreKeyKind::Installed;
            return k;
        case StoreInstallKind::UpdateAvailable:
            k.kind = StoreKeyKind::Update;
            if (sm.updateToVersion.empty()) {
                // Newer by version and not plannable here: no build for this host, or
                // another ABI. The key says so rather than doing nothing.
                k.reason = sm.updateBlockedReason;
            } else if (in.busyAny) {
                k.reason = kTransferReason;
            }
            k.enabled = k.reason.empty() && !sm.updateToVersion.empty();
            return k;
        case StoreInstallKind::NotInstalled:
            break;
    }
    k.kind = StoreKeyKind::Get;
    // The gate asked as if the notice were acknowledged: whatever it says is a
    // reason a tick cannot cure.
    std::string reason = sm.blockedReasonIfAcknowledged;
    if (reason.empty() && !sm.plate.legalNotice.empty()) {
        if (in.onPage) {
            if (!in.noticeTicked) { reason = kNoticeReason; }
        } else {
            // On a card, GET on a plugin with a notice OPENS ITS PAGE: the notice cannot
            // be skipped, and the key is not greyed for it.
            k.opensPage = true;
        }
    }
    k.reason = reason;
    k.enabled = reason.empty();
    return k;
}

const char* storeKeyLabel(StoreKeyKind k) {
    switch (k) {
        case StoreKeyKind::Get: return tr("GET");
        case StoreKeyKind::Fitting: return tr("FITTING...");
        case StoreKeyKind::Installed: return tr("INSTALLED");
        case StoreKeyKind::Update: return tr("UPDATE");
    }
    return tr("GET");
}

const char* storeKeyCensusState(const StoreKey& k) {
    if ((k.kind == StoreKeyKind::Get || k.kind == StoreKeyKind::Update) && !k.enabled) {
        return "greyed";
    }
    switch (k.kind) {
        case StoreKeyKind::Get: return "get";
        case StoreKeyKind::Fitting: return "fitting";
        case StoreKeyKind::Installed: return "installed";
        case StoreKeyKind::Update: return "update";
    }
    return "get";
}

const char* storeExperimentalBadge() { return tr("EXPERIMENTAL"); }

const char* storeBuildBadge(const StoreModule& sm) {
    if (sm.haveBuildHere) { return ""; }
    if (sm.buildsWindows && !sm.buildsLinux) { return tr("WINDOWS ONLY"); }
    if (sm.buildsLinux && !sm.buildsWindows) { return tr("LINUX ONLY"); }
    return "";
}

std::string storeHeaderMeta(const StoreModule& sm, const std::string& hostPlatform) {
    const ModulePlate& p = sm.plate;
    std::string out = p.maker.empty() ? std::string(tr("maker not stated")) : p.maker;
    const auto add = [&](const std::string& piece) { out += "  \xc2\xb7  " + piece; };
    add(cascade::core::formatText(tr("version %s"), p.version.empty() ? "?" : p.version.c_str()));
    if (sm.haveBuildHere) {
        if (p.haveSizeBytes) { add(bytesText(p.sizeBytes)); }
        add(hostPlatform.empty() ? p.platforms : hostPlatform);
    } else {
        add(tr("no build for this system"));
    }
    return out;
}

std::string storeFromTo(const std::string& from, const std::string& to) {
    return cascade::core::formatText(tr("%s to %s"), from.c_str(), to.c_str());
}

int storeUpdateCount(const PluginStoreModel& m) {
    int n = 0;
    for (const StoreModule& sm : m.modules) {
        if (!sm.updateToVersion.empty()) { ++n; }
    }
    return n;
}

bool storeMatchesQuery(const StoreModule& sm, const std::string& lowerQuery) {
    if (lowerQuery.empty()) { return true; }
    const std::string hay = lowerAscii(sm.plate.name + " " + sm.plate.summary + " " +
                                       sm.plate.blurb + " " +
                                       storeCategoryHeading(storeCategoryFor(sm.plate.category)));
    return hay.find(lowerQuery) != std::string::npos;
}

std::vector<StoreSection> storeBrowseSections(const PluginStoreModel& m,
                                              const std::string& lowerQuery) {
    std::array<std::vector<int>, kStoreCategoryCount> by;
    for (int i = 0; i < static_cast<int>(m.modules.size()); ++i) {
        const StoreModule& sm = m.modules[static_cast<std::size_t>(i)];
        if (!storeMatchesQuery(sm, lowerQuery)) { continue; }
        by[static_cast<std::size_t>(storeCategoryFor(sm.plate.category))].push_back(i);
    }
    std::vector<StoreSection> out;
    for (int c = 0; c < kStoreCategoryCount; ++c) {
        std::vector<int>& v = by[static_cast<std::size_t>(c)];
        if (v.empty()) { continue; }
        std::sort(v.begin(), v.end(), [&](int a, int b) {
            const StoreModule& x = m.modules[static_cast<std::size_t>(a)];
            const StoreModule& y = m.modules[static_cast<std::size_t>(b)];
            const std::string la = lowerAscii(x.plate.name), lb = lowerAscii(y.plate.name);
            if (la != lb) { return la < lb; }
            return x.id < y.id;
        });
        StoreSection s;
        s.category = static_cast<StoreCategory>(c);
        s.modules = std::move(v);
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<int> storeUpdateRows(const PluginStoreModel& m, const std::string& lowerQuery) {
    std::vector<int> rows;
    for (int i = 0; i < static_cast<int>(m.modules.size()); ++i) {
        const StoreModule& sm = m.modules[static_cast<std::size_t>(i)];
        if (sm.updateToVersion.empty() || !storeMatchesQuery(sm, lowerQuery)) { continue; }
        rows.push_back(i);
    }
    std::sort(rows.begin(), rows.end(), [&](int a, int b) {
        return lowerAscii(m.modules[static_cast<std::size_t>(a)].plate.name) <
               lowerAscii(m.modules[static_cast<std::size_t>(b)].plate.name);
    });
    return rows;
}

StoreCatalogueLine storeCatalogueLine(const PluginStoreModel& m, std::int64_t nowUnix) {
    StoreCatalogueLine l;
    if (!m.haveCatalogue) {
        l.text = tr("CATALOGUE NOT READ");
        return l;
    }
    l.cached = m.catalogueFromCache;
    if (m.refreshFailed && !m.sourceError.empty()) {
        // THE COPY ON SCREEN IS NOT THE LATEST, and the reason is the user's evidence,
        // verbatim: a kept copy from an earlier session, or an earlier read of this one.
        l.text = cascade::core::formatText(tr("Catalogue from %s; could not refresh: %s"),
                                           dateText(m.catalogueReadTime).c_str(),
                                           m.sourceError.c_str());
        l.tone = StoreLineTone::Amber;
        return l;
    }
    l.text = cascade::core::formatText(tr("Catalogue read %s"),
                                       readTimeText(m.catalogueReadTime, nowUnix).c_str());
    return l;
}

int storeColumnsFor(float contentWidth) { return contentWidth >= 1120.0f ? 3 : 2; }

float storeProsePx() { return fonts::panelPx(); }

ImGuiWindowFlags storeFaceWindowFlags() {
    // The view's own body child scrolls; this pane holds exactly the view.
    return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
}

void forgetCatalogueConsent(PluginStoreDeck& deck) {
    deck.legalAck = false;
    deck.addAllAck = false;
}

std::vector<std::string> modulePageSections(bool hasNoticeBox) {
    std::vector<std::string> s = {"screenshots", "whatitdoes", "whatsnew"};
    if (hasNoticeBox) { s.push_back("beforeyoufitit"); }
    s.push_back("details");
    return s;
}

std::string moduleWhatsNewText(const ModulePlate& m) {
    if (!m.whatsNew.empty()) { return m.whatsNew; }
    return cascade::core::formatText(tr("%s: first release."),
                                     m.version.empty() ? "?" : m.version.c_str());
}

// ===========================================================================
// REACH, FACTS, AND THE REASONS
// ===========================================================================

namespace {

const char* kNotRead = FOX_TR_NOOP("not read");

// THE THREE BITS THAT MAKE A MODULE SOMETHING SIGNAL CAN BE ROUTED TO.
constexpr std::uint32_t kSignalCaps =
    CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER | CASCADE_CAP_IMAGE_DECODER;

enum class PlateState { NotFitted, Refused, Stopped, NoSignal, Started };

PlateState plateState(const ModulePlate& m) {
    if (!m.fitted) { return PlateState::NotFitted; }
    if (!m.loaded) { return PlateState::Refused; }
    if (!m.running) { return PlateState::Stopped; }
    if (m.haveCapabilities && (m.capabilities & kSignalCaps) == 0u) { return PlateState::NoSignal; }
    return PlateState::Started;
}

}  // namespace

std::vector<ReachRow> moduleReachRows(const ModulePlate& m) {
    std::vector<ReachRow> r;
    if (!m.haveCapabilities) { return r; }
    const std::uint32_t c = m.capabilities;
    const auto add = [&](const char* key, const std::string& detail, bool outward) {
        r.push_back(ReachRow{key, detail, outward});
    };
    if ((c & CASCADE_CAP_DECODER) != 0u) {
        add(tr("Audio decoder"), tr("Fed the demodulated audio the speakers get."), false);
    }
    if ((c & CASCADE_CAP_IQ_DECODER) != 0u) {
        add(tr("I/Q decoder"), tr("Fed complex baseband straight from the receiver."), false);
    }
    if ((c & CASCADE_CAP_IMAGE_DECODER) != 0u) {
        add(tr("Image decoder"), tr("Fed samples; returns pictures the host displays."), false);
    }
    if ((c & CASCADE_CAP_AUDIO_OUT) != 0u) {
        // REPLACES, and the word is the whole row: while it decodes, what the
        // speakers play is the module's and the demodulated audio is not there.
        add(tr("Plays sound through FoxSDR"),
            tr("Replaces the receiver's audio while it is decoding."), false);
    }
    if ((c & CASCADE_CAP_AUDIO_PROCESSOR) != 0u) {
        add(tr("Audio processor"), tr("Changes the receiver's audio before you hear it."), false);
    }
    if ((c & CASCADE_CAP_TRACK_SOURCE) != 0u) {
        add(tr("Map targets"), tr("Publishes positions the host draws on its map."), false);
    }
    if ((c & CASCADE_CAP_PANEL) != 0u) {
        add(tr("A window of its own"), tr("Rows and controls the host draws for it."), false);
    }
    if ((c & CASCADE_CAP_INSTRUMENT) != 0u) {
        add(tr("An instrument of its own"),
            tr("A face the host draws as a piece of equipment, fed by the module."), false);
    }
    if ((c & CASCADE_CAP_PRESET) != 0u) {
        add(tr("Presets"),
            tr("Publishes where it listens. A suggestion - pressing one is the user tuning, not "
               "the module."),
            false);
    }
    if ((c & CASCADE_CAP_HOST_CLIENT) != 0u) {
        std::string d;
        if (!m.haveTuneGrant) {
            d = tr("Refused unless you grant it, per module. This grant and the radio-settings "
                   "grant are the only permissions the console actually enforces.");
        } else if (m.tuneGranted) {
            d = tr("GRANTED. It may retune the receiver on its own, without asking again.");
        } else {
            d = tr("Not granted, so every request to retune is answered DENIED.");
        }
        add(tr("Can ask to move the receiver"), d, true);
    }
    if ((c & CASCADE_CAP_RECEIVER_LOCATOR) != 0u) {
        add(tr("Can read your receiver's locator"),
            tr("Sees a 6-character Maidenhead grid square for wherever the receiver's position is "
               "set (GPS, \"Set RX here\", or typed) - a few kilometres' precision, not an exact "
               "point. What it does with that is up to the module; some report it onward, such as "
               "an optional PSK Reporter upload."),
            true);
    }
    if ((c & CASCADE_CAP_BASEMAP) != 0u) {
        add(tr("Map imagery"),
            tr("Supplies the map tiles from whatever source it chose - which may be an online "
               "tile server. Nothing here points it at one."),
            true);
    }
    if ((c & CASCADE_CAP_TRACK_INFO) != 0u) {
        add(tr("Target look-up"),
            tr("Looks up who a target is, from whatever source it chose - which may be an online "
               "service."),
            true);
    }
    if (r.empty()) {
        if (m.capabilities == 0u) {
            add(tr("Declares nothing"), tr("The record carries no capability bits."), false);
        } else {
            add(tr("Declares a capability this build does not know"),
                tr("The module was built against a newer host."), true);
        }
    }
    return r;
}

std::string moduleReachesLine(const ModulePlate& m) {
    if (!m.haveCapabilities) {
        return (m.fitted && !m.loaded) ? tr("not known: the host did not accept this file")
                                       : tr("not declared until it is fitted");
    }
    std::string out;
    for (const ReachRow& r : moduleReachRows(m)) {
        if (!out.empty()) { out += ", "; }
        out += r.key;
    }
    return out;
}

std::string moduleReachSummary(const ModulePlate& m) {
    // NEVER "reaches nothing". Every plugin here is native code mapped into this
    // process; there is no data-only module type.
    if (!m.haveCapabilities) {
        return (m.fitted && !m.loaded) ? tr("not known: the host did not accept this file")
                                       : tr("not declared until it is fitted");
    }
    if ((m.capabilities & CASCADE_CAP_HOST_CLIENT) != 0u) {
        return m.haveTuneGrant && m.tuneGranted ? tr("granted: may move the receiver")
                                                : tr("asks to move the receiver");
    }
    if ((m.capabilities & (CASCADE_CAP_BASEMAP | CASCADE_CAP_TRACK_INFO)) != 0u) {
        return tr("may fetch from a server it chose");
    }
    return tr("publishes to the host only");
}

ImU32 moduleReachColour(const ModulePlate& m) {
    if (!m.haveCapabilities) { return theme::kInkFaint; }
    if ((m.capabilities &
         (CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_BASEMAP | CASCADE_CAP_TRACK_INFO)) != 0u) {
        return theme::kGold;
    }
    return theme::kInkMuted;
}

std::string moduleMachineText(const ModulePlate& m) {
    switch (plateState(m)) {
        case PlateState::NotFitted: return tr("not fitted");
        case PlateState::Refused: return tr("fitted, refused");
        case PlateState::Stopped: return tr("fitted, stopped");
        case PlateState::NoSignal: return tr("fitted, takes no signal");
        case PlateState::Started: return tr("fitted and started");
    }
    return tr("not fitted");
}

std::vector<PageFact> modulePageFacts(const ModulePlate& m) {
    std::vector<PageFact> f;
    const bool read = m.haveDescriptor;
    const auto hatch = [&](const char* key, const std::string& why) {
        PageFact x;
        x.key = key;
        x.value = why;
        x.hatched = true;
        return x;
    };

    {
        PageFact x{tr("MAKER"), m.maker, false, false, 0};
        if (!read) {
            x = hatch(tr("MAKER"), tr(kNotRead));
        } else if (m.maker.empty()) {
            x = hatch(tr("MAKER"), tr("not stated"));
        }
        f.push_back(x);
    }
    {
        // "No licence" is a decision, not a blank - but only where one was looked for.
        PageFact x{tr("LICENCE"), m.licence, false, false, 0};
        if (!read) {
            x = hatch(tr("LICENCE"), tr(kNotRead));
        } else if (m.licence.empty()) {
            x = hatch(tr("LICENCE"), tr("none declared"));
            x.tone = theme::kGold;
        }
        f.push_back(x);
    }
    {
        PageFact x{tr("VERSION"), m.version, false, false, theme::kAmber};
        if (!read || m.version.empty()) { x = hatch(tr("VERSION"), read ? tr("not stated") : tr(kNotRead)); }
        f.push_back(x);
    }
    {
        PageFact x{tr("PLUGIN ABI"), {}, false, false, 0};
        if (!m.haveAbi) {
            // abiVersion 0 means "not recorded": unknown, never a mismatch.
            x = hatch(tr("PLUGIN ABI"), tr("not recorded"));
        } else if (m.abiVersion == m.hostAbiVersion) {
            x.value = cascade::core::formatText(tr("%u, matches this build"), m.abiVersion);
            x.tone = theme::kPhosphor;
        } else {
            x.value = cascade::core::formatText(tr("%u, this build needs %u"), m.abiVersion,
                                                m.hostAbiVersion);
            x.tone = theme::kGold;
        }
        f.push_back(x);
    }
    {
        PageFact x{tr("DOWNLOAD"), {}, false, false, theme::kAmber};
        if (m.haveSizeBytes) {
            x.value = bytesText(m.sizeBytes);
        } else {
            x = hatch(tr("DOWNLOAD"), tr("not stated"));
        }
        f.push_back(x);
    }
    {
        PageFact x{tr("BUILDS FOR"), m.platforms, false, false, 0};
        if (m.platforms.empty()) { x = hatch(tr("BUILDS FOR"), tr("not stated")); }
        f.push_back(x);
    }
    {
        PageFact x{tr("REACHES"), moduleReachesLine(m), false, false, 0};
        if (!m.haveCapabilities) { x.hatched = true; }
        f.push_back(x);
    }
    {
        PageFact x{tr("HOMEPAGE"), m.homepage, false, true, 0};
        if (m.homepage.empty()) { x = hatch(tr("HOMEPAGE"), tr("not stated")); }
        f.push_back(x);
    }
    {
        PageFact x{tr("SHA-256"), m.sha256, false, true, theme::kAmber};
        if (m.sha256.empty()) { x = hatch(tr("SHA-256"), tr("not stated")); }
        f.push_back(x);
    }
    {
        PageFact x{tr("PUBLISHED"), m.published, false, false, 0};
        if (m.published.empty()) { x = hatch(tr("PUBLISHED"), tr("not stated")); }
        f.push_back(x);
    }
    if (m.fitted) {
        PageFact x{tr("ON THIS MACHINE"), moduleMachineText(m), false, false, 0};
        x.tone = plateState(m) == PlateState::Refused ? theme::kAlarmHot : theme::kIvory;
        f.push_back(x);
        if (!m.fileName.empty()) {
            f.push_back(PageFact{tr("FILE"), m.fileName, false, true, theme::kInkMuted});
        }
    }
    if (!m.retirementFloor.empty()) {
        f.push_back(PageFact{tr("RETIRED BELOW"), m.retirementFloor, false, false, theme::kGold});
    }
    return f;
}

// --- a reason kept in English, drawn in the language in force -----------------
namespace {
constexpr const char* kAbiReasonFormat =
    FOX_TR_NOOP("not compatible with this version (built for plugin ABI %u, this build "
                "requires exactly %u)");
constexpr const char* kNoBuildReasonFormat = FOX_TR_NOOP("no build for %s");
}  // namespace

std::string pluginAbiMismatchReason(unsigned builtFor, unsigned required) {
    return cascade::core::formatText(kAbiReasonFormat, builtFor, required);
}

std::string pluginNoBuildReason(const std::string& platform) {
    const std::string_view fmt(kNoBuildReasonFormat);
    const std::size_t at = fmt.find("%s");
    return std::string(fmt.substr(0, at)) + platform + std::string(fmt.substr(at + 2));
}

std::string trStoredReason(const std::string& english) {
    if (english.empty()) { return english; }
    // tr() hands back its own argument when nothing translates it, so a different
    // pointer is a catalogue hit.
    const char* hit = tr(english.c_str());
    if (hit != english.c_str()) { return hit; }

    unsigned builtFor = 0;
    unsigned required = 0;
    if (std::sscanf(english.c_str(), kAbiReasonFormat, &builtFor, &required) == 2 &&
        pluginAbiMismatchReason(builtFor, required) == english) {
        return cascade::core::formatText(tr(kAbiReasonFormat), builtFor, required);
    }

    const std::string_view fmt(kNoBuildReasonFormat);
    const std::string_view lead = fmt.substr(0, fmt.find("%s"));
    if (english.size() > lead.size() && english.compare(0, lead.size(), lead) == 0) {
        const std::string platform = english.substr(lead.size());
        if (pluginNoBuildReason(platform) == english) {
            const char* local = tr(kNoBuildReasonFormat);
            std::vector<char> buf(std::strlen(local) + platform.size() + 8);
            cascade::core::formatUtf8(buf.data(), buf.size(), local, platform.c_str());
            return buf.data();
        }
    }
    return english;
}

// ===========================================================================
// OLD VERSIONS - the words
// ===========================================================================

std::string storeCleanupKeyLabel(std::size_t count) {
    return cascade::core::formatText(tr("CLEAN UP OLD VERSIONS (%zu)"), count);
}

std::string storeOldCopyLine(const StoreOldCopy& c) {
    return cascade::core::formatText(tr("%s %s - %s (%s stays)"), c.name.c_str(),
                                     c.version.c_str(), c.file.c_str(), c.keptVersion.c_str());
}

std::string storeCleanupConfirmLabel(std::size_t count) {
    return count == 1 ? std::string(tr("Remove 1 file"))
                      : cascade::core::formatText(tr("Remove %zu files"), count);
}

std::string pluginCleanupReport(const cascade::core::PluginCleanupResult& r) {
    std::string out;
    const auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (const std::string& e : v) { s += (s.empty() ? "" : ", ") + e; }
        return s;
    };
    if (!r.removed.empty()) {
        out = r.removed.size() == 1
                  ? cascade::core::formatText(tr("Removed the old version %s."), r.removed[0].c_str())
                  : cascade::core::formatText(tr("Removed %zu old versions: %s."), r.removed.size(),
                                              join(r.removed).c_str());
    }
    if (!r.queued.empty()) {
        const std::string q = cascade::core::formatText(
            tr("In use, so removed the next time FoxSDR starts: %s."), join(r.queued).c_str());
        out += (out.empty() ? "" : " ") + q;
    }
    if (!r.failed.empty()) {
        const std::string f =
            cascade::core::formatText(tr("Could not remove: %s."), join(r.failed).c_str());
        out += (out.empty() ? "" : " ") + f;
    }
    return out;
}

// ===========================================================================
// GET EVERYTHING - what it picks, and what the key says
// ===========================================================================

AddAllPlan planAddAll(const PluginStoreModel& model, bool noticesAcknowledged) {
    AddAllPlan plan;

    for (int i = 0; i < static_cast<int>(model.modules.size()); ++i) {
        const StoreModule& sm = model.modules[static_cast<std::size_t>(i)];
        const std::string& name = sm.plate.name;
        const std::string shown = name.empty() ? std::string(tr("(unnamed module)")) : name;
        if (sm.plate.fitted || sm.install != StoreInstallKind::NotInstalled) {
            // A FITTED MODULE IS ONLY EVER AN UPDATE HERE. It is never listed as
            // skipped: "already installed" is the outcome the user pressed this key
            // for, not a thing that went wrong.
            if (!sm.updateToVersion.empty()) { plan.update.push_back(i); }
            continue;
        }
        // THE SAME GATE A SINGLE GET GOES THROUGH, asked of every row - with the
        // notice treated as acknowledged only when the user has ticked the one box.
        const std::string& why =
            noticesAcknowledged ? sm.blockedReasonIfAcknowledged : sm.blockedReason;
        if (why.empty()) {
            plan.install.push_back(i);
            continue;
        }
        // HELD BY A NOTICE AND NOTHING ELSE is the one skip the user can undo from
        // this panel, so it is counted apart from the rest.
        if (!sm.plate.legalNotice.empty() && sm.blockedReasonIfAcknowledged.empty()) {
            ++plan.heldByNotice;
        }
        plan.skipped.push_back(shown + " - " + why);
    }

    const int n = static_cast<int>(plan.install.size());
    const int m = static_cast<int>(plan.update.size());
    std::string buf;
    if (n > 0 && m > 0) {
        cascade::core::formatUtf8(buf,
                                  n == 1 ? tr("ADD %d PLUGIN, UPDATE %d") : tr("ADD %d PLUGINS, UPDATE %d"),
                                  n, m);
        plan.label = buf;
    } else if (n > 0) {
        if (plan.skipped.empty()) {
            plan.label = tr("ADD ALL PLUGINS");
        } else {
            cascade::core::formatUtf8(buf, n == 1 ? tr("ADD %d PLUGIN") : tr("ADD %d PLUGINS"), n);
            plan.label = buf;
        }
    } else if (m > 0) {
        cascade::core::formatUtf8(buf, m == 1 ? tr("UPDATE %d PLUGIN") : tr("UPDATE %d PLUGINS"), m);
        plan.label = buf;
    } else {
        plan.label = tr("ADD ALL PLUGINS");
    }

    // --- and why it may not be pressed --------------------------------------
    if (!model.haveCatalogue) {
        if (!model.sourceStatus.empty()) {
            plan.blockedReason = tr("the catalogue was read and it lists no modules at all");
        } else if (!model.sourceError.empty()) {
            plan.blockedReason =
                tr("the last check did not return a catalogue - its reason is shown at the top "
                   "of the list");
        } else {
            plan.blockedReason =
                tr("no catalogue has been read yet - press CHECK NOW and this application asks "
                   "the source once");
        }
    } else if (model.busy) {
        plan.blockedReason = tr("a transfer is already in progress");
    } else if (n == 0 && m == 0) {
        plan.blockedReason =
            plan.skipped.empty()
                ? tr("every module in the catalogue is already fitted, and none has a newer build")
                : tr("nothing in the catalogue can be fitted on this machine - each module's own "
                     "reason is on its page");
    }
    return plan;
}

// ===========================================================================
// THE SHARED VOCABULARY
// ===========================================================================

float outlineKeyHeight() { return std::max(26.0f * S(), faceH(fonts::legend(), keyPx()) + 12.0f * S()); }

float outlineKeyWidth(const char* label) {
    return std::max(66.0f * S(), trackedWidth(fonts::legend(), keyPx(), label, keyTracking()) +
                                     26.0f * S());
}

float chassisKeyWidth(const char* label) {
    return std::max(60.0f * S(), trackedWidth(fonts::legend(), keyPx(), label, keyTracking()) +
                                     24.0f * S());
}

bool drawOutlineKey(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* label,
                    ImU32 ink, bool enabled, const char* id, const char* hoverText,
                    KeyRect* out) {
    if (dl == nullptr || br.x - tl.x < 8.0f || br.y - tl.y < 8.0f) { return false; }
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    const bool pressed = ImGui::InvisibleButton("##key", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    ImGui::PopID();
    if (out != nullptr) {
        out->tl = tl;
        out->br = br;
    }
    const float r = 2.0f;
    const ImU32 line = enabled ? ink : theme::withAlpha(theme::kBrassDark, 0.9f);
    const ImU32 text = enabled ? ink : theme::kInkFaint;
    if (enabled && hovered) {
        dl->AddRectFilled(tl, br, theme::withAlpha(ink, 0.14f), r);
        dl->AddRect(ImVec2(tl.x - 2.0f, tl.y - 2.0f), ImVec2(br.x + 2.0f, br.y + 2.0f),
                    theme::withAlpha(ink, 0.25f), r + 2.0f, 0, 2.0f);
    }
    dl->AddRect(tl, br, line, r, 0, 1.0f);
    drawKeyWord(dl, tl, br, label, text);
    if (focused) {
        dl->AddRect(ImVec2(tl.x - 3.0f, tl.y - 3.0f), ImVec2(br.x + 3.0f, br.y + 3.0f),
                    theme::kBrassBright, r + 2.0f, 0, theme::kHairline);
    }
    if (hovered && hoverText != nullptr && hoverText[0] != '\0') {
        ImGui::SetTooltip("%s", hoverText);
    }
    return pressed && enabled;
}

bool drawChassisKey(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* label,
                    bool enabled, const char* id, KeyRect* out) {
    if (dl == nullptr || br.x - tl.x < 8.0f || br.y - tl.y < 8.0f) { return false; }
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton("##key", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool focused = ImGui::IsItemFocused();
    ImGui::EndDisabled();
    ImGui::PopID();
    if (out != nullptr) {
        out->tl = tl;
        out->br = br;
    }
    const float r = theme::kKeyRounding;
    if (!enabled) {
        dl->AddRectFilled(tl, br, theme::kWell, r);
        dl->AddRect(tl, br, theme::withAlpha(theme::kBrassDark, 0.80f), r, 0, theme::kHairline);
    } else {
        if (!held) {
            dl->AddRectFilled(ImVec2(tl.x + 1.0f, tl.y + 2.0f), ImVec2(br.x + 1.0f, br.y + 2.0f),
                              theme::withAlpha(theme::kVoid, 0.45f), r);
        }
        const ImU32 top = held ? theme::kBrassMid : (hovered ? theme::kIvory : theme::kCream);
        const ImU32 bot = held ? theme::kBrassDark : theme::kBrassBright;
        dl->AddRectFilled(tl, br, bot, r);
        if (br.x - tl.x > r * 2.0f) {
            dl->AddRectFilledMultiColor(ImVec2(tl.x + r, tl.y), ImVec2(br.x - r, br.y), top, top,
                                        bot, bot);
        }
        addBenchBevel(dl, tl, br, r, !held);
    }
    if (focused) {
        dl->AddRect(ImVec2(tl.x - 2.0f, tl.y - 2.0f), ImVec2(br.x + 2.0f, br.y + 2.0f),
                    theme::kBrassBright, r + 1.0f, 0, theme::kHairline);
    }
    drawKeyWord(dl, tl, br, label, enabled ? theme::kEnamel : theme::kInkMuted, held ? 1.0f : 0.0f);
    return pressed;
}

bool drawSearchField(const ImVec2& tl, float w, const char* hint, char* buf, std::size_t bufSize,
                     KeyRect* out) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* uf = fonts::ui();
    const float px = fonts::uiPx();
    const float fieldH = std::max(30.0f * S(), px + 14.0f * S());
    const ImVec2 br(tl.x + w, tl.y + fieldH);
    dl->AddRectFilled(tl, br, theme::kVoid, 2.0f);
    dl->AddRect(tl, br, theme::kBrassDark, 2.0f, 0, 1.0f);
    drawLoupe(dl, ImVec2(tl.x + 16.0f * S(), tl.y + fieldH * 0.5f - 2.0f * S()), 5.0f * S(),
              theme::kInkFaint);
    const float padL = 32.0f * S();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(padL, std::max(2.0f, (fieldH - faceH(uf, px)) * 0.5f)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));  // theme-exempt: transparent over the well
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));  // theme-exempt: transparent over the well
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));  // theme-exempt: transparent over the well
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kIvory));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme::vec(theme::kInkMuted));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));  // theme-exempt: the well draws its own edge
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushFont(uf, px / uiscale::factor());
    ImGui::SetCursorScreenPos(tl);
    ImGui::SetNextItemWidth(w);
    const bool changed = inputTextWithFittedHint("##search", hint, buf, bufSize);
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(6);
    if (ImGui::IsItemActive() || ImGui::IsItemFocused()) {
        dl->AddRect(tl, br, theme::kPhosphor, 2.0f, 0, 1.0f);
    }
    if (out != nullptr) {
        out->tl = tl;
        out->br = br;
    }
    return changed;
}

float drawSectionHeading(ImDrawList* dl, const ImVec2& tl, float width, const char* text) {
    ImFont* f = fonts::legend();
    const float px = std::max(11.0f, keyPx() * 0.9f);
    const float track = px * 0.18f;
    const float tw = trackedWidth(f, px, text, track);
    const float h = faceH(f, px);
    addTrackedText(dl, f, px, tl, theme::kCream, text, track, tl.x + width);
    const float x0 = tl.x + tw + 12.0f * S();
    if (x0 < tl.x + width) {
        dl->AddLine(ImVec2(x0, tl.y + h * 0.55f), ImVec2(tl.x + width, tl.y + h * 0.55f),
                    theme::withAlpha(theme::kBrassMid, 0.8f), 1.0f);
    }
    return h + 10.0f * S();
}

std::string ellipsize(ImFont* font, float px, const char* text, float maxW) {
    if (text == nullptr) { return {}; }
    if (textW(font, px, text) <= maxW) { return text; }
    const float dotsW = textW(font, px, "...");
    if (maxW <= dotsW) { return "..."; }
    const std::vector<const char*> b = charStarts(text);
    // The largest prefix that, with the dots, still fits: binary search over characters.
    std::size_t lo = 0, hi = b.size() - 1;
    while (lo < hi) {
        const std::size_t mid = (lo + hi + 1) / 2;
        const float w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text, b[mid]).x;
        if (w + dotsW <= maxW) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    std::string out(text, b[lo]);
    while (!out.empty() && out.back() == ' ') { out.pop_back(); }
    return out + "...";
}

float addEllipsized(ImDrawList* dl, ImFont* font, float px, const ImVec2& at, ImU32 col,
                    const char* text, float maxW) {
    const std::string s = ellipsize(font, px, text, maxW);
    dl->AddText(font, px, at, col, s.c_str());
    return textW(font, px, s.c_str());
}

void drawCategoryGlyph(ImDrawList* dl, const ImVec2& tl, float box, const std::string& category,
                       ImU32 colour, float strokePx) {
    if (dl == nullptr || box < 4.0f) { return; }
    const Glyph& g = glyphFor(category);
    const float k = box / 56.0f;
    std::vector<ImVec2> pts;
    // A faint wider pass under the line is the glow the mock-up's shadow gave it.
    for (int pass = 0; pass < 2; ++pass) {
        const ImU32 col = pass == 0 ? theme::withAlpha(colour, 0.20f) : colour;
        const float th = pass == 0 ? strokePx * 3.0f : strokePx;
        for (std::size_t i = 0; i < g.lines.size(); ++i) {
            pts.clear();
            for (const ImVec2& p : g.lines[i]) { pts.push_back(ImVec2(tl.x + p.x * k, tl.y + p.y * k)); }
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col,
                            g.closed[i] ? ImDrawFlags_Closed : ImDrawFlags_None, th);
        }
        for (const ImVec2& d : g.dots) {
            dl->AddCircleFilled(ImVec2(tl.x + d.x * k, tl.y + d.y * k), th * 0.55f, col, 8);
        }
    }
}

// --- the cleanup foot -------------------------------------------------------------

bool drawCleanupFoot(const std::vector<StoreOldCopy>& oldCopies, const std::string& report,
                     bool busy, float width, const char* censusKey, const char* censusYes) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* uf = fonts::ui();
    const float px = fonts::uiPx();
    const std::size_t oldCount = oldCopies.size();
    bool accepted = false;
    ImGui::PushID("cleanupfoot");
    if (oldCount > 0) {
        const std::string label = storeCleanupKeyLabel(oldCount);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float kw = outlineKeyWidth(label.c_str());
        const float kh = outlineKeyHeight();
        KeyRect kr;
        if (drawOutlineKey(dl, at, ImVec2(at.x + kw, at.y + kh), label.c_str(),
                           theme::kInkMuted, !busy, "cleanupold", nullptr, &kr)) {
            ImGui::OpenPopup("###cleanupconfirm");
        }
        if (censusKey != nullptr) { censusRect(censusKey, kr.tl.x, kr.tl.y, kr.br.x, kr.br.y); }
        // The old copies, a line each, beside the key (at most three).
        std::string note;
        for (std::size_t k = 0; k < oldCount && k < 3; ++k) {
            note += (k == 0 ? "" : "\n") + storeOldCopyLine(oldCopies[k]);
        }
        if (oldCount > 3) {
            note += "\n" + cascade::core::formatText(tr("and %zu more"), oldCount - 3);
        }
        const float nx = at.x + kw + 16.0f * S();
        const float nw = std::max(60.0f, width - (nx - at.x));
        dl->AddText(uf, px, ImVec2(nx, at.y), theme::kInkMuted, note.c_str(), nullptr, nw);
        const float nh = wrapH(uf, px, nw, note.c_str());
        float y = at.y + std::max(kh, nh) + 8.0f * S();
        if (!report.empty()) {
            dl->AddText(uf, px, ImVec2(at.x, y), theme::kPhosphor, report.c_str(), nullptr, width);
            y += wrapH(uf, px, width, report.c_str()) + 6.0f * S();
        }
        ImGui::SetCursorScreenPos(ImVec2(at.x, y));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
    } else if (!report.empty()) {
        // A clean-up that has just taken the last one: its report stays where the key was.
        const ImVec2 at = ImGui::GetCursorScreenPos();
        dl->AddText(uf, px, at, theme::kPhosphor, report.c_str(), nullptr, width);
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + wrapH(uf, px, width, report.c_str()) + 6.0f * S()));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
    }

    // THE ONE CONFIRMATION, listing every file that will go. Accepting it is the
    // whole of the request; the caller removes exactly the copies its own state still
    // calls superseded.
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 0.0f),
                                        ImVec2(std::max(360.0f, width * 0.8f), FLT_MAX));
    if (ImGui::BeginPopupModal(trId("Remove old plugin versions###cleanupconfirm"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (oldCount == 0) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::PushTextWrapPos(std::max(320.0f, width * 0.5f));
            ImGui::TextWrapped("%s", tr("Each of these is an older copy of a plugin whose newer "
                                        "version is installed and running. Only these files are "
                                        "deleted."));
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            for (const StoreOldCopy& c : oldCopies) {
                ImGui::BulletText("%s", storeOldCopyLine(c).c_str());
            }
            ImGui::Spacing();
            const std::string yes = storeCleanupConfirmLabel(oldCount) + "###cleanupyes";
            if (ImGui::Button(yes.c_str())) {
                accepted = true;
                ImGui::CloseCurrentPopup();
            }
            {
                const ImVec2 a = ImGui::GetItemRectMin();
                const ImVec2 b = ImGui::GetItemRectMax();
                if (censusYes != nullptr) { censusRect(censusYes, a.x, a.y, b.x, b.y); }
            }
            ImGui::SameLine();
            if (ImGui::Button(trId("Keep them###cleanupno"))) { ImGui::CloseCurrentPopup(); }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return accepted;
}

// ===========================================================================
// THE PICTURES
// ===========================================================================

PagePictureCache::~PagePictureCache() { releaseAll(); }

void PagePictureCache::releaseAll() {
    for (auto& kv : entries_) {
        if (kv.second.tex != 0u) { glDeleteTextures(1, &kv.second.tex); }
    }
    entries_.clear();
}

const PagePictureCache::Entry* PagePictureCache::find(const StorePicture& p) {
    if (p.state != PictureState::Ready || p.sha256.empty()) { return nullptr; }
    const auto it = entries_.find(p.sha256);
    if (it != entries_.end()) { return &it->second; }
    if (decodedThisFrame_) { return nullptr; }  // one decode a frame, so eight do not freeze it
    decodedThisFrame_ = true;

    Entry e;
    std::vector<unsigned char> bytes;
    {
        std::ifstream in(std::filesystem::path(p.path), std::ios::binary);
        if (in.good()) {
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    cascade::core::PngImage img;
    std::string err;
    if (bytes.empty()) {
        e.error = "the picture file cannot be read";
    } else if (!cascade::core::decodePng(bytes, 4096, img, err)) {
        e.error = err;
    } else {
        GLint maxTex = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
        if (maxTex > 0 && (img.width > maxTex || img.height > maxTex)) {
            e.error = "the picture is larger than this display can hold";
        } else {
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, img.rgba.data());
            e.tex = tex;
            e.width = img.width;
            e.height = img.height;
        }
    }
    return &(entries_[p.sha256] = e);
}

// ===========================================================================
// THE PAGE BODY - one renderer, two callers
// ===========================================================================

namespace {

// THE SENTENCE THE DESIGN GOT WRONG, corrected here and stated once. The mock says
// the reach list is "enforced by the console - a module cannot take anything not on
// this list". It is not: plugins load in-process (LoadLibraryExW / dlopen), there is
// no sandbox and no permission model, and the CASCADE_CAP_* bits say what a module
// PROVIDES rather than what it may take.
const char* kReachLead = FOX_TR_NOOP(
    "Declared by the maker, not enforced. A fitted module is loaded into this "
    "application's own process and runs with every privilege the application has: "
    "there is no sandbox and no permission model. This list is what the module says "
    "it PROVIDES, not a limit on what it can take.");

constexpr float kFrameW = 520.0f;
constexpr float kFrameH = 325.0f;
constexpr float kFrameGap = 14.0f;

// A CAPTION IN ITS OWN FRAME'S WIDTH: at most two lines, broken where ImGui would break them, the
// second ending in "..." when the caption is longer. A caption never runs past its frame.
std::vector<std::string> captionLines(ImFont* f, float px, const std::string& caption, float w) {
    std::string flat = caption;
    for (char& c : flat) {
        if (c == '\n' || c == '\r' || c == '\t') { c = ' '; }
    }
    std::vector<std::string> out;
    const char* s = flat.c_str();
    const char* const end = s + flat.size();
    while (out.size() < 2) {
        while (s < end && *s == ' ') { ++s; }
        if (s >= end) { break; }
        if (out.size() == 1) {
            // The last line: what remains, cut with the dots when it is more than fits.
            out.push_back(ellipsize(f, px, s, w));
            break;
        }
        const char* e = f->CalcWordWrapPosition(px, s, end, w);
        if (e <= s) { e = s + 1; }  // a word wider than the frame: one character at a time
        std::string line(s, e);
        while (!line.empty() && line.back() == ' ') { line.pop_back(); }
        out.push_back(line);
        s = e;
    }
    return out;
}

// A small chassis-grey arrow key at an end of the strip.
bool drawStripArrow(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, bool left, bool dim,
                    const char* id) {
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    ImGui::SetNextItemAllowOverlap();
    const bool pressed = ImGui::InvisibleButton("##arrow", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    const float a = dim ? 0.35f : (hovered ? 1.0f : 0.85f);
    dl->AddRectFilled(tl, br, theme::withAlpha(theme::kBrassMid, a), 2.0f);
    dl->AddRect(tl, br, theme::withAlpha(theme::kBrassBright, a), 2.0f, 0, 1.0f);
    const float cx = (tl.x + br.x) * 0.5f, cy = (tl.y + br.y) * 0.5f, k = 7.0f * S();
    const float s = left ? -1.0f : 1.0f;
    const ImVec2 pts[3] = {ImVec2(cx - s * k * 0.5f, cy - k), ImVec2(cx + s * k * 0.5f, cy),
                           ImVec2(cx - s * k * 0.5f, cy + k)};
    dl->AddPolyline(pts, 3, theme::withAlpha(theme::kIvory, a), ImDrawFlags_None, 2.0f);
    return pressed && !dim;
}

// The strip. Returns its total height (frames and captions).
float drawPictureStrip(const ModulePageIn& in, ModulePageState& state, PagePictureCache& cache,
                       const ImVec2& o, float W) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* uf = fonts::ui();
    const float px = fonts::uiPx();
    const float k = S();
    const float fw = kFrameW * k, fh = kFrameH * k, gap = kFrameGap * k;
    const std::vector<StorePicture> none;
    const std::vector<StorePicture>& pics = in.pictures != nullptr ? *in.pictures : none;
    const int n = pics.empty() ? 1 : static_cast<int>(pics.size());
    const float total = static_cast<float>(n) * fw + static_cast<float>(n - 1) * gap;
    const float maxScroll = std::max(0.0f, total - W);

    // The tallest caption, so the strip is one height whatever it holds.
    const float capLineH = faceH(uf, px);
    std::vector<std::vector<std::string>> caps;
    float capH = 0.0f;
    for (const StorePicture& p : pics) {
        caps.push_back(captionLines(uf, px, p.caption, fw));
        capH = std::max(capH, capLineH * static_cast<float>(caps.back().size()));
    }
    const float capTop = fh + 8.0f * k;
    const float stripH = capTop + capH;

    // THE STRIP IS ONE ITEM: the wheel over it scrolls it sideways (and only while it
    // can, so the page's own scroll still works at its ends), and the arrows are laid
    // over its ends.
    ImGui::SetCursorScreenPos(o);
    ImGui::SetNextItemAllowOverlap();
    ImGui::PushID("strip");
    ImGui::InvisibleButton("##strip", ImVec2(W, fh));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && maxScroll > 0.0f) {
        const ImGuiIO& io = ImGui::GetIO();
        const float wheel = io.MouseWheel + io.MouseWheelH;
        if (wheel != 0.0f) {
            const float next = std::clamp(state.stripTarget - wheel * 70.0f * k, 0.0f, maxScroll);
            if (next != state.stripTarget) {
                state.stripTarget = next;
                ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
            }
        }
    }
    ImGui::PopID();
    state.stripTarget = std::clamp(state.stripTarget, 0.0f, maxScroll);
    // Eased toward the target: a click on an arrow glides.
    const float dt = std::min(0.05f, ImGui::GetIO().DeltaTime);
    state.stripX += (state.stripTarget - state.stripX) * std::min(1.0f, dt * 14.0f);
    if (std::fabs(state.stripTarget - state.stripX) < 0.5f) { state.stripX = state.stripTarget; }
    state.stripX = std::clamp(state.stripX, 0.0f, maxScroll);

    dl->PushClipRect(o, ImVec2(o.x + W, o.y + stripH), true);
    for (int i = 0; i < n; ++i) {
        const float x = o.x - state.stripX + static_cast<float>(i) * (fw + gap);
        if (x + fw < o.x || x > o.x + W) { continue; }
        const ImVec2 ftl(x, o.y), fbr(x + fw, o.y + fh);
        dl->AddRectFilled(ftl, fbr, theme::kVoid);
        const ImVec2 mid((ftl.x + fbr.x) * 0.5f, (ftl.y + fbr.y) * 0.5f);
        if (pics.empty()) {
            // NO PICTURES PUBLISHED: the frame in the theme - a faint grid, the
            // category glyph large and dim, and the sentence.
            const float step = 26.0f * k;
            for (float gx = ftl.x + step; gx < fbr.x; gx += step) {
                dl->AddLine(ImVec2(gx, ftl.y), ImVec2(gx, fbr.y), theme::withAlpha(theme::kBrassDark, 0.30f));
            }
            for (float gy = ftl.y + step; gy < fbr.y; gy += step) {
                dl->AddLine(ImVec2(ftl.x, gy), ImVec2(fbr.x, gy), theme::withAlpha(theme::kBrassDark, 0.30f));
            }
            const float gb = 130.0f * k;
            drawCategoryGlyph(dl, ImVec2(mid.x - gb * 0.5f, mid.y - gb * 0.5f - 18.0f * k), gb,
                              in.glyphCategory != nullptr ? in.glyphCategory : "",
                              theme::withAlpha(theme::kPhosphor, 0.28f), 1.15f * gb / 56.0f);
            const char* msg = tr("No pictures published yet");
            dl->AddText(uf, px, ImVec2(mid.x - textW(uf, px, msg) * 0.5f, mid.y + gb * 0.5f - 6.0f * k),
                        theme::kInkMuted, msg);
        } else {
            const StorePicture& p = pics[static_cast<std::size_t>(i)];
            const PagePictureCache::Entry* e = cache.find(p);
            std::string word;
            ImU32 wcol = theme::kInkMuted;
            if (e != nullptr && e->tex != 0u) {
                // CONTAINED in the frame, centred - a picture of another shape is never stretched.
                const float sc = std::min(fw / static_cast<float>(e->width), fh / static_cast<float>(e->height));
                const float iw = static_cast<float>(e->width) * sc, ih = static_cast<float>(e->height) * sc;
                dl->AddImage(static_cast<ImTextureID>(static_cast<std::uintptr_t>(e->tex)),
                             ImVec2(mid.x - iw * 0.5f, mid.y - ih * 0.5f),
                             ImVec2(mid.x + iw * 0.5f, mid.y + ih * 0.5f));
            } else if (e != nullptr) {
                word = trStoredReason(e->error);
            } else if (p.state == PictureState::Failed) {
                word = trStoredReason(p.reason);
            } else {
                word = tr("loading...");
            }
            if (census::enabled()) {
                // WHAT THE FRAME SHOWS: the decoded picture, the reason it would not decode, the
                // reason it was not fetched, or the wait.
                const char* frameState = (e != nullptr && e->tex != 0u) ? "texture"
                                         : e != nullptr                  ? "error"
                                         : p.state == PictureState::Failed ? "failed"
                                                                           : "loading";
                census::note(std::string(in.census != nullptr ? in.census : "store") + ":picture:",
                             in.id + ":" + std::to_string(i) + ":" + frameState);
            }
            if (!word.empty()) {
                const float ww = fw - 40.0f * k;
                const float wh = wrapH(uf, px, ww, word.c_str());
                dl->AddText(uf, px, ImVec2(mid.x - std::min(ww, textW(uf, px, word.c_str())) * 0.5f,
                                           mid.y - wh * 0.5f),
                            wcol, word.c_str(), nullptr, ww);
            }
            const std::vector<std::string>& cl = caps[static_cast<std::size_t>(i)];
            for (std::size_t li = 0; li < cl.size(); ++li) {
                dl->AddText(uf, px, ImVec2(x, o.y + capTop + capLineH * static_cast<float>(li)),
                            theme::kInkMuted, cl[li].c_str());
            }
        }
        dl->AddRect(ftl, fbr, theme::withAlpha(theme::kBrassMid, 0.9f), 0.0f, 0, 1.0f);
    }
    dl->PopClipRect();

    // The arrows: both ends, dimmed at the ends, absent with one frame in view.
    if (maxScroll > 0.0f) {
        const float aw = 28.0f * k, ah = 56.0f * k;
        const float ay = o.y + fh * 0.5f - ah * 0.5f;
        if (drawStripArrow(dl, ImVec2(o.x + 6.0f * k, ay), ImVec2(o.x + 6.0f * k + aw, ay + ah), true,
                           state.stripTarget <= 0.5f, "left")) {
            state.stripTarget = std::max(0.0f, state.stripTarget - (fw + gap));
        }
        if (drawStripArrow(dl, ImVec2(o.x + W - 6.0f * k - aw, ay), ImVec2(o.x + W - 6.0f * k, ay + ah),
                           false, state.stripTarget >= maxScroll - 0.5f, "right")) {
            state.stripTarget = std::min(maxScroll, state.stripTarget + (fw + gap));
        }
    }
    return stripH;
}

}  // namespace

void drawModulePageBody(const ModulePageIn& in, ModulePageState& state, PagePictureCache& cache) {
    if (in.plate == nullptr) { return; }
    const ModulePlate& m = *in.plate;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* uf = fonts::ui();
    const float k = S();
    const float pr = prose();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float W = std::max(160.0f, in.width);
    const std::string pfx = in.census != nullptr ? in.census : "store";
    float y = o.y;

    const auto section = [&](const char* name, float y0, float y1) {
        if (census::enabled()) {
            census::note(pfx + ":section:", name);
            census::rect(pfx + ":section:" + name, o.x, y0, o.x + W, y1);
        }
    };
    const float gapAfter = 22.0f * k;

    if (in.catalogued) {
        // ---- SCREENSHOTS --------------------------------------------------------
        {
            const float y0 = y;
            y += drawSectionHeading(dl, ImVec2(o.x, y), W, tr("SCREENSHOTS"));
            if (census::enabled()) {
                const int n = in.pictures != nullptr ? static_cast<int>(in.pictures->size()) : 0;
                census::note(pfx + ":pictures:", in.id + ":" + std::to_string(n));
            }
            cache.beginFrame();
            y += drawPictureStrip(in, state, cache, ImVec2(o.x, y), W);
            section("screenshots", y0, y);
            y += gapAfter;
        }
        // ---- WHAT IT DOES -------------------------------------------------------
        {
            const float y0 = y;
            y += drawSectionHeading(dl, ImVec2(o.x, y), W, tr("WHAT IT DOES"));
            const std::string& text = m.blurb.empty() ? m.summary : m.blurb;
            if (text.empty()) {
                dl->AddText(uf, pr, ImVec2(o.x, y), theme::kInkMuted, tr("not stated"));
                y += faceH(uf, pr);
            } else {
                dl->AddText(uf, pr, ImVec2(o.x, y), theme::kIvory, text.c_str(), nullptr, W);
                y += wrapH(uf, pr, W, text.c_str());
            }
            section("whatitdoes", y0, y);
            y += gapAfter;
        }
        // ---- WHAT'S NEW ---------------------------------------------------------
        {
            const float y0 = y;
            y += drawSectionHeading(dl, ImVec2(o.x, y), W, tr("WHAT'S NEW"));
            const std::string text = moduleWhatsNewText(m);
            dl->AddText(uf, pr, ImVec2(o.x, y), theme::kIvory, text.c_str(), nullptr, W);
            y += wrapH(uf, pr, W, text.c_str());
            section("whatsnew", y0, y);
            y += gapAfter;
        }
        // ---- BEFORE YOU FIT IT --------------------------------------------------
        if (in.noticeBox && !m.legalNotice.empty()) {
            const float y0 = y;
            y += drawSectionHeading(dl, ImVec2(o.x, y), W, tr("BEFORE YOU FIT IT"));
            const float pad = 14.0f * k;
            const float textH = wrapH(uf, pr, W - pad * 2.0f, m.legalNotice.c_str());
            const ImGuiStyle& st = ImGui::GetStyle();
            const float tickH = pr + st.FramePadding.y * 2.0f;
            const float boxH = pad + textH + 12.0f * k + tickH + pad;
            const ImVec2 btl(o.x, y), bbr(o.x + W, y + boxH);
            dl->AddRectFilled(btl, bbr, theme::withAlpha(theme::kAmber, 0.06f));
            dl->AddRect(btl, bbr, theme::kAmber, 0.0f, 0, 1.0f);
            // VERBATIM. Some decoders demodulate transmissions whose interception is an
            // offence in some countries; this is the author saying so, and paraphrasing
            // it would be answering for them.
            dl->AddText(uf, pr, ImVec2(btl.x + pad, btl.y + pad), theme::kIvory, m.legalNotice.c_str(),
                        nullptr, W - pad * 2.0f);
            if (in.noticeTick != nullptr) {
                ImGui::SetCursorScreenPos(ImVec2(btl.x + pad, btl.y + pad + textH + 12.0f * k));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kIvory));
                ImGui::PushStyleColor(ImGuiCol_CheckMark, theme::vec(theme::kAmber));
                ImGui::PushFont(uf, pr / uiscale::factor());
                ImGui::Checkbox(trId("I have read the notice above and accept responsibility"),
                                in.noticeTick);
                const ImVec2 a = ImGui::GetItemRectMin();
                const ImVec2 b = ImGui::GetItemRectMax();
                censusRect(pfx + ":page:tick", a.x, a.y, b.x, b.y);
                ImGui::PopFont();
                ImGui::PopStyleColor(2);
            }
            y = bbr.y;
            section("beforeyoufitit", y0, y);
            y += gapAfter;
        }
    }
    // ---- DETAILS ---------------------------------------------------------------
    {
        const float y0 = y;
        y += drawSectionHeading(dl, ImVec2(o.x, y), W, tr("DETAILS"));
        const char* label = state.showDetails ? tr("HIDE DETAILS") : tr("SHOW DETAILS");
        const float kw = chassisKeyWidth(label), kh = outlineKeyHeight();
        KeyRect dk;
        if (drawChassisKey(dl, ImVec2(o.x, y), ImVec2(o.x + kw, y + kh), label, true, "details", &dk)) {
            state.showDetails = !state.showDetails;
        }
        censusRect(pfx + ":page:details", dk.tl.x, dk.tl.y, dk.br.x, dk.br.y);
        if (state.showDetails && census::enabled()) { census::note(pfx + ":details:", "open"); }
        y += kh + 10.0f * k;
        if (state.showDetails) {
            const std::vector<PageFact> facts = modulePageFacts(m);
            const float keyW = 178.0f * k;
            const float valW = std::max(60.0f, W - keyW - 28.0f * k);
            const float fpx = fonts::uiPx();
            ImFont* lf = fonts::legend();
            const float tpx = std::max(11.0f, keyPx() * 0.9f);
            const ImVec2 gtl(o.x, y);
            float gy = y;
            for (std::size_t i = 0; i < facts.size(); ++i) {
                const PageFact& f = facts[i];
                // A figure (a version, a digest) in the figure face; words in the reading face.
                ImFont* face = (f.copyable && !f.hatched && f.key == tr("SHA-256")) || (!f.hatched && figureLike(f.value.c_str()))
                                   ? fonts::reading()
                                   : uf;
                const float vh = wrapH(face, fpx, valW, f.value.c_str());
                const float rowH = std::max(faceH(lf, tpx), std::max(vh, faceH(face, fpx))) + 16.0f * k;
                const ImVec2 rtl(o.x, gy), rbr(o.x + W, gy + rowH);
                dl->AddRectFilled(rtl, rbr, theme::withAlpha(theme::kWell, 0.7f));
                dl->AddLine(ImVec2(rtl.x, rbr.y), ImVec2(rbr.x, rbr.y),
                            theme::withAlpha(theme::kBrassDark, 0.8f), 1.0f);
                // FITTED TO ITS COLUMN (gui/text_fit.hpp): a translated "AUF DIESEM RECHNER" must be
                // drawn smaller, not cut off at the column's edge.
                const float kpx = fitTrackedPx(lf, tpx, f.key.c_str(), 0.14f, keyW - 8.0f * k, fitFloorFor(tpx));
                addTrackedText(dl, lf, kpx, ImVec2(rtl.x + 14.0f * k, rtl.y + 8.0f * k),
                               theme::kInkMuted, f.key.c_str(), kpx * 0.14f, rtl.x + keyW);
                const ImVec2 vat(rtl.x + keyW + 14.0f * k, rtl.y + 8.0f * k);
                if (f.hatched) {
                    // No source for this value: ruled, with the reason lettered over it.
                    const ImVec2 h0(vat.x, vat.y + 1.0f), h1(vat.x + std::min(valW, textW(face, fpx, f.value.c_str()) + 24.0f * k), vat.y + faceH(face, fpx) - 1.0f);
                    dl->PushClipRect(h0, h1, true);
                    for (float hx = h0.x - (h1.y - h0.y); hx < h1.x; hx += 6.0f) {
                        dl->AddLine(ImVec2(hx, h1.y), ImVec2(hx + (h1.y - h0.y), h0.y),
                                    theme::withAlpha(theme::kInkMuted, 0.20f), 2.0f);
                    }
                    dl->PopClipRect();
                }
                const ImU32 col = f.hatched ? (f.tone != 0 ? f.tone : theme::kInkFaint)
                                            : (f.tone != 0 ? f.tone : theme::kIvory);
                if (f.copyable && !f.hatched) {
                    // A URL OR A DIGEST IS FOR COPYING: selectable text, read-only.
                    std::vector<char> buf(f.value.begin(), f.value.end());
                    buf.push_back('\0');
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::SetCursorScreenPos(ImVec2(vat.x - 4.0f * k, vat.y - 2.0f * k));
                    ImGui::SetNextItemWidth(valW + 8.0f * k);
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));  // theme-exempt: plain text on the row
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(col));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * k, 2.0f * k));
                    ImGui::PushFont(face, fpx / uiscale::factor());
                    ImGui::InputText("##copy", buf.data(), buf.size(), ImGuiInputTextFlags_ReadOnly);
                    ImGui::PopFont();
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor(2);
                    ImGui::PopID();
                } else {
                    dl->AddText(face, fpx, vat, col, f.value.c_str(), nullptr, valW);
                }
                gy += rowH;
            }
            dl->AddRect(gtl, ImVec2(o.x + W, gy), theme::withAlpha(theme::kBrassDark, 0.9f), 0.0f, 0, 1.0f);
            y = gy;
            if (in.showReachLead) {
                // WHAT REACHES MEANS, once, under the grid: declared, not enforced. The claim
                // this file refuses to make is the design's "a module cannot take anything not
                // on this list"; this is the sentence it is replaced with.
                y += 10.0f * k;
                const char* lead = tr(kReachLead);
                dl->AddText(uf, fonts::uiPx(), ImVec2(o.x, y), theme::kInkMuted, lead, nullptr, W);
                y += wrapH(uf, fonts::uiPx(), W, lead);
            }
        }
        section("details", y0, y);
        y += gapAfter;
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, y));
    ImGui::Dummy(ImVec2(W, 1.0f));
}

// ===========================================================================
// THE WINDOW
// ===========================================================================

PluginStoreView::~PluginStoreView() {}

namespace {

// One catalogue card. Returns what was pressed: 1 = open the page, 2 = the key.
struct CardResult {
    bool openPage = false;
    bool keyPressed = false;
};

float cardKeyWidth() {
    return std::max({outlineKeyWidth(tr("GET")), outlineKeyWidth(tr("FITTING...")),
                     outlineKeyWidth(tr("INSTALLED")), outlineKeyWidth(tr("UPDATE"))});
}

float cardHeight() {
    const float k = S();
    const float pad = 10.0f * k;
    const float nameH = faceH(fonts::ui(), prose());
    const float sumH = faceH(fonts::ui(), fonts::uiPx());
    const float rowH = std::max(outlineKeyHeight(), badgeHeight());
    return std::max(96.0f * k, pad + nameH + 2.0f * k + sumH + 6.0f * k + rowH + pad);
}

CardResult drawCard(ImDrawList* dl, const StoreModule& sm, const StoreKey& key, const ImVec2& tl,
                    float w, float h, float progress) {
    CardResult res;
    const float k = S();
    ImFont* uf = fonts::ui();
    const float pad = 10.0f * k;
    const ImVec2 br(tl.x + w, tl.y + h);
    const bool hoverCard = ImGui::IsMouseHoveringRect(tl, br);
    dl->AddRectFilled(tl, br, theme::kEnamelDark);
    dl->AddRect(tl, br, hoverCard ? theme::kBrassMid : theme::withAlpha(theme::kBrassDark, 0.9f),
                0.0f, 0, 1.0f);

    ImGui::PushID(sm.id.c_str());
    // --- the glyph ------------------------------------------------------------
    const float gb = 58.0f * k;
    const ImVec2 gtl(tl.x + pad, tl.y + (h - gb) * 0.5f);
    const ImVec2 gbr(gtl.x + gb, gtl.y + gb);
    ImGui::SetCursorScreenPos(gtl);
    if (ImGui::InvisibleButton("##glyph", ImVec2(gb, gb))) { res.openPage = true; }
    const bool gHover = ImGui::IsItemHovered();
    dl->AddRectFilled(gtl, gbr, theme::kVoid);
    dl->AddRect(gtl, gbr, gHover ? theme::kPhosphor : theme::kBrassDark, 0.0f, 0, 1.0f);
    drawCategoryGlyph(dl, ImVec2(gtl.x + 1.0f, gtl.y + 1.0f), gb - 2.0f, sm.plate.category,
                      theme::kPhosphor, 1.6f * (gb - 2.0f) / 56.0f);

    // --- the text column ------------------------------------------------------
    const float tx = gbr.x + 12.0f * k;
    const float tw = std::max(40.0f, br.x - pad - tx);
    float ty = tl.y + pad - 1.0f * k;
    const char* nameText = sm.plate.name.empty() ? tr("(unnamed module)") : sm.plate.name.c_str();
    const float npx = fitTextPx(uf, prose(), nameText, tw, prose() * 0.8f);
    ImGui::SetCursorScreenPos(ImVec2(tx, ty));
    const float nameH = faceH(uf, prose());
    const std::string shownName = ellipsize(uf, npx, nameText, tw);
    const float nameW = std::min(tw, textW(uf, npx, shownName.c_str()));
    if (ImGui::InvisibleButton("##name", ImVec2(std::max(8.0f, nameW), nameH))) { res.openPage = true; }
    const bool nHover = ImGui::IsItemHovered();
    const ImVec2 nmin = ImGui::GetItemRectMin(), nmax = ImGui::GetItemRectMax();
    dl->AddText(uf, npx, ImVec2(tx, ty + (nameH - faceH(uf, npx)) * 0.5f),
                nHover || gHover ? theme::kPhosphor : theme::kIvory, shownName.c_str());
    ty += nameH + 2.0f * k;
    const std::string& sum = sm.plate.summary.empty() ? sm.plate.blurb : sm.plate.summary;
    addEllipsized(dl, uf, fonts::uiPx(), ImVec2(tx, ty), theme::kInkMuted, sum.c_str(), tw);

    // --- the lower row: badges at the left, the one key at the right ------------
    const float rowH = std::max(outlineKeyHeight(), badgeHeight());
    const float rowY = br.y - pad - rowH;
    const float kw = cardKeyWidth();
    const ImVec2 ktl(br.x - pad - kw, rowY), kbr(br.x - pad, rowY + rowH);
    KeyRect kr{ktl, kbr};
    const char* word = storeKeyLabel(key.kind);
    // The words the key's hover says: why a greyed key is greyed.
    const std::string why = (!key.enabled && !key.reason.empty()) ? trStoredReason(key.reason)
                                                                  : std::string();
    switch (key.kind) {
        case StoreKeyKind::Get:
            if (drawOutlineKey(dl, ktl, kbr, word, theme::kPhosphor, key.enabled, "key", why.c_str())) {
                res.keyPressed = true;
            }
            break;
        case StoreKeyKind::Update: {
            if (drawOutlineKey(dl, ktl, kbr, word, theme::kAmber, key.enabled, "key", why.c_str())) {
                res.keyPressed = true;
            }
            // "vX to vY" in amber, beside the key.
            const std::string ft = storeFromTo(sm.installedVersion, sm.updateToVersion.empty()
                                                                        ? sm.plate.version
                                                                        : sm.updateToVersion);
            // In the reading face (digits of one width) the versions looked like a string of
            // separate figures; the text face sets them as the mock-up does.
            ImFont* rf = fonts::ui();
            const float rpx = fonts::tinyPx() * 1.15f;
            const float rw = textW(rf, rpx, ft.c_str());
            dl->AddText(rf, rpx, ImVec2(ktl.x - 10.0f * k - rw, rowY + (rowH - faceH(rf, rpx)) * 0.5f),
                        theme::kAmber, ft.c_str());
            break;
        }
        case StoreKeyKind::Fitting: {
            ImGui::PushID("key");
            ImGui::SetCursorScreenPos(ktl);
            ImGui::InvisibleButton("##fitting", ImVec2(kbr.x - ktl.x, kbr.y - ktl.y));
            ImGui::PopID();
            dl->AddRect(ktl, kbr, theme::kPhosphorDim, 2.0f, 0, 1.0f);
            drawKeyWord(dl, ktl, kbr, word, theme::kPhosphor);
            drawFittingLine(dl, ktl, kbr, progress);
            break;
        }
        case StoreKeyKind::Installed: {
            ImGui::PushID("key");
            ImGui::SetCursorScreenPos(ktl);
            ImGui::InvisibleButton("##installed", ImVec2(kbr.x - ktl.x, kbr.y - ktl.y));
            ImGui::PopID();
            drawKeyWord(dl, ktl, kbr, word, theme::kInkMuted);
            break;
        }
    }
    // Badges: EXPERIMENTAL in amber, a build badge in muted outline.
    {
        float bx = tx;
        const float by = rowY + (rowH - badgeHeight()) * 0.5f;
        const float room = (key.kind == StoreKeyKind::Update ? ktl.x - 150.0f * k : ktl.x) - bx;
        const auto badge = [&](const char* label, ImU32 ink) {
            if (label == nullptr || label[0] == '\0') { return; }
            const float bw = badgeWidth(label);
            if (bx + bw - tx > room) { return; }
            drawBadge(dl, ImVec2(bx, by), label, ink);
            bx += bw + 5.0f * k;
        };
        if (sm.plate.experimental) { badge(storeExperimentalBadge(), theme::kAmber); }
        badge(storeBuildBadge(sm), theme::kInkMuted);
    }
    if (census::enabled()) {
        const std::string base = "store:card:" + sm.id;
        census::rect(base, tl.x, tl.y, br.x, br.y);
        census::rect(base + ":key", kr.tl.x, kr.tl.y, kr.br.x, kr.br.y);
        census::rect(base + ":name", nmin.x, nmin.y, nmax.x, nmax.y);
        census::note("store:key:", sm.id + ":" + storeKeyCensusState(key));
    }
    ImGui::PopID();
    return res;
}

}  // namespace

void PluginStoreView::draw(float width, float height, const PluginStoreModel& model,
                           PluginStoreDeck& deck) {
    // Cleared first, so a request is answered once or not at all.
    cleanup_ = false;
    checkNow_ = false;
    cancel_ = false;
    addAll_ = false;
    updateAll_ = false;
    fitIndex_ = -1;
    updateIndex_ = -1;
    pageOpened_.clear();

    ImGui::PushID("pluginstore");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float k = S();

    // TOO NARROW TO DRAW HONESTLY, so it says so instead of drawing a squashed bar.
    if (width < kStoreMinWidth || height < 200.0f) {
        const char* small =
            tr("This window is too narrow to lay out the catalogue. Widen it and the plugins come "
               "back.");
        if (width > 80.0f) {
            dl->AddText(fonts::ui(), fonts::uiPx(), origin, theme::kGold, small, nullptr,
                        std::max(60.0f, width - 8.0f));
        }
        ImGui::Dummy(ImVec2(std::max(1.0f, width), std::max(1.0f, height)));
        ImGui::PopID();
        return;
    }

    ImFont* uf = fonts::ui();
    const float upx = fonts::uiPx();

    // --- the page bookkeeping: an id, so a refresh cannot swap the plugin under it ----
    int pageIdx = -1;
    if (!deck.pageId.empty()) {
        for (int i = 0; i < static_cast<int>(model.modules.size()); ++i) {
            if (model.modules[static_cast<std::size_t>(i)].id == deck.pageId) {
                pageIdx = i;
                break;
            }
        }
        // A plugin the catalogue no longer lists has no page to show. Only once a
        // catalogue is on screen: an empty one is "not read", not "gone".
        if (pageIdx < 0 && model.haveCatalogue) { deck.pageId.clear(); }
    }
    if (deck.pageId != lastPage_) {
        // The edge: a new page. The tick belongs to the page that had it, the strip starts at
        // its first picture, and the caller is told to ask for the pictures.
        deck.legalAck = false;
        deck.page.reset();
        lastPage_ = deck.pageId;
        if (!deck.pageId.empty()) { pageOpened_ = deck.pageId; }
    }
    pageId_ = deck.pageId;
    if (deck.tab != lastTab_) { lastTab_ = deck.tab; }

    const std::string query = lowerAscii(std::string(deck.search));
    const int updateCount = storeUpdateCount(model);

    // ======================= THE TOP BAR ===========================================
    //
    // ONE ROW: the search field at the left, the two tabs in the middle, the
    // catalogue line and its key at the right. Nothing else is above the list.
    const float pad = 16.0f * k;
    const float barH = std::max(56.0f * k, outlineKeyHeight() + 30.0f * k);
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + barH), theme::kEnamelDark);
    dl->AddLine(ImVec2(origin.x, origin.y + barH - 1.0f), ImVec2(origin.x + width, origin.y + barH - 1.0f),
                theme::kBrassDark, 1.0f);
    const float cy = origin.y + (barH - 6.0f * k) * 0.5f;
    const float keyH = outlineKeyHeight();
    {
        // search
        const float sw = std::clamp(width * 0.2f, 200.0f * k, 300.0f * k);
        KeyRect sr;
        const float fieldH = std::max(30.0f * k, upx + 14.0f * k);
        if (drawSearchField(ImVec2(origin.x + pad, cy - fieldH * 0.5f), sw, tr("Search plugins"),
                            deck.search, sizeof deck.search, &sr)) {
            // Typing is looking for something: that is on the list, not on a page.
            deck.pageId.clear();
        }
        censusRect("store:search", sr.tl.x, sr.tl.y, sr.br.x, sr.br.y);

        // the tabs
        const std::string updLabel = cascade::core::formatText(tr("UPDATES (%zu)"),
                                                               static_cast<std::size_t>(updateCount));
        const char* browseLabel = tr("BROWSE");
        const float tabH = std::max(30.0f * k, keyH + 4.0f * k);
        const float bw = std::max(chassisKeyWidth(browseLabel), 100.0f * k) + 16.0f * k;
        const float uw = std::max(chassisKeyWidth(updLabel.c_str()), 120.0f * k) + 16.0f * k;
        const float tabsW = bw + 8.0f * k + uw;
        // Centred in the bar, but never closer to the search field than a gap.
        float tx = origin.x + (width - tabsW) * 0.5f;
        tx = std::max(tx, origin.x + pad + sw + 24.0f * k);
        const float ty = cy - tabH * 0.5f;
        const bool browseOn = deck.tab == 0;
        if (drawTab(dl, ImVec2(tx, ty), ImVec2(tx + bw, ty + tabH), browseLabel, browseOn, "tabbrowse")) {
            deck.tab = 0;
            deck.pageId.clear();
        }
        censusRect("store:tab:browse", tx, ty, tx + bw, ty + tabH);
        const float ux = tx + bw + 8.0f * k;
        if (drawTab(dl, ImVec2(ux, ty), ImVec2(ux + uw, ty + tabH), updLabel.c_str(), !browseOn, "tabupdates")) {
            deck.tab = 1;
            deck.pageId.clear();
        }
        censusRect("store:tab:updates", ux, ty, ux + uw, ty + tabH);
        if (census::enabled()) {
            census::note("store:tab:", deck.tab == 0 ? "browse" : "updates");
            census::note("store:count:updates:", updateCount);
        }

        // the catalogue line and its key, at the right
        const StoreCatalogueLine line = storeCatalogueLine(model, static_cast<std::int64_t>(std::time(nullptr)));
        const bool asked = model.haveCatalogue || !model.sourceStatus.empty() || !model.sourceError.empty();
        const char* checkLabel = asked ? tr("CHECK AGAIN") : tr("CHECK NOW");
        const float cw = chassisKeyWidth(checkLabel);
        const float ckx = origin.x + width - pad - cw;
        KeyRect ck;
        if (drawChassisKey(dl, ImVec2(ckx, cy - keyH * 0.5f), ImVec2(ckx + cw, cy + keyH * 0.5f), checkLabel,
                           !model.busy && !model.pictureBusy && !model.sourceUrl.empty(), "check", &ck)) {
            checkNow_ = true;
        }
        censusRect("store:check", ck.tl.x, ck.tl.y, ck.br.x, ck.br.y);
        float rightEdge = ckx - 14.0f * k;
        if (model.busy) {
            const char* cl = tr("CANCEL");
            const float clw = chassisKeyWidth(cl);
            const float clx = ckx - 10.0f * k - clw;
            if (drawChassisKey(dl, ImVec2(clx, cy - keyH * 0.5f), ImVec2(clx + clw, cy + keyH * 0.5f), cl,
                               true, "cancel")) {
                cancel_ = true;
            }
            rightEdge = clx - 14.0f * k;
        }
        const float lineLeft = ux + uw + 18.0f * k;
        const float room = rightEdge - lineLeft;
        if (room > 40.0f) {
            const std::string shown = ellipsize(uf, upx, line.text.c_str(), room);
            const float lw = textW(uf, upx, shown.c_str());
            const ImVec2 lat(rightEdge - lw, cy - faceH(uf, upx) * 0.5f);
            dl->AddText(uf, upx, lat, line.tone == StoreLineTone::Amber ? theme::kAmber : theme::kInkMuted,
                        shown.c_str());
            if (shown != line.text && ImGui::IsMouseHoveringRect(lat, ImVec2(lat.x + lw, lat.y + faceH(uf, upx)))) {
                ImGui::SetTooltip("%s", line.text.c_str());
            }
        }
        if (line.cached && census::enabled()) { census::note("store:catalogue:cache"); }
    }

    // ======================= THE BODY ================================================
    const float bodyH = std::max(40.0f, height - barH);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + barH));
    const bool onPage = pageIdx >= 0;
    const char* childId = onPage ? "##storepage" : (deck.tab == 0 ? "##storelist" : "##storeupdates");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(theme::kWell));
    ImGui::BeginChild(childId, ImVec2(width, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    ImGui::PopStyleColor();
    {
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const ImVec2 co = ImGui::GetCursorScreenPos();
        const float availW = ImGui::GetContentRegionAvail().x;
        const float contentW = std::max(300.0f, availW - 2.0f * pad);
        const float x0 = co.x + pad;
        float y = co.y + pad;

        // THE INSTALL ERROR, wherever the user is: a GET on a card that fails must not
        // be silent. (The page shows it under its own key as well.)
        const auto nameOf = [&](const std::string& id) {
            for (const StoreModule& sm : model.modules) {
                if (sm.id == id) { return sm.plate.name; }
            }
            return std::string();
        };
        const auto drawError = [&]() {
            // A CATALOGUE THAT LOADED WITH A WARNING (it could not be kept for the next
            // start, its version policy could not be saved): the list is real, and so is
            // the reason, verbatim.
            if (model.haveCatalogue && !model.refreshFailed && !model.sourceError.empty()) {
                cdl->AddText(uf, upx, ImVec2(x0, y), theme::kAmber, model.sourceError.c_str(), nullptr, contentW);
                y += wrapH(uf, upx, contentW, model.sourceError.c_str()) + 10.0f * k;
            }
            if (model.resultError.empty() || onPage) { return; }
            const std::string who = nameOf(model.resultId);
            const std::string text = (who.empty() ? std::string() : who + ": ") + trStoredReason(model.resultError);
            cdl->AddText(uf, upx, ImVec2(x0, y), theme::kAlarmHot, text.c_str(), nullptr, contentW);
            if (census::enabled()) { census::note("store:error:", model.resultId); }
            y += wrapH(uf, upx, contentW, text.c_str()) + 10.0f * k;
        };

        const CatalogueState cs = catalogueState(model);
        const auto drawEmptyState = [&]() {
            std::string why;
            ImU32 col = theme::kInkMuted;
            switch (cs) {
                case CatalogueState::NeverAsked:
                    why = tr("No catalogue has been read yet. Press CHECK NOW above and this "
                             "application asks the source once.");
                    break;
                case CatalogueState::Failed:
                    why = std::string(tr("The catalogue could not be read:")) + " " + model.sourceError;
                    col = theme::kAlarmHot;
                    break;
                case CatalogueState::ReadEmpty:
                    why = tr("The catalogue was read and it lists no plugins.");
                    break;
                case CatalogueState::Read:
                    why = tr("No plugin matches that search.");
                    break;
            }
            cdl->AddText(uf, prose(), ImVec2(x0, y + 10.0f * k), col, why.c_str(), nullptr, contentW);
            y += 10.0f * k + wrapH(uf, prose(), contentW, why.c_str()) + 10.0f * k;
        };

        if (onPage) {
            // ================= THE PLUGIN PAGE ======================================
            const StoreModule& sm = model.modules[static_cast<std::size_t>(pageIdx)];
            const ModulePlate& p = sm.plate;
            // "< BROWSE"
            {
                const char* bl = tr("< BROWSE");
                const float bw = chassisKeyWidth(bl);
                const float bh = outlineKeyHeight();
                KeyRect br;
                if (drawChassisKey(cdl, ImVec2(x0, y), ImVec2(x0 + bw, y + bh), bl, true, "back", &br)) {
                    deck.pageId.clear();
                    deck.tab = 0;
                    if (census::enabled()) { census::note("store:page:closed:", "back"); }
                }
                censusRect("store:page:back", br.tl.x, br.tl.y, br.br.x, br.br.y);
                y += bh + 14.0f * k;
            }
            if (census::enabled()) { census::note("store:page:", sm.id); }
            // THE PAGE COLUMN GROWS WITH THE WINDOW: all of it less the margins, up to 1180 px, so the
            // pictures' strip shows about two frames in a 1480 px window and one and a half in 1234.
            const float colW = std::min(contentW, 1180.0f * k);
            // ---- the header -------------------------------------------------------
            StoreKeyIn kin;
            kin.busyId = model.busyId;
            kin.busyAny = model.busy;
            kin.onPage = true;
            kin.noticeTicked = deck.legalAck;
            const StoreKey key = storeKeyFor(sm, kin);
            const float gb = 96.0f * k;
            const float hpad = 16.0f * k;
            const std::string meta = storeHeaderMeta(sm, model.hostPlatform);
            const float namePx = prose() * 1.45f;
            const float keyW2 = cardKeyWidth() + 20.0f * k;
            const float textColW = std::max(80.0f, colW - hpad * 3.0f - gb - keyW2 - 10.0f * k);
            const float nameH = wrapH(uf, namePx, textColW, p.name.c_str());
            const float metaH = wrapH(uf, upx, textColW, meta.c_str());
            const float badgeRowH = (p.experimental || storeBuildBadge(sm)[0] != '\0') ? badgeHeight() + 6.0f * k : 0.0f;
            const float headH = std::max(gb + hpad * 2.0f, hpad * 2.0f + nameH + 4.0f * k + metaH + badgeRowH);
            const ImVec2 htl(x0, y), hbr(x0 + colW, y + headH);
            cdl->AddRectFilled(htl, hbr, theme::kEnamelDark);
            cdl->AddRect(htl, hbr, theme::withAlpha(theme::kBrassDark, 0.9f), 0.0f, 0, 1.0f);
            const ImVec2 gtl(htl.x + hpad, htl.y + (headH - gb) * 0.5f);
            cdl->AddRectFilled(gtl, ImVec2(gtl.x + gb, gtl.y + gb), theme::kVoid);
            cdl->AddRect(gtl, ImVec2(gtl.x + gb, gtl.y + gb), theme::kBrassDark, 0.0f, 0, 1.0f);
            drawCategoryGlyph(cdl, ImVec2(gtl.x + 2.0f, gtl.y + 2.0f), gb - 4.0f, p.category,
                              theme::kPhosphor, 1.6f * (gb - 4.0f) / 56.0f);
            float hy = htl.y + hpad;
            const float tx = gtl.x + gb + hpad;
            cdl->AddText(uf, namePx, ImVec2(tx, hy), theme::kIvory, p.name.c_str(), nullptr, textColW);
            hy += nameH + 4.0f * k;
            cdl->AddText(uf, upx, ImVec2(tx, hy), theme::kInkMuted, meta.c_str(), nullptr, textColW);
            hy += metaH + 6.0f * k;
            {
                float bx = tx;
                if (p.experimental) {
                    drawBadge(cdl, ImVec2(bx, hy), storeExperimentalBadge(), theme::kAmber);
                    bx += badgeWidth(storeExperimentalBadge()) + 5.0f * k;
                }
                const char* bb = storeBuildBadge(sm);
                if (bb[0] != '\0') { drawBadge(cdl, ImVec2(bx, hy), bb, theme::kInkMuted); }
            }
            // the ONE key, at the right
            {
                const float kw = cardKeyWidth() + 20.0f * k;
                const float kh = outlineKeyHeight() + 6.0f * k;
                const ImVec2 ktl(hbr.x - hpad - kw, htl.y + (headH - kh) * 0.5f);
                const ImVec2 kbr(ktl.x + kw, ktl.y + kh);
                const std::string why = (!key.enabled && !key.reason.empty()) ? trStoredReason(key.reason)
                                                                               : std::string();
                const char* word = storeKeyLabel(key.kind);
                bool pressed = false;
                switch (key.kind) {
                    case StoreKeyKind::Get:
                        pressed = drawOutlineKey(cdl, ktl, kbr, word, theme::kPhosphor, key.enabled, "pagekey",
                                                 why.c_str());
                        if (pressed) { fitIndex_ = pageIdx; }
                        break;
                    case StoreKeyKind::Update: {
                        const std::string ft = storeFromTo(sm.installedVersion, sm.updateToVersion.empty()
                                                                                    ? p.version
                                                                                    : sm.updateToVersion);
                        ImFont* rf = fonts::ui();
                        const float rpx = fonts::tinyPx() * 1.25f;
                        const float rw = textW(rf, rpx, ft.c_str());
                        cdl->AddText(rf, rpx, ImVec2(ktl.x - 12.0f * k - rw, ktl.y + (kh - faceH(rf, rpx)) * 0.5f),
                                     theme::kAmber, ft.c_str());
                        pressed = drawOutlineKey(cdl, ktl, kbr, word, theme::kAmber, key.enabled, "pagekey",
                                                 why.c_str());
                        if (pressed) { updateIndex_ = pageIdx; }
                        break;
                    }
                    case StoreKeyKind::Fitting:
                        ImGui::PushID("pagekey");
                        ImGui::SetCursorScreenPos(ktl);
                        ImGui::InvisibleButton("##fitting", ImVec2(kbr.x - ktl.x, kbr.y - ktl.y));
                        ImGui::PopID();
                        cdl->AddRect(ktl, kbr, theme::kPhosphorDim, 2.0f, 0, 1.0f);
                        drawKeyWord(cdl, ktl, kbr, word, theme::kPhosphor);
                        drawFittingLine(cdl, ktl, kbr, model.progress);
                        break;
                    case StoreKeyKind::Installed:
                        ImGui::PushID("pagekey");
                        ImGui::SetCursorScreenPos(ktl);
                        ImGui::InvisibleButton("##installed", ImVec2(kbr.x - ktl.x, kbr.y - ktl.y));
                        ImGui::PopID();
                        drawKeyWord(cdl, ktl, kbr, word, theme::kInkMuted);
                        break;
                }
                censusRect("store:page:key", ktl.x, ktl.y, kbr.x, kbr.y);
                if (census::enabled()) {
                    census::note("store:key:", sm.id + ":" + storeKeyCensusState(key));
                    // The PAGE's own key state, apart from the card's: "greyed before the tick, get
                    // after" is a statement about this key and no other.
                    census::note("store:page:key:", storeKeyCensusState(key));
                }
            }
            y = hbr.y;
            // THE INSTALL REPORT AND ERROR, directly UNDER the header (and its key) of the plugin they
            // concern and of no other: PluginRepo's own words, verbatim - a sha256 mismatch names both
            // digests, and paraphrasing it would throw away the only evidence the user has.
            if (model.resultId == sm.id) {
                const std::string& txt = model.resultError.empty() ? model.resultReport : model.resultError;
                if (!txt.empty()) {
                    const std::string shown = model.resultError.empty() ? txt : trStoredReason(txt);
                    y += 10.0f * k;
                    cdl->AddText(uf, upx, ImVec2(x0, y), model.resultError.empty() ? theme::kPhosphor : theme::kAlarmHot,
                                 shown.c_str(), nullptr, colW);
                    y += wrapH(uf, upx, colW, shown.c_str());
                    if (census::enabled()) {
                        census::note("store:page:result:", model.resultError.empty() ? "report" : "error");
                    }
                }
            }
            y += 22.0f * k;
            ImGui::SetCursorScreenPos(ImVec2(x0, y));
            ImGui::Dummy(ImVec2(1.0f, 1.0f));

            // ---- the body: the shared renderer -------------------------------------
            ModulePageIn in;
            in.census = "store";
            in.id = sm.id;
            in.plate = &p;
            in.pictures = &sm.pictures;
            in.catalogued = true;
            in.noticeBox = !p.legalNotice.empty() && sm.install == StoreInstallKind::NotInstalled;
            in.noticeTick = &deck.legalAck;
            in.width = colW;
            in.glyphCategory = p.category.c_str();
            ImGui::SetCursorScreenPos(ImVec2(x0, y));
            drawModulePageBody(in, deck.page, pictures_);
            y = ImGui::GetCursorScreenPos().y;
        } else if (deck.tab == 0) {
            // ================= BROWSE ================================================
            drawError();
            const std::vector<StoreSection> sections = storeBrowseSections(model, query);
            if (sections.empty()) {
                drawEmptyState();
            }
            const int cols = storeColumnsFor(width);
            const float gapX = 14.0f * k, gapY = 12.0f * k;
            const float cardW = (contentW - gapX * static_cast<float>(cols - 1)) / static_cast<float>(cols);
            const float cardH = cardHeight();
            StoreKeyIn kin;
            kin.busyId = model.busyId;
            kin.busyAny = model.busy;
            for (const StoreSection& sec : sections) {
                const float hh = drawSectionHeading(cdl, ImVec2(x0, y), contentW, storeCategoryHeading(sec.category));
                if (census::enabled()) {
                    census::note("store:section:", storeCategoryCensusName(sec.category));
                    census::rect(std::string("store:section:") + storeCategoryCensusName(sec.category), x0, y,
                                 x0 + contentW, y + hh);
                }
                y += hh;
                for (std::size_t i = 0; i < sec.modules.size(); ++i) {
                    const int mi = sec.modules[i];
                    const StoreModule& sm = model.modules[static_cast<std::size_t>(mi)];
                    const int col = static_cast<int>(i) % cols;
                    const int row = static_cast<int>(i) / cols;
                    const ImVec2 ctl(x0 + static_cast<float>(col) * (cardW + gapX),
                                     y + static_cast<float>(row) * (cardH + gapY));
                    const StoreKey key = storeKeyFor(sm, kin);
                    const CardResult r = drawCard(cdl, sm, key, ctl, cardW, cardH, model.progress);
                    if (r.openPage) {
                        deck.pageId = sm.id;
                        deck.tab = 0;
                    }
                    if (r.keyPressed) {
                        if (key.kind == StoreKeyKind::Get) {
                            if (key.opensPage) {
                                deck.pageId = sm.id;
                            } else {
                                fitIndex_ = mi;
                            }
                        } else if (key.kind == StoreKeyKind::Update) {
                            updateIndex_ = mi;
                        }
                    }
                }
                const int rows = (static_cast<int>(sec.modules.size()) + cols - 1) / cols;
                y += static_cast<float>(rows) * (cardH + gapY) + 10.0f * k;
            }

            // ---- GET EVERYTHING, at the foot ---------------------------------------
            if (!model.modules.empty() && query.empty()) {
                y += 8.0f * k;
                cdl->AddLine(ImVec2(x0, y), ImVec2(x0 + contentW, y), theme::withAlpha(theme::kBrassDark, 0.8f), 1.0f);
                y += 16.0f * k;
                const AddAllPlan plan = planAddAll(model, deck.addAllAck);
                int noticeModules = 0;
                std::string noticeNames;
                for (const StoreModule& sm : model.modules) {
                    if (sm.install != StoreInstallKind::NotInstalled || sm.plate.fitted || sm.plate.legalNotice.empty()) { continue; }
                    if (!sm.blockedReasonIfAcknowledged.empty()) { continue; }
                    ++noticeModules;
                    if (!noticeNames.empty()) { noticeNames += ", "; }
                    noticeNames += sm.plate.name.empty() ? tr("(unnamed module)") : sm.plate.name;
                }
                const char* gl = tr("GET EVERYTHING");
                const float kw = outlineKeyWidth(gl) + 16.0f * k;
                const float kh = outlineKeyHeight() + 6.0f * k;
                const bool enabled = plan.blockedReason.empty() && !model.addAllRunning;
                KeyRect kr;
                if (drawOutlineKey(cdl, ImVec2(x0, y), ImVec2(x0 + kw, y + kh), gl, theme::kInkMuted, enabled,
                                   "geteverything",
                                   enabled ? nullptr : trStoredReason(plan.blockedReason).c_str(), &kr)) {
                    addAll_ = true;
                }
                censusRect("store:geteverything", kr.tl.x, kr.tl.y, kr.br.x, kr.br.y);
                // the note beside it
                std::string lead;
                ImU32 lcol = theme::kInkMuted;
                if (model.addAllRunning) {
                    lead = model.addAllProgress.empty()
                               ? std::string(tr("Working through the catalogue, one module at a time."))
                               : model.addAllProgress;
                    lcol = theme::kGold;
                } else if (!plan.blockedReason.empty()) {
                    lead = cascade::core::formatText(tr("Cannot add all: %s"),
                                                     trStoredReason(plan.blockedReason).c_str());
                } else {
                    lead = cascade::core::formatText(
                        tr("%d to fetch and %d to update, one after another. Each is fetched over "
                           "https and refused unless its bytes hash to the sha256 the catalogue "
                           "published - the same gate a single GET goes through. A module that "
                           "fails does not stop the rest."),
                        static_cast<int>(plan.install.size()), static_cast<int>(plan.update.size()));
                }
                const float nx = x0 + kw + 18.0f * k;
                const float nw = std::max(120.0f, contentW - kw - 18.0f * k);
                cdl->AddText(uf, upx, ImVec2(nx, y), lcol, lead.c_str(), nullptr, nw);
                float ny = y + wrapH(uf, upx, nw, lead.c_str());
                if (!model.addAllRunning && noticeModules > 0) {
                    const std::string skip = cascade::core::formatText(
                        tr("%d of these carry a legal notice from their maker: %s. Each notice is "
                           "on that plugin's page."),
                        noticeModules, noticeNames.c_str());
                    ny += 6.0f * k;
                    cdl->AddText(uf, upx, ImVec2(nx, ny), theme::kGold, skip.c_str(), nullptr, nw);
                    ny += wrapH(uf, upx, nw, skip.c_str()) + 6.0f * k;
                    // THE COMBINED TICK: one consent covering every notice in the run.
                    const std::string ack = cascade::core::formatText(
                        noticeModules == 1 ? tr("I accept the %d legal notice on its plugin's page")
                                           : tr("I accept the %d legal notices on their plugins' pages"),
                        noticeModules);
                    ImGui::SetCursorScreenPos(ImVec2(nx, ny));
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kCream));
                    ImGui::PushFont(uf, upx / uiscale::factor());
                    ImGui::Checkbox(ack.c_str(), &deck.addAllAck);
                    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                    censusRect("store:geteverything:tick", a.x, a.y, b.x, b.y);
                    ImGui::PopFont();
                    ImGui::PopStyleColor();
                    ny = b.y + 6.0f * k;
                }
                if (!model.addAllSummary.empty()) {
                    ny += 4.0f * k;
                    cdl->AddText(uf, upx, ImVec2(nx, ny), model.addAllFailed ? theme::kAlarmHot : theme::kPhosphor,
                                 model.addAllSummary.c_str(), nullptr, nw);
                    ny += wrapH(uf, upx, nw, model.addAllSummary.c_str());
                }
                y = std::max(y + kh, ny) + 24.0f * k;
            }
        } else {
            // ================= UPDATES ===============================================
            drawError();
            const std::vector<int> rows = storeUpdateRows(model, query);
            const bool haveRows = updateCount > 0;
            // The heading, with UPDATE ALL at its right.
            {
                const char* heading = haveRows ? tr("UPDATES AVAILABLE") : tr("NO UPDATES");
                const char* ua = tr("UPDATE ALL");
                const float uw = outlineKeyWidth(ua) + 8.0f * k;
                const float uh = outlineKeyHeight();
                const float hh = drawSectionHeading(cdl, ImVec2(x0, y), haveRows ? contentW - uw - 14.0f * k : contentW,
                                                    heading);
                if (haveRows) {
                    const AddAllPlan plan = planAddAll(model, false);
                    const bool enabled = !model.busy && !model.addAllRunning && !plan.update.empty() && model.haveCatalogue;
                    KeyRect kr;
                    if (drawOutlineKey(cdl, ImVec2(x0 + contentW - uw, y - 4.0f * k), ImVec2(x0 + contentW, y - 4.0f * k + uh),
                                       ua, theme::kAmber, enabled, "updateall",
                                       enabled ? nullptr : tr("a transfer is already in progress"), &kr)) {
                        updateAll_ = true;
                    }
                    censusRect("store:updateall", kr.tl.x, kr.tl.y, kr.br.x, kr.br.y);
                }
                y += std::max(hh, uh) + 4.0f * k;
            }
            if (!model.haveCatalogue) {
                drawEmptyState();
            }
            const float rowH = std::max(64.0f * k, prose() + upx + 28.0f * k);
            StoreKeyIn kin;
            kin.busyId = model.busyId;
            kin.busyAny = model.busy;
            const ImVec2 listTL(x0, y);
            for (std::size_t ri = 0; ri < rows.size(); ++ri) {
                const int mi = rows[ri];
                const StoreModule& sm = model.modules[static_cast<std::size_t>(mi)];
                const ImVec2 rtl(x0, y), rbr(x0 + contentW, y + rowH);
                cdl->AddRectFilled(rtl, rbr, theme::kEnamelDark);
                if (ri > 0) { cdl->AddLine(ImVec2(rtl.x, rtl.y), ImVec2(rbr.x, rtl.y), theme::withAlpha(theme::kBrassDark, 0.8f)); }
                ImGui::PushID(sm.id.c_str());
                const float gb = 46.0f * k;
                const ImVec2 gtl(rtl.x + 14.0f * k, rtl.y + (rowH - gb) * 0.5f);
                ImGui::SetCursorScreenPos(gtl);
                bool open = ImGui::InvisibleButton("##glyph", ImVec2(gb, gb));
                const bool gHover = ImGui::IsItemHovered();
                cdl->AddRectFilled(gtl, ImVec2(gtl.x + gb, gtl.y + gb), theme::kVoid);
                cdl->AddRect(gtl, ImVec2(gtl.x + gb, gtl.y + gb), gHover ? theme::kPhosphor : theme::kBrassDark);
                drawCategoryGlyph(cdl, ImVec2(gtl.x + 1.0f, gtl.y + 1.0f), gb - 2.0f, sm.plate.category,
                                  theme::kPhosphor, 1.8f * (gb - 2.0f) / 56.0f);
                const StoreKey key = storeKeyFor(sm, kin);
                const float kw = cardKeyWidth();
                const float kh = outlineKeyHeight();
                const ImVec2 ktl(rbr.x - 14.0f * k - kw, rtl.y + (rowH - kh) * 0.5f);
                const float tx = gtl.x + gb + 14.0f * k;
                const float tw = std::max(60.0f, ktl.x - 16.0f * k - tx);
                const float nameH = faceH(uf, prose());
                const float ty = rtl.y + (rowH - nameH - 2.0f * k - faceH(uf, upx)) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(tx, ty));
                const float nw = std::min(tw, textW(uf, prose(), sm.plate.name.c_str()));
                if (ImGui::InvisibleButton("##name", ImVec2(std::max(8.0f, nw), nameH))) { open = true; }
                const bool nHover = ImGui::IsItemHovered();
                cdl->AddText(uf, prose(), ImVec2(tx, ty), nHover || gHover ? theme::kPhosphor : theme::kIvory,
                             sm.plate.name.c_str());
                const std::string ft = storeFromTo(sm.installedVersion, sm.updateToVersion);
                ImFont* rf = fonts::ui();
                const float rpx = fonts::tinyPx() * 1.15f;
                cdl->AddText(rf, rpx, ImVec2(tx + nw + 14.0f * k, ty + (nameH - faceH(rf, rpx)) * 0.5f),
                             theme::kAmber, ft.c_str());
                const std::string& line = !sm.plate.whatsNew.empty() ? sm.plate.whatsNew : sm.updateReason;
                addEllipsized(cdl, uf, upx, ImVec2(tx, ty + nameH + 2.0f * k), theme::kInkMuted, line.c_str(), tw);
                KeyRect kr;
                const std::string why = (!key.enabled && !key.reason.empty()) ? trStoredReason(key.reason) : std::string();
                if (key.kind == StoreKeyKind::Fitting) {
                    ImGui::SetCursorScreenPos(ktl);
                    ImGui::InvisibleButton("##fitting", ImVec2(kw, kh));
                    cdl->AddRect(ktl, ImVec2(ktl.x + kw, ktl.y + kh), theme::kPhosphorDim, 2.0f);
                    drawKeyWord(cdl, ktl, ImVec2(ktl.x + kw, ktl.y + kh), storeKeyLabel(key.kind), theme::kPhosphor);
                    drawFittingLine(cdl, ktl, ImVec2(ktl.x + kw, ktl.y + kh), model.progress);
                    kr = KeyRect{ktl, ImVec2(ktl.x + kw, ktl.y + kh)};
                } else if (drawOutlineKey(cdl, ktl, ImVec2(ktl.x + kw, ktl.y + kh), storeKeyLabel(StoreKeyKind::Update),
                                          theme::kAmber, key.enabled, "key", why.c_str(), &kr)) {
                    updateIndex_ = mi;
                }
                if (census::enabled()) {
                    const std::string base = "store:card:" + sm.id;
                    census::rect(base, rtl.x, rtl.y, rbr.x, rbr.y);
                    census::rect(base + ":key", kr.tl.x, kr.tl.y, kr.br.x, kr.br.y);
                    census::rect(base + ":name", tx, ty, tx + nw, ty + nameH);
                    census::note("store:key:", sm.id + ":" + storeKeyCensusState(key));
                    census::note("store:update:", sm.id + ":" + ft);
                }
                ImGui::PopID();
                if (open) { deck.pageId = sm.id; deck.tab = 0; }
                y += rowH;
            }
            if (!rows.empty()) {
                cdl->AddRect(listTL, ImVec2(x0 + contentW, y), theme::withAlpha(theme::kBrassDark, 0.9f));
            }
            if (haveRows) {
                y += 12.0f * k;
                const char* note = tr("Updates are fetched when you press CHECK AGAIN. Nothing updates itself.");
                cdl->AddText(uf, upx, ImVec2(x0, y), theme::kInkMuted, note, nullptr, contentW);
                y += wrapH(uf, upx, contentW, note) + 16.0f * k;
            }
            // ---- CLEAN UP OLD VERSIONS, at the foot ------------------------------------
            ImGui::SetCursorScreenPos(ImVec2(x0, y));
            if (drawCleanupFoot(model.oldCopies, model.cleanupReport, model.busy, contentW, "storekey:cleanup",
                                "storekey:cleanupyes")) {
                cleanup_ = true;
            }
            y = ImGui::GetCursorScreenPos().y + 12.0f * k;
        }

        ImGui::SetCursorScreenPos(ImVec2(x0, y + 8.0f * k));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
    }
    ImGui::EndChild();

    // Esc returns from a page - and only that, and not while a field has the keyboard.
    if (onPage && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        deck.pageId.clear();
        if (census::enabled()) { census::note("store:page:closed:", "esc"); }
    }

    ImGui::PopID();
}

}  // namespace cascade::gui
