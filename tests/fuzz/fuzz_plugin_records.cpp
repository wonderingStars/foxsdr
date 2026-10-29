// Fuzz target: everything a plugin HANDS BACK - track records, paths, panel rows and columns, an instrument's state, decoded text, a picture's
// numbers and pixels - consumed by the REAL host code that consumes it (plugin containment P2, docs/engine-stage4.md 6.29).
//
// The plugin here is a table of plain C functions in this file (no module): each reads the fuzz input and answers with it, exactly as a plugin
// would - a count that exceeds the room it was given, coordinates that are NaN or off the globe, cells with no terminator, an image whose
// stride is shorter than a row, a text poll that says it wrote more than it did. The consumers are the product's own: PluginUi::poll (tracks,
// paths, panels, instruments) and PluginRunner (text, pictures), through the guarded calls and the guarded reads of memory a plugin lent.
//
// THE PROPERTIES, after every frame: what the interface holds is bounded (4000 tracks, 64 paths of 20000 points, 2000 rows of at most 8 cells,
// 8 columns), on the globe and finite where it is drawn, and every cell and text slot is a C string; a picture the runner holds is exactly
// width x height x bytes-per-pixel with both within the ABI's bound; no line of decoded text is unbounded; and every picture the plugin lent was
// handed back (a release for every successful poll, unless the plugin was switched off, which the guard answers by never calling it again).
// And, of course, the host does not fault: a bounds error in a consumer is an AddressSanitizer report in a fuzz build.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/plugin_fault.hpp"
#include "core/plugin_host_data.hpp"
#include "core/plugin_runner.hpp"
#include "core/plugin_ui.hpp"
#include "fuzz_common.hpp"

using cascade::core::LoadedPlugin;
using cascade::core::PluginRunner;
using cascade::core::PluginUi;

