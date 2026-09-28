// test_patch_graph_draft_input.cpp - the Patch page's draft of the graph,
// driven through ImGui's own input queue (docs/engine-stage3.md OPEN 6,
// Design A: the page edits a draft and hands the whole graph to the Engine as
// FOXAPP_OP_PATCH_SET_GRAPH).
//
// WHY THROUGH THE INPUT QUEUE, as tests/test_scanner_draft_input.cpp does for
// the Scanner form: a test that applies hand-built commands would not notice a
// page that edits its draft and never sends it, sends it after the patch's
// sets were built, or sends a stale copy over what the engine did meanwhile.
// This one presses the parts bin's keys and drags on the real canvas
// (AppWindow::drawPatchView, the frame in drawUi's order: the once-a-frame
// commit, the view, the command drain) and reads back the ENGINE's graph.
//
//   A  a part pressed in the parts bin reaches the engine
//   B  a node dragged by its title bar reaches the engine, every frame of the
//      drag, and is already there when patchPublishSets builds from it in
//      that same frame (Engine::testHooks_.patchPublishing)
//   C  a wire dragged from a port to a port reaches the engine
//   D  a wire clicked and Deleted is cut in the engine
//   E  a node clicked and Deleted is removed from the engine, and every
//      other node keeps its id
//   F  the engine's graph changing on its own (ALL OFF throws the radio's
//      switch; a new patch document is loaded) is what the page shows on the
//      next frame, and each of the page's own edits leaves the engine's graph
//      EXACTLY the page's
//   G  a drag in progress while the engine changes the dragged node and
//      another: the drag is never thrown back, and neither change is lost
//   H  a draft the engine cannot take (a frequency that is not a number) is
//      refused, the engine's graph unchanged, and the page shows the engine's
//   K  a wire drag left in the air by the view going away (the button
//      released while it was hidden) does not stop the page following the
//      engine
//   I  a draft that was never copied from the engine is never sent: the
//      once-a-frame commit before any frame is drawn leaves a loaded patch
//      alone
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/patch_draft.hpp"
#include "gui/app_window.hpp"
#include "gui/fonts.hpp"
#include "gui/patch_view_math.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "source/device_source.hpp"
#include "source/soapy_source.hpp"
#include "test_check.hpp"

namespace pc = cascade::core::patch;
namespace pg = cascade::gui::patch;

namespace {

std::vector<cascade::source::NativeDeviceInfo> noNativeRadios() { return {}; }
std::vector<cascade::source::SoapyDeviceInfo> noSoapyRadios() { return {}; }
std::string g_pluginDir;
std::string pluginDir() { return g_pluginDir; }

// What patchPublishSets saw of one node, this frame.
pc::NodeId g_watch = pc::kNoNode;
bool g_published = false;
float g_publishedX = 0.0f;
float g_publishedY = 0.0f;
void onPublish(const pc::Graph& g) {
    if (const pc::Node* n = g.find(g_watch)) {
        g_published = true;
        g_publishedX = n->x;
        g_publishedY = n->y;
    }
}

}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    static void installHooks() {
        cascade::engine::Engine::testHooks_.nativeScan = &noNativeRadios;
        cascade::engine::Engine::testHooks_.soapyScan = &noSoapyRadios;
        cascade::engine::Engine::testHooks_.pluginDir = &pluginDir;
        cascade::engine::Engine::testHooks_.patchPublishing = &onPublish;
    }
    static void draw(AppWindow& a) { a.drawPatchView(); }
    // drawUi's once-a-frame commit.
    static void flush(AppWindow& a) { a.commitPatchDraft(); }
    static void drain(AppWindow& a) { a.engine_.drainLocalCommands(); }
    static FoxCommandResult apply(AppWindow& a, const cascade::core::cmd::QueuedCommand& q) {
        return a.engine_.applyCommand(q.c, q.longText);
    }
    static FoxCommandResult apply(AppWindow& a, const FoxCommand& c) { return a.engine_.applyCommand(c); }
    // A patch document, as applyConfig loads one.
    static void load(AppWindow& a, const pc::Graph& g) { (void)a.loadPatchDocument(g); }
    static const pc::Graph& engine(AppWindow& a) { return a.engine_.patchGraph_; }
    static const pc::Graph& draft(AppWindow& a) { return a.patchDraft_; }
    static pc::Graph& mutableDraft(AppWindow& a) { return a.patchDraft_; }
    // The Engine's own runtime changing a node (as patchReconcile does when
    // an I/Q recording tells a Radio its rate) - the test stands in for it.
    static void engineSetsRate(AppWindow& a, pc::NodeId id, double hz) {
        if (pc::Node* n = a.engine_.patchGraph_.mutableNode(id)) { n->rateHz = hz; }
    }
    static float originX(AppWindow& a) { return a.patchCanvasOriginX_; }
    static float originY(AppWindow& a) { return a.patchCanvasOriginY_; }
    static pg::View view(AppWindow& a) {
        return pg::View{pg::Vec2{a.patchCanvasOriginX_ + a.patchUi_.view.pan.x,
                                 a.patchCanvasOriginY_ + a.patchUi_.view.pan.y},
                        a.patchUi_.view.zoom};
    }
    static pc::NodeId dragging(AppWindow& a) { return a.patchUi_.dragNode; }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

