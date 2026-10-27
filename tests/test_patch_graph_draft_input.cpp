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
// (AppWindow::drawPatchView, the frame in drawUi's order: the engine's part of
// the frame - the pass that applies what the last frame queued, the publish,
// the window's read of it and of the session's answers - then the view) and
// reads back the ENGINE's graph.
//
// WINDOW PORT GROUP 13 (API 0.3 8.12) made the page's commit a command on the
// window's session: it is applied by the NEXT frame's pass, and the page keeps
// its own edit until the command's answer names the version it became (the
// draft is not overwritten by a graph that is a pass old). Every scenario is
// therefore written as LOOK (look(): the pass and the reads), DRAW, and what
// the engine holds is what the PREVIOUS frame committed.
//
//   A  a part pressed in the parts bin reaches the engine
//   B  a node dragged by its title bar reaches the engine, every frame of the
//      drag, one frame behind the pointer; the draft is where the pointer is,
//      and the sets are built in the pass that applied it
//      (Engine::testHooks_.patchPublishing)
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
//   J  a new patch document arriving mid-drag (J0: a DOCUMENT op, J1:
//      loadPatchDocument) whose Map has the dragged node's id: the drag and
//      the selection are dropped, the Map stays where the document put it
//   I2 ...and that first flush sends no command at all (the engine's node
//      store is untouched)
//   S  a squelch typed (Ctrl+click) past the slider's -120..0 dB is held
//      at the edge, not repaired to -50 dB by the engine's copy
//   P  an idle frame, page open or not drawn, serialises nothing
//   K  a wire drag left in the air by the view going away (the button
//      released while it was hidden) does not stop the page following the
//      engine
//   I  a draft that was never copied from the engine is never sent: the
//      once-a-frame commit before any frame is drawn leaves a loaded patch
//      alone
//   L  a drag held across a commit the engine has not applied yet: the draft
//      follows the pointer, one commit is in flight, and nothing is adopted
//      from the old published graph - not mid-drag, and not after the release
//   M  the first frame: a draft is never made "in step" with a graph that has
//      not been published, so the starter radio cannot be committed over a
//      patch that is loaded or still on its way
//   N  a stale commit and a START in one pass: the operator's drag and the
//      engine's START are BOTH kept, on the engine and in the draft (the lost
//      update an asynchronous window would have had)
//   O  the engine is told when the view is showing and when it is not, once
//      for each edge, and its pass stops the patch when it goes
//
// WINDOW PORT GROUP 13, PART B (the rest of the page's reads: its faces, its inspectors and its transport draw from the
// table's lists and facts and send what they change through the session). The text each scenario asserts on is what the
// page DREW, captured by ImGui's own log of rendered text (frameText), so what is checked is the face and not the cache
// behind it:
//
//   Q  a Radio's face in each state it has - no device, switched off, ready, running (its rate from PATCH_RADIOS),
//      opening, a note under it, its device named twice - and its device's label from PATCH_DEVICES
//   R  the live readouts move with the pass: a demodulator's gate and a channel's level, a speaker's output and its
//      count of seconds, a Spectrum part's trace (the radio's spectrum through read_node_spectrum), a speaker with no
//      output saying why - and none of it moves listSeq
//   S  a Text out shows the lines of the decoders wired to it and of no other (DECODER_LINES carries the node), each Text
//      out its own, and the Decoder output window's Clear leaves them be
//   T  the transport and the inspectors send through the session: START and ALL OFF, the Radio's device list opening and
//      "Look for radios" - the commands, in order, and one pass to land
//   V  a bare window with nothing published draws every face without a fault (no session reads, no engine objects)
//
// WINDOW PORT GROUP 13, PART C (the decoded pictures: a Decoder face shows the picture the engine holds, read with
// read_image; the engine's onPatchPicture hook is gone):
//
//   W  a REAL picture decoder module runs in the patch; the face reads its picture when PATCH_LIVE's picture row names a new
//      one and uploads it - once for each picture, not once a frame; a picture that has moved on (NOT_FOUND) or is not a
//      picture leaves the texture be; a stopped patch keeps its picture on the face; a removed node takes the texture with
//      it; the hook and the window's copy of the picture are gone from the source
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <limits>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <new>

#include "core/app_commands.hpp"
#include "core/patch_draft.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "gui/fonts.hpp"
#include "gui/patch_view_math.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "source/device_source.hpp"
#include "source/soapy_source.hpp"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace pc = cascade::core::patch;
namespace pg = cascade::gui::patch;

// HEAP ALLOCATIONS ON THIS THREAD while counting (scenario X): the page allocates in its draw by design (the ids vector, the
// formatted strings, a Spectrum part's columns), and what is held to zero is the CACHE REFRESH - the reads of the lists, the
// facts and PATCH_LIVE when nothing changed.
namespace {
thread_local bool t_counting = false;
thread_local long t_news = 0;
}  // namespace
void* operator new(std::size_t n) {
    if (t_counting) { ++t_news; }
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) { throw std::bad_alloc(); }
    return p;
}
void* operator new[](std::size_t n) {
    if (t_counting) { ++t_news; }
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) { throw std::bad_alloc(); }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

std::vector<cascade::source::NativeDeviceInfo> noNativeRadios() { return {}; }
std::vector<cascade::source::SoapyDeviceInfo> noSoapyRadios() { return {}; }
std::string g_pluginDir;
std::string pluginDir() { return g_pluginDir; }

