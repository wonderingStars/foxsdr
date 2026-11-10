// scene3d.hpp - THE SHARED 3D SCENE FRAMEWORK (docs/engine-stage4.md 6.64): a pure CPU renderer into a caller-owned RGBA buffer, with no OpenGL and no Slint types, so a 3D view
// works on every renderer including the software-GL fallback and is testable headless. The Slint side (ui/scene3d.slint, scene3d_slint.hpp) only hands the picture to an Image and
// puts Slint text labels over the screen positions this file projects - the words are never rasterised here.
//
// WORLD: right-handed, Z up. X is east, Y north in the geographic views; a view maps its own data onto that (waterfall: X frequency, Y age, Z level).
// CAMERA: an orbit camera. azimuthDeg 0 puts the eye south of the target looking north (X to the right); a positive azimuth moves the eye to the east. elevationDeg is the angle
// above the horizon. The distance is set so a sphere of `sceneRadius` fits the shorter side of the picture at zoom 1; zoom divides it. Perspective, field of view `fovDeg`.
// PIPELINE: every vertex goes to view space, a triangle or a line is clipped against the near plane, projected, and rasterised with a z-buffer holding 1/depth (bigger is nearer).
//   Triangles are opaque and write depth unless their alpha is under 255 (they blend and do not write depth). Lines and markers test depth (with a small bias) and never write it, so the
//   grid behind a surface is hidden by it and the foot lines in front are not.
// DETERMINISM: single-threaded, no randomness, no time: the same scene and camera give byte-identical pixels (test_slint_scene3d pins it).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "host_logic.hpp"