// False while the Patch view is not drawn (the receiver view was chosen):
// drawUi still runs its once-a-frame commit, the view does not.
bool g_viewShown = true;

// One frame, in drawUi's order for what this test touches: the once-a-frame
// commit near the top, then the view, then the next drain.
void frame(AppWindow& a) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    g_published = false;
    ImGui::NewFrame();
    A::flush(a);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(1400, 900));
    ImGui::Begin("patch", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (g_viewShown) { A::draw(a); }
    ImGui::End();
    ImGui::Render();
    A::drain(a);
}

void frames(AppWindow& a, int n) {
    for (int i = 0; i < n; ++i) { frame(a); }
}

void mouseTo(AppWindow& a, float x, float y) {
    ImGui::GetIO().AddMousePosEvent(x, y);
    frame(a);
}

void press(AppWindow& a) {
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    frame(a);
}

void release(AppWindow& a) {
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    frame(a);
}

void click(AppWindow& a, float x, float y) {
    mouseTo(a, x, y);
    press(a);
    release(a);
}

void pressDelete(AppWindow& a) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Delete, true);
    frame(a);
    io.AddKeyEvent(ImGuiKey_Delete, false);
    frame(a);
}

// A world point on the canvas, on the screen.
ImVec2 screen(AppWindow& a, pg::Vec2 w) {
    const pg::Vec2 s = pg::worldToScreen(A::view(a), w);
    return ImVec2(s.x, s.y);
}

// A point on a node's title bar, clear of its close key.
ImVec2 header(AppWindow& a, pc::NodeId id) {
    const pc::Node* n = A::engine(a).find(id);
    return n == nullptr ? ImVec2(0, 0) : screen(a, pg::Vec2{n->x + 24.0f, n->y + 8.0f});
}

bool inStep(AppWindow& a) {
    return pc::graphCommandText(A::engine(a)) == pc::graphCommandText(A::draft(a));
}

bool hasWire(const pc::Graph& g, const pc::Wire& w) {
    for (const pc::Wire& x : g.wires()) {
        if (x == w) { return true; }
    }
    return false;
}

// The patch every scenario starts from: a radio, a channel beside it and a
// demodulator under the channel, nothing wired. Loaded as a document, as the
// config's patch is.
struct Starter {
    pc::NodeId radio = pc::kNoNode;
    pc::NodeId chan = pc::kNoNode;
    pc::NodeId demod = pc::kNoNode;
};