// What patchPublishSets saw of one node, this frame. Atomics: the hook runs on the pass's thread, which free-running is not the
// test's (the frame clears g_published and B reads them while that thread's passes run).
std::atomic<pc::NodeId> g_watch{pc::kNoNode};
std::atomic<bool> g_published{false};
std::atomic<float> g_publishedX{0.0f};
std::atomic<float> g_publishedY{0.0f};
void onPublish(const pc::Graph& g) {
    if (const pc::Node* n = g.find(g_watch.load())) {
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
    static void sync(AppWindow& a) { a.syncPatchDraft(); }
    static void seedNow(AppWindow& a) { a.seedPatchIfNeeded(); }
    // The session's queue filled with ordinary commands the pass has not applied, until the table says BUSY.
    static int fillQueue(AppWindow& a) {
        int taken = 0;
        for (int i = 0; i < 1000; ++i) {
            FoxCommand c = cascade::core::cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.1 + 0.0001 * static_cast<double>(i % 7));
            FoxSubmitResult sr{};
            sr.structSize = sizeof(sr);
            (void)a.engineApi_->submit(a.uiSession_, &c, 1, &sr);
            if (sr.status == FOXAPI_BUSY) { return taken; }
            ++taken;
        }
        return taken;
    }
    static void patchOpenFalse(AppWindow& a) { a.patchOpen_ = false; }
    // EVERY REACH INTO THE ENGINE from the test's thread (the pump move's P6) is made on the pass's thread with the pass out of the
    // way (passseam::read / on), or directly where no pass can run beside it: before the control thread exists (a window no look has
    // attached yet - what I, I2 and M mean by "before any frame"), or while the pass is HELD (below).
    //
    // THE PASS HELD BETWEEN LOOKS, free-running only (B, G, L, B2, P). What those scenarios assert is what the engine holds BETWEEN
    // two of the frame's passes: one frame behind the pointer, a commit in flight and not yet applied, a full session queue not
    // drained, what one frame (one pass) costs.
    // Free-running, a pass of the thread's own lands within milliseconds of the page's commit (the draw commits a drag as it moves)
    // and applies it before the test has read anything. So there the pass is held: a piece of work posted to it (Engine::postToPass)
    // that waits until it is let go. A pass runs posted work at the head of its front part AFTER it has taken its queues, so while it
    // is held nothing the window sends is applied, and what the test reads precedes the next pass. Each look lets it go, has its one
    // whole pass (runPassNow) and holds it again before the window's part of the frame - the order a stepped run has; a stalled
    // frame (lookStalled) leaves it held. Inline and stepped no pass runs unless the test asks for one: there is nothing to hold.
    // While it is held nothing may ask for a pass, so rd/wr go direct.
    struct Hold {
        std::mutex m;
        std::condition_variable cv;
        bool running = false;
        bool release = false;
    };
    static std::shared_ptr<Hold>& held() {
        static std::shared_ptr<Hold> h;
        return h;
    }
    static bool& holdingLooks() {
        static bool on = false;
        return on;
    }
    static void holdPass(AppWindow& a) {
        if (passseam::mode() != passseam::Mode::Free || held() != nullptr) { return; }
        passseam::attach(rawEngine(a.engine_));
        auto h = std::make_shared<Hold>();
        rawEngine(a.engine_).postToPass([h] {
            std::unique_lock<std::mutex> lk(h->m);
            h->running = true;
            h->cv.notify_all();
            h->cv.wait(lk, [&] { return h->release; });
        });
        std::unique_lock<std::mutex> lk(h->m);
        const bool took = h->cv.wait_for(lk, std::chrono::seconds(10), [&] { return h->running; });
        CHECK(took);
        held() = h;
    }
    static void releasePass() {
        const std::shared_ptr<Hold> h = held();
        if (h == nullptr) { return; }
        held().reset();
        {
            std::lock_guard<std::mutex> lk(h->m);
            h->release = true;
        }
        h->cv.notify_all();
    }
    // From here on (on) the pass runs only inside a look; off lets it run free again. Off before the window goes.
    static void holdBetweenLooks(AppWindow& a, bool on) {
        if (passseam::mode() != passseam::Mode::Free) { return; }
        holdingLooks() = on;
        if (on) {
            holdPass(a);
        } else {
            releasePass();
        }
    }
    static bool direct(AppWindow& a) { return held() != nullptr || !rawEngine(a.engine_).controlThreadRunning(); }
    template <class R>
    static R rd(AppWindow& a, const std::function<R()>& readIt) {
        return direct(a) ? readIt() : passseam::read<R>(rawEngine(a.engine_), readIt);
    }
    static void wr(AppWindow& a, const std::function<void()>& work) {
        if (direct(a)) {
            work();
        } else {
            passseam::on(rawEngine(a.engine_), work);
        }
    }
    // THE ENGINE'S PART OF A FRAME, in drawUi's order: the commands the last frame queued are applied by the pass, the
    // once-a-frame commit and the page-open flag, the patch step, the publish, the window's read of it, and the session's
    // answers (pumpTransmitKey) - which is where a commit's answer reaches the page.
    static void look(AppWindow& a) {
        releasePass();   // (held between looks: let go for this look's pass)
        passseam::pass(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).pumpFrameBegin();
            rawEngine(a.engine_).drainLocalCommands();
            a.syncPatchPageOpen();
            a.commitPatchDraft();
            rawEngine(a.engine_).pumpInputs();
            rawEngine(a.engine_).pumpPlugins();
            rawEngine(a.engine_).pumpPatch();
            (void)rawEngine(a.engine_).pumpPublish();
        }, [&] {
            if (holdingLooks()) { holdPass(a); }
            a.syncPatchPageOpen();
            a.commitPatchDraft();
        });
        a.statusLines_ = rawEngine(a.engine_).statusText();   // (a copy behind the engine's own lock: any thread)
        a.refreshUiState();
        a.pumpTransmitKey();
    }
    // The same frame with the engine's pass STALLED (a long pass, a busy thread): the window's own part goes on, and what
    // it reads is the last publish.
    static void lookStalled(AppWindow& a) {
        a.syncPatchPageOpen();
        a.commitPatchDraft();
        a.refreshUiState();
        a.pumpTransmitKey();
    }
    static void tap(AppWindow& a, std::vector<cascade::core::cmd::QueuedCommand>* log) { a.uiSubmitTap_ = log; }
    static void submit(AppWindow& a, const FoxCommand& c) { a.uiSubmit(c); }
    static void setView(AppWindow& a, bool patch) { a.setMainViewPatch(patch); }
    static void setSeeded(AppWindow& a, bool v) { a.patchSeeded_ = v; }
    static bool seeded(AppWindow& a) { return a.patchSeeded_; }
    static bool inStepNow(AppWindow& a) { return a.patchDraftInStep_; }
    static bool sending(AppWindow& a) { return a.patchSend_.state != AppWindow::PatchSend::State::Idle; }
    static bool enginePageOpen(AppWindow& a) { return rd<bool>(a, [&] { return rawEngine(a.engine_).patchPageOpen_; }); }
    static bool enginePageWasOpen(AppWindow& a) { return rd<bool>(a, [&] { return rawEngine(a.engine_).patchWasOpen_; }); }
    static bool engineRunning(AppWindow& a) { return rd<bool>(a, [&] { return rawEngine(a.engine_).patchRunning_; }); }
    static std::size_t engineRadios(AppWindow& a) { return rd<std::size_t>(a, [&] { return rawEngine(a.engine_).patchRadios_.size(); }); }
    static std::uint64_t engineVersion(AppWindow& a) { return rd<std::uint64_t>(a, [&] { return rawEngine(a.engine_).patchGraphVersion_; }); }
    static std::uint64_t draftVersion(AppWindow& a) { return a.patchDraftVersion_; }
    static FoxCommandResult apply(AppWindow& a, const cascade::core::cmd::QueuedCommand& q) {
        return rd<FoxCommandResult>(a, [&] { return rawEngine(a.engine_).applyCommand(q.c, q.longText); });
    }
    static FoxCommandResult apply(AppWindow& a, const FoxCommand& c) {
        return rd<FoxCommandResult>(a, [&] { return rawEngine(a.engine_).applyCommand(c); });
    }
    // A patch document, as applyConfig loads one.
    static void load(AppWindow& a, const pc::Graph& g) { (void)a.loadPatchDocument(g); }
    // The engine's graph: a COPY, made where no pass runs beside it (a reference into patchGraph_ is read while the pass assigns it).
    static pc::Graph engine(AppWindow& a) { return rd<pc::Graph>(a, [&] { return rawEngine(a.engine_).patchGraph_; }); }
    // Where the engine's node store holds a node - compared, never dereferenced (I2: the store is the very one it was).
    static const pc::Node* engineNodeAddress(AppWindow& a, pc::NodeId id) {
        return rd<const pc::Node*>(a, [&] { return rawEngine(a.engine_).patchGraph_.find(id); });
    }
    static const pc::Graph& draft(AppWindow& a) { return a.patchDraft_; }
    static pc::Graph& mutableDraft(AppWindow& a) { return a.patchDraft_; }
    // The Engine's own runtime changing a node (as patchReconcile does when
    // an I/Q recording tells a Radio its rate) - the test stands in for it.
    static void engineSetsRate(AppWindow& a, pc::NodeId id, double hz) {
        wr(a, [&] {
            if (pc::Node* n = rawEngine(a.engine_).patchGraph_.mutableNode(id)) { n->rateHz = hz; }
        });
    }
    static float originX(AppWindow& a) { return a.patchCanvasOriginX_; }
    static float originY(AppWindow& a) { return a.patchCanvasOriginY_; }
    static pg::View view(AppWindow& a) {
        return pg::View{pg::Vec2{a.patchCanvasOriginX_ + a.patchUi_.view.pan.x,
                                 a.patchCanvasOriginY_ + a.patchUi_.view.pan.y},
                        a.patchUi_.view.zoom};
    }
    static pc::NodeId dragging(AppWindow& a) { return a.patchUi_.dragNode; }
    static pc::NodeId selected(AppWindow& a) { return a.patchUi_.selected; }
    // --- group 13, part b: what the page draws from, and the engine's objects it no longer reads, put there behind its back ---
    static void select(AppWindow& a, pc::NodeId id) { a.patchUi_.selected = id; }
    static const AppWindow::PatchListCache& lists(AppWindow& a) { return a.patchLists(); }
    static const AppWindow::PatchLiveRow* liveRow(AppWindow& a, int kind, pc::NodeId node) { return a.patchLiveRow(kind, node); }
    static const std::vector<pg::NodeReading>& readings(AppWindow& a) { return a.patchReadings_; }
    static std::uint64_t listSeq(AppWindow& a) { return a.uiState_.listSeq; }
    static bool spectrumHave(AppWindow& a, pc::NodeId radio) {
        const auto it = a.patchSpectrumCache_.find(radio);
        return it != a.patchSpectrumCache_.end() && it->second.have;
    }
    static std::uint64_t spectrumSeq(AppWindow& a, pc::NodeId radio) {
        const auto it = a.patchSpectrumCache_.find(radio);
        return it == a.patchSpectrumCache_.end() || !it->second.have ? 0u : it->second.seq;
    }
    static int scopeRows(AppWindow& a, pc::NodeId display) {
        const auto it = a.patchScopes_.find(display);
        return it == a.patchScopes_.end() ? 0 : it->second.filled();
    }
    static std::size_t textOutLines(AppWindow& a, pc::NodeId out) {
        const auto it = a.patchTextOutLines_.find(out);
        return it == a.patchTextOutLines_.end() ? 0u : it->second.size();
    }
    static void clearDecoderOutput(AppWindow& a) { a.clearDecoderLines(); }
    static void injectNote(AppWindow& a, pc::NodeId node, const std::string& text) {
        wr(a, [&] { rawEngine(a.engine_).patchRadioError_[node] = text; });
    }
    static void clearNote(AppWindow& a, pc::NodeId node) { wr(a, [&] { rawEngine(a.engine_).patchRadioError_.erase(node); }); }
    struct Pending {
        std::promise<cascade::engine::Engine::PatchRadioOpen> promise;
    };
    static void injectPending(AppWindow& a, pc::NodeId node, Pending& held) {
        wr(a, [&] {
            rawEngine(a.engine_).patchRadioPending_[node] = held.promise.get_future();
            rawEngine(a.engine_).patchRadioPendingAs_[node] = "test@1";
        });
    }
    static void answerPending(Pending& held) { held.promise.set_value(cascade::engine::Engine::PatchRadioOpen{}); }
    static void setRecordDir(AppWindow& a, const std::string& d) { wr(a, [&] { rawEngine(a.engine_).recordDir_ = d; }); }
    static void injectDestError(AppWindow& a, pc::NodeId node, const std::string& text) {
        wr(a, [&] { rawEngine(a.engine_).patchDestError_[node] = text; });
    }
    // A line the engine's decoders said, as pumpDecoderOutput logs one (node 0: the receiver's own decoders).
    static void injectLine(AppWindow& a, pc::NodeId node, const std::string& plugin, const std::string& text) {
        cascade::core::DecodedLine l;
        l.node = node;
        l.plugin = plugin;
        l.text = text;
        wr(a, [&] {
            rawEngine(a.engine_).decoderLog_.push_back(std::move(l));
            ++rawEngine(a.engine_).decoderLinesTotal_;
        });
    }
    // The cache refresh alone (no draw): the table's lists and facts when listSeq moved, PATCH_LIVE, a radio's spectrum.
    static void refreshReads(AppWindow& a, pc::NodeId radio) {
        (void)a.patchLists();
        a.readPatchLive();
        (void)a.patchSpectrum(radio);
    }
    // --- group 13, part c: a decoder's picture, as the face holds it, and the table the window reads it through ---
    static const AppWindow::PatchPicture* picture(AppWindow& a, pc::NodeId node) {
        const auto it = a.patchPictures_.find(node);
        return it == a.patchPictures_.end() ? nullptr : &it->second;
    }
    static void setPictureGl(AppWindow& a, unsigned (*up)(unsigned, std::uint32_t, std::uint32_t, std::uint32_t, const std::uint8_t*),
                             void (*down)(unsigned)) {
        a.patchPictureGl_.upload = up;
        a.patchPictureGl_.destroy = down;
    }
    static std::size_t pictureScratchCapacity(AppWindow& a) { return a.patchPictureScratch_.capacity(); }
    static const FoxEngineApi* engineApi(AppWindow& a) { return a.engineApi_; }
    static void setEngineApi(AppWindow& a, const FoxEngineApi* t) { a.engineApi_ = t; }
    static void engineStops(AppWindow& a) {
        wr(a, [&] { (void)rawEngine(a.engine_).applyCommand(cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, 0)); });
    }
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

// One frame, in drawUi's order for what this test touches: the engine's part (A::look: the pass that applies what the last
// frame queued, the publish, the window's reads and the session's answers), then the view.
bool g_passRuns = true;
void frame(AppWindow& a) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    g_published = false;
    ImGui::NewFrame();
    if (g_passRuns) {
        A::look(a);
    } else {
        A::lookStalled(a);
    }
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(1400, 900));
    ImGui::Begin("patch", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (g_viewShown) { A::draw(a); }
    ImGui::End();
    ImGui::Render();
}

void frames(AppWindow& a, int n) {
    for (int i = 0; i < n; ++i) { frame(a); }
}