namespace foxsdr::scene3d {

using foxsdr::slinthost::Rgba;

constexpr float kPi = 3.14159265358979323846f;

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 normalize(Vec3 a);
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

Rgba mix(Rgba a, Rgba b, float t);                 // straight linear blend of all four channels, t clamped to 0..1
Rgba withAlpha(Rgba c, std::uint8_t a);
Rgba scaled(Rgba c, float k);                      // RGB times k (clamped), alpha kept

// ---- the camera ----
struct Camera {
    float azimuthDeg = -58.0f;
    float elevationDeg = 32.0f;
    float zoom = 1.0f;
    float fovDeg = 32.0f;
    Vec3 target{};
    float sceneRadius = 1.0f;
};
constexpr float kMinElevationDeg = 4.0f, kMaxElevationDeg = 89.0f, kMinZoom = 0.35f, kMaxZoom = 5.0f;
Camera clamped(Camera c);                          // elevation, zoom and azimuth (wrapped to -180..180) into their ranges
Vec3 eyePosition(const Camera& c);

struct ScreenPoint {
    float x = 0.0f, y = 0.0f;                      // pixels, y down, from the picture's top-left corner
    float depth = 0.0f;                            // distance along the view direction
    bool inFront = false;                          // beyond the near plane
};

// ---- colour ramps ----
class Ramp {
public:
    struct Stop {
        float at;
        Rgba c;
    };
    Ramp() = default;
    explicit Ramp(const std::vector<Stop>& stops);   // at ascending in 0..1; two stops at least else a flat grey
    Rgba at(float t) const;                          // t clamped to 0..1 (a NaN is 0)
    static Ramp heat();                              // the waterfall's strength key (host_logic heat())
    static Ramp altitude();                          // the radar's altitude key: indigo through magenta and coral to amber
    static Ramp viridis();                           // the radiosonde track's key: purple, blue, green, yellow
    static Ramp timeTunnel();                        // the constellation's time key: cyan to magenta
    static Ramp blueHeight();                        // the single-hue shading: deep navy to light blue (the surface's SHADING = Height)
private:
    std::array<Rgba, 256> lut_{};
};

// ---- meshes ----
struct Vertex {
    Vec3 p;
    Rgba c{255, 255, 255, 255};
    float u = 0.0f, v = 0.0f;                        // texture coordinates (used when a texture is bound)
};
struct Mesh {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> idx;                  // triangles, three per entry
    void clear() { v.clear(); idx.clear(); }
};

// A height field on a regular grid: rows x cols values, row-major. x = origin.x + sx * col/(cols-1), y = origin.y + sy * row/(rows-1), z = origin.z + zScale * norm(value).
// A value that is not finite leaves a HOLE: the quads that touch it are not built (a waterfall that has not filled yet, a survey cell never seen).
struct HeightFieldSpec {
    int cols = 0, rows = 0;
    const float* values = nullptr;
    const float* colourValues = nullptr;             // the values the colour follows; null = the heights'
    Vec3 origin{};
    float sx = 1.0f, sy = 1.0f, zScale = 1.0f;
    float lo = 0.0f, hi = 1.0f;                      // the value range that maps to 0..1 (clamped)
    float colourLo = 0.0f, colourHi = 1.0f;          // the range the colour follows (when colourValues is set)
    const Ramp* ramp = nullptr;
};
Mesh buildHeightField(const HeightFieldSpec& spec);

// A UV sphere, Z up (the poles on the Z axis), u = longitude (0 at -180 degrees to 1 at +180), v = 0 at the north pole to 1 at the south. The seam column is duplicated, so a
// texture never wraps inside a triangle.
Mesh buildSphere(float radius, int lonSegments, int latSegments, Rgba colour);
Vec3 latLonToVec(double latDeg, double lonDeg, float radius);                  // x = cos(lat)cos(lon), y = cos(lat)sin(lon), z = sin(lat)
// The great-circle arc between two points of a sphere of `radius`, n + 1 points, lifted off the surface by sin(pi t) * liftFrac * radius (an arc through the air, not a chord).
std::vector<Vec3> greatCircleArc(double lat0, double lon0, double lat1, double lon1, float radius, float liftFrac, int n);

// An equirectangular picture (width x height, RGBA, row 0 the north) sampled bilinearly; u wraps, v clamps.
struct Texture {
    int w = 0, h = 0;
    const Rgba* px = nullptr;
};
Rgba sampleTexture(const Texture& t, float u, float v);

// ---- the primitives ----
enum class Dash { Solid, Dashed, Dotted };
enum class Marker { Dot, Square, TriangleUp, TriangleDown, Star, Cross, Diamond };

// A picture's worth of 3D. Hold one per view and call begin() each frame; its depth buffer is reused while the size stays.
class Scene {
public:
    // Binds the colour buffer (w * h Rgba, row-major, top row first), clears it to `background` and the depth to infinity. Nothing is allocated unless the size grew.
    void begin(Rgba* px, int w, int h, Rgba background, const Camera& camera);
    int width() const { return w_; }
    int height() const { return h_; }
    const Camera& camera() const { return cam_; }

    ScreenPoint project(Vec3 p) const;
    // The nearest surface at a pixel after drawing (the z-buffer): 0 = nothing was drawn there, else the distance. A label anchor is hidden behind a surface when its own depth is bigger.
    float depthAt(int x, int y) const;
    bool occluded(Vec3 p) const;                     // the point is behind what was drawn at its pixel (a small tolerance), or off the picture, or behind the eye

    // Lighting for lit meshes: a direction FROM the surface TO the light (world), and the share of the colour that is unlit. Two-sided.
    void setLight(Vec3 towardLight, float ambient);
    void bindTexture(const Texture* t) { tex_ = t; }   // a mesh drawn while one is bound multiplies its vertex colour by the picture at (u, v)

    std::size_t drawMesh(const Mesh& m, bool lit);   // triangles drawn (after clipping)
    void triangle(const Vertex& a, const Vertex& b, const Vertex& c, bool lit);
    void line(Vec3 a, Vec3 b, Rgba colour, float widthPx, Dash dash = Dash::Solid);
    // A polyline with one colour per point (the segment takes the colour of its end) - a track coloured by altitude; `colours` may be empty (then `colour`).
    void polyline(const std::vector<Vec3>& pts, const std::vector<Rgba>& colours, Rgba colour, float widthPx, Dash dash = Dash::Solid);
    void marker(Vec3 p, Marker shape, Rgba colour, float sizePx);
    void point(Vec3 p, Rgba colour, float radiusPx);   // a filled dot (anti-aliased)