namespace {

// ---- the script: what the fake plugin answers this time -----------------------------------------------------------------------------
struct Script {
    std::int32_t tracksReturn = 0;
    std::vector<CascadeTrack> tracks;
    std::int32_t pathsReturn = 0;
    std::vector<CascadePath> paths;
    std::vector<CascadePathPoint> points;
    std::uint32_t columnsReturn = 1;
    char headings[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS] = {};
    std::int32_t rowsReturn = 0;
    std::vector<CascadePanelRow> rows;
    std::int32_t stateReturn = 0;
    CascadeInstrumentState state{};
    std::int32_t textReturn = 0;
    std::string text;
    std::int32_t imageReturn = 0;
    CascadeImage image{};
    std::vector<std::uint8_t> pixels;
};
Script g;
int g_handle = 0;
int g_borrowed = 0;   // pictures lent and not yet handed back (across the script changing between frames)

void* fakeCreate() { return &g_handle; }
void* fakeCreateRate(std::uint32_t) { return &g_handle; }
void* fakeCreateImage(double, double) { return &g_handle; }
void fakeDestroy(void*) {}

std::int32_t fakePollTracks(void*, CascadeTrack* out, std::uint32_t cap) {
    const std::size_t n = std::min<std::size_t>(g.tracks.size(), cap);
    for (std::size_t i = 0; i < n; ++i) { out[i] = g.tracks[i]; }
    return g.tracksReturn;
}

std::int32_t fakePollPaths(void*, CascadePath* out, std::uint32_t cap) {
    const std::size_t n = std::min<std::size_t>(g.paths.size(), cap);
    for (std::size_t i = 0; i < n; ++i) { out[i] = g.paths[i]; }
    return g.pathsReturn;
}

std::uint32_t fakeColumns(void*, char headings[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS]) {
    std::memcpy(headings, g.headings, sizeof(g.headings));
    return g.columnsReturn;
}

std::int32_t fakePollRows(void*, CascadePanelRow* out, std::uint32_t cap) {
    const std::size_t n = std::min<std::size_t>(g.rows.size(), cap);
    for (std::size_t i = 0; i < n; ++i) { out[i] = g.rows[i]; }
    return g.rowsReturn;
}

std::int32_t fakePollState(void*, CascadeInstrumentState* out) {
    *out = g.state;
    return g.stateReturn;
}

void fakeProcess(void*, const float*, std::size_t) {}

std::int32_t fakePollText(void*, char* buf, std::size_t cap) {
    const std::size_t n = std::min<std::size_t>(g.text.size(), cap);
    std::memcpy(buf, g.text.data(), n);
    return g.textReturn;
}

std::int32_t fakePollNoText(void*, char*, std::size_t) { return 0; }

std::int32_t fakePollImage(void*, CascadeImage* out) {
    const std::uint32_t size = out->structSize;
    *out = g.image;
    out->structSize = size;
    if (g.imageReturn > 0) { ++g_borrowed; }
    return g.imageReturn;
}

void fakeReleaseImage(void*, const CascadeImage*) { --g_borrowed; }

// ---- the tables ---------------------------------------------------------------------------------------------------------------------
CascadeTrackSourceApi makeTracks() {
    CascadeTrackSourceApi a{};
    a.structSize = sizeof(a);
    a.create = &fakeCreate;
    a.poll_tracks = &fakePollTracks;
    a.poll_paths = &fakePollPaths;
    a.destroy = &fakeDestroy;
    return a;
}
CascadePanelApi makePanel() {
    CascadePanelApi a{};
    a.structSize = sizeof(a);
    a.title = "Fuzz panel";
    a.create = &fakeCreate;
    a.columns = &fakeColumns;
    a.poll_rows = &fakePollRows;
    a.destroy = &fakeDestroy;
    return a;
}
CascadeInstrumentApi makeInstrument() {
    CascadeInstrumentApi a{};
    a.structSize = sizeof(a);
    a.title = "Fuzz instrument";
    a.kind = CASCADE_INSTRUMENT_GENERIC;
    a.create = &fakeCreate;
    a.poll_state = &fakePollState;
    a.columns = &fakeColumns;
    a.poll_rows = &fakePollRows;
    a.destroy = &fakeDestroy;
    return a;
}
CascadeDecoderApi makeDecoder() {
    CascadeDecoderApi a{};
    a.structSize = sizeof(a);
    a.create = &fakeCreateRate;
    a.process = &fakeProcess;
    a.poll_text = &fakePollText;
    a.destroy = &fakeDestroy;
    return a;
}
CascadeImageDecoderApi makeImage() {
    CascadeImageDecoderApi a{};
    a.structSize = sizeof(a);
    a.inputKind = CASCADE_INPUT_AUDIO;
    a.create = &fakeCreateImage;
    a.process = &fakeProcess;
    a.poll_image = &fakePollImage;
    a.release_image = &fakeReleaseImage;
    a.poll_text = &fakePollNoText;
    a.destroy = &fakeDestroy;
    return a;
}

// ---- decoding the script out of the input -----------------------------------------------------------------------------------------
void fillChars(fuzz::Bytes& in, char* dst, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) { dst[i] = static_cast<char>(in.u8()); }
}

CascadeTrack drawTrack(fuzz::Bytes& in) {
    CascadeTrack t{};
    fillChars(in, t.id, sizeof(t.id));
    fillChars(in, t.label, sizeof(t.label));
    t.latDeg = in.rawDouble();
    t.lonDeg = in.rawDouble();
    t.altM = in.rawDouble();
    t.courseDeg = in.rawDouble();
    t.speedMps = in.rawDouble();
    t.ageMs = in.u64();
    t.kind = in.u32();
    t.flags = in.u32();
    // half the time a plausible position: the placed branches are where the host's code is
    if ((in.u8() & 1u) != 0u) {
        t.latDeg = static_cast<double>(static_cast<int>(in.inRange(0, 180)) - 90) + 0.5;
        t.lonDeg = static_cast<double>(static_cast<int>(in.inRange(0, 360)) - 180) + 0.5;
    }
    return t;
}

CascadePanelRow drawRow(fuzz::Bytes& in) {
    CascadePanelRow r{};
    r.kind = in.u32() % 5u;
    r.flags = in.u32();
    for (auto& cell : r.cells) {
        fillChars(in, cell, sizeof(cell));
        if ((in.u8() & 3u) != 0u) { cell[in.inRange(0, CASCADE_PANEL_CELL_CHARS - 1)] = '\0'; }   // sometimes terminated, sometimes to the brim
    }
    return r;
}