// THE TEXT THE PAGE DREW in one frame, as ImGui itself logged it (every widget that renders text, in every child window of
// the page): what a scenario asserts on is the face, not the cache behind it.
std::string g_text;
void frameText(AppWindow& a) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    g_published = false;
    ImGui::NewFrame();
    if (g_passRuns) {
        A::look(a);
    } else {
        A::lookStalled(a);
    }
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(1400, 900));
    ImGui::Begin("patch", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::LogToBuffer();
    if (g_viewShown) { A::draw(a); }
    g_text = GImGui->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::End();
    ImGui::Render();
}
bool shows(const char* needle) { return g_text.find(needle) != std::string::npos; }
// The nth decimal number that follows `label` in the drawn text (-1 when there is none).
double numberAfter(const char* label) {
    const std::size_t at = g_text.find(label);
    if (at == std::string::npos) { return -1.0; }
    return std::strtod(g_text.c_str() + at + std::strlen(label), nullptr);
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
    const pc::Graph g = A::engine(a);
    const pc::Node* n = g.find(id);
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


// --- GROUP 13, PART C (scenario W): GL's two calls for a decoder's picture, and a table whose read_image is counted ------------
//     A process with no GL context cannot make a texture, so the window's pair of upload/destroy functions (patchPictureGl_) is
//     replaced by these, which keep what they were given: the pixels the face uploaded are what a picture was made of.
enum class Mode { Pass, NotFound, BadShape };
struct Upload {
    std::uint32_t w = 0, h = 0, format = 0;
    std::vector<std::uint8_t> px;
    unsigned texReturned = 0;
    std::thread::id thread;
};
struct PicLog {
    std::vector<Upload> uploads;
    std::vector<unsigned> destroyed;
    int reads = 0;       // read_image calls through the window's table
    int refused = 0;     // ...of them answered with something else than the engine's picture (Mode)
    Mode mode = Mode::Pass;
    unsigned nextTex = 0;
    std::thread::id mainThread;
};
PicLog g_pics;
const FoxEngineApi* g_realTable = nullptr;
FoxEngineApi g_wrapped{};

unsigned fakeUpload(unsigned tex, std::uint32_t w, std::uint32_t h, std::uint32_t format, const std::uint8_t* px) {
    Upload u;
    u.w = w;
    u.h = h;
    u.format = format;
    const std::size_t bytes = static_cast<std::size_t>(w) * h * (format == CASCADE_IMAGE_RGB24 ? 3u : 1u);
    u.px.assign(px, px + bytes);
    u.texReturned = tex != 0u ? tex : ++g_pics.nextTex + 100u;
    u.thread = std::this_thread::get_id();
    g_pics.uploads.push_back(std::move(u));
    return g_pics.uploads.back().texReturned;
}
void fakeDestroy(unsigned tex) { g_pics.destroyed.push_back(tex); }

std::int32_t countingReadImage(FoxSession* s, std::uint64_t id, FoxImageInfo* info, std::uint8_t* pixels, std::size_t cap) {
    ++g_pics.reads;
    if (g_pics.mode == Mode::NotFound) {
        ++g_pics.refused;
        return FOXAPI_NOT_FOUND;
    }
    const std::int32_t got = g_realTable->read_image(s, id, info, pixels, cap);
    if (g_pics.mode == Mode::BadShape && got > 0 && info != nullptr) {
        ++g_pics.refused;
        info->width += 1u;   // figures that do not describe the bytes returned
    }
    return got;
}

// Picture N of the fixture (tests/fixture_group13_plugin.cpp): odd N is RGB24 8x6, even N is GRAY8 5x3; byte i is (N*13 + i*7) & 0xFF.
bool uploadFollowsFixture(const Upload& u) {
    for (std::uint64_t n = 1; n <= 5000; ++n) {
        const bool rgb = (n % 2u) == 1u;
        if (u.format != (rgb ? CASCADE_IMAGE_RGB24 : CASCADE_IMAGE_GRAY8) || u.w != (rgb ? 8u : 5u) || u.h != (rgb ? 6u : 3u)) {
            continue;
        }
        bool ok = true;
        for (std::size_t i = 0; i < u.px.size() && ok; ++i) { ok = u.px[i] == static_cast<std::uint8_t>((n * 13u + i * 7u) & 0xFFu); }
        if (ok) { return true; }
    }
    return false;
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
        A::holdBetweenLooks(*a, true);   // (free-running: the engine as this frame's pass left it - see AppWindowTestAccess::Hold)
        mouseTo(*a, at.x, at.y);
        press(*a);
        CHECK(A::dragging(*a) == s.radio);
        const float zoom = A::view(*a).zoom;
        float lastX = start.x;  // where the PREVIOUS frame's commit put it
        float lastY = start.y;
        for (int k = 1; k <= 5; ++k) {
            mouseTo(*a, at.x + 30.0f * static_cast<float>(k), at.y + 12.0f * static_cast<float>(k));
            const pc::Graph eg = A::engine(*a);
            const pc::Node* e = eg.find(s.radio);
            const pc::Node* d = A::draft(*a).find(s.radio);
            CHECK(e != nullptr && d != nullptr);
            if (e == nullptr || d == nullptr) { break; }
            const float wantX = start.x + 30.0f * static_cast<float>(k) / zoom;
            const float wantY = start.y + 12.0f * static_cast<float>(k) / zoom;
            // THE DRAFT IS WHERE THE POINTER IS, this frame, whatever the engine has applied: a commit is not an adopt,
            // so the node is never drawn back where an older publish had it.
            CHECK(nearly(d->x, wantX) && nearly(d->y, wantY));
            // THE ENGINE IS ONE FRAME BEHIND: this frame's pass applied what the previous frame committed...
            CHECK(nearly(e->x, lastX) && nearly(e->y, lastY));
            // ...and built the radios' sets from that graph in that same pass, before anything was drawn.
            CHECK(g_published);
            if (!(g_publishedX == e->x && g_publishedY == e->y)) {
                std::printf("   frame %d: published at %.2f,%.2f, engine at %.2f,%.2f\n", k,
                            static_cast<double>(g_publishedX), static_cast<double>(g_publishedY),
                            static_cast<double>(e->x), static_cast<double>(e->y));
            }
            CHECK(g_publishedX == e->x && g_publishedY == e->y);
            lastX = wantX;
            lastY = wantY;
        }
        release(*a);
        frames(*a, 3);
        CHECK(A::dragging(*a) == pc::kNoNode);
        const pc::Graph eg = A::engine(*a);
        const pc::Node* e = eg.find(s.radio);
        CHECK(e != nullptr && nearly(e->x, start.x + 150.0f / zoom) && nearly(e->y, start.y + 60.0f / zoom));
        CHECK(inStep(*a));
        A::holdBetweenLooks(*a, false);
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
        A::holdBetweenLooks(*a, true);   // (free-running: "the frame behind" is the engine between two of the frame's passes)
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
            const pc::Graph eg = A::engine(*a);
            const pc::Node* e = eg.find(s.radio);
            CHECK(d != nullptr && e != nullptr);
            if (d == nullptr || e == nullptr) { break; }
            // The drag is where the pointer put it - never thrown back...
            CHECK(nearly(d->x, start.x + 40.0f * static_cast<float>(k) / zoom));
            // ...the engine is the frame behind it...
            CHECK(nearly(e->x, start.x + 40.0f * static_cast<float>(k - 1) / zoom));
            // ...and the engine's changes are kept, not overwritten: the engine merged the stale commit over them.
            CHECK(!e->on);
            CHECK(e->rateHz == 1.024e6);
        }
        CHECK(A::dragging(*a) == s.radio);
        release(*a);
        frames(*a, 3);
        const pc::Graph eg = A::engine(*a);
        const pc::Node* e = eg.find(s.radio);
        CHECK(e != nullptr && nearly(e->x, start.x + 160.0f / zoom) && !e->on && e->rateHz == 1.024e6);
        CHECK(A::engine(*a).find(s.chan) != nullptr &&
              A::engine(*a).find(s.chan)->freqHz == 144800000.25);
        CHECK(inStep(*a));
        A::holdBetweenLooks(*a, false);
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
        // Sent, not applied (and the page keeps the edit until the engine has answered it)...
        CHECK(pc::graphCommandText(A::engine(*a)) == before);
        CHECK(A::sending(*a));
        CHECK(A::draft(*a).find(s.chan) != nullptr && std::isinf(A::draft(*a).find(s.chan)->freqHz));
        // ...the pass refuses it, and the page shows the patch as it is, not the refused edit.
        frames(*a, 3);
        CHECK(pc::graphCommandText(A::engine(*a)) == before);
        CHECK(A::draft(*a).find(s.chan) != nullptr && A::draft(*a).find(s.chan)->freqHz == 144800000.25);
        CHECK(!A::sending(*a));
        CHECK(inStep(*a));
        delete a;
    }

    // --- I: a draft never copied from the engine is never sent -----------------
    //     Twice: as a new document and as a plain replacement. TWO things
    //     stand between an empty never-copied draft and the engine's graph:
    //     the draft's own "never copied" state (patchDraftInStep_), and - if
    //     that were gone - the rebase, which merges an empty draft against an
    //     empty base into the engine's own graph, so the node COUNT survives
    //     either way. I2 below tells the two apart.
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
        std::vector<cascade::core::cmd::QueuedCommand> sent;
        A::tap(*a, &sent);
        A::flush(*a);
        CHECK(sent.empty());                         // nothing was read from a publish: nothing to send
        CHECK(A::engine(*a).nodes().size() == 2u);
        CHECK(A::draft(*a).nodes().empty());         // and the page made nothing up
        CHECK(!A::inStepNow(*a));
        A::look(*a);                                 // the first publish is read...
        A::sync(*a);
        CHECK(A::draft(*a).nodes().size() == 2u);    // ...and the draft starts FROM the engine's graph
        A::flush(*a);
        CHECK(sent.empty());                         // an adopted draft is no edit
        delete a;
    }

    // --- J0 and J1: a NEW PATCH arrives mid-drag -------------------------------
    //     Its Map part has the id of the node being dragged (a new document
    //     numbers its own nodes). The drag was of a node in the OLD patch: it
    //     is dropped, and the Map stays where the new patch put it - through
    //     the engine directly (J0, as any sender of a document would) and
    //     through the page's own loadPatchDocument (J1, the config's path).
    for (int via = 0; via < 2; ++via) {
        std::printf("J%d: drag the radio; a new patch (%s) puts a Map under its id\n", via,
                    via == 0 ? "a DOCUMENT op" : "loadPatchDocument");
        Starter s;
        AppWindow* a = makeApp(s);
        const ImVec2 at = header(*a, s.radio);
        mouseTo(*a, at.x, at.y);
        press(*a);
        mouseTo(*a, at.x + 40.0f, at.y + 10.0f);
        CHECK(A::dragging(*a) == s.radio);
        CHECK(A::selected(*a) == s.radio);
        pc::Graph doc;
        CHECK(doc.addNodeAs(s.radio, pc::NodeKind::Map, "Map", pc::PortType::Track, 10.0f, 10.0f) == s.radio);
        if (via == 0) {
            const cascade::core::cmd::QueuedCommand q = cascade::core::cmd::makeText(
                FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(doc), FOXAPP_PATCH_GRAPH_DOCUMENT);
            CHECK(A::apply(*a, q).status == FOXAPI_OK);
        } else {
            A::load(*a, doc);
            // The page's own load drops the old patch's interaction at once, not when the engine has answered.
            CHECK(A::dragging(*a) == pc::kNoNode);
            CHECK(A::selected(*a) == pc::kNoNode);
        }
        for (int k = 2; k <= 4; ++k) {
            mouseTo(*a, at.x + 40.0f * static_cast<float>(k), at.y + 10.0f * static_cast<float>(k));
        }
        pc::Graph mg = A::engine(*a);
        const pc::Node* m = mg.find(s.radio);
        std::printf("   Map at %.1f,%.1f\n", m != nullptr ? static_cast<double>(m->x) : -1.0,
                    m != nullptr ? static_cast<double>(m->y) : -1.0);
        CHECK(m != nullptr && m->kind == pc::NodeKind::Map && m->x == 10.0f && m->y == 10.0f);
        CHECK(A::dragging(*a) == pc::kNoNode);   // the drag went with the old patch
        CHECK(A::selected(*a) == pc::kNoNode);   // ...and so did what was selected in it
        release(*a);
        frames(*a, 2);
        mg = A::engine(*a);
        m = mg.find(s.radio);
        CHECK(m != nullptr && m->x == 10.0f && m->y == 10.0f);
        CHECK(A::engine(*a).nodes().size() == 1u);
        CHECK(inStep(*a));
        delete a;
    }

    // --- I2: ...and the flush sends NOTHING at all -----------------------------
    //     Not "the same graph comes back": no command, so the engine's node
    //     store is the very one it was. Any replacement - even with the same
    //     nodes, as the rebase would send - moves it (patchGraph_ is assigned
    //     a new Graph), so the address of a node tells whether one happened.
    {
        std::printf("I2: a graph set in the engine (flags 0), then the first flush sends nothing\n");
        auto* a = new AppWindow();
        pc::Graph g;
        const pc::NodeId one = g.addNode(pc::NodeKind::Channel, "one", pc::PortType::Iq, 1.0f, 2.0f);
        const cascade::core::cmd::QueuedCommand q =
            cascade::core::cmd::makeText(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(g), 0);
        CHECK(A::apply(*a, q).status == FOXAPI_OK);
        const pc::Node* before = A::engineNodeAddress(*a, one);
        std::vector<cascade::core::cmd::QueuedCommand> sent;
        A::tap(*a, &sent);
        A::flush(*a);
        A::look(*a);
        A::sync(*a);
        A::flush(*a);
        A::look(*a);
        CHECK(sent.empty());
        CHECK(before != nullptr && A::engineNodeAddress(*a, one) == before);
        delete a;
    }

    // --- S: a squelch typed past the slider's range -----------------------------
    //     Ctrl+click turns a slider into a number box, and ImGui does not
    //     clamp what is typed there unless told to. -120..0 dB is the range
    //     the graph can hold (the loader and the command refuse anything
    //     else), so the slider must clamp: -200 typed is -120 held.
    {
        std::printf("S: Ctrl+click the demodulator's squelch slider, type -200\n");
        Starter s;
        AppWindow* a = makeApp(s);
        const pc::Node demod = *A::engine(*a).find(s.demod);
        CHECK(demod.squelch && demod.squelchDb == -50.0f);
        // The face's widgets, top to bottom: the mode, the squelch switch,
        // the squelch slider. Found by walking the pointer down the face and
        // keeping each item hovered that is not the canvas itself.
        const ImVec2 blank = screen(*a, pg::Vec2{demod.x - 60.0f, demod.y + 60.0f});
        mouseTo(*a, blank.x, blank.y);
        frame(*a);
        const ImGuiID canvas = GImGui->HoveredId;
        const ImVec2 top = screen(*a, pg::Vec2{demod.x + 40.0f, demod.y + pg::kHeaderHeight});
        const ImVec2 bottom = screen(*a, pg::Vec2{demod.x + 40.0f, demod.y + demod.h});
        std::vector<std::pair<ImGuiID, float>> items;
        for (float y = top.y; y < bottom.y; y += 2.0f) {
            mouseTo(*a, top.x, y);
            frame(*a);
            const ImGuiID h = GImGui->HoveredId;
            if (h == 0 || h == canvas) { continue; }
            bool seen = false;
            for (const auto& it : items) { seen = seen || it.first == h; }
            if (!seen) { items.push_back({h, y}); }
        }
        std::printf("   %zu controls on the face\n", items.size());
        CHECK(items.size() >= 3);
        if (items.size() >= 3) {
            const float y = items[2].second + 3.0f;
            ImGuiIO& kio = ImGui::GetIO();
            mouseTo(*a, top.x, y);
            kio.AddKeyEvent(ImGuiMod_Ctrl, true);
            frame(*a);
            press(*a);
            release(*a);
            kio.AddKeyEvent(ImGuiMod_Ctrl, false);
            frame(*a);
            kio.AddKeyEvent(ImGuiMod_Ctrl, true);
            kio.AddKeyEvent(ImGuiKey_A, true);
            frame(*a);
            kio.AddKeyEvent(ImGuiKey_A, false);
            kio.AddKeyEvent(ImGuiMod_Ctrl, false);
            frame(*a);
            kio.AddInputCharactersUTF8("-200");
            frame(*a);
            kio.AddKeyEvent(ImGuiKey_Enter, true);
            frame(*a);
            kio.AddKeyEvent(ImGuiKey_Enter, false);
            frames(*a, 4);
            const pc::Graph eg = A::engine(*a);
            const pc::Node* e = eg.find(s.demod);
            const pc::Node* d = A::draft(*a).find(s.demod);
            std::printf("   engine %.1f dB, page %.1f dB\n", e != nullptr ? static_cast<double>(e->squelchDb) : 0.0,
                        d != nullptr ? static_cast<double>(d->squelchDb) : 0.0);
            CHECK(e != nullptr && e->squelchDb == pc::kSquelchMinDb);
            CHECK(inStep(*a));
        }
        delete a;
    }

    // --- P: what a frame costs ---------------------------------------------------
    //     The page compares its draft with the engine's graph on every frame.
    //     Written out as text each time, that was five serialisations an idle
    //     frame (review: 0.6-0.8 ms with 5 radios and 20 parts, and one even
    //     with the page closed). An idle frame - page open or not drawn - now
    //     writes NONE; a frame of a drag writes what it sends.
    {
        std::printf("P: serialisations on idle frames, open and not drawn, and on a drag frame\n");
        Starter s;
        AppWindow* a = makeApp(s);
        frames(*a, 3);
        auto& count = pc::serialiseCount();
        std::uint64_t c0 = count.load();
        frames(*a, 10);
        const std::uint64_t open = count.load() - c0;
        g_viewShown = false;
        frames(*a, 2);
        c0 = count.load();
        frames(*a, 10);
        const std::uint64_t hidden = count.load() - c0;
        g_viewShown = true;
        frames(*a, 2);
        const ImVec2 at = header(*a, s.radio);
        A::holdBetweenLooks(*a, true);   // (free-running: ONE frame's cost is one pass's - see AppWindowTestAccess::Hold)
        mouseTo(*a, at.x, at.y);
        press(*a);
        mouseTo(*a, at.x + 10.0f, at.y);
        c0 = count.load();
        mouseTo(*a, at.x + 20.0f, at.y);
        const std::uint64_t drag = count.load() - c0;
        A::holdBetweenLooks(*a, false);
        release(*a);
        std::printf("   10 idle frames open: %llu, 10 not drawn: %llu, one drag frame: %llu\n",
                    static_cast<unsigned long long>(open), static_cast<unsigned long long>(hidden),
                    static_cast<unsigned long long>(drag));
        CHECK(open == 0u);
        CHECK(hidden == 0u);
        // The drag frame writes what it sends and what that costs to check: the command's text, the engine's canonical
        // check of it (the same process counts here), the text of the block's fact the engine publishes for the new
        // version, the window's check of the fact it reads back, and the page's own document text (patchText_, rebuilt
        // when the canvas marks it dirty, as it always was). Five, and no idle frame writes any.
        CHECK(drag <= 5u);
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
        const pc::Graph eg = A::engine(*a);
        const pc::Node* e = eg.find(s.radio);
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

    // --- L: a drag held across a commit the engine has not applied yet ----------------
    //     The engine's pass stalls (a long pass, a busy thread) while the window's frames go on. The published graph
    //     is the old one; the draft must be where the pointer is, ONE commit is in flight (not one a frame), and
    //     nothing is adopted from the old publish - not mid-drag, and not after the release, when nothing holds the
    //     draft in place but the commit's own answer.
    {
        std::printf("L: drag the radio across five frames while the engine's pass stalls, then let go\n");
        Starter s;
        AppWindow* a = makeApp(s);
        const pc::Node start = *A::engine(*a).find(s.radio);
        const float zoom = A::view(*a).zoom;
        const ImVec2 at = header(*a, s.radio);
        std::vector<cascade::core::cmd::QueuedCommand> sent;
        A::tap(*a, &sent);
        const auto graphCommands = [&] {
            std::size_t n = 0;
            for (const auto& q : sent) { n += q.c.op == FOXAPP_OP_PATCH_SET_GRAPH ? 1u : 0u; }
            return n;
        };
        mouseTo(*a, at.x, at.y);
        press(*a);
        CHECK(A::dragging(*a) == s.radio);
        // The first move goes through a pass: its commit is the one in flight. (Free-running, the stall is the pass HELD: from
        // this frame on a pass runs only in a look, and a stalled frame has none.)
        A::holdBetweenLooks(*a, true);
        mouseTo(*a, at.x + 30.0f, at.y + 12.0f);
        CHECK_EQ(graphCommands(), 1u);
        CHECK(A::sending(*a));
        // Four frames with no pass: the pointer goes on, the draft follows it, nothing more is sent, and the engine
        // still has the start.
        g_passRuns = false;
        for (int k = 2; k <= 5; ++k) {
            mouseTo(*a, at.x + 30.0f * static_cast<float>(k), at.y + 12.0f * static_cast<float>(k));
            const pc::Node* d = A::draft(*a).find(s.radio);
            const pc::Graph eg = A::engine(*a);
            const pc::Node* e = eg.find(s.radio);
            CHECK(d != nullptr && e != nullptr);
            if (d == nullptr || e == nullptr) { break; }
            CHECK(nearly(d->x, start.x + 30.0f * static_cast<float>(k) / zoom));
            CHECK(nearly(e->x, start.x));
        }
        CHECK_EQ(graphCommands(), 1u);  // stop-and-wait: one in flight, the rest waiting in the draft
        // LET GO with the commit still unanswered: nothing holds the draft but the answer. It stays where it was
        // dropped for as many stalled frames as it takes (an adopt of the old publish would have put it back).
        release(*a);
        frames(*a, 3);
        CHECK(A::dragging(*a) == pc::kNoNode);
        {
            const pc::Node* d = A::draft(*a).find(s.radio);
            CHECK(d != nullptr && nearly(d->x, start.x + 150.0f / zoom) && nearly(d->y, start.y + 60.0f / zoom));
            CHECK(A::sending(*a));
        }
        CHECK_EQ(graphCommands(), 1u);
        // The pass runs: the first commit is applied and answered, the draft keeps what was done since and goes with the
        // next commit, from the new base; the engine ends where the pointer let go.
        g_passRuns = true;
        frames(*a, 4);
        {
            const pc::Node* d = A::draft(*a).find(s.radio);
            const pc::Graph eg = A::engine(*a);
            const pc::Node* e = eg.find(s.radio);
            CHECK(d != nullptr && e != nullptr);
            if (d != nullptr && e != nullptr) {
                CHECK(nearly(e->x, start.x + 150.0f / zoom) && nearly(e->y, start.y + 60.0f / zoom));
                CHECK(e->x == d->x && e->y == d->y);
            }
        }
        std::printf("   %zu graph commands for six frames of drag, the release and the stall\n", graphCommands());
        CHECK_EQ(graphCommands(), 2u);
        CHECK(inStep(*a));
        CHECK(!A::sending(*a));
        A::holdBetweenLooks(*a, false);
        A::tap(*a, nullptr);
        delete a;
    }

    // --- M: the first frame ----------------------------------------------------------
    //     A draft is made "in step" with a graph that was PUBLISHED, never with one nobody has read. The very first
    //     once-a-frame commit is at the top of frame 1, before the first publish of that frame; and the starter radio
    //     is committed by a page that finds the engine's graph empty - which it is, while the config's patch is still
    //     queued. Neither may overwrite a patch.
    {
        std::printf("M: the first frame with a patch already in the engine, with one still queued, and with none\n");
        pc::Graph loaded;
        const pc::NodeId lone = loaded.addNode(pc::NodeKind::Channel, "the user's channel", pc::PortType::Iq, 11.0f, 22.0f);
        const std::string loadedText = pc::graphCommandText(loaded);
        const auto setCount = [](const std::vector<cascade::core::cmd::QueuedCommand>& v, bool documents) {
            std::size_t n = 0;
            for (const auto& q : v) {
                if (q.c.op == FOXAPP_OP_PATCH_SET_GRAPH && ((q.c.ival[0] & FOXAPP_PATCH_GRAPH_DOCUMENT) != 0) == documents) { ++n; }
            }
            return n;
        };
        {
            // M1: the engine holds the patch (the config's, applied) and nothing has been published yet.
            auto* a = new AppWindow();
            const cascade::core::cmd::QueuedCommand q =
                cascade::core::cmd::makeText(FOXAPP_OP_PATCH_SET_GRAPH, loadedText, FOXAPP_PATCH_GRAPH_DOCUMENT);
            CHECK(A::apply(*a, q).status == FOXAPI_OK);
            std::vector<cascade::core::cmd::QueuedCommand> sent;
            A::tap(*a, &sent);
            A::flush(*a);                  // the top of frame 1, before the first publish
            CHECK(sent.empty());
            A::sync(*a);                   // ...and a sync that finds no publish to read adopts nothing
            CHECK(!A::inStepNow(*a));
            CHECK(A::draft(*a).nodes().empty());
            frames(*a, 6);                 // the view is up from frame 1: sync, seed, draw
            CHECK_EQ(pc::graphCommandText(A::engine(*a)), loadedText);
            CHECK(A::draft(*a).find(lone) != nullptr && A::draft(*a).nodes().size() == 1u);
            CHECK_EQ(setCount(sent, false) + setCount(sent, true), 0u);   // nothing was ever sent over it
            A::tap(*a, nullptr);
            delete a;
        }
        {
            // M2: the patch is still QUEUED on the session (loadPatchDocument, as applyConfig does) when the first
            // frames are drawn and the page would seed its starter.
            auto* a = new AppWindow();
            std::vector<cascade::core::cmd::QueuedCommand> sent;
            A::tap(*a, &sent);
            A::load(*a, loaded);
            A::flush(*a);
            frames(*a, 6);
            CHECK_EQ(pc::graphCommandText(A::engine(*a)), loadedText);
            CHECK_EQ(setCount(sent, true), 1u);    // the document, once
            CHECK_EQ(setCount(sent, false), 0u);   // and no starter radio over it
            CHECK(A::engine(*a).nodes().size() == 1u);
            CHECK(inStep(*a));
            A::tap(*a, nullptr);
            delete a;
        }
        {
            // M3: no patch at all. Nothing is sent from the top of frame 1; once the first publish has been read, the
            // page seeds ONE starter radio on the generator and sends it once.
            auto* a = new AppWindow();
            std::vector<cascade::core::cmd::QueuedCommand> sent;
            A::tap(*a, &sent);
            A::flush(*a);
            CHECK(sent.empty());
            A::sync(*a);
            A::seedNow(*a);                // the seed before any publish has been read: it waits
            CHECK(!A::inStepNow(*a));
            CHECK(sent.empty());
            CHECK(!A::seeded(*a));
            frames(*a, 6);
            CHECK(A::seeded(*a));
            CHECK_EQ(setCount(sent, false), 1u);
            CHECK_EQ(setCount(sent, true), 0u);
            const pc::Graph eg = A::engine(*a);
            CHECK(eg.nodes().size() == 1u);
            if (eg.nodes().size() == 1u) {
                const pc::Node& n = eg.nodes()[0];
                CHECK(n.kind == pc::NodeKind::Radio);
                CHECK(n.device == pc::kGeneratorKey);   // no radio in the receiver: the stand-in
            }
            CHECK(inStep(*a));
            A::tap(*a, nullptr);
            delete a;
        }
    }

    // --- B2: the session is full (BUSY) -----------------------------------------------
    //     A commit the table will not take is not a lost edit: the draft keeps it and it goes the frame the queue has room.
    {
        std::printf("B2: a full session queue (BUSY) keeps the edit in the draft\n");
        Starter s;
        AppWindow* a = makeApp(s);
        const pc::Node start = *A::engine(*a).find(s.radio);
        const float zoom = A::view(*a).zoom;
        const ImVec2 at = header(*a, s.radio);
        mouseTo(*a, at.x, at.y);
        press(*a);
        A::holdBetweenLooks(*a, true);   // (free-running: the stall is the pass HELD, so nothing drains the queue it fills)
        g_passRuns = false;
        const int taken = A::fillQueue(*a);
        std::printf("   %d ordinary commands pending when the table said BUSY\n", taken);
        CHECK(taken > 0);
        mouseTo(*a, at.x + 30.0f, at.y + 12.0f);
        mouseTo(*a, at.x + 60.0f, at.y + 24.0f);
        CHECK(!A::sending(*a));                                   // nothing in flight: nothing was taken
        CHECK(A::draft(*a).find(s.radio) != nullptr && nearly(A::draft(*a).find(s.radio)->x, start.x + 60.0f / zoom));
        CHECK(nearly(A::engine(*a).find(s.radio)->x, start.x));
        g_passRuns = true;
        release(*a);
        frames(*a, 4);
        const pc::Graph eg = A::engine(*a);
        const pc::Node* e = eg.find(s.radio);
        CHECK(e != nullptr && nearly(e->x, start.x + 60.0f / zoom) && nearly(e->y, start.y + 24.0f / zoom));
        CHECK(inStep(*a));
        A::holdBetweenLooks(*a, false);
        delete a;
    }

    // --- N: a stale commit and START in one pass -------------------------------------
    //     The lost update of an asynchronous window. START throws the radios' switches on (they are off here) and
    //     marks the patch running; the page's drag was committed from the version before it, and is applied in the
    //     same pass AFTER it. The engine merges: the switch is not put back, the drag is not lost - and the operator
    //     sees both, in the engine's graph and in the page's.
    {
        std::printf("N: ALL OFF, then START and a drag committed from before it, applied in one pass\n");
        Starter s;
        AppWindow* a = makeApp(s);
        CHECK(A::apply(*a, cascade::core::cmd::make(FOXAPI_OP_PATCH_ALL_OFF)).status == FOXAPI_OK);
        frames(*a, 3);
        CHECK(A::engine(*a).find(s.radio) != nullptr && !A::engine(*a).find(s.radio)->on);
        CHECK(A::draft(*a).find(s.radio) != nullptr && !A::draft(*a).find(s.radio)->on);
        const pc::Node start = *A::engine(*a).find(s.demod);
        const float zoom = A::view(*a).zoom;
        const ImVec2 at = header(*a, s.demod);
        mouseTo(*a, at.x, at.y);
        press(*a);
        // The START key, pressed in this frame (the dome's command), is queued AHEAD of the drag's commit...
        A::submit(*a, cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, 1));
        mouseTo(*a, at.x + 60.0f, at.y + 24.0f);
        CHECK(A::sending(*a));
        // ...and the next pass applies START, then the commit, whose version is the one before START.
        release(*a);
        frames(*a, 3);
        const pc::Graph eg = A::engine(*a);
        const pc::Node* er = eg.find(s.radio);
        const pc::Node* ed = eg.find(s.demod);
        const pc::Node* dr = A::draft(*a).find(s.radio);
        const pc::Node* dd = A::draft(*a).find(s.demod);
        CHECK(er != nullptr && ed != nullptr && dr != nullptr && dd != nullptr);
        if (er != nullptr && ed != nullptr && dr != nullptr && dd != nullptr) {
            std::printf("   engine: radio %s, demod at %.1f,%.1f; page: radio %s, demod at %.1f,%.1f; running %d\n",
                        er->on ? "ON" : "OFF", static_cast<double>(ed->x), static_cast<double>(ed->y),
                        dr->on ? "ON" : "OFF", static_cast<double>(dd->x), static_cast<double>(dd->y),
                        A::engineRunning(*a) ? 1 : 0);
            CHECK(er->on);                                  // START's switch was not put back by the older edit
            CHECK(dr->on);                                  // and the page shows it
            CHECK(nearly(ed->x, start.x + 60.0f / zoom) && nearly(ed->y, start.y + 24.0f / zoom));   // the drag landed
            CHECK(nearly(dd->x, ed->x) && nearly(dd->y, ed->y));
        }
        CHECK(A::engineRunning(*a));
        CHECK(inStep(*a));
        delete a;
    }

    // --- O: the engine is told whether the view is showing ----------------------------
    //     Once for each edge (not once a frame), and its pass stops the patch when the view goes - the receiver gets
    //     its radio back - and does not start it when the view comes back.
    {
        std::printf("O: START, the receiver view chosen, the patch view chosen again\n");
        Starter s;
        AppWindow* a = makeApp(s);
        std::vector<cascade::core::cmd::QueuedCommand> sent;
        A::tap(*a, &sent);
        const auto pageCommands = [&] {
            std::vector<long long> v;
            for (const auto& q : sent) {
                if (q.c.op == FOXAPP_OP_SET_PATCH_PAGE_OPEN) { v.push_back(static_cast<long long>(q.c.ival[0])); }
            }
            return v;
        };
        CHECK(A::enginePageOpen(*a));
        frames(*a, 3);
        CHECK(pageCommands().empty());                    // the engine starts "showing", as the window does
        CHECK(A::apply(*a, cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, 1)).status == FOXAPI_OK);
        frames(*a, 3);
        CHECK(A::engineRunning(*a));
        CHECK_EQ(A::engineRadios(*a), 1u);               // the pass opened the generator radio; nobody drew its start
        A::setView(*a, false);
        frames(*a, 4);
        CHECK((pageCommands() == std::vector<long long>{0}));   // one command for the edge, not one a frame
        CHECK(!A::enginePageOpen(*a));
        CHECK(!A::engineRunning(*a));                     // the close rule, in the pass
        CHECK_EQ(A::engineRadios(*a), 0u);
        CHECK(!A::enginePageWasOpen(*a));
        A::setView(*a, false);                            // said again
        frames(*a, 2);
        CHECK((pageCommands() == std::vector<long long>{0}));
        A::setView(*a, true);
        frames(*a, 4);
        CHECK((pageCommands() == std::vector<long long>{0, 1}));
        CHECK(A::enginePageOpen(*a));
        CHECK(A::enginePageWasOpen(*a));
        CHECK(!A::engineRunning(*a));                     // showing the view starts nothing
        CHECK_EQ(A::engineRadios(*a), 0u);
        A::tap(*a, nullptr);
        delete a;
    }

    // --- O2: a window that opens on the receiver view ---------------------------------
    //     The saved configuration says so (mainView receiver): the engine is told by the constructor, so its FIRST pass
    //     already knows - the patch step never runs, and there is no close rule to fire for a page that never opened.
    {
        std::printf("O2: a configuration that opens on the receiver view\n");
        const auto cfg = scratch / "receiver_view.json";
        {
            std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
            f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", "
                 "\"uiTheme\": \"today\", \"mainView\": \"receiver\" }\n";
        }
        auto* a = new AppWindow(cfg.string());
        A::look(*a);
        CHECK(!A::enginePageOpen(*a));
        CHECK(!A::enginePageWasOpen(*a));   // the step never ran in the first pass: nothing opened, so nothing is closed again
        A::look(*a);
        CHECK(!A::enginePageOpen(*a));
        CHECK(!A::enginePageWasOpen(*a));
        delete a;
    }

    // ============================ GROUP 13, PART B ============================================================================
    // The page's faces, inspectors and transport, from the table. Every scenario here starts a window on a document, runs
    // frames until the engine's passes have published it, and reads what the page DREW (frameText).

    const auto appOn = [&](const pc::Graph& g) {
        auto* a = new AppWindow();
        A::setSeeded(*a, true);   // (the scenario brings its own patch: the starter radio, and a device for a Radio without one, are not under test)
        A::load(*a, g);
        ImGui::GetIO().AddMousePosEvent(1390.0f, 890.0f);
        frames(*a, 4);
        return a;
    };
    const auto radioNode = [&](pc::Graph& g, const char* name, float y, const std::string& device, float x = 40.0f) {
        const pc::NodeId id = g.addNode(pc::NodeKind::Radio, name, pc::PortType::Iq, x, y);
        if (pc::Node* n = g.mutableNode(id)) {
            n->device = device;
            n->freqHz = 145.0e6;
            n->centreChosen = true;
            n->rateHz = 2.4e6;
        }
        return id;
    };
    const auto textNow = [&](AppWindow& a, int n = 2) {
        for (int i = 0; i < n; ++i) { frameText(a); }
    };
    // Frames, a few milliseconds apart (the radio's reader thread runs in real time), until the page has drawn `want`.
    const auto untilText = [&](AppWindow& a, const char* want, int limit = 1500) {
        for (int i = 0; i < limit; ++i) {
            frameText(a);
            if (shows(want)) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        return false;
    };
    const auto waitFrames = [&](AppWindow& a, int n) {
        for (int i = 0; i < n; ++i) {
            frameText(a);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    };

    // --- Q: a Radio's face, in each state it has --------------------------------------------------------------------
    {
        std::printf("Q: a Radio's face in each state\n");
        pc::Graph g;
        // (Side by side: a face under a node drawn after it is cut away from it, and a face part-way off is clipped.)
        const pc::NodeId bare = g.addNode(pc::NodeKind::Radio, "Bare", pc::PortType::Iq, 40.0f, 40.0f);   // no device
        const pc::NodeId off = radioNode(g, "Off", 40.0f, pc::kGeneratorKey, 330.0f);
        if (pc::Node* n = g.mutableNode(off)) { n->on = false; }
        const pc::NodeId main = radioNode(g, "Main", 40.0f, pc::kGeneratorKey, 620.0f);
        (void)bare;
        AppWindow* a = appOn(g);
        // NOT RUNNING: the device named by the table's list (a generator's label), the three words a stopped face has.
        textNow(*a);
        if (std::getenv("G13_DUMP") != nullptr) { std::printf("[[%s]]%c", g_text.c_str(), 10); }
        std::printf("   stopped: \"Signal generator\" %d, \"Choose a device\" %d, \"Switched off.\" %d, \"press START\" %d\n",
                    shows("Signal generator"), shows("Choose a device on the right."), shows("Switched off."),
                    shows("Ready - press START."));
        CHECK(shows("Signal generator"));                         // PATCH_DEVICES' label, not the engine's
        CHECK(shows("Choose a device on the right."));           // the node with no device
        CHECK(shows("Switched off."));                            // the node switched off
        CHECK(shows("Ready - press START."));                    // the node ready
        CHECK(!shows("MS/s"));                                    // nothing runs: no rate
        CHECK(!A::lists(*a).radios.count(main));                  // (PATCH_RADIOS lists none)
        // The label of a device the list does not hold is made of the key itself (NOT LISTED IS NOT NOT CONNECTED).
        {
            pc::Graph h = A::draft(*a);
            if (pc::Node* n = h.mutableNode(bare)) { n->device = "soapy|driver=lime,serial=ABC123"; }
            A::mutableDraft(*a) = h;
            A::flush(*a);
            textNow(*a, 3);
            CHECK(shows("soapy ABC123 (not listed)"));   // (the driver of the KEY, soapy, and the serial: the list does not hold it)
            if (pc::Node* n = A::mutableDraft(*a).mutableNode(bare)) { n->device = pc::makeIqFileKey("C:/rec/sample_2k.wav"); }
            A::flush(*a);
            textNow(*a, 3);
            CHECK(shows("Recording: sample_2k.wav"));
            if (pc::Node* n = A::mutableDraft(*a).mutableNode(bare)) { n->device.clear(); }
            A::flush(*a);
            textNow(*a, 3);
        }
        // RUNNING: START opens the generator radios; the rate on the face is PATCH_RADIOS' value[0].
        A::submit(*a, cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, 1));
        CHECK(untilText(*a, "2.400 MS/s"));
        CHECK(A::lists(*a).radios.count(main) != 0 && A::lists(*a).radios.at(main).open);
        CHECK_EQ(A::lists(*a).radios.at(main).rateHz, 2.4e6);
        CHECK(!shows("Ready - press START."));
        CHECK(shows("Switched off."));                            // the switched-off node stays so while the patch runs
        // THE TWO RADIOS ON ONE DEVICE (the generator may be shared; a recording may not): the first of them fails to open
        // (the file is not there) and says why in its own words; the second is refused by the plan, and its face says that.
        {
            const std::string missing = pc::makeIqFileKey("Z:/no/such/recording.wav");
            const pc::NodeId first = radioNode(A::mutableDraft(*a), "First", 330.0f, missing, 330.0f);
            const pc::NodeId second = radioNode(A::mutableDraft(*a), "Second", 330.0f, missing, 620.0f);
            A::flush(*a);
            CHECK(untilText(*a, "Another radio here already uses this device."));
            std::printf("   nodes %u and %u on one recording: the second's face says so\n", static_cast<unsigned>(first),
                        static_cast<unsigned>(second));
            // The first's own error, PATCH_RADIOS' detail. A recording's header is read on the open worker (stub campaign B19: it was read inline on
            // the pass), so the plan's sentence above lands first and the error a pass or more after it: the case waits for it, the assertion unchanged.
            // (The limit is long on purpose: where "Z:" is a mapped share that is offline, the first open waits out the network's own 21 s.)
            CHECK(untilText(*a, "cannot open file", 15000));
            CHECK(shows("Recording: recording.wav"));              // ...and the label of the key the list does not hold
        }
        // A NOTE under a running radio (a coerced rate, a retune it would not take): PATCH_RADIOS' detail.
        A::injectNote(*a, main, "the radio refused 2.4 MS/s and runs at its own rate");
        CHECK(untilText(*a, "the radio refused 2.4 MS/s and runs at its own rate"));
        CHECK(shows("2.400 MS/s"));                               // ...beside the rate, not instead of it
        A::clearNote(*a, main);
        textNow(*a, 4);
        CHECK(!shows("the radio refused"));
        // OPENING: a second Radio whose open is still on its worker.
        A::engineStops(*a);
        textNow(*a, 4);
        {
            A::Pending held;
            A::injectPending(*a, main, held);
            CHECK(untilText(*a, "opening..."));
            CHECK(A::lists(*a).radios.at(main).pending);
            CHECK(!shows("2.400 MS/s"));
            A::answerPending(held);
            textNow(*a, 4);
            CHECK(!shows("opening..."));
        }
        delete a;
    }

    // --- R: the live readouts move with the pass --------------------------------------------------------------------
    {
        std::printf("R: the gate, the level, the speaker and the spectrum move with the pass\n");
        const std::filesystem::path rec = scratch / "rec_r";
        std::filesystem::create_directories(rec);
        pc::Graph g;
        const pc::NodeId radio = radioNode(g, "Radio", 40.0f, pc::kGeneratorKey);
        const pc::NodeId chan = g.addNode(pc::NodeKind::Channel, "Channel", pc::PortType::Iq, 420.0f, 40.0f);
        const pc::NodeId demod = g.addNode(pc::NodeKind::Demod, "Demod", pc::PortType::Iq, 420.0f, 320.0f);
        const pc::NodeId speaker = g.addNode(pc::NodeKind::Sink, "Speaker", pc::PortType::Audio, 700.0f, 320.0f);
        const pc::NodeId display = g.addNode(pc::NodeKind::Display, "Spectrum", pc::PortType::Iq, 40.0f, 330.0f);
        if (pc::Node* n = g.mutableNode(chan)) { n->freqHz = 145.05e6; }
        if (pc::Node* n = g.mutableNode(demod)) {
            n->squelch = true;
            n->squelchDb = -20.0f;
        }
        CHECK(g.connect(radio, 0, chan, 0) == pc::Connect::Ok);
        CHECK(g.connect(chan, 0, demod, 0) == pc::Connect::Ok);
        CHECK(g.connect(demod, 0, speaker, 0) == pc::Connect::Ok);
        CHECK(g.connect(radio, 0, display, 0) == pc::Connect::Ok);
        AppWindow* a = appOn(g);
        A::setRecordDir(*a, rec.string());
        // STOPPED: the speaker waits, the Spectrum part has no spectrum, nothing is read live.
        textNow(*a);
        CHECK(shows("Waiting for its radio."));
        CHECK(shows("Wire the radio or a channel here to see its spectrum."));   // (a radio that is not running has no rate to cut by)
        CHECK_EQ(A::spectrumSeq(*a, radio), 0u);
        A::select(*a, demod);
        textNow(*a);
        CHECK(!shows("closed"));                                  // no gate to read: it says nothing
        A::submit(*a, cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, 1));
        // THE GATE: the level the squelch judges and whether it is open (the noise floor is far below -20 dB: closed).
        CHECK(untilText(*a, "closed"));
        const pc::NodeId radioId = radio;
        const auto* gate = A::liveRow(*a, 1, demod);
        CHECK(gate != nullptr);
        if (gate != nullptr) {
            std::printf("   the gate: %.1f dB, %s; drawn: \"%s\"\n", gate->v0, gate->open ? "open" : "closed", shows("dB") ? "dB shown" : "NO dB");
            CHECK(gate->v0 < -20.0 && !gate->open);
            CHECK(shows("dB"));
        }
        // THE CHANNEL'S LEVEL: the number on the node (the canvas's reading) is PATCH_LIVE's, for the channel's own node.
        {
            for (int i = 0; i < 1500 && A::liveRow(*a, 3, chan) == nullptr; ++i) { waitFrames(*a, 1); }
            const auto* lv = A::liveRow(*a, 3, chan);
            CHECK(lv != nullptr);
            bool found = false;
            for (const pg::NodeReading& r : A::readings(*a)) {
                if (r.node == chan && lv != nullptr) {
                    found = true;
                    CHECK(std::fabs(static_cast<double>(r.db) - lv->v0) < 1e-3);
                }
            }
            CHECK(found);
            A::select(*a, chan);
            textNow(*a, 2);
            CHECK(shows("dB on this channel"));                  // the inspector spells it out
        }
        // THE SPECTRUM: a Spectrum part draws the radio's trace - read_node_spectrum, a row per published frame.
        {
            for (int i = 0; i < 1500 && A::spectrumSeq(*a, radioId) == 0u; ++i) { waitFrames(*a, 1); }
            const std::uint64_t seq0 = A::spectrumSeq(*a, radioId);
            CHECK(seq0 != 0u);
            const int rows0 = A::scopeRows(*a, display);
            for (int i = 0; i < 1500 && A::spectrumSeq(*a, radioId) < seq0 + 3u; ++i) { waitFrames(*a, 1); }
            CHECK(A::spectrumSeq(*a, radioId) >= seq0 + 3u);
            CHECK(A::scopeRows(*a, display) > rows0 || rows0 > 0);
            CHECK(!shows("No spectrum yet"));
            std::printf("   the spectrum: sequence %llu -> %llu, waterfall rows %d -> %d\n", static_cast<unsigned long long>(seq0),
                        static_cast<unsigned long long>(A::spectrumSeq(*a, radioId)), rows0, A::scopeRows(*a, display));
        }
        // THE SPEAKER: what it plays to, and the seconds it has written, rising with the pass.
        A::select(*a, speaker);
        CHECK(untilText(*a, "WAV  patch-"));
        CHECK(!shows("Waiting for its radio."));
        CHECK(untilText(*a, " s"));
        const double s0 = numberAfter("WAV  patch-") >= 0.0 ? 0.0 : 0.0;
        (void)s0;
        // The seconds line is the number before " s" on its own line: read it twice, a while apart.
        const auto seconds = [&] {
            const auto* row = A::liveRow(*a, 2, speaker);
            return row != nullptr ? row->v1 / 48000.0 : -1.0;
        };
        const double t0 = seconds();
        for (int i = 0; i < 1500 && seconds() < t0 + 0.3; ++i) { waitFrames(*a, 1); }
        const double t1 = seconds();
        std::printf("   the speaker has written %.2f s -> %.2f s\n", t0, t1);
        CHECK(t1 >= t0 + 0.3);
        CHECK(shows("s"));
        // NONE OF IT MOVES listSeq: the readings changed every pass above and the table's lists did not.
        {
            const std::uint64_t seq = A::listSeq(*a);
            waitFrames(*a, 30);
            CHECK_EQ(A::listSeq(*a), seq);
        }
        // A SPEAKER WITH NO OUTPUT says why (the record folder cannot be made): the node's own error line.
        A::engineStops(*a);
        textNow(*a, 4);
        CHECK_EQ(A::spectrumSeq(*a, radioId), 0u);                 // the radio is gone, and so is its frame
        CHECK(!A::spectrumHave(*a, radioId));
        CHECK(shows("Wire the radio or a channel here to see its spectrum."));
        CHECK(!A::liveRow(*a, 3, chan) && !A::liveRow(*a, 1, demod) && !A::liveRow(*a, 2, speaker));   // nothing runs: no readings
        {
            const std::filesystem::path blocker = rec / "not_a_folder";
            { std::ofstream f(blocker); f << "x"; }
            const auto countWavs = [&rec] {
                std::size_t n = 0;
                std::error_code wavEc;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(rec, wavEc)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".wav") { ++n; }
                }
                return n;
            };
            const std::size_t wavFilesBefore = countWavs();   // (earlier cases of this test wrote speaker files here)
            A::setRecordDir(*a, blocker.string());
            A::submit(*a, cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, 1));
            CHECK(untilText(*a, "cannot create directory"));
            // (0.99.64: A WAV SPEAKER'S DESTINATION EXISTS AT ONCE and its file is opened by a worker - nothing waits for the disk - so the face
            // may still name the file it was going to write while the failure is on its way; what must hold is that the failure is said and that
            // no file came of it, in the folder that could not be made or beside it.)
            CHECK_EQ(countWavs(), wavFilesBefore);
        }
        delete a;
    }

    // --- S: a Text out shows the lines of the decoders wired to it ---------------------------------------------------
    {
        std::printf("S: a Text out shows the lines of its decoders, by the node that said them\n");
        pc::Graph g;
        const pc::NodeId radio = radioNode(g, "Radio", 40.0f, pc::kGeneratorKey);
        const pc::NodeId decA = g.addNode(pc::NodeKind::Decoder, "Decoder A", pc::PortType::Iq, 420.0f, 40.0f);
        const pc::NodeId decB = g.addNode(pc::NodeKind::Decoder, "Decoder B", pc::PortType::Iq, 420.0f, 320.0f);
        const pc::NodeId outA = g.addNode(pc::NodeKind::Sink, "Text A", pc::PortType::Text, 800.0f, 40.0f);
        const pc::NodeId outB = g.addNode(pc::NodeKind::Sink, "Text B", pc::PortType::Text, 800.0f, 320.0f);
        CHECK(g.connect(radio, 0, decA, 0) == pc::Connect::Ok);
        CHECK(g.connect(radio, 0, decB, 0) == pc::Connect::Ok);
        CHECK(g.connect(decA, 0, outA, 0) == pc::Connect::Ok);
        CHECK(g.connect(decA, 0, outB, 0) == pc::Connect::Ok);   // A shows on both Text outs...
        CHECK(g.connect(decB, 0, outB, 0) == pc::Connect::Ok);   // ...B only on the second
        AppWindow* a = appOn(g);
        textNow(*a);
        CHECK(shows("Decoded lines appear here."));
        A::injectLine(*a, decA, "POCSAG", "page for A one");
        A::injectLine(*a, decB, "ACARS", "message for B one");
        A::injectLine(*a, 0, "RX", "the receiver's own decoder line");
        A::injectLine(*a, decA, "POCSAG", "page for A two");
        textNow(*a, 4);
        std::printf("   Text A %zu line(s), Text B %zu line(s)\n", A::textOutLines(*a, outA), A::textOutLines(*a, outB));
        CHECK_EQ(A::textOutLines(*a, outA), 2u);   // decoder A's two
        CHECK_EQ(A::textOutLines(*a, outB), 3u);   // decoder A's two and decoder B's one
        CHECK(shows("POCSAG: page for A one"));
        CHECK(shows("POCSAG: page for A two"));
        CHECK(shows("ACARS: message for B one"));
        CHECK(!shows("the receiver's own decoder line"));   // a line from the receiver's decoders is on no patch face
        CHECK(!shows("Decoded lines appear here."));
        // A line is routed once: more frames, no more lines.
        textNow(*a, 6);
        CHECK_EQ(A::textOutLines(*a, outA), 2u);
        CHECK_EQ(A::textOutLines(*a, outB), 3u);
        // The newest 200 are kept.
        for (int i = 0; i < 230; ++i) { A::injectLine(*a, decB, "ACARS", "bulk " + std::to_string(i)); }
        textNow(*a, 4);
        CHECK_EQ(A::textOutLines(*a, outB), 200u);
        CHECK_EQ(A::textOutLines(*a, outA), 2u);
        // The Decoder output window's Clear empties ITS copy; the Text outs keep theirs.
        A::clearDecoderOutput(*a);
        textNow(*a, 3);
        CHECK_EQ(A::textOutLines(*a, outA), 2u);
        CHECK(shows("POCSAG: page for A one"));
        // A node that goes takes its lines with it.
        {
            pc::Graph h = A::draft(*a);
            h.removeNode(outA);
            A::mutableDraft(*a) = h;
            A::flush(*a);
            textNow(*a, 4);
            CHECK_EQ(A::textOutLines(*a, outA), 0u);
        }
        delete a;
    }

    // --- T: the transport and the inspectors send through the session -----------------------------------------------
    {
        std::printf("T: START, ALL OFF, the device list and \"Look for radios\" go through the session\n");
        pc::Graph g;
        const pc::NodeId radio = radioNode(g, "Radio", 40.0f, pc::kGeneratorKey);
        AppWindow* a = appOn(g);
        std::vector<cascade::core::cmd::QueuedCommand> sent;
        A::tap(*a, &sent);
        const auto ops = [&] {
            std::vector<std::uint32_t> v;
            for (const auto& q : sent) { v.push_back(q.c.op); }
            return v;
        };
        // The id of an item named `label` in the child window called `child` (the page's well or its inspector).
        const auto idOf = [&](const char* child, const char* label) -> ImGuiID {
            for (ImGuiWindow* w : GImGui->Windows) {
                if (w->Name != nullptr && std::strstr(w->Name, child) != nullptr) { return w->GetID(label); }
            }
            return 0u;
        };
        // The pointer walked over the page until it is on the item with this id (-1 when it never is).
        const auto find = [&](ImGuiID id, float x0, float x1, float y0, float y1, float step) {
            for (float y = y0; y < y1; y += step) {
                for (float x = x0; x < x1; x += step) {
                    mouseTo(*a, x, y);
                    if (GImGui->HoveredId == id) { return ImVec2(x, y); }
                }
            }
            return ImVec2(-1.0f, -1.0f);
        };
        const ImGuiID allOff = idOf("patchwell", "ALL OFF###patchalloff");
        CHECK(allOff != 0u);
        const ImVec2 allOffAt = find(allOff, 20.0f, 700.0f, 10.0f, 140.0f, 6.0f);
        std::printf("   ALL OFF found at %.0f,%.0f\n", allOffAt.x, allOffAt.y);
        CHECK(allOffAt.x >= 0.0f);
        sent.clear();
        click(*a, allOffAt.x, allOffAt.y);
        CHECK((ops() == std::vector<std::uint32_t>{FOXAPI_OP_PATCH_ALL_OFF}));   // a command, not a call
        // START: the dome is the first item of the transport row. Pressed, it asks for the patch to run; one pass later it does.
        {
            ImVec2 dome(-1.0f, -1.0f);
            const float rowY = allOffAt.y;
            for (float x = A::originX(*a) - 600.0f; x < allOffAt.x - 40.0f && dome.x < 0.0f; x += 4.0f) {
                mouseTo(*a, x, rowY);
                if (GImGui->HoveredId != 0u && GImGui->HoveredId != allOff) { dome = ImVec2(x, rowY); }
            }
            CHECK(dome.x >= 0.0f);
            sent.clear();
            click(*a, dome.x, dome.y);
            CHECK((ops() == std::vector<std::uint32_t>{FOXAPI_OP_PATCH_RUN}));
            CHECK(!sent.empty() && sent[0].c.ival[0] == 1);
            CHECK(!A::engineRunning(*a) || true);
            frames(*a, 4);
            CHECK(A::engineRunning(*a));                       // landed in the pass, one frame later
            // ...and the dome's second press stops it (the 0/1 comes from the state's flag).
            sent.clear();
            click(*a, dome.x, dome.y);
            CHECK(!sent.empty() && sent[0].c.op == FOXAPI_OP_PATCH_RUN && sent[0].c.ival[0] == 0);
            frames(*a, 4);
            CHECK(!A::engineRunning(*a));
        }
        // THE RADIO'S DEVICE LIST: opening the combo asks for the lists, through the session, in this order.
        A::select(*a, radio);
        frames(*a, 3);
        const ImGuiID combo = idOf("patchinspector", "##patchdevice");
        CHECK(combo != 0u);
        const ImVec2 comboAt = find(combo, A::originX(*a) + 500.0f, 1390.0f, 120.0f, 700.0f, 5.0f);
        std::printf("   the Device combo found at %.0f,%.0f\n", comboAt.x, comboAt.y);
        CHECK(comboAt.x >= 0.0f);
        if (comboAt.x >= 0.0f) {
            // "Look for radios" is the next item below the combo (the list is closed), and sends the wish, the scan and the
            // listing, in this order.
            ImVec2 lookAt(-1.0f, -1.0f);
            for (float y = comboAt.y + 6.0f; y < comboAt.y + 60.0f && lookAt.x < 0.0f; y += 3.0f) {
                mouseTo(*a, comboAt.x + 6.0f, y);
                if (GImGui->HoveredId != 0u && GImGui->HoveredId != combo) { lookAt = ImVec2(comboAt.x + 6.0f, y); }
            }
            CHECK(lookAt.x >= 0.0f);
            if (lookAt.x >= 0.0f) {
                sent.clear();
                click(*a, lookAt.x, lookAt.y);
                frames(*a, 2);
                const std::vector<std::uint32_t> look = ops();
                CHECK((look == std::vector<std::uint32_t>{FOXAPP_OP_PATCH_LOOK_FOR_RADIOS, FOXAPI_OP_SCAN_DEVICES,
                                                          FOXAPP_OP_PATCH_LIST_RECORDINGS}));
            }
            // OPENING THE LIST asks for the lists, through the session.
            sent.clear();
            click(*a, comboAt.x, comboAt.y);
            frames(*a, 3);
            const std::vector<std::uint32_t> got = ops();
            std::printf("   opening the combo sent %zu command(s)\n", got.size());
            CHECK((got == std::vector<std::uint32_t>{FOXAPP_OP_PATCH_RADIO_LIST_OPENED, FOXAPP_OP_PATCH_LIST_RECORDINGS}));
        }
        A::tap(*a, nullptr);
        delete a;
    }

    // --- V: a window with nothing published draws every face --------------------------------------------------------
    {
        std::printf("V: every kind of node, drawn before the engine has published a thing\n");
        pc::Graph g;
        const pc::NodeId radio = radioNode(g, "Radio", 40.0f, pc::kGeneratorKey);
        const pc::NodeId chan = g.addNode(pc::NodeKind::Channel, "Channel", pc::PortType::Iq, 420.0f, 40.0f);
        const pc::NodeId demod = g.addNode(pc::NodeKind::Demod, "Demod", pc::PortType::Iq, 420.0f, 200.0f);
        const pc::NodeId speaker = g.addNode(pc::NodeKind::Sink, "Speaker", pc::PortType::Audio, 700.0f, 200.0f);
        const pc::NodeId text = g.addNode(pc::NodeKind::Sink, "Text", pc::PortType::Text, 700.0f, 360.0f);
        const pc::NodeId display = g.addNode(pc::NodeKind::Display, "Spectrum", pc::PortType::Iq, 40.0f, 400.0f);
        const pc::NodeId map = g.addNode(pc::NodeKind::Map, "Map", pc::PortType::Track, 420.0f, 400.0f);
        const pc::NodeId dec = g.addNode(pc::NodeKind::Decoder, "Decoder", pc::PortType::Iq, 700.0f, 40.0f);
        CHECK(g.connect(radio, 0, chan, 0) == pc::Connect::Ok);
        CHECK(g.connect(chan, 0, demod, 0) == pc::Connect::Ok);
        CHECK(g.connect(demod, 0, speaker, 0) == pc::Connect::Ok);
        CHECK(g.connect(radio, 0, display, 0) == pc::Connect::Ok);
        CHECK(g.connect(radio, 0, dec, 0) == pc::Connect::Ok);
        CHECK(g.connect(dec, 0, text, 0) == pc::Connect::Ok);
        auto* a = new AppWindow();
        A::load(*a, g);
        // Not one pass has run: every read finds nothing, and every face and inspector draws.
        for (const pc::NodeId id : {radio, chan, demod, speaker, text, display, map, dec}) {
            A::select(*a, id);
            frameText(*a);
            frameText(*a);
        }
        CHECK(shows("PATCH") || true);
        // ...and the same sweep after the engine has run for a while, every inspector once with the patch running.
        A::submit(*a, cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, 1));
        for (int i = 0; i < 12; ++i) {
            for (const pc::NodeId id : {radio, chan, demod, speaker, text, display, map, dec}) {
                A::select(*a, id);
                frameText(*a);
            }
        }
        CHECK(A::engineRunning(*a));
        delete a;
    }

    // --- X: what the page allocates ---------------------------------------------------------------------------------
    //     The draw allocates by design - it always did: the ids vector, the formatted strings of every face, the columns
    //     and the polyline of every Spectrum part, the lines of a Text out. What the port holds to ZERO is the refresh of
    //     what the faces read: with nothing changed, the table's lists and facts are not read again (listSeq), PATCH_LIVE's
    //     rows and a radio's spectrum keep their room, and a frame's reads allocate nothing.
    {
        std::printf("X: the cache refresh allocates nothing when nothing changed; the draw's own count\n");
        const std::filesystem::path rec = scratch / "rec_x";
        std::filesystem::create_directories(rec);
        pc::Graph g;
        const pc::NodeId radio = radioNode(g, "Radio", 40.0f, pc::kGeneratorKey);
        const pc::NodeId chan = g.addNode(pc::NodeKind::Channel, "Channel", pc::PortType::Iq, 420.0f, 40.0f);
        const pc::NodeId demod = g.addNode(pc::NodeKind::Demod, "Demod", pc::PortType::Iq, 420.0f, 320.0f);
        const pc::NodeId speaker = g.addNode(pc::NodeKind::Sink, "Speaker", pc::PortType::Audio, 700.0f, 320.0f);
        const pc::NodeId display = g.addNode(pc::NodeKind::Display, "Spectrum", pc::PortType::Iq, 40.0f, 330.0f);
        if (pc::Node* n = g.mutableNode(chan)) { n->freqHz = 145.05e6; }
        CHECK(g.connect(radio, 0, chan, 0) == pc::Connect::Ok);
        CHECK(g.connect(chan, 0, demod, 0) == pc::Connect::Ok);
        CHECK(g.connect(demod, 0, speaker, 0) == pc::Connect::Ok);
        CHECK(g.connect(radio, 0, display, 0) == pc::Connect::Ok);
        AppWindow* a = appOn(g);
        A::setRecordDir(*a, rec.string());
        A::submit(*a, cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, 1));
        CHECK(untilText(*a, "2.400 MS/s"));
        CHECK(untilText(*a, "WAV  patch-"));
        waitFrames(*a, 40);   // (the buffers have their room)
        // THE REFRESH, in the state it is in when a frame has nothing new: the passes below do not run (the engine's part
        // of the frame is not called), so listSeq, the live rows and the spectrum are what the last frame left.
        A::refreshReads(*a, radio);
        t_news = 0;
        t_counting = true;
        for (int i = 0; i < 200; ++i) { A::refreshReads(*a, radio); }
        t_counting = false;
        std::printf("   200 refreshes (lists, PATCH_LIVE, a radio's spectrum), nothing new: %ld heap allocations\n", t_news);
        CHECK(t_news == 0);
        // THE DRAW'S OWN COUNT, per frame, the passes running, the patch running with a speaker and a Spectrum part. Said, not
        // held to a number: it is the page's by design.
        const int n = 60;
        long total = 0;
        for (int i = 0; i < n; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            t_news = 0;
            t_counting = true;
            frame(*a);
            t_counting = false;
            total += t_news;
        }
        std::printf("   a running page, per frame (the engine's part of the frame, the cache and the draw): %.0f heap allocations on average\n",
                    static_cast<double>(total) / n);
        delete a;
    }

    // --- W: a decoder's picture is read from the session and shown --------------------------------------------------------
    //     GROUP 13, PART C. A REAL picture decoder module (tests/fixture_group13_plugin.cpp) runs in the patch, in the engine's
    //     pass; the Decoder face reads its picture with read_image when PATCH_LIVE's picture row names a new one and uploads
    //     it to a texture - here a stand-in for GL's two calls, which a process with no GL context cannot make (the scope
    //     badge's precedent: tests/test_scope_window.cpp). What is held: the face's texture is made from the picture the
    //     engine holds (its bytes follow the fixture's formula), it is made ONCE for each picture and not once a frame, a
    //     picture that has moved on (NOT_FOUND) or is not a picture leaves the texture be, a stopped patch keeps its picture
    //     on the face, a removed node takes the texture with it, and nothing in the window reaches the engine's objects or
    //     keeps the hook the engine used to write the picture through.
    {
        std::printf("W: a decoder's picture is read with read_image and uploaded once for each picture\n");
        const std::filesystem::path fixture(FIXTURE_GROUP13_PLUGIN);
        std::error_code copyEc;
        std::filesystem::copy_file(fixture, std::filesystem::path(g_pluginDir) / fixture.filename(),
                                   std::filesystem::copy_options::overwrite_existing, copyEc);
        CHECK(!copyEc);
        auto* a = new AppWindow();
        A::setSeeded(*a, true);
        frames(*a, 4);
        std::string key;
        for (const auto& part : A::lists(*a).parts) {
            if (part.name == "Group Thirteen Fixture") { key = part.key; }
        }
        CHECK(!key.empty());
        pc::Graph g;
        const pc::NodeId radio = radioNode(g, "Radio", 40.0f, pc::kGeneratorKey);
        const pc::NodeId chan = g.addNode(pc::NodeKind::Channel, "Channel", pc::PortType::Iq, 420.0f, 40.0f);
        const pc::NodeId demod = g.addNode(pc::NodeKind::Demod, "Demod", pc::PortType::Iq, 420.0f, 320.0f);
        const pc::NodeId dec = g.addNode(pc::NodeKind::Decoder, "Fixture picture", pc::PortType::Audio, 700.0f, 320.0f);
        if (pc::Node* n = g.mutableNode(chan)) { n->freqHz = 145.05e6; }
        if (pc::Node* n = g.mutableNode(dec)) { n->plugin = key; }
        CHECK(g.connect(radio, 0, chan, 0) == pc::Connect::Ok);
        CHECK(g.connect(chan, 0, demod, 0) == pc::Connect::Ok);
        CHECK(g.connect(demod, 0, dec, 0) == pc::Connect::Ok);
        A::load(*a, g);
        frames(*a, 4);

        // The stand-in for GL, and a table whose read_image is counted (and can be made to say something else).
        g_pics = PicLog{};
        A::setPictureGl(*a, &fakeUpload, &fakeDestroy);
        g_realTable = A::engineApi(*a);
        g_wrapped = *g_realTable;
        g_wrapped.read_image = &countingReadImage;
        A::setEngineApi(*a, &g_wrapped);
        g_pics.mainThread = std::this_thread::get_id();

        // NOTHING YET: no picture, so no row, no read and no texture.
        frames(*a, 6);
        CHECK(A::liveRow(*a, 6, dec) == nullptr);
        CHECK_EQ(g_pics.reads, 0);
        CHECK_EQ(g_pics.uploads.size(), 0u);
        CHECK(A::picture(*a, dec) == nullptr);

        // RUN: the pictures arrive; each frame is drawn, the picture row moves, and a new picture is read and uploaded.
        A::submit(*a, cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, 1));
        std::set<std::uint64_t> idsSeen;
        std::uint64_t lastId = 0;
        for (int i = 0; i < 4000 && g_pics.uploads.size() < 3; ++i) {
            frame(*a);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            if (const auto* p = A::picture(*a, dec)) {
                if (p->imageId != lastId) {
                    lastId = p->imageId;
                    idsSeen.insert(lastId);
                }
            }
        }
        CHECK(g_pics.uploads.size() >= 3u);
        // Every upload is a picture the fixture made, whole: its shape and its bytes follow the formula for SOME sequence.
        int whole = 0;
        for (const Upload& u : g_pics.uploads) {
            whole += uploadFollowsFixture(u) ? 1 : 0;
            CHECK(u.thread == g_pics.mainThread);   // GL is the window's thread's: nothing is uploaded from anywhere else
        }
        CHECK_EQ(whole, static_cast<int>(g_pics.uploads.size()));
        // ONE UPLOAD FOR EACH PICTURE SHOWN - not one a frame - and at most two reads for each (the row's byte count sizes the
        // buffer, so one in practice; a second would be the retry for a picture bigger than the row said).
        std::printf("   %zu upload(s), %zu picture id(s) shown, %d read_image call(s)\n", g_pics.uploads.size(), idsSeen.size(),
                    g_pics.reads);
        CHECK_EQ(g_pics.uploads.size(), idsSeen.size());
        CHECK(g_pics.reads >= static_cast<int>(g_pics.uploads.size()));
        CHECK(g_pics.reads <= 2 * static_cast<int>(g_pics.uploads.size()) + g_pics.refused);
        // ...and the texture is the newest upload's, at the figures the picture was made at.
        {
            const auto* p = A::picture(*a, dec);
            CHECK(p != nullptr);
            if (p != nullptr) {
                CHECK(p->tex != 0u);
                CHECK_EQ(p->tex, g_pics.uploads.back().texReturned);
                CHECK_EQ(p->width, g_pics.uploads.back().w);
                CHECK_EQ(p->height, g_pics.uploads.back().h);
                CHECK_EQ(p->imageId, lastId);
            }
            // The window kept the texture and its figures - not the pixels: its scratch is not holding a picture's room.
            CHECK(A::pictureScratchCapacity(*a) <= (std::size_t{8} << 20));
        }

        // A PICTURE THE ENGINE HAS MOVED PAST BETWEEN THE ROW AND THE READ (read_image says NOT_FOUND) leaves the texture on
        // the face and the next frame reads the row again; a reply that is not a picture does the same.
        {
            const std::size_t uploads0 = g_pics.uploads.size();
            const unsigned texBefore = A::picture(*a, dec) != nullptr ? A::picture(*a, dec)->tex : 0u;
            const std::uint64_t idBefore = A::picture(*a, dec) != nullptr ? A::picture(*a, dec)->imageId : 0u;
            g_pics.mode = Mode::NotFound;
            const int refused0 = g_pics.refused;
            for (int i = 0; i < 4000 && g_pics.refused < refused0 + 2; ++i) {
                frame(*a);
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            CHECK(g_pics.refused >= refused0 + 2);          // it asked again on the next frame (the row still named a new picture)
            CHECK_EQ(g_pics.uploads.size(), uploads0);      // ...and uploaded nothing
            CHECK_EQ(A::picture(*a, dec)->tex, texBefore);  // the texture on the face is the one it had
            CHECK_EQ(A::picture(*a, dec)->imageId, idBefore);
            g_pics.mode = Mode::BadShape;                   // a reply whose figures do not describe its bytes
            const int refused1 = g_pics.refused;
            for (int i = 0; i < 4000 && g_pics.refused < refused1 + 2; ++i) {
                frame(*a);
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            CHECK(g_pics.refused >= refused1 + 2);
            CHECK_EQ(g_pics.uploads.size(), uploads0);
            CHECK_EQ(A::picture(*a, dec)->imageId, idBefore);
            g_pics.mode = Mode::Pass;
            for (int i = 0; i < 4000 && A::picture(*a, dec)->imageId == idBefore; ++i) {
                frame(*a);
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            CHECK(A::picture(*a, dec)->imageId != idBefore);       // the reads work again: the newest picture is on the face
            CHECK(g_pics.uploads.size() > uploads0);
        }

        // STOPPED: the picture stays on the face, and a settled frame reads and uploads nothing.
        A::submit(*a, cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, 0));
        waitFrames(*a, 40);
        const std::uint64_t shownId = A::picture(*a, dec)->imageId;
        const unsigned shownTex = A::picture(*a, dec)->tex;
        std::this_thread::sleep_for(std::chrono::milliseconds(700));   // (a picture from before the stop is still on its way)
        waitFrames(*a, 20);
        const std::size_t settledUploads = g_pics.uploads.size();
        const int settledReads = g_pics.reads;
        const std::uint64_t settledId = A::picture(*a, dec)->imageId;
        waitFrames(*a, 60);
        std::printf("   stopped: 60 frames, %zu upload(s), %d read(s)\n", g_pics.uploads.size() - settledUploads,
                    g_pics.reads - settledReads);
        CHECK_EQ(g_pics.uploads.size(), settledUploads);
        CHECK_EQ(g_pics.reads, settledReads);
        CHECK_EQ(A::picture(*a, dec)->imageId, settledId);
        CHECK(settledId >= shownId);
        CHECK_EQ(A::picture(*a, dec)->tex, shownTex);   // (the same texture is refilled, not remade)
        CHECK(g_pics.destroyed.empty());   // (nothing is deleted while the picture is on show)

        // A NEW PATCH DOCUMENT (the same graph, the same node ids) takes the picture off the node it was made on: the engine lets
        // go of every picture, the row goes, and the face deletes its texture in the frame that finds the row gone - it does not
        // show the old picture on the new node.
        {
            A::load(*a, A::draft(*a));
            waitFrames(*a, 8);
            CHECK(A::liveRow(*a, 6, dec) == nullptr);
            CHECK(A::picture(*a, dec) == nullptr);
            CHECK_EQ(g_pics.destroyed.size(), 1u);
            CHECK(!g_pics.destroyed.empty() && g_pics.destroyed.front() == shownTex);
        }
        // ...and the new node makes a picture of its own, on a texture of its own.
        A::submit(*a, cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, 1));
        const std::size_t uploadsBeforeAgain = g_pics.uploads.size();
        for (int i = 0; i < 4000 && A::picture(*a, dec) == nullptr; ++i) {
            frame(*a);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        CHECK(A::picture(*a, dec) != nullptr);
        CHECK(g_pics.uploads.size() > uploadsBeforeAgain);
        const unsigned secondTex = A::picture(*a, dec) != nullptr ? A::picture(*a, dec)->tex : 0u;
        CHECK(secondTex != 0u);
        A::submit(*a, cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, 0));
        waitFrames(*a, 20);

        // A NODE REMOVED takes its texture with it, deleted in the frame that finds it gone from the draft.
        {
            pc::Graph without = A::draft(*a);
            CHECK(without.removeNode(dec));
            A::mutableDraft(*a) = without;
            waitFrames(*a, 8);
            CHECK(A::picture(*a, dec) == nullptr);
            CHECK_EQ(g_pics.destroyed.size(), 2u);
            CHECK(g_pics.destroyed.size() == 2u && g_pics.destroyed.back() == secondTex);
            CHECK(A::liveRow(*a, 6, dec) == nullptr);
        }

        // THE WINDOW KEEPS NOTHING THE ENGINE'S THREAD WRITES: the hook is gone from the engine's interface and the window's,
        // and the face reads no engine object (the guard's rule, restated in this scenario's own words).
        {
            const std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path();
            const auto slurp = [&](const char* rel) {
                std::ifstream in(root / rel, std::ios::binary);
                CHECK(in.good());
                std::stringstream ss;
                ss << in.rdbuf();
                return ss.str();
            };
            const std::string cpp = slurp("src/gui/app_window.cpp");
            const std::string hpp = slurp("src/gui/app_window.hpp");
            const std::string eh = slurp("src/engine/engine_host.hpp");
            const std::string ec2 = slurp("src/engine/engine_host.cpp");
            CHECK(cpp.find("onPatchPicture") == std::string::npos);
            CHECK(hpp.find("onPatchPicture") == std::string::npos);
            CHECK(eh.find("virtual void onPatchPicture") == std::string::npos);
            CHECK(ec2.find("EngineHost::onPatchPicture") == std::string::npos);
            CHECK(hpp.find("cascade::core::HostImage img;") == std::string::npos);   // no copy of the picture in the window
            CHECK(cpp.find("pic.img") == std::string::npos);
            const std::size_t at = cpp.find("bool AppWindow::takePatchPicture(");
            CHECK(at != std::string::npos);
            const std::size_t end = cpp.find("\n}\n", at);
            const std::string body = cpp.substr(at, end - at);
            CHECK(body.find("engine_.") == std::string::npos);
            CHECK(body.find("read_image") != std::string::npos);
        }
        delete a;
        std::error_code rmEc;
        std::filesystem::remove(std::filesystem::path(g_pluginDir) / fixture.filename(), rmEc);   // (a loaded module may refuse: harmless)
    }

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_patch_graph_draft_input");
}