    std::size_t trianglesDrawn() const { return trisDrawn_; }
    std::size_t pixelsWritten() const { return pxWritten_; }

private:
    struct VP {            // a vertex in view space
        float x, y, z;     // x right, y up, z forward (distance)
        Rgba c;
        float u, v;
    };
    VP toView(const Vertex& vtx) const;
    void triView(const VP& a, const VP& b, const VP& c, bool lit);
    void rasterTriangle(const VP& a, const VP& b, const VP& c, float shade);
    void drawLineView(VP a, VP b, float widthPx, Dash dash);
    bool toScreen(const VP& p, float& sx, float& sy, float& inv) const;
    void blendPixel(int x, int y, Rgba c, float cover);

    Rgba* px_ = nullptr;
    int w_ = 0, h_ = 0;
    std::vector<float> inv_;                         // 1 / depth, 0 = nothing
    std::vector<VP> vcache_;                         // a mesh's vertices in view space
    Camera cam_;
    Vec3 eye_{}, fwd_{}, right_{}, up_{};
    float focal_ = 1.0f;
    Vec3 light_{0.0f, 0.0f, 1.0f};
    float ambient_ = 0.5f;
    const Texture* tex_ = nullptr;
    std::size_t trisDrawn_ = 0, pxWritten_ = 0;
};

// ---- the axis box: the frame every "box" view sits in ----
// Draws the floor grid, the two walls behind the data (chosen from the eye's side, so they turn as the camera does), the box's edges, and tick marks; and reports where the
// tick labels belong, as 3D positions the caller projects (labels are Slint text). Call it BEFORE the data (the walls are behind) and drawBoxFront AFTER (the front edges).
struct BoxSpec {
    Vec3 lo{}, hi{1.0f, 1.0f, 1.0f};
    int xTicks = 5, yTicks = 5, zTicks = 5;          // the number of gridlines per axis (>= 2 draws both ends)
    Rgba edge{150, 156, 168, 255}, grid{80, 90, 110, 110}, floorGrid{80, 90, 110, 90};
    float edgeWidthPx = 1.4f;
    bool floor = true, walls = true;
};
struct AxisTick {
    Vec3 pos;                                        // on the box's edge, where the tick starts
    Vec3 labelPos;                                   // a little outside the box, where its label is centred
    float frac;                                      // 0..1 along the axis
    int axis;                                        // 0 x, 1 y, 2 z
};
struct BoxLayout {
    std::vector<AxisTick> ticks;
    Vec3 axisTitle[3];                               // where each axis's caption goes (outside the middle of its edge)
};
BoxLayout drawBoxBack(Scene& s, const BoxSpec& b);
void drawBoxFront(Scene& s, const BoxSpec& b);

// ---- labels over the picture ----
// A Slint text label placed at a projected anchor. x, y are FRACTIONS of the picture (0..1) so the well places it whatever size the image is shown at.
struct Label3D {
    float x = 0.0f, y = 0.0f;
    std::string text;
    std::uint32_t rgba = 0xE0E0E0FF;
    int align = 0;                                   // 0 centred on the anchor, 1 starts at it, 2 ends at it
    int kind = 0;                                    // 0 tick, 1 axis caption, 2 marker/aircraft label, 3 note (a view's own meaning)
};
// Projects a world point into a label; returns false (and adds nothing) when the point is behind the eye, off the picture, or hidden behind drawn geometry when `hideBehind`.
bool addLabel(const Scene& s, std::vector<Label3D>& out, Vec3 p, const std::string& text, std::uint32_t rgba, int align, int kind, bool hideBehind);

// The drag-to-rotate and wheel rules, shared by the well's host side and the tests: a drag of (dx, dy) pixels turns the camera, a wheel notch zooms by a fixed factor.
constexpr float kDegPerPixel = 0.45f;
constexpr float kZoomPerNotch = 1.12f;
Camera dragged(Camera c, float dxPx, float dyPx);
Camera zoomed(Camera c, int notches);

}  // namespace foxsdr::scene3d