AppWindow* makeApp(Starter& s) {
    auto* a = new AppWindow();
    pc::Graph g;
    s.radio = g.addNode(pc::NodeKind::Radio, "Radio", pc::PortType::Iq, 40.0f, 40.0f);
    s.chan = g.addNode(pc::NodeKind::Channel, "Channel", pc::PortType::Iq, 420.0f, 40.0f);
    s.demod = g.addNode(pc::NodeKind::Demod, "Demod", pc::PortType::Iq, 420.0f, 320.0f);
    if (pc::Node* n = g.mutableNode(s.radio)) {
        n->device = pc::kGeneratorKey;
        n->freqHz = 145.0e6;
        n->rateHz = 2.4e6;
    }
    if (pc::Node* n = g.mutableNode(s.chan)) { n->freqHz = 144800000.25; }
    A::load(*a, g);
    ImGui::GetIO().AddMousePosEvent(1390.0f, 890.0f);
    frames(*a, 3);
    return a;
}

// The parts bin's keys, left to right: found by walking the pointer down the
// well's left edge to the last hovered item above the canvas (the row of
// fixed parts), then along that row.
std::vector<float> partKeys(AppWindow& a, float& rowY) {
    std::vector<float> xs;
    const float x0 = A::originX(a) + 12.0f;
    const float canvasTop = A::originY(a);
    ImGuiID last = 0;
    rowY = -1.0f;
    for (float y = 2.0f; y < canvasTop - 2.0f; y += 2.0f) {
        mouseTo(a, x0, y);
        frame(a);
        const ImGuiID h = GImGui->HoveredId;
        if (h != 0 && h != last) {
            last = h;
            rowY = y + 4.0f;
        }
    }
    if (rowY < 0.0f) { return xs; }
    last = 0;
    for (float x = A::originX(a) + 2.0f; x < A::originX(a) + 900.0f; x += 3.0f) {
        mouseTo(a, x, rowY);
        frame(a);
        const ImGuiID h = GImGui->HoveredId;
        if (h != 0 && h != last) {
            last = h;
            xs.push_back(x + 6.0f);
        }
    }
    mouseTo(a, 1390.0f, 890.0f);
    return xs;
}