void drawScript(fuzz::Bytes& in) {
    g = Script{};
    const unsigned nTracks = in.inRange(0, 12);
    for (unsigned i = 0; i < nTracks; ++i) { g.tracks.push_back(drawTrack(in)); }
    g.tracksReturn = (in.u8() & 3u) == 0u ? static_cast<std::int32_t>(in.u32()) : static_cast<std::int32_t>(nTracks);
    const unsigned nPoints = in.inRange(0, 20);
    for (unsigned i = 0; i < nPoints; ++i) { g.points.push_back({in.rawDouble(), in.rawDouble()}); }
    const unsigned nPaths = in.inRange(0, 3);
    for (unsigned i = 0; i < nPaths; ++i) {
        CascadePath p{};
        fillChars(in, p.id, sizeof(p.id));
        p.kind = in.u32();
        p.flags = in.u32();
        p.points = (in.u8() & 7u) == 0u ? nullptr : g.points.data();
        p.count = in.inRange(0, nPoints);   // (honest: the plugin lends what it says; lies are the hostile fixture's)
        g.paths.push_back(p);
    }
    g.pathsReturn = (in.u8() & 3u) == 0u ? static_cast<std::int32_t>(in.u32()) : static_cast<std::int32_t>(nPaths);
    g.columnsReturn = (in.u8() & 3u) == 0u ? in.u32() % 300u : in.inRange(0, 9);
    for (auto& h : g.headings) { fillChars(in, h, sizeof(h)); }
    const unsigned nRows = in.inRange(0, 10);
    for (unsigned i = 0; i < nRows; ++i) { g.rows.push_back(drawRow(in)); }
    g.rowsReturn = (in.u8() & 3u) == 0u ? static_cast<std::int32_t>(in.u32()) : static_cast<std::int32_t>(nRows);
    g.state.structSize = sizeof(g.state);
    g.state.flags = in.u32();
    g.state.seq = in.u32();
    for (double& v : g.state.values) { v = in.rawDouble(); }
    for (auto& t : g.state.text) { fillChars(in, t, sizeof(t)); }
    g.stateReturn = static_cast<std::int32_t>(in.u32() % 5u) - 1;
    g.text = in.str(in.inRange(0, 3000));
    g.textReturn = (in.u8() & 3u) == 0u ? static_cast<std::int32_t>(in.u32()) : static_cast<std::int32_t>(g.text.size());
    // a picture: numbers from the input, and a buffer that holds what the host would read if it believed them (or a refusal)
    g.imageReturn = static_cast<std::int32_t>(in.u32() % 4u) - 1;
    g.image.structSize = sizeof(g.image);
    g.image.width = (in.u8() & 7u) == 0u ? in.u32() : in.inRange(0, 70);
    g.image.height = (in.u8() & 7u) == 0u ? in.u32() : in.inRange(0, 70);
    g.image.format = (in.u8() & 7u) == 0u ? in.u32() : (in.u8() & 1u) + 1u;
    g.image.stride = (in.u8() & 3u) == 0u ? in.u32() : g.image.width * (g.image.format == CASCADE_IMAGE_RGB24 ? 3u : 1u) + in.inRange(0, 9);
    g.image.complete = static_cast<std::int32_t>(in.u32());
    g.image.sequence = in.u64();
    const std::uint64_t bpp = g.image.format == CASCADE_IMAGE_RGB24 ? 3u : 1u;
    std::uint64_t need = g.image.height == 0 ? 0 : static_cast<std::uint64_t>(g.image.height - 1) * g.image.stride + static_cast<std::uint64_t>(g.image.width) * bpp;
    if (need > (4u << 20)) {   // more than the buffer this run can hold: a size the host would read (it is within its bounds) is shrunk, one it refuses is kept
        const bool refused = g.image.width > CASCADE_IMAGE_MAX_DIM || g.image.height > CASCADE_IMAGE_MAX_DIM || g.image.stride < g.image.width * bpp ||
                             static_cast<std::uint64_t>(g.image.width) * g.image.height > 64u * 1024u * 1024u || bpp > 3u;
        if (!refused) {
            g.image.height = 1;
            g.image.width = std::min<std::uint32_t>(g.image.width, 1000);
            g.image.stride = g.image.width * static_cast<std::uint32_t>(bpp);
            need = g.image.stride;
        } else {
            need = 64;
        }
    }
    g.pixels.resize(static_cast<std::size_t>(need));
    for (auto& b : g.pixels) { b = in.u8(); }
    g.image.pixels = (in.u8() & 15u) == 0u ? nullptr : g.pixels.data();
}