// Not "near": windows.h defines near (and far, small, interface) as macros.
bool nearly(float a, float b) { return std::fabs(a - b) < 1e-3f; }

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch =
        std::filesystem::temp_directory_path() / ("foxsdr_patch_graph_draft_" + std::to_string(pid));
    std::filesystem::create_directories(scratch / "plugins");
    g_pluginDir = (scratch / "plugins").string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    A::installHooks();

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1400.0f, 900.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    // --- A: a part pressed in the parts bin -----------------------------------
    {
        std::printf("A: the Channel key in the parts bin\n");
        Starter s;
        AppWindow* a = makeApp(s);
        CHECK(inStep(*a));
        float rowY = 0.0f;
        const std::vector<float> keys = partKeys(*a, rowY);
        std::printf("   %zu part keys on the row at y=%.0f\n", keys.size(), rowY);
        CHECK(keys.size() >= 7);  // Radio, Channel, Demod, Spectrum, Speaker, Text out, Map
        if (keys.size() >= 2) {
            const std::size_t before = A::engine(*a).count(pc::NodeKind::Channel);
            click(*a, keys[1], rowY);
            frames(*a, 2);
            std::printf("   engine channels %zu -> %zu\n", before, A::engine(*a).count(pc::NodeKind::Channel));
            CHECK(A::engine(*a).count(pc::NodeKind::Channel) == before + 1);
            CHECK(inStep(*a));
        }
        delete a;
    }

    // --- B: a node dragged by its title bar -----------------------------------
    {
        std::printf("B: drag the radio 150,60 across five frames\n");
        Starter s;
        AppWindow* a = makeApp(s);
        const pc::Node start = *A::engine(*a).find(s.radio);
        const ImVec2 at = header(*a, s.radio);
        g_watch = s.radio;
        mouseTo(*a, at.x, at.y);
        press(*a);
        CHECK(A::dragging(*a) == s.radio);
        const float zoom = A::view(*a).zoom;
        for (int k = 1; k <= 5; ++k) {
            mouseTo(*a, at.x + 30.0f * static_cast<float>(k), at.y + 12.0f * static_cast<float>(k));
            const pc::Node* e = A::engine(*a).find(s.radio);
            const pc::Node* d = A::draft(*a).find(s.radio);
            CHECK(e != nullptr && d != nullptr);
            if (e == nullptr || d == nullptr) { break; }
            const float wantX = start.x + 30.0f * static_cast<float>(k) / zoom;
            const float wantY = start.y + 12.0f * static_cast<float>(k) / zoom;
            CHECK(nearly(e->x, wantX) && nearly(e->y, wantY));
            CHECK(e->x == d->x && e->y == d->y);
            // THE SAME FRAME: the sets were built from where it was dragged TO.
            CHECK(g_published);
            if (!(g_publishedX == e->x && g_publishedY == e->y)) {
                std::printf("   frame %d: published at %.2f,%.2f, dragged to %.2f,%.2f\n", k,
                            static_cast<double>(g_publishedX), static_cast<double>(g_publishedY),
                            static_cast<double>(e->x), static_cast<double>(e->y));
            }
            CHECK(g_publishedX == e->x && g_publishedY == e->y);
        }
        release(*a);
        frames(*a, 2);
        CHECK(A::dragging(*a) == pc::kNoNode);
        const pc::Node* e = A::engine(*a).find(s.radio);
        CHECK(e != nullptr && nearly(e->x, start.x + 150.0f / zoom) && nearly(e->y, start.y + 60.0f / zoom));
        CHECK(inStep(*a));
        g_watch = pc::kNoNode;
        delete a;
    }

    // --- C and D: a wire made by dragging port to port, then cut -------------
    {
        std::printf("C: wire the radio's output to the channel's input\n");
        Starter s;
        AppWindow* a = makeApp(s);
        const pc::Node radio = *A::engine(*a).find(s.radio);
        const pc::Node chan = *A::engine(*a).find(s.chan);
        const ImVec2 from = screen(*a, pg::outputPortPos(radio, 0));
        const ImVec2 to = screen(*a, pg::inputPortPos(chan, 0));
        mouseTo(*a, from.x, from.y);
        press(*a);
        mouseTo(*a, (from.x + to.x) * 0.5f, from.y + 20.0f);
        mouseTo(*a, to.x, to.y);
        release(*a);
        frames(*a, 2);
        const pc::Wire w{s.radio, 0, s.chan, 0};
        CHECK(hasWire(A::engine(*a), w));
        CHECK(inStep(*a));

        std::printf("D: click that wire and press Delete\n");
        const ImVec2 mid((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f);
        click(*a, mid.x, mid.y);
        pressDelete(*a);
        frames(*a, 2);
        CHECK(!hasWire(A::engine(*a), w));
        CHECK(A::engine(*a).find(s.radio) != nullptr && A::engine(*a).find(s.chan) != nullptr);
        CHECK(inStep(*a));
        delete a;
    }

    // --- E: a node clicked and Deleted ----------------------------------------
    {
        std::printf("E: click the channel's title bar and press Delete\n");
        Starter s;
        AppWindow* a = makeApp(s);
        const ImVec2 at = header(*a, s.chan);
        click(*a, at.x, at.y);
        pressDelete(*a);
        frames(*a, 2);
        CHECK(A::engine(*a).find(s.chan) == nullptr);
        CHECK(A::engine(*a).nodes().size() == 2u);
        // The MIDDLE id went: the others keep theirs (a document's numbering
        // would close the gap and hand the demodulator the channel's id).
        CHECK(A::engine(*a).find(s.radio) != nullptr && A::engine(*a).find(s.demod) != nullptr);
        CHECK(A::engine(*a).find(s.demod) != nullptr &&
              A::engine(*a).find(s.demod)->kind == pc::NodeKind::Demod);
        CHECK(inStep(*a));
        delete a;
    }

    // --- F: the engine's own changes show on the page ----------------------------
    {
        std::printf("F: ALL OFF, then a new patch loaded - the page follows each\n");
        Starter s;
        AppWindow* a = makeApp(s);
        CHECK(A::draft(*a).find(s.radio) != nullptr && A::draft(*a).find(s.radio)->on);
        CHECK(A::apply(*a, cascade::core::cmd::make(FOXAPI_OP_PATCH_ALL_OFF)).status == FOXAPI_OK);
        frame(*a);
        const pc::Node* d = A::draft(*a).find(s.radio);
        CHECK(d != nullptr && !d->on);
        CHECK(inStep(*a));

        pc::Graph other;
        const pc::NodeId solo = other.addNode(pc::NodeKind::Map, "Map", pc::PortType::Track, 10.0f, 10.0f);
        const cascade::core::cmd::QueuedCommand q = cascade::core::cmd::makeText(
            FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(other), FOXAPP_PATCH_GRAPH_DOCUMENT);
        CHECK(A::apply(*a, q).status == FOXAPI_OK);
        frame(*a);
        CHECK(A::draft(*a).nodes().size() == 1u && A::draft(*a).find(solo) != nullptr);
        CHECK(inStep(*a));
        delete a;
    }

    // --- G: a drag while the engine changes the graph ---------------------------
    {
        std::printf("G: drag the radio; mid-drag ALL OFF and a rate the engine learns\n");
        Starter s;
        AppWindow* a = makeApp(s);
        const pc::Node start = *A::engine(*a).find(s.radio);
        const float zoom = A::view(*a).zoom;
        const ImVec2 at = header(*a, s.radio);
        mouseTo(*a, at.x, at.y);
        press(*a);
        mouseTo(*a, at.x + 40.0f, at.y + 10.0f);
        // Between two frames of the drag, the engine changes two fields of the
        // dragged node: its switch (ALL OFF, a command) and its rate (as the
        // patch runtime sets a recording's own).
        CHECK(A::apply(*a, cascade::core::cmd::make(FOXAPI_OP_PATCH_ALL_OFF)).status == FOXAPI_OK);
        A::engineSetsRate(*a, s.radio, 1.024e6);
        for (int k = 2; k <= 4; ++k) {
            mouseTo(*a, at.x + 40.0f * static_cast<float>(k), at.y + 10.0f * static_cast<float>(k));
            const pc::Node* d = A::draft(*a).find(s.radio);
            const pc::Node* e = A::engine(*a).find(s.radio);
            CHECK(d != nullptr && e != nullptr);
            if (d == nullptr || e == nullptr) { break; }
            // The drag is where the pointer put it - never thrown back...
            CHECK(nearly(d->x, start.x + 40.0f * static_cast<float>(k) / zoom));
            CHECK(nearly(e->x, d->x) && e->y == d->y);
            // ...and the engine's changes are kept, not overwritten.
            CHECK(!e->on);
            CHECK(e->rateHz == 1.024e6);
        }
        CHECK(A::dragging(*a) == s.radio);
        release(*a);
        frames(*a, 2);
        const pc::Node* e = A::engine(*a).find(s.radio);
        CHECK(e != nullptr && nearly(e->x, start.x + 160.0f / zoom) && !e->on && e->rateHz == 1.024e6);
        CHECK(A::engine(*a).find(s.chan) != nullptr &&
              A::engine(*a).find(s.chan)->freqHz == 144800000.25);
        CHECK(inStep(*a));
        delete a;
    }

    // --- H: a draft the engine refuses -----------------------------------------
    {
        std::printf("H: a channel frequency that is not a number\n");
        Starter s;
        AppWindow* a = makeApp(s);
        const std::string before = pc::graphCommandText(A::engine(*a));
        if (pc::Node* n = A::mutableDraft(*a).mutableNode(s.chan)) {
            n->freqHz = std::numeric_limits<double>::infinity();
        }
        A::flush(*a);
        CHECK(pc::graphCommandText(A::engine(*a)) == before);
        // ...and the page shows the patch as it is, not the refused edit.
        CHECK(A::draft(*a).find(s.chan) != nullptr && A::draft(*a).find(s.chan)->freqHz == 144800000.25);
        frames(*a, 2);
        CHECK(pc::graphCommandText(A::engine(*a)) == before);
        CHECK(inStep(*a));
        delete a;
    }

    // --- I: a draft never copied from the engine is never sent -----------------
    //     Twice: as a new document (which the epoch alone would also catch)
    //     and as a plain replacement, where nothing but the draft's own
    //     "never copied" state stands between an empty draft and the graph.
    for (const std::int64_t flags : {std::int64_t{FOXAPP_PATCH_GRAPH_DOCUMENT}, std::int64_t{0}}) {
        std::printf("I: a patch put in the engine (flags %lld), then the commit before any frame\n",
                    static_cast<long long>(flags));
        auto* a = new AppWindow();
        pc::Graph g;
        (void)g.addNode(pc::NodeKind::Channel, "one", pc::PortType::Iq, 1.0f, 2.0f);
        (void)g.addNode(pc::NodeKind::Demod, "two", pc::PortType::Iq, 3.0f, 4.0f);
        const cascade::core::cmd::QueuedCommand q =
            cascade::core::cmd::makeText(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(g), flags);
        CHECK(A::apply(*a, q).status == FOXAPI_OK);
        A::flush(*a);
        CHECK(A::engine(*a).nodes().size() == 2u);
        CHECK(A::draft(*a).nodes().size() == 2u);  // it starts FROM the engine's graph
        delete a;
    }

    // --- K: a wire drag abandoned by the view going away ----------------------
    //     The canvas ends a wire drag on the mouse's RELEASE edge, which it
    //     sees only while it is drawn. Released while the view is hidden, the
    //     drag must not outlive the button - or the page would stop
    //     following the engine for good (every sync is skipped mid-drag).
    {
        std::printf("K: start a wire, hide the view, release; ALL OFF; show the view\n");
        Starter s;
        AppWindow* a = makeApp(s);
        const pc::Node radio = *A::engine(*a).find(s.radio);
        const ImVec2 from = screen(*a, pg::outputPortPos(radio, 0));
        mouseTo(*a, from.x, from.y);
        press(*a);
        mouseTo(*a, from.x + 60.0f, from.y + 40.0f);
        g_viewShown = false;
        frame(*a);
        release(*a);
        frames(*a, 2);
        CHECK(A::apply(*a, cascade::core::cmd::make(FOXAPI_OP_PATCH_ALL_OFF)).status == FOXAPI_OK);
        g_viewShown = true;
        frames(*a, 3);
        const pc::Node* e = A::engine(*a).find(s.radio);
        const pc::Node* d = A::draft(*a).find(s.radio);
        std::printf("   engine on=%d, page on=%d\n", e != nullptr && e->on ? 1 : 0, d != nullptr && d->on ? 1 : 0);
        CHECK(e != nullptr && !e->on);
        CHECK(d != nullptr && !d->on);
        CHECK(inStep(*a));
        // ...and the abandoned wire is GONE, not waiting: a drag from bare
        // canvas onto the channel's input must not connect the radio port
        // touched long before.
        const pc::Node chan = *A::engine(*a).find(s.chan);
        const ImVec2 port = screen(*a, pg::inputPortPos(chan, 0));
        mouseTo(*a, port.x - 60.0f, port.y + 30.0f);
        press(*a);
        mouseTo(*a, port.x, port.y);
        release(*a);
        frames(*a, 2);
        CHECK(!hasWire(A::engine(*a), pc::Wire{s.radio, 0, s.chan, 0}));
        delete a;
    }

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_patch_graph_draft_input");
}