void requireSaneInterface(const PluginUi& ui) {
    FUZZ_REQUIRE(ui.trackCount() <= 4000);
    for (const cascade::core::HostTrack& t : ui.tracks()) {
        FUZZ_REQUIRE(std::isfinite(t.t.latDeg) && std::isfinite(t.t.lonDeg));
        FUZZ_REQUIRE(t.t.latDeg >= -90.0 && t.t.latDeg <= 90.0 && t.t.lonDeg >= -180.0 && t.t.lonDeg <= 180.0);
    }
    FUZZ_REQUIRE(ui.unplacedTracks().size() <= 4000);
    FUZZ_REQUIRE(ui.paths().size() <= 64);
    for (const cascade::core::HostPath& p : ui.paths()) { FUZZ_REQUIRE(p.points.size() <= 20000); }
    for (const cascade::core::HostPanel& p : ui.panels()) {
        FUZZ_REQUIRE(!p.headings.empty() && p.headings.size() <= CASCADE_PANEL_MAX_COLUMNS);
        FUZZ_REQUIRE(p.rows.size() <= 2000);
        for (const CascadePanelRow& r : p.rows) {
            for (const auto& cell : r.cells) { FUZZ_REQUIRE(std::memchr(cell, '\0', sizeof(cell)) != nullptr); }
        }
    }
    for (const cascade::core::HostInstrument& in : ui.instruments()) {
        for (const auto& t : in.state.text) { FUZZ_REQUIRE(std::memchr(t, '\0', sizeof(t)) != nullptr); }
        FUZZ_REQUIRE(in.headings.size() <= CASCADE_PANEL_MAX_COLUMNS);
        FUZZ_REQUIRE(in.rows.size() <= 2000);
        for (const CascadePanelRow& r : in.rows) {
            for (const auto& cell : r.cells) { FUZZ_REQUIRE(std::memchr(cell, '\0', sizeof(cell)) != nullptr); }
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Bytes in(data, size);
    cascade::core::fault::resetForTest();   // nothing switched off by the last input

    static const CascadeTrackSourceApi tracks = makeTracks();
    static const CascadePanelApi panel = makePanel();
    static const CascadeInstrumentApi instrument = makeInstrument();
    static const CascadeDecoderApi decoder = makeDecoder();
    static const CascadeImageDecoderApi image = makeImage();
    LoadedPlugin lp;
    lp.path = "fuzz-1.0.0-abi3-linux-x64.so";
    lp.name = "Fuzz";
    lp.version = "1.0.0";
    lp.loaded = true;
    lp.capabilities = CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_PANEL | CASCADE_CAP_INSTRUMENT | CASCADE_CAP_DECODER | CASCADE_CAP_IMAGE_DECODER;
    lp.trackSource = &tracks;
    lp.panel = &panel;
    lp.instrument = &instrument;
    lp.decoder = &decoder;
    lp.imageDecoder = &image;

    g_borrowed = 0;
    drawScript(in);
    PluginUi ui;
    ui.rebuild({lp});
    PluginRunner runner;
    runner.rebuild({lp}, 48000.0, 2400000.0, 100000000.0);

    std::vector<cascade::core::HostImage> images;
    std::vector<float> audio(480, 0.0f);
    for (int frame = 0; frame < 3; ++frame) {
        ui.poll();
        requireSaneInterface(ui);
        runner.processAudio(audio.data(), audio.size());
        runner.pollImages(images);
        for (const cascade::core::HostImage& img : images) {
            FUZZ_REQUIRE(img.width <= CASCADE_IMAGE_MAX_DIM && img.height <= CASCADE_IMAGE_MAX_DIM);
            if (!img.pixels.empty()) {
                const std::size_t bpp = img.format == CASCADE_IMAGE_RGB24 ? 3u : 1u;
                FUZZ_REQUIRE(img.format == CASCADE_IMAGE_RGB24 || img.format == CASCADE_IMAGE_GRAY8);
                FUZZ_REQUIRE(img.pixels.size() == static_cast<std::size_t>(img.width) * img.height * bpp);
            }
        }
        for (const cascade::core::DecodedLine& line : runner.drainText()) { FUZZ_REQUIRE(line.text.size() <= 16384); }
        // the next poll answers differently (the same shapes, different numbers)
        if (in.remaining() > 0) { drawScript(in); }
    }
    // every picture lent was handed back, unless the plugin was switched off (the guard then never calls it again)
    FUZZ_REQUIRE(g_borrowed == 0 || cascade::core::fault::isQuarantinedKey(lp.path));
    runner.clear();
    ui.clear();
    return 0;
}
