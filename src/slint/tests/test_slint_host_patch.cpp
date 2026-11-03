// The PATCH page's pure logic (src/slint/patch_logic.hpp) against a STUB TABLE: what the band, the information pane, the canvas with its faces, the inspector and the presets
// page show, and the exact edit or command each control has - the op with its arguments and grant, the draft machine's commit and answers, the canvas gestures - which are the
// ImGui window's own. No Slint, no engine, no window.
//
// It also READS the ImGui window's sources for every English sentence the logic words (they must be the drawers' own, so a catalogue key can never be one nobody draws) and for the
// ops, the constants and the rules the logic restates (kParts, the squelch range, the tower count, the refusal hold).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "patch_logic.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "core/airband_data.hpp"
#include "core/i18n.hpp"
#include "core/mp3_support.hpp"
#include "core/patch_presets.hpp"
#include "core/utf8_text.hpp"

using namespace foxsdr::slintpatch;
using cascade::i18n::tr;
using Names = std::vector<std::string>;
namespace cc = cascade::core;

namespace {

// ---- a table of lists and facts a test fills in (the other banks' tests') ----
class StubTable : public TableReader {
public:
    std::map<std::uint32_t, std::vector<FoxListItem>> lists;
    std::map<std::string, std::string> facts;
    std::map<std::string, int> factReads;                 // how many times each fact was read
    std::map<std::uint32_t, int> listReads;               // how many readList(list, i, out != null) calls each list had
    std::function<void(const std::string&)> factHook;     // called before a fact is answered

    std::int32_t readList(std::uint32_t list, std::uint32_t index, FoxListItem* out) override {
        const auto it = lists.find(list);
        const std::int32_t n = it == lists.end() ? 0 : static_cast<std::int32_t>(it->second.size());
        if (out != nullptr) { ++listReads[list]; }
        if (out != nullptr && static_cast<std::int32_t>(index) < n) { *out = it->second[index]; }
        return n;
    }
    bool fact(const std::string& key, std::string& out) override {
        ++factReads[key];
        if (factHook) { factHook(key); }
        const auto it = facts.find(key);
        if (it == facts.end()) { out.clear(); return false; }
        out = it->second;
        return true;
    }
    void add(std::uint32_t list, std::uint64_t id, const char* name, const char* detail, std::uint32_t flags = 0, double v0 = 0, double v1 = 0, double v2 = 0, double v3 = 0) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        it.id = id;
        std::snprintf(it.name, sizeof(it.name), "%s", name);
        std::snprintf(it.detail, sizeof(it.detail), "%s", detail);
        it.flags = flags;
        it.value[0] = v0; it.value[1] = v1; it.value[2] = v2; it.value[3] = v3;
        lists[list].push_back(it);
    }
};

// What the session was given: every command in order, with the ticket it answered (0 refuses). `offers` are the repeatable ones.
class RecSub : public Submitter {
public:
    std::vector<Command> sent, offered;
    std::uint64_t next = 100;
    bool refuse = false;
    bool refuseOffer = false;
    int refuseNth = -1;                // the call (counted from `calls` = 0) the session refuses
    int calls = 0;
    std::uint64_t submit(const Command& c) override {
        sent.push_back(c);
        const int n = calls++;
        if (refuse || n == refuseNth) { return 0; }
        return next++;
    }
    std::uint64_t offer(const Command& c) override {
        offered.push_back(c);
        if (refuseOffer) { return 0; }
        return next++;
    }
    void clear() { sent.clear(); offered.clear(); }
    std::size_t count(std::uint32_t op) const {
        std::size_t n = 0;
        for (const Command& c : sent) { if (c.q.c.op == op) { ++n; } }
        return n;
    }
    const Command* last(std::uint32_t op) const {
        for (std::size_t i = sent.size(); i-- > 0;) { if (sent[i].q.c.op == op) { return &sent[i]; } }
        return nullptr;
    }
};

class StubConfig : public PatchConfig {
public:
    std::string patchText;
    std::vector<cc::PatchPreset> presetList;
    std::string previousText;
    int patchSaves = 0, presetSaves = 0;
    std::string patch() const override { return patchText; }
    std::vector<cc::PatchPreset> presets() const override { return presetList; }
    std::string previous() const override { return previousText; }
    void savePatch(const std::string& text) override { patchText = text; ++patchSaves; }
    void savePresets(const std::vector<cc::PatchPreset>& list, const std::string& previous) override {
        presetList = list;
        previousText = previous;
        ++presetSaves;
    }
};

// A vector read that cannot go out of range: a missing element is a default one and the CHECK that wanted it fails by name (never an out-of-bounds read).
template <typename T>
const T& at(const std::vector<T>& v, std::size_t i) {
    static const T none{};
    return i < v.size() ? v[i] : none;
}
bool same(double a, double b) { return a == b; }   // (a comparison of two constants is not a condition the compiler lets pass /W4)
bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string show(const std::string& s) { return "\"" + s + "\""; }
std::string show(const char* s) { return std::string("\"") + s + "\""; }
std::string show(bool b) { return b ? "true" : "false"; }
std::string show(double d) { char b[48]; std::snprintf(b, sizeof(b), "%.9g", d); return b; }
std::string show(float d) { return show(static_cast<double>(d)); }
std::string show(int d) { return std::to_string(d); }
std::string show(unsigned d) { return std::to_string(d); }
std::string show(long long d) { return std::to_string(d); }
std::string show(unsigned long long d) { return std::to_string(d); }
std::string show(long d) { return std::to_string(d); }
std::string show(unsigned long d) { return std::to_string(d); }
template <class T, class = std::enable_if_t<std::is_integral_v<T>>>
std::string show(const std::vector<T>& v) { std::string s = "["; for (const auto& x : v) { s += std::to_string(x) + ","; } return s + "]"; }
std::string show(const Names& v) { std::string s = "["; for (const auto& x : v) { s += show(x) + ","; } return s + "]"; }
#define EQ(a, b)                                                                                              \
    do {                                                                                                      \
        ++g_checksRun;                                                                                        \
        if (!((a) == (b))) {                                                                                  \
            ++g_checksFailed;                                                                                 \
            std::printf("FAIL %s:%d  %s == %s\n      got: %s\n", __FILE__, __LINE__, #a, #b, show(a).c_str()); \
        }                                                                                                     \
    } while (0)

constexpr std::uint32_t kActive = FOXAPI_ITEM_ACTIVE, kFault = FOXAPI_ITEM_FAULT, kRefused = FOXAPI_ITEM_REFUSED;
constexpr std::uint32_t kPatchSetGraph = FOXAPP_OP_PATCH_SET_GRAPH;

// the sources
std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
std::string srcPath(const char* rel) {
    std::string p = __FILE__;
    for (int i = 0; i < 3; ++i) {
        const std::size_t cut = p.find_last_of("/\\");
        if (cut == std::string::npos) { break; }
        p.resize(cut);
    }
    return p + "/" + rel;
}
std::string joinedSource(const char* rel) {
    std::string s = slurp(srcPath(rel));
    static const std::regex join(R"re("[ \t]*\r?\n[ \t]*")re");
    return std::regex_replace(s, join, "");
}

// ---- the graph builders ----
using pc::NodeKind;
using pc::PortType;

struct Chain {                                   // radio -> channel -> demod -> speaker, spread on the canvas, with a Text out and a decoder alongside when asked
    pc::Graph g;
    pc::NodeId radio = 0, channel = 0, demod = 0, speaker = 0, decoder = 0, textOut = 0, display = 0, map = 0;
};
Chain chain(bool withDecoder = false) {
    Chain c;
    c.radio = c.g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 60, 80);
    if (pc::Node* n = c.g.mutableNode(c.radio)) { n->device = "siggen"; n->freqHz = 100.0e6; n->centreChosen = true; n->rateHz = 2.0e6; }
    c.channel = c.g.addNode(NodeKind::Channel, "Tower", PortType::Iq, 360, 80);
    if (pc::Node* n = c.g.mutableNode(c.channel)) { n->freqHz = 100.1e6; }
    c.demod = c.g.addNode(NodeKind::Demod, "Demod", PortType::Iq, 660, 80);
    c.speaker = c.g.addNode(NodeKind::Sink, "Speaker", PortType::Audio, 960, 80);
    c.g.connect(c.radio, 0, c.channel, 0);
    c.g.connect(c.channel, 0, c.demod, 0);
    c.g.connect(c.demod, 0, c.speaker, 0);
    if (withDecoder) {
        c.decoder = c.g.addNode(NodeKind::Decoder, "ACARS", PortType::Audio, 660, 300);
        if (pc::Node* n = c.g.mutableNode(c.decoder)) { n->plugin = "acars_1.dll"; }
        c.textOut = c.g.addNode(NodeKind::Sink, "Text out", PortType::Text, 960, 300);
        c.g.connect(c.demod, 0, c.decoder, 0);
        c.g.connect(c.decoder, 0, c.textOut, 0);
    }
    return c;
}

FoxReceiverState mkState(std::uint32_t flags = 0) {
    FoxReceiverState s{};
    s.structSize = static_cast<std::uint32_t>(sizeof(s));
    s.flags = flags;
    s.listSeq = 1;
    return s;
}

// A page over a stub table: the engine has published `version` / `epoch` of the graph; ticks run with the page showing.
struct Fx {
    StubTable t;
    PatchPage page;
    RecSub sub;
    StubConfig cfg;
    FoxReceiverState st = mkState();
    std::uint64_t version = 1, epoch = 1;
    explicit Fx(const pc::Graph& g = pc::Graph{}, bool seedFirst = false) {
        page.setConfig(&cfg);
        page.setCanvasSize(800.0f, 500.0f);
        publish(g);
        page.setViewShown(true);
        if (!seedFirst) { tick(); }
    }
    // The engine publishes `g` as version + 1 (same epoch unless newEpoch) and the table's listSeq moves.
    void publish(const pc::Graph& g, bool newEpoch = false) {
        if (newEpoch) { ++epoch; }
        ++version;
        t.facts["fact.patch.graph"] = pc::graphCommandText(g);
        t.facts["fact.patch.version"] = std::to_string(version);
        t.facts["fact.patch.epoch"] = std::to_string(epoch);
        ++st.listSeq;
    }
    void tick(bool showing = true) { page.tick(t, st, sub, showing); }
    void tickBump(bool showing = true) { ++st.listSeq; tick(showing); }
    pc::Graph& g() { return page.draft().graph(); }
    pc::Node* node(pc::NodeId id) { return page.draft().graph().mutableNode(id); }
    // A pointer press-and-release on world point `p` (the view is the default one unless a test moved it).
    void click(float x, float y) { page.canvasPressed(x, y, 0); page.canvasReleased(x, y, 0); }
    void select(pc::NodeId id) { page.canvas().ui.selected = id; }
    // THE ENGINE TAKES THE COMMIT IN FLIGHT: it publishes the graph the commit carried as the next version and answers OK; the next tick reads both.
    void engineTakes() {
        const PatchDraft::Send& s = page.draft().send();
        if (s.state != PatchDraft::Send::State::Sent) { return; }
        const Command* k = sub.last(kPatchSetGraph);
        if (k == nullptr) { return; }
        pc::Graph taken;
        std::string why;
        if (!pc::graphFromCommandText(foxsdr::slintsignal::textOfCommand(*k), taken, why)) { return; }
        const std::uint64_t ticket = s.ticket;
        publish(taken);
        page.onAnswer(answer(ticket, kPatchSetGraph, FOXAPI_OK, static_cast<double>(version)));
        tick();
    }
    // The answer the engine gives a SET_GRAPH commit: OK, the version it is now, merged or not.
    FoxCommandResult answer(std::uint64_t ticket, std::uint32_t op, int status = FOXAPI_OK, double applied0 = 0.0, double applied1 = 0.0) {
        FoxCommandResult r{};
        r.structSize = static_cast<std::uint32_t>(sizeof(r));
        r.status = status;
        r.ticket = ticket;
        r.op = op;
        r.applied[0] = applied0;
        r.applied[1] = applied1;
        std::snprintf(r.message, sizeof(r.message), "refused for the test");
        return r;
    }
};


// NODE READS THAT CANNOT FAULT: a node the draft does not hold reads as a default one (and a write to it goes nowhere), so a broken expectation names itself instead of crashing the run.
const pc::Node& nd(const pc::Graph& g, pc::NodeId id) {
    static const pc::Node none{};
    const pc::Node* n = g.find(id);
    return n != nullptr ? *n : none;
}
pc::Node& nm(pc::Graph& g, pc::NodeId id) {
    static pc::Node sink;
    sink = pc::Node{};
    pc::Node* n = g.mutableNode(id);
    return n != nullptr ? *n : sink;
}
const pc::Node& lastNode(const pc::Graph& g) {
    static const pc::Node none{};
    return g.nodes().empty() ? none : g.nodes().back();
}
const pc::Node& nodeFromEnd(const pc::Graph& g, std::size_t back) {
    static const pc::Node none{};
    return back < g.nodes().size() ? g.nodes()[g.nodes().size() - 1 - back] : none;
}
const std::string& backOf(const std::deque<std::string>& q) {
    static const std::string none;
    return q.empty() ? none : q.back();
}
const std::deque<std::string>& queueOf(const PatchPage& page, pc::NodeId id) {
    static const std::deque<std::string> none;
    const auto it = page.sinkLines().queues().find(id);
    return it != page.sinkLines().queues().end() ? it->second : none;
}
#define RUN(fn) do { std::printf("-- %s\n", #fn); fn(); } while (0)

const FoxCommand& fc(const Command& c) { return c.q.c; }
std::string textOf(const Command& c) { return foxsdr::slintsignal::textOfCommand(c); }
const Command& cmdAt(const RecSub& s, std::size_t i) {
    static const Command none{};
    return i < s.sent.size() ? s.sent[i] : none;
}

}  // namespace

namespace {

// ================================================================================================================================
// the sentences are the ImGui window's own
// ================================================================================================================================

std::string unescapeHex(const std::string& s) {
    const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x' && hex(s[i + 2]) >= 0 && hex(s[i + 3]) >= 0) {
            out += static_cast<char>(hex(s[i + 2]) * 16 + hex(s[i + 3]));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

// EVERY tr("...") LITERAL OF patch_logic.cpp is a sentence one of the drawers says: the logic adds no key - except the two the contract names as NEW keys (the decoder reading's
// singular and plural, which the window words with an untranslatable "%llu line%s"). A literal split over lines is joined first, as the drawers' own are.
void everySentenceIsADrawers() {
    const std::string mine = joinedSource("slint/patch_logic.cpp");
    CHECK(!mine.empty());
    const std::string drawers = joinedSource("gui/app_window.cpp") + joinedSource("gui/app_window_patch.cpp") + joinedSource("gui/app_window_patch_radios.cpp") +
                                joinedSource("gui/app_window_patch_blueprints.cpp") + joinedSource("gui/app_window_patch_presets.cpp") + joinedSource("gui/patch_view.cpp") + joinedSource("gui/scope_view.cpp");   // (START and STOP are the dome's own word)
    static const std::regex literal(R"re(\btr\(\s*("(?:[^"\\]|\\.)*")\s*\))re");
    const std::set<std::string> newKeys = {"\"%zu line\"", "\"%zu lines\""};
    int seen = 0, missing = 0, news = 0;
    std::set<std::string> distinct;
    for (std::sregex_iterator it(mine.begin(), mine.end(), literal), end; it != end; ++it) {
        const std::string quoted = unescapeHex((*it)[1]);
        const std::string body = quoted.substr(0, quoted.size() - 1);
        ++seen;
        distinct.insert(quoted);
        if (newKeys.count(quoted) != 0) { ++news; continue; }
        if (drawers.find(quoted) == std::string::npos && drawers.find(body + "##") == std::string::npos && drawers.find(body + "###") == std::string::npos) {
            std::printf("  a sentence of patch_logic.cpp that no drawer says: %s\n", quoted.c_str());
            ++missing;
        }
    }
    CHECK(distinct.size() >= 100);   // the whole of them were found (the pattern did not quietly match nothing)
    CHECK(missing == 0);
    CHECK(news >= 2);                // the two new keys are in use
    // the problem lines are core::patch::problemText's own, in the window's drawers (tr(problemText(p)))
    CHECK(has(drawers, "tr(cascade::core::patch::problemText(pr))"));
    // the sentences the contract lists are present (a spot check of each drawer)
    for (const char* key : {"\"This patch can run.\"", "\"Nothing selected.\"", "\"Choose a device on the right.\"", "\"Replace preset?\"", "\"Listen to the tower: %s\"",
                            "\"that would make a loop\"", "\"RUNNING - %zu of %zu radio open\""}) {
        CHECK(has(drawers, key));
        if (std::string(key) != "\"Replace preset?\"") { CHECK(has(mine, key)); }   // (the page's own title: the UI draws it)
    }
}

// The ImGui window's own facts the logic restates.
void sourcesStateWhatWeDo() {
    const std::string app = joinedSource("gui/app_window.cpp");
    const std::string radios = joinedSource("gui/app_window_patch_radios.cpp");
    const std::string view = joinedSource("gui/patch_view.cpp");
    const std::string vmath = joinedSource("gui/patch_view_math.hpp");
    const std::string blue = joinedSource("gui/app_window_patch_blueprints.cpp");
    const std::string pres = joinedSource("gui/app_window_patch_presets.cpp");
    CHECK(!app.empty() && !radios.empty() && !view.empty() && !vmath.empty() && !blue.empty() && !pres.empty());
    // the commit's text and flags
    CHECK(has(app, "FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(patchDraft_), 0, static_cast<std::int64_t>(patchDraftVersion_)"));
    CHECK(has(app, "FOXAPP_OP_PATCH_SET_GRAPH, cascade::core::patch::graphCommandText(g), FOXAPP_PATCH_GRAPH_DOCUMENT"));
    CHECK(has(app, "if (r.op == FOXAPP_OP_SCAN_NATIVE_ONLY) { patchScanDone_ = std::max<std::uint64_t>(patchScanDone_, r.ticket); }"));
    // the parts bin: seven keys in this order, with these labels and feeds
    const std::size_t at = app.find("static const Part kParts[] = {");
    CHECK(at != std::string::npos);
    const std::string parts = app.substr(at, 1100);
    const char* labels[7] = {"\"Radio\"", "\"Channel\"", "\"Demod\"", "\"Spectrum\"", "\"Speaker\"", "\"Text out\"", "\"Map\""};
    std::size_t pos = 0;
    for (const char* l : labels) {
        const std::size_t f = parts.find(l, pos);
        CHECK(f != std::string::npos);
        if (f != std::string::npos) { pos = f; }
    }
    CHECK(has(app, "full = kParts[i].kind == cascade::core::patch::NodeKind::Radio &&"));
    // the gestures
    CHECK(has(view, "constexpr double kRefusalHoldSec = 2.5;") && has(view, "constexpr float kWireGrabPx = 7.0f;") && has(view, "constexpr float kGridWorld = 26.0f;"));
    CHECK(has(view, "if (step >= 6.0f)"));
    CHECK(has(view, "pressTarget(g, mouseWorld, dv.zoom, kWireGrabPx)"));
    CHECK(has(view, "const ImVec2 box0{at.x + 12.0f, at.y + 10.0f};"));
    CHECK(has(vmath, "inline constexpr float kPortGrabRadius = 9.0f;") && has(vmath, "inline constexpr float kMinZoom = 0.35f;") && has(vmath, "inline constexpr float kMaxZoom = 2.50f;"));
    CHECK(has(vmath, "inline constexpr float kWheelPanPx = 48.0f;") && has(vmath, "inline constexpr float kWheelZoomStep = 1.12f;") && has(vmath, "inline constexpr float kWheelMaxNotches = 4.0f;"));
    CHECK(has(vmath, "inline constexpr float kKeyZoomStep = 1.25f;") && has(vmath, "inline constexpr float kFitMarginPx = 24.0f;"));
    // the faces
    CHECK(has(app, "if (zoom < 0.6f) { return; }"));
    CHECK(has(app, "const float faceW = s1.x - s0.x - 10.0f * drawZoom;") && has(app, "if (faceW < 40.0f) { continue; }"));
    CHECK(has(app, "(hasReadingLine ? 22.0f : 4.0f) * drawZoom"));
    CHECK(has(app, "std::max(1, static_cast<int>((s1.y - top) / lineH) - 1)"));
    CHECK(has(app, "while (q.size() > 200) { q.pop_front(); }"));
    CHECK(has(app, "q.push_back(row.plugin + \": \" + row.text);"));
    // the radio's rates, the squelch range, the sink's seconds
    CHECK(has(radios, "constexpr double kPatchRatesHz[] = {1.0e6,  1.024e6, 2.0e6, 2.048e6, 2.4e6,") && has(radios, "2.5e6,  3.0e6,   4.0e6, 6.0e6,   8.0e6, 10.0e6};"));
    CHECK(has(radios, "pc::kSquelchMinDb, pc::kSquelchMaxDb, \"%.0f dB\"") && has(radios, "ImGuiSliderFlags_AlwaysClamp"));
    CHECK(has(radios, "d->v1 / pc::kOutRateHz"));
    CHECK(kRates[0] == 1.0e6 && kRates[10] == 10.0e6 && kRateCount == 11 && kRates[1] == 1.024e6 && kRates[3] == 2.048e6 && kRates[4] == 2.4e6 && kRates[5] == 2.5e6 && kRates[6] == 3.0e6 && kRates[7] == 4.0e6 && kRates[8] == 6.0e6 && kRates[9] == 8.0e6 && kRates[2] == 2.0e6);
    // the blueprints and the presets
    CHECK(has(blue, "constexpr double kBlueprintDefaultRateHz = 2.0e6;") && has(blue, "constexpr std::size_t kTowerNearest = 6;") && kBlueprintDefaultRateHz == 2.0e6 && kTowerNearest == 6);
    CHECK(has(pres, "constexpr float kW = 440.0f;") && has(pres, "constexpr float kH = 460.0f;"));
    CHECK(kNameBytes == 63 && has(app, "char nameBuf[64];"));
    CHECK(same(kRefusalTicks, 76) && same(kWireGrabPx, 7.0) && same(kGridWorld, 26.0) && same(static_cast<double>(kSinkLinesCap), 200.0));
}

// ================================================================================================================================
// the reads
// ================================================================================================================================

void fillLists(StubTable& t) {
    t.lists.clear();
    t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars_1.dll", 0, 1, 0, 0);
    t.add(FOXAPI_LIST_PATCH_PARTS, 2, "ADS-B", "adsb_2.dll", 0, 0, 2.0e6, 1);
    t.add(FOXAPI_LIST_PATCH_PARTS, 3, "SSTV", "sstv_1.dll#image", 0, 1, 12000, 0);
    t.add(FOXAPI_LIST_PATCH_PLAN, 3, "", "channel", 0, 100000.0, 48000.0, 42.0, 1.0);
    t.add(FOXAPI_LIST_PATCH_PLAN, 5, "acars_1.dll", "decoder:channel", 0, 48000.0, 48000.0, 100.1e6, 1.0);
    t.add(FOXAPI_LIST_PATCH_PLAN, 6, "adsb_2.dll", "decoder:radio", 0, 2.0e6, 2.0e6, 100.0e6, 1.0);
    t.add(FOXAPI_LIST_PATCH_PLAN, 7, "acars_1.dll", "decoder:audio", 0, 48000.0, 48000.0, 0.0, 1.0);
    t.add(FOXAPI_LIST_PATCH_PROBLEMS, 3, "2", "no frequency set");                // NoFrequency
    t.add(FOXAPI_LIST_PATCH_PROBLEMS, 4, "0", "x");                               // NothingFeedsIt
    t.add(FOXAPI_LIST_PATCH_PROBLEMS, 4, "99", "from a newer engine");            // skipped
    t.add(FOXAPI_LIST_PATCH_PROBLEMS, 4, "-1", "from a newer engine");            // skipped
    t.add(FOXAPI_LIST_PATCH_PROBLEMS, 6, "14", "y");                              // NoTracks: the last one this window can word
    t.add(FOXAPI_LIST_PATCH_PROBLEMS, 6, "15", "z");                              // one past it: skipped
    t.add(FOXAPI_LIST_PATCH_RADIOS, 1, "siggen", "", kActive, 2.0e6, 100.0e6, 0);
    t.add(FOXAPI_LIST_PATCH_RADIOS, 2, "rtlsdr|serial=1", "the device would not open", kFault, 0.0, 0.0, 1);
    t.add(FOXAPI_LIST_PATCH_NODES, 1, "Radio", "radio", kActive);
    t.add(FOXAPI_LIST_PATCH_NODES, 2, "Radio", "radio");
    t.add(FOXAPI_LIST_PATCH_NODES, 4, "Tower", "channel", kRefused);
    t.add(FOXAPI_LIST_PATCH_DEVICES, 1, "Signal generator", "siggen");
    t.add(FOXAPI_LIST_PATCH_DEVICES, 2, "NESDR Smart", "rtlsdr|serial=1");
    t.add(FOXAPI_LIST_PATCH_DEVICES, 3, "Recording a", "iqfile|path=C:\\rec\\a.cu8", 0, 2.4e6);
    t.add(FOXAPI_LIST_PATCH_DEVICES, 4, "Long one", std::string(159, 'k').c_str());
    t.add(FOXAPI_LIST_BOOKMARKS, 1, "a", "");
    t.add(FOXAPI_LIST_BOOKMARKS, 2, "b", "KORD Chicago O'Hare");
    t.add(FOXAPI_LIST_BOOKMARKS, 3, "c", "KORD Chicago O'Hare");
    t.add(FOXAPI_LIST_BOOKMARKS, 4, "d", "EGLL London");
    t.add(FOXAPI_LIST_AUDIO_DEVICES, 1, "Speakers (Realtek)", "");
    t.add(FOXAPI_LIST_AUDIO_DEVICES, 2, "HDMI", "");
    t.facts["fact.patch.runnable"] = "1";
    t.facts["fact.patch.conv.1"] = "2 125000000 1";
    t.facts["fact.patch.conv.2"] = "3 1 0";                       // mode 3: not one
    t.facts["fact.patch.device.4"] = std::string(200, 'k');
    t.facts["fact.patch.receiverRadio"] = "rtlsdr|serial=1";
    t.facts["fact.patch.receiverCentre"] = "100000000";
    t.facts["fact.recorder.dir"] = "C:\\Rec";
}

void listsAreRead() {
    StubTable t;
    fillLists(t);
    PatchLists l;
    readLists(t, 5, l);
    // the parts bin
    EQ(l.parts.size(), std::size_t{3});
    EQ(at(l.parts, 0).key, std::string("acars_1.dll"));
    EQ(at(l.parts, 0).name, std::string("ACARS"));
    CHECK(at(l.parts, 0).feed == PortType::Audio && !at(l.parts, 0).image && !at(l.parts, 0).tracks && at(l.parts, 0).requiredRateHz == 0.0);
    CHECK(at(l.parts, 1).feed == PortType::Iq && at(l.parts, 1).tracks && at(l.parts, 1).requiredRateHz == 2.0e6 && !at(l.parts, 1).image);
    CHECK(at(l.parts, 2).image && at(l.parts, 2).key == "sstv_1.dll#image" && at(l.parts, 2).requiredRateHz == 12000.0);
    // the plan
    EQ(l.plan.channels.size(), std::size_t{1});
    CHECK(at(l.plan.channels, 0).node == 3u && at(l.plan.channels, 0).offsetHz == 100000.0 && at(l.plan.channels, 0).outRateHz == 48000.0 && at(l.plan.channels, 0).decimation == 42u && at(l.plan.channels, 0).radio == 1u);
    EQ(l.plan.decoders.size(), std::size_t{3});
    CHECK(at(l.plan.decoders, 0).node == 5u && at(l.plan.decoders, 0).source == pc::DecoderSource::Channel && at(l.plan.decoders, 0).rateHz == 48000.0 && at(l.plan.decoders, 0).inRateHz == 48000.0 && at(l.plan.decoders, 0).centreHz == 100.1e6 && at(l.plan.decoders, 0).radio == 1u);
    CHECK(at(l.plan.decoders, 1).node == 6u && at(l.plan.decoders, 1).source == pc::DecoderSource::Radio && at(l.plan.decoders, 1).rateHz == 2.0e6);
    CHECK(at(l.plan.decoders, 2).node == 7u && at(l.plan.decoders, 2).source == pc::DecoderSource::Audio);
    // the problems: a code this window cannot word is left out
    EQ(l.plan.problems.size(), std::size_t{3});
    CHECK(at(l.plan.problems, 0).node == 3u && at(l.plan.problems, 0).problem == pc::Problem::NoFrequency);
    CHECK(at(l.plan.problems, 1).node == 4u && at(l.plan.problems, 1).problem == pc::Problem::NothingFeedsIt);
    CHECK(at(l.plan.problems, 2).node == 6u && at(l.plan.problems, 2).problem == pc::Problem::NoTracks);
    CHECK(l.plan.runnable);
    // the radios
    EQ(l.radios.size(), std::size_t{2});
    EQ(l.radios_open, 1);
    const RadioRow& r1 = l.radios[1];
    CHECK(r1.device == "siggen" && r1.open && !r1.fault && !r1.pending && r1.rate_hz == 2.0e6 && r1.centre_hz == 100.0e6 && r1.note.empty());
    const RadioRow& r2 = l.radios[2];
    CHECK(r2.device == "rtlsdr|serial=1" && !r2.open && r2.fault && r2.pending && r2.note == "the device would not open");
    // the nodes: the refused ones, the converter of a radio (a mode the window does not know is no converter)
    EQ(l.refused, std::vector<pc::NodeId>({4u}));
    EQ(l.conv.size(), std::size_t{1});
    CHECK(l.conv.count(1) == 1 && l.conv[1].mode == cc::ConverterMode::Down && l.conv[1].loHz == 125000000.0 && l.conv[1].inverted);
    // the devices, a cut key read whole, the receiver's radio and its tuning
    EQ(l.devices.size(), std::size_t{4});
    CHECK(at(l.devices, 0).label == "Signal generator" && at(l.devices, 0).key == "siggen" && at(l.devices, 0).rate_hz == 0.0);
    CHECK(at(l.devices, 2).key == "iqfile|path=C:\\rec\\a.cu8" && at(l.devices, 2).rate_hz == 2.4e6);
    EQ(at(l.devices, 3).key, std::string(200, 'k'));
    EQ(l.receiver_radio, std::string("rtlsdr|serial=1"));
    CHECK(l.receiver_tuned);
    // what the page also reads
    EQ(l.bookmark_groups, Names({"KORD Chicago O'Hare", "EGLL London"}));
    EQ(l.audio_devices, Names({"Speakers (Realtek)", "HDMI"}));
    EQ(l.recorder_dir, std::string("C:\\Rec"));
    EQ(l.list_seq, std::uint64_t{5});

    // THE SAME listSeq IS NOT READ AGAIN; a new one is
    t.lists[FOXAPI_LIST_PATCH_PARTS].clear();
    t.facts["fact.patch.runnable"] = "0";
    readLists(t, 5, l);
    EQ(l.parts.size(), std::size_t{3});
    CHECK(l.plan.runnable);
    readLists(t, 6, l);
    EQ(l.parts.size(), std::size_t{0});
    CHECK(!l.plan.runnable);
    EQ(l.list_seq, std::uint64_t{6});

    // a converter fact of a radio the table no longer lists is not kept; no tuning = not tuned
    t.facts["fact.patch.receiverCentre"] = "";
    readLists(t, 7, l);
    CHECK(!l.receiver_tuned);
    CHECK(l.conv.size() == 1);
}

void liveIsRead() {
    StubTable t;
    t.add(FOXAPI_LIST_PATCH_LIVE, 4, "Speaker out", "", kActive, 0.25, 96000, 2);
    t.add(FOXAPI_LIST_PATCH_LIVE, 4, "", "No such device", 0, 0, 0, 5);
    t.add(FOXAPI_LIST_PATCH_LIVE, 3, "", "", 0, -61.5, 0, 3);
    t.add(FOXAPI_LIST_PATCH_LIVE, 8, "", "last line said", 0, 12, 0, 4);
    t.add(FOXAPI_LIST_PATCH_LIVE, 9, "caption", "", kActive, 7, 1234, 6);
    const std::vector<LiveRow> rows = readLive(t);
    EQ(rows.size(), std::size_t{5});
    CHECK(at(rows, 0).node == 4u && at(rows, 0).kind == 2 && at(rows, 0).v0 == 0.25 && at(rows, 0).v1 == 96000.0 && at(rows, 0).open && at(rows, 0).name == "Speaker out");
    CHECK(at(rows, 1).kind == 5 && at(rows, 1).detail == "No such device" && !at(rows, 1).open);
    CHECK(at(rows, 3).node == 8u && at(rows, 3).kind == 4 && at(rows, 3).v0 == 12.0 && at(rows, 3).detail == "last line said");
    // liveRow(kind, node): the pair, never the node alone
    const LiveRow* a = liveRow(rows, 2, 4);
    CHECK(a != nullptr && a->name == "Speaker out");
    const LiveRow* b = liveRow(rows, 5, 4);
    CHECK(b != nullptr && b->detail == "No such device");
    CHECK(liveRow(rows, 3, 4) == nullptr);
    CHECK(liveRow(rows, 2, 8) == nullptr);
    CHECK(liveRow(rows, 6, 9) != nullptr);
    StubTable none;
    CHECK(readLive(none).empty());
}

void lookIsRead() {
    const Chain c = chain();
    StubTable t;
    PatchLook look;
    // NO PUBLISH YET: an empty version is invalid and the next listSeq asks again
    readLook(t, 1, look);
    CHECK(!look.valid);
    EQ(look.list_seq, std::uint64_t{1});
    EQ(t.factReads["fact.patch.graph"], 0);
    // a publish
    t.facts["fact.patch.graph"] = pc::graphCommandText(c.g);
    t.facts["fact.patch.version"] = "7";
    t.facts["fact.patch.epoch"] = "2";
    readLook(t, 2, look);
    CHECK(look.valid);
    CHECK(look.version == 7u && look.epoch == 2u);
    CHECK(pc::graphsEqual(look.graph, c.g));
    EQ(t.factReads["fact.patch.graph"], 1);
    EQ(t.factReads["fact.patch.version"], 3);   // 1 (empty) + 2 (the look and its check)
    // the same listSeq: nothing is read
    const int reads = t.factReads["fact.patch.version"];
    readLook(t, 2, look);
    EQ(t.factReads["fact.patch.version"], reads);
    // a listSeq that moved with the same version and epoch: the graph is NOT parsed again
    readLook(t, 3, look);
    EQ(t.factReads["fact.patch.graph"], 1);
    EQ(t.factReads["fact.patch.version"], reads + 1);
    CHECK(look.valid && look.list_seq == 3u);
    // a version that moved: the graph is read, then the version again
    pc::Graph g2 = c.g;
    g2.removeNode(c.speaker);
    t.facts["fact.patch.graph"] = pc::graphCommandText(g2);
    t.facts["fact.patch.version"] = "8";
    readLook(t, 4, look);
    EQ(t.factReads["fact.patch.graph"], 2);
    CHECK(look.version == 8u && pc::graphsEqual(look.graph, g2));
    // an epoch that moved with the same version
    t.facts["fact.patch.epoch"] = "3";
    readLook(t, 5, look);
    EQ(t.factReads["fact.patch.graph"], 3);
    CHECK(look.epoch == 3u);

    // A PUBLISH BETWEEN THE READS: the two versions differ - invalid, and the next tick looks again (listSeq is not taken)
    PatchLook look2;
    StubTable t2;
    t2.facts["fact.patch.graph"] = pc::graphCommandText(c.g);
    t2.facts["fact.patch.version"] = "10";
    t2.facts["fact.patch.epoch"] = "1";
    t2.factHook = [&t2](const std::string& key) { if (key == "fact.patch.graph") { t2.facts["fact.patch.version"] = "11"; } };
    readLook(t2, 9, look2);
    CHECK(!look2.valid);
    EQ(look2.list_seq, ~std::uint64_t{0});
    t2.factHook = nullptr;
    readLook(t2, 9, look2);
    CHECK(look2.valid && look2.version == 11u);

    // A GRAPH THAT DOES NOT PARSE: invalid (and a warning in the log), not asked again until listSeq moves
    PatchLook look3;
    StubTable t3;
    t3.facts["fact.patch.graph"] = "this is not a patch";
    t3.facts["fact.patch.version"] = "1";
    t3.facts["fact.patch.epoch"] = "1";
    readLook(t3, 4, look3);
    CHECK(!look3.valid);
    EQ(look3.list_seq, std::uint64_t{4});
    const int graphReads = t3.factReads["fact.patch.graph"];
    readLook(t3, 4, look3);
    EQ(t3.factReads["fact.patch.graph"], graphReads);
    t3.facts["fact.patch.graph"] = pc::graphCommandText(c.g);
    readLook(t3, 5, look3);
    CHECK(look3.valid);
}

void smallRules() {
    EQ(std::string(kindCaption(NodeKind::Radio)), std::string("RADIO"));
    EQ(std::string(kindCaption(NodeKind::Channel)), std::string("CHANNEL"));
    EQ(std::string(kindCaption(NodeKind::Demod)), std::string("DEMOD"));
    EQ(std::string(kindCaption(NodeKind::Decoder)), std::string("DECODER"));
    EQ(std::string(kindCaption(NodeKind::Display)), std::string("DISPLAY"));
    EQ(std::string(kindCaption(NodeKind::Sink)), std::string("SINK"));
    EQ(std::string(kindCaption(NodeKind::Map)), std::string("MAP"));
    EQ(std::string(kindCaption(static_cast<NodeKind>(99))), std::string("NODE"));
    EQ(std::string(refusalText(pc::Connect::Ok)), std::string(""));
    EQ(std::string(refusalText(pc::Connect::UnknownNode)), std::string("that node is gone"));
    EQ(std::string(refusalText(pc::Connect::NoSuchPort)), std::string("there is no port there"));
    EQ(std::string(refusalText(pc::Connect::TypeMismatch)), std::string("those two carry different things"));
    EQ(std::string(refusalText(pc::Connect::InputOccupied)), std::string("that input already has a wire"));
    EQ(std::string(refusalText(pc::Connect::AlreadyWired)), std::string("those are already wired together"));
    EQ(std::string(refusalText(pc::Connect::SelfLoop)), std::string("a node cannot feed itself"));
    EQ(std::string(refusalText(pc::Connect::WouldCycle)), std::string("that would make a loop"));
    EQ(std::string(refusalText(static_cast<pc::Connect>(99))), std::string("that connection was refused"));
    EQ(portTone(PortType::Iq), 0);
    EQ(portTone(PortType::Audio), 1);
    EQ(portTone(PortType::Text), 2);
    EQ(portTone(PortType::Control), 3);
    EQ(portTone(PortType::Track), 4);
    EQ(partLabelEnglish(0), std::string("Radio"));
    EQ(partLabelEnglish(1), std::string("Channel"));
    EQ(partLabelEnglish(2), std::string("Demod"));
    EQ(partLabelEnglish(3), std::string("Spectrum"));
    EQ(partLabelEnglish(4), std::string("Speaker"));
    EQ(partLabelEnglish(5), std::string("Text out"));
    EQ(partLabelEnglish(6), std::string("Map"));
    EQ(partLabelEnglish(7), std::string(""));
    // the converter's name
    cc::ConverterSetting s;
    s.mode = cc::ConverterMode::Down;
    s.loHz = 125.0e6;
    EQ(converterNameText(s), std::string("125 MHz down-converter"));
    s.inverted = true;
    EQ(converterNameText(s), std::string("125 MHz down-converter, spectrum inverted"));
    s.mode = cc::ConverterMode::Up;
    EQ(converterNameText(s), std::string("125 MHz up-converter, spectrum inverted"));
    s.inverted = false;
    EQ(converterNameText(s), std::string("125 MHz up-converter"));
    // a MHz box's text
    double v = 0;
    CHECK(parseMhz("100.1", v) && v == 100.1);
    CHECK(parseMhz("  7 ", v) && v == 7.0);
    CHECK(parseMhz("0", v) && v == 0.0);
    CHECK(parseMhz("-0.28364", v) && v == -0.28364);
    CHECK(!parseMhz("", v) && !parseMhz("abc", v) && !parseMhz("1.2abc", v) && !parseMhz("nan", v) && !parseMhz("inf", v) && !parseMhz("  ", v));
    // a name cut to 63 bytes on a character boundary
    EQ(cutBytes(std::string(70, 'a'), 63), std::string(63, 'a'));
    EQ(cutBytes(std::string(62, 'a') + "\xC3\xA9", 63), std::string(62, 'a'));
    EQ(cutBytes("short", 63), std::string("short"));
    EQ(cutBytes(std::string(61, 'a') + "\xC3\xA9", 63), std::string(61, 'a') + "\xC3\xA9");
    // the document the config keeps: serialise with pan and zoom
    pc::Graph g = chain().g;
    pg::View view;
    view.pan = (pg::Vec2{12.0f, -34.0f});
    view.zoom = 0.75f;
    const std::string doc = documentOf(g, view);
    EQ(doc, pc::serialise(g, 12.0f, -34.0f, 0.75f));
    const pc::LoadResult lr = pc::parse(doc);
    CHECK(lr.ok && lr.panX == 12.0f && lr.panY == -34.0f && lr.zoom == 0.75f);
}

void opsAreTheWindows() {
    // START / STOP: FOXAPI_OP_PATCH_RUN with the value the new state wants
    Command c = opRun(0);
    CHECK(c.valid);
    CHECK(fc(c).op == 0x0902u && fc(c).ival[0] == 1);
    c = opRun(FOXAPI_RX_PATCH_RUNNING);
    CHECK(c.valid && fc(c).op == 0x0902u && fc(c).ival[0] == 0);
    CHECK(c.grant == foxsdr::slintsignal::grantFor(0x0902u));
    c = opAllOff();
    CHECK(c.valid && fc(c).op == 0x0903u && fc(c).ival[0] == 0 && fc(c).ival[1] == 0 && textOf(c).empty());
    c = opScanNativeOnly();
    CHECK(c.valid && fc(c).op == 0x8406u && fc(c).ival[0] == 0 && textOf(c).empty());
    EQ(c.grant, foxsdr::slintsignal::grantFor(0x8406u));
    // the device list opened: the wish, then the recordings, in this order
    std::vector<Command> v = opDeviceOpened();
    EQ(v.size(), std::size_t{2});
    CHECK(fc(at(v, 0)).op == 0x8407u && fc(at(v, 1)).op == 0x8422u);
    // Look for radios: nothing while a scan is running; else the wish, the scan itself, the recordings
    EQ(opLook(FOXAPI_SRC_SCAN_PENDING).size(), std::size_t{0});
    EQ(opLook(FOXAPI_SRC_SCAN_PENDING | FOXAPI_SRC_CARD_OPENING).size(), std::size_t{0});
    v = opLook(FOXAPI_SRC_CARD_OPENING);
    EQ(v.size(), std::size_t{3});
    CHECK(fc(at(v, 0)).op == 0x8413u && fc(at(v, 1)).op == 0x0301u && fc(at(v, 2)).op == 0x8422u);
    for (const Command& k : v) { CHECK(k.valid && k.grant == foxsdr::slintsignal::grantFor(fc(k).op)); }
    // the view-shown flag
    c = opViewShown(true);
    CHECK(c.valid && fc(c).op == 0x8421u && fc(c).ival[0] == 1);
    c = opViewShown(false);
    CHECK(c.valid && fc(c).op == 0x8421u && fc(c).ival[0] == 0);
    // a picture: whole when width * height * bytes-per-pixel is the bytes got
    CHECK(pictureWhole(CASCADE_IMAGE_RGB24, 10, 4, 120));
    CHECK(!pictureWhole(CASCADE_IMAGE_RGB24, 10, 4, 119));
    CHECK(pictureWhole(CASCADE_IMAGE_GRAY8, 10, 4, 40));
    CHECK(!pictureWhole(CASCADE_IMAGE_GRAY8, 10, 4, 120));
    CHECK(!pictureWhole(CASCADE_IMAGE_RGB24, 0, 4, 0));
    CHECK(!pictureWhole(77, 10, 4, 40));
}

}  // namespace

namespace {

// ================================================================================================================================
// the draft machine
// ================================================================================================================================

PatchLook mkLook(const pc::Graph& g, std::uint64_t version, std::uint64_t epoch, bool valid = true) {
    PatchLook l;
    l.valid = valid;
    l.version = version;
    l.epoch = epoch;
    l.graph = g;
    return l;
}

void draftAdoptsTheFirstLook() {
    const Chain c = chain();
    PatchDraft d;
    PatchLists lists;
    RecSub sub;
    // NEVER A DEFAULT: a look that was not published is not in step with anything, and nothing is committed from the draft
    d.sync(mkLook(c.g, 1, 1, false), lists, false);
    CHECK(!d.inStep());
    d.graph().addNode(NodeKind::Radio, "Radio", PortType::Iq, 1, 1);
    d.commit(mkLook(c.g, 1, 1, false), sub);
    CHECK(sub.sent.empty() && d.send().state == PatchDraft::Send::State::Idle);
    // the first valid look is adopted whole
    PatchDraft e;
    const std::uint64_t serial = e.forgetSerial();
    e.sync(mkLook(c.g, 4, 2), lists, false);
    CHECK(e.inStep() && e.version() == 4u && e.epoch() == 2u);
    CHECK(pc::graphsEqual(e.graph(), c.g) && pc::graphsEqual(e.base(), c.g));
    CHECK(!e.dirty);                         // the first adoption is not a change
    EQ(e.forgetSerial(), serial);            // ...and drops no interaction
    // an engine that did not change leaves it alone
    e.sync(mkLook(c.g, 4, 2), lists, false);
    CHECK(!e.dirty && e.version() == 4u);
}

void draftCommitsAnEdit() {
    const Chain c = chain();
    PatchDraft d;
    PatchLists lists;
    RecSub sub;
    const PatchLook look = mkLook(c.g, 4, 2);
    d.sync(look, lists, false);
    // nothing edited: no command
    d.commit(look, sub);
    CHECK(sub.sent.empty());
    // an edit: EXACTLY FOXAPP_OP_PATCH_SET_GRAPH 0x841B, the draft's command text, ival[0] 0 (an edit, not a document), ival[1] the version the draft is in step with
    nm(d.graph(), c.radio).x = 200.0f;
    d.commit(look, sub);
    EQ(sub.sent.size(), std::size_t{1});
    const Command& k = cmdAt(sub, 0);
    CHECK(k.valid && fc(k).op == 0x841Bu && fc(k).op == kPatchSetGraph);
    EQ(textOf(k), pc::graphCommandText(d.graph()));
    EQ(fc(k).ival[0], std::int64_t{0});
    EQ(fc(k).ival[1], std::int64_t{4});
    EQ(k.grant, foxsdr::slintsignal::grantFor(0x841Bu));
    CHECK(d.send().state == PatchDraft::Send::State::Sent && d.send().ticket == 100u && !d.send().document);
    CHECK(pc::graphsEqual(d.send().sent, d.graph()));
    // ONE COMMIT IN FLIGHT: another edit stays in the draft
    nm(d.graph(), c.channel).x = 500.0f;
    d.commit(look, sub);
    EQ(sub.sent.size(), std::size_t{1});
    // the text carries the next id and a centre chosen
    CHECK(has(textOf(k), "next-id ") && has(textOf(k), "centre-chosen 1"));

    // A TICKET OF 0 = NOT TAKEN: the edit stays and goes next time
    PatchDraft e;
    RecSub refuse;
    refuse.refuse = true;
    e.sync(look, lists, false);
    nm(e.graph(), c.radio).x = 5.0f;
    e.commit(look, refuse);
    EQ(refuse.sent.size(), std::size_t{1});
    CHECK(e.send().state == PatchDraft::Send::State::Idle);
    refuse.refuse = false;
    e.commit(look, refuse);
    EQ(refuse.sent.size(), std::size_t{2});
    CHECK(e.send().state == PatchDraft::Send::State::Sent);

    // THE NODE'S SETTINGS TRAVEL: the text is what the engine will parse back to the draft
    pc::Graph back;
    std::string why;
    CHECK(pc::graphFromCommandText(textOf(cmdAt(refuse, 1)), back, why) && pc::graphsEqual(back, e.graph()));
}

void draftSettlesAnAnswer() {
    const Chain c = chain();
    PatchDraft d;
    PatchLists lists;
    RecSub sub;
    const PatchLook look = mkLook(c.g, 2, 1);
    d.sync(look, lists, false);
    nm(d.graph(), c.radio).x = 200.0f;
    d.commit(look, sub);
    // an answer for another ticket, or while idle, is nobody's
    PatchDraft idle;
    FoxCommandResult stray{};
    stray.ticket = 100;
    stray.status = FOXAPI_OK;
    idle.onAnswer(stray, look);
    CHECK(idle.send().state == PatchDraft::Send::State::Idle && !idle.adoptWanted());
    FoxCommandResult other{};
    other.ticket = 999;
    other.status = FOXAPI_FAILED;
    d.onAnswer(other, look);
    CHECK(d.send().state == PatchDraft::Send::State::Sent && !d.adoptWanted());
    // an edit made while the commit is out
    nm(d.graph(), c.channel).x = 500.0f;
    // THE ANSWER: OK, the version it is now (3), merged
    FoxCommandResult ok{};
    ok.ticket = 100;
    ok.status = FOXAPI_OK;
    ok.applied[0] = 3.0;
    ok.applied[1] = 1.0;
    d.onAnswer(ok, look);
    CHECK(d.send().state == PatchDraft::Send::State::Applied);
    EQ(d.send().new_version, std::uint64_t{3});
    CHECK(d.send().merged);
    // the publish that shows it is not read yet: waits (and no second commit goes)
    d.sync(look, lists, false);
    CHECK(d.send().state == PatchDraft::Send::State::Applied);
    d.commit(look, sub);
    EQ(sub.sent.size(), std::size_t{1});
    // the engine's graph as it now is: the radio moved, and the engine added a decoder of its own
    pc::Graph engine = c.g;
    engine.mutableNode(c.radio)->x = 200.0f;
    const pc::NodeId extra = engine.addNode(NodeKind::Display, "Spectrum", PortType::Iq, 40, 400);
    const PatchLook look3 = mkLook(engine, 3, 1);
    d.dirty = false;
    d.sync(look3, lists, false);
    CHECK(d.send().state == PatchDraft::Send::State::Idle);
    EQ(d.version(), std::uint64_t{3});
    CHECK(pc::graphsEqual(d.base(), engine));
    CHECK(d.graph().find(extra) != nullptr);                     // what the engine added is in the draft
    CHECK(nd(d.graph(), c.channel).x == 500.0f);               // the edit made meanwhile is on top
    CHECK(nd(d.graph(), c.radio).x == 200.0f);
    CHECK(d.dirty);                                              // the config's document follows
    // the rest of the edit goes with the next commit, from the new base
    d.commit(look3, sub);
    EQ(sub.sent.size(), std::size_t{2});
    EQ(fc(cmdAt(sub, 1)).ival[1], std::int64_t{3});
    CHECK(has(textOf(cmdAt(sub, 1)), "500"));
}

void draftRefusedAnswerAdoptsTheEngine() {
    const Chain c = chain();
    PatchDraft d;
    PatchLists lists;
    RecSub sub;
    const PatchLook look = mkLook(c.g, 2, 1);
    d.sync(look, lists, false);
    nm(d.graph(), c.radio).x = 200.0f;
    d.commit(look, sub);
    FoxCommandResult bad{};
    bad.ticket = 100;
    bad.status = FOXAPI_FAILED;
    std::snprintf(bad.message, sizeof(bad.message), "no");
    d.onAnswer(bad, look);
    CHECK(d.send().state == PatchDraft::Send::State::Idle);
    CHECK(d.adoptWanted());
    // an edit while the adoption is wanted is not sent: the page shows the patch as the engine holds it
    nm(d.graph(), c.channel).x = 5.0f;
    d.commit(look, sub);
    EQ(sub.sent.size(), std::size_t{1});
    CHECK(!d.adoptWanted());
    CHECK(pc::graphsEqual(d.graph(), c.g));
    // and with an adoption wanted, sync adopts first
    PatchDraft e;
    e.sync(look, lists, false);
    nm(e.graph(), c.radio).x = 9.0f;
    RecSub sub2;
    e.commit(look, sub2);
    FoxCommandResult bad2{};
    bad2.ticket = 100;
    bad2.status = FOXAPI_FAILED;
    e.onAnswer(bad2, look);
    CHECK(e.adoptWanted());
    e.sync(look, lists, false);
    CHECK(!e.adoptWanted() && pc::graphsEqual(e.graph(), c.g));
}

void draftDropsAnEditOfAnOldPatch() {
    const Chain c = chain();
    PatchDraft d;
    PatchLists lists;
    RecSub sub;
    d.sync(mkLook(c.g, 2, 1), lists, false);
    nm(d.graph(), c.radio).x = 200.0f;
    // A NEW PATCH was loaded under the edit (another epoch): a commit would send the old patch's edit over it. It goes.
    pc::Graph fresh;
    fresh.addNode(NodeKind::Radio, "Other", PortType::Iq, 1, 2);
    d.commit(mkLook(fresh, 5, 2), sub);
    CHECK(sub.sent.empty());
    CHECK(pc::graphsEqual(d.graph(), fresh) && d.epoch() == 2u && d.version() == 5u);
    // the interaction of the old patch is forgotten with it
    CHECK(d.forgetSerial() == 1u);

    // the same through sync: an epoch change with no edit adopts and forgets
    PatchDraft e;
    e.sync(mkLook(c.g, 2, 1), lists, false);
    e.addWait(c.radio, 7);
    e.sync(mkLook(fresh, 3, 2), lists, false);
    CHECK(pc::graphsEqual(e.graph(), fresh) && e.epoch() == 2u);
    EQ(e.forgetSerial(), std::uint64_t{1});
    CHECK(e.waits().empty());                // a Radio part waiting for its scan was of the old patch
    CHECK(e.dirty);                          // the engine changed it: the config's document follows

    // an edit sent, answered, and a NEW EPOCH published meanwhile: the edit was of the old patch and goes
    PatchDraft f;
    RecSub sub2;
    f.sync(mkLook(c.g, 2, 1), lists, false);
    nm(f.graph(), c.radio).x = 200.0f;
    f.commit(mkLook(c.g, 2, 1), sub2);
    FoxCommandResult ok{};
    ok.ticket = 100;
    ok.status = FOXAPI_OK;
    ok.applied[0] = 6;
    f.onAnswer(ok, mkLook(c.g, 2, 1));
    f.settle(mkLook(fresh, 6, 2));
    CHECK(f.send().state == PatchDraft::Send::State::Idle);
    CHECK(pc::graphsEqual(f.graph(), fresh) && f.epoch() == 2u);
}

void draftFollowsTheEngine() {
    const Chain c = chain();
    PatchDraft d;
    PatchLists lists;
    d.sync(mkLook(c.g, 2, 1), lists, false);
    // the engine changed it (START's switches, a centre the radio reported) and the draft has no edit: adopted, and the config follows
    pc::Graph e1 = c.g;
    e1.mutableNode(c.radio)->on = false;
    d.sync(mkLook(e1, 3, 1), lists, false);
    CHECK(d.version() == 3u && pc::graphsEqual(d.graph(), e1) && pc::graphsEqual(d.base(), e1));
    CHECK(d.dirty);
    // with an edit not committed yet, the edit is kept on top
    d.dirty = false;
    nm(d.graph(), c.channel).x = 777.0f;
    pc::Graph e2 = e1;
    e2.mutableNode(c.radio)->freqHz = 99.0e6;
    d.sync(mkLook(e2, 4, 1), lists, false);
    CHECK(d.version() == 4u && pc::graphsEqual(d.base(), e2));
    CHECK(nd(d.graph(), c.channel).x == 777.0f && nd(d.graph(), c.radio).freqHz == 99.0e6 && !nd(d.graph(), c.radio).on);
    CHECK(d.dirty);
    // NEVER MID-DRAG: the node under the pointer would jump back for a frame
    pc::Graph e3 = e2;
    e3.mutableNode(c.radio)->x = 1.0f;
    d.dirty = false;
    d.sync(mkLook(e3, 5, 1), lists, true);
    CHECK(d.version() == 4u && !d.dirty && nd(d.graph(), c.radio).x == c.g.find(c.radio)->x);
    d.sync(mkLook(e3, 5, 1), lists, false);
    CHECK(d.version() == 5u);
}

void draftLoadsADocument() {
    const Chain c = chain();
    PatchDraft d;
    PatchLists lists;
    RecSub sub;
    d.sync(mkLook(c.g, 2, 1), lists, false);
    d.addWait(c.radio, 55);
    // the document: SET_GRAPH with FOXAPP_PATCH_GRAPH_DOCUMENT in ival[0] and ival[1] 0 = unconditional
    pc::Graph doc;
    const pc::NodeId r = doc.addNode(NodeKind::Radio, "Doc radio", PortType::Iq, 10, 20);
    doc.mutableNode(r)->device = "siggen";
    const std::uint64_t serial = d.forgetSerial();
    CHECK(d.loadDocument(doc, sub));
    EQ(sub.sent.size(), std::size_t{1});
    const Command& k = cmdAt(sub, 0);
    CHECK(fc(k).op == 0x841Bu && fc(k).ival[0] == FOXAPP_PATCH_GRAPH_DOCUMENT && fc(k).ival[0] == 1 && fc(k).ival[1] == 0);
    EQ(textOf(k), pc::graphCommandText(doc));
    CHECK(pc::graphsEqual(d.graph(), doc) && pc::graphsEqual(d.base(), doc));
    EQ(d.version(), std::uint64_t{0});
    CHECK(d.inStep());
    CHECK(d.send().state == PatchDraft::Send::State::Sent && d.send().document && d.send().ticket == 100u);
    CHECK(d.forgetSerial() > serial);
    CHECK(d.waits().empty());
    CHECK(!d.adoptWanted());
    // nothing is committed over a document in flight
    nm(d.graph(), r).x = 99.0f;
    d.commit(mkLook(c.g, 2, 1), sub);
    EQ(sub.sent.size(), std::size_t{1});
    // the answer, and the publish that shows it: adopted WHOLE (the engine added the next id, say), and it is the page's own doing: the config is not told to follow
    FoxCommandResult ok{};
    ok.ticket = 100;
    ok.status = FOXAPI_OK;
    ok.applied[0] = 5;
    d.onAnswer(ok, mkLook(c.g, 2, 1));
    CHECK(d.send().state == PatchDraft::Send::State::Applied);
    pc::Graph shown = doc;
    shown.mutableNode(r)->on = false;
    d.dirty = false;
    d.sync(mkLook(shown, 5, 3), lists, false);
    CHECK(d.send().state == PatchDraft::Send::State::Idle);
    CHECK(pc::graphsEqual(d.graph(), shown) && d.version() == 5u && d.epoch() == 3u);
    CHECK(!d.dirty);

    // A DOCUMENT THE SESSION WOULD NOT TAKE: false, the draft is not the document
    PatchDraft e;
    RecSub refuse;
    refuse.refuse = true;
    e.sync(mkLook(c.g, 2, 1), lists, false);
    CHECK(!e.loadDocument(doc, refuse));
    CHECK(pc::graphsEqual(e.graph(), c.g) && e.send().state == PatchDraft::Send::State::Idle);
}

void draftChoosesTheRadiosOfWaitingParts() {
    PatchDraft d;
    PatchLists lists;
    lists.devices = {{"Signal generator", "siggen", 0}, {"NESDR", "rtlsdr|serial=1", 0}, {"Airspy", "airspy|serial=9", 0}};
    lists.receiver_radio = "";
    pc::Graph g;
    const pc::NodeId a = g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 1, 1);
    const pc::NodeId b = g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 2, 2);
    const pc::NodeId keep = g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 3, 3);
    g.mutableNode(keep)->device = "siggen";
    d.sync(mkLook(g, 2, 1), lists, false);
    d.addWait(a, 10);
    d.addWait(b, 11);
    d.addWait(keep, 12);
    // before the scan has answered nothing is chosen
    d.noteScanAnswer(9);
    d.sync(mkLook(g, 2, 1), lists, false);
    CHECK(nd(d.graph(), a).device.empty() && nd(d.graph(), b).device.empty());
    CHECK(d.waits().size() == 3u);
    // the answer for the first press: only the first (in the order pressed), the second still waits
    d.noteScanAnswer(10);
    d.sync(mkLook(g, 2, 1), lists, false);
    EQ(nd(d.graph(), a).device, std::string("rtlsdr|serial=1"));
    CHECK(nd(d.graph(), b).device.empty());
    CHECK(d.dirty);
    CHECK(d.waits().size() == 2u);
    // a later answer covers the rest, each taking the first device nothing holds; a node that already has a device keeps it
    d.noteScanAnswer(12);
    d.sync(mkLook(g, 2, 1), lists, false);
    EQ(nd(d.graph(), b).device, std::string("airspy|serial=9"));
    EQ(nd(d.graph(), keep).device, std::string("siggen"));
    CHECK(d.waits().empty());
    // a smaller ticket never lowers the answer
    d.noteScanAnswer(3);
    EQ(d.scanDone(), std::uint64_t{12});
}

void defaultDeviceOrder() {
    PatchLists lists;
    lists.devices = {{"Signal generator", "siggen", 0}, {"Recording", "iqfile|path=C:\\a.cu8", 2.4e6}, {"NESDR", "rtlsdr|serial=1", 0}, {"Airspy", "airspy|serial=9", 0}};
    pc::Graph g;
    // no receiver radio: the first list row that is neither the generator nor a recording
    EQ(defaultDeviceKey(g, lists), std::string("rtlsdr|serial=1"));
    // the receiver's own radio first
    lists.receiver_radio = "airspy|serial=9";
    EQ(defaultDeviceKey(g, lists), std::string("airspy|serial=9"));
    // taken by a Radio node (sameDevice, a serial match across spellings): the next
    const pc::NodeId r1 = g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 0, 0);
    g.mutableNode(r1)->device = "airspy|serial=9,label=whatever";
    EQ(defaultDeviceKey(g, lists), std::string("rtlsdr|serial=1"));
    const pc::NodeId r2 = g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 0, 0);
    g.mutableNode(r2)->device = "rtlsdr|serial=1";
    // every radio held: the generator
    EQ(defaultDeviceKey(g, lists), std::string("siggen"));
    // a non-Radio node holding the same text does not hold a radio
    pc::Graph h;
    const pc::NodeId s = h.addNode(NodeKind::Sink, "Speaker", PortType::Audio, 0, 0);
    h.mutableNode(s)->device = "rtlsdr|serial=1";
    PatchLists l2 = lists;
    l2.receiver_radio.clear();
    EQ(defaultDeviceKey(h, l2), std::string("rtlsdr|serial=1"));
    // nothing listed at all: the generator
    EQ(defaultDeviceKey(pc::Graph{}, PatchLists{}), std::string("siggen"));
}

}  // namespace

namespace {

// ================================================================================================================================
// the tick: the offer, the restore, the seed, the config's document
// ================================================================================================================================

void viewShownIsOffered() {
    StubTable t;
    PatchPage page;
    RecSub sub;
    FoxReceiverState st = mkState();
    // THE SLINT WINDOW STARTS ON THE RECEIVER and the engine believes "showing": the first tick offers 0
    page.tick(t, st, sub, false);
    EQ(sub.offered.size(), std::size_t{1});
    CHECK(fc(at(sub.offered, 0)).op == 0x8421u && fc(at(sub.offered, 0)).ival[0] == 0);
    EQ(at(sub.offered, 0).grant, foxsdr::slintsignal::grantFor(0x8421u));
    // only on a difference from the last one TAKEN
    page.tick(t, st, sub, false);
    page.tick(t, st, sub, false);
    EQ(sub.offered.size(), std::size_t{1});
    // the page is shown: 1
    page.setViewShown(true);
    page.tick(t, st, sub, true);
    EQ(sub.offered.size(), std::size_t{2});
    CHECK(fc(at(sub.offered, 1)).ival[0] == 1);
    page.tick(t, st, sub, true);
    EQ(sub.offered.size(), std::size_t{2});
    // an offer the session would not take is repeated every tick until it is
    sub.refuseOffer = true;
    page.setViewShown(false);
    page.tick(t, st, sub, false);
    page.tick(t, st, sub, false);
    EQ(sub.offered.size(), std::size_t{4});
    CHECK(fc(at(sub.offered, 3)).ival[0] == 0);
    sub.refuseOffer = false;
    page.tick(t, st, sub, false);
    EQ(sub.offered.size(), std::size_t{5});
    page.tick(t, st, sub, false);
    EQ(sub.offered.size(), std::size_t{5});
    // an offer is not a submit: nothing was counted as sent
    EQ(sub.sent.size(), std::size_t{0});
}

void notShowingBuildsNothing() {
    Fx f(chain().g, true);
    f.page.setViewShown(false);
    f.tick(false);
    // the draft is synced anyway (a document, START and ALL OFF change the engine's graph whether the page is showing or not)
    CHECK(f.page.draft().inStep() && f.page.look().valid);
    CHECK(f.page.lists().list_seq == ~std::uint64_t{0});    // no lists read while not showing
    // ...but no seed (the window seeds only while it draws the view), no lists, no live rows
    CHECK(!f.page.seeded());
    // the engine changed the graph meanwhile: followed, not stale when the page is opened
    pc::Graph g2 = chain().g;
    g2.mutableNode(1)->on = false;
    f.publish(g2);
    f.tick(false);
    CHECK(!nd(f.page.draft().graph(), 1).on);
    // an edit through a callback while not showing still reaches the engine at the tick
    nm(f.page.draft().graph(), 1).x = 321.0f;
    f.sub.clear();
    f.tick(false);
    EQ(f.sub.count(kPatchSetGraph), std::size_t{1});
    f.engineTakes();                                       // the engine takes it: the draft is in step again
    // showing: the reads happen
    f.page.setViewShown(true);
    f.tickBump(true);
    CHECK(f.page.lists().list_seq == f.st.listSeq);        // the lists are read while showing
    CHECK(f.page.seeded());
}

void theSeedIsTheReceiversRadio() {
    Fx f(pc::Graph{}, true);
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 1, "Signal generator", "siggen");
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 2, "NESDR Smart", "rtlsdr|serial=1");
    f.t.facts["fact.patch.receiverRadio"] = "rtlsdr|serial=1";
    f.t.facts["fact.patch.receiverCentre"] = "145500000";
    f.st.flags = FOXAPI_RX_DEVICE_OPEN;
    f.st.centreHz = 145.5e6;
    f.tickBump();
    CHECK(f.page.seeded());
    const pc::Graph& g = f.page.draft().graph();
    EQ(g.nodes().size(), std::size_t{1});
    const pc::Node& n = at(g.nodes(), 0);
    CHECK(n.kind == NodeKind::Radio && n.name == "Radio" && n.x == 60.0f && n.y == 80.0f);
    EQ(n.device, std::string("rtlsdr|serial=1"));
    CHECK(n.freqHz == 145.5e6 && n.centreChosen);
    // the starter counts as a change, and is committed at once (the document is not: an edit, with the version it was made from)
    EQ(f.sub.count(kPatchSetGraph), std::size_t{1});
    const Command& k = cmdAt(f.sub, 0);
    EQ(fc(k).ival[0], std::int64_t{0});
    EQ(textOf(k), pc::graphCommandText(g));
    CHECK(has(textOf(k), "centre-chosen 1"));
    // ...and the config keeps it
    CHECK(f.cfg.patchSaves >= 1);
    CHECK(has(f.cfg.patchText, "foxsdr-patch"));
    // once only: ticking again does not seed a second radio
    f.tickBump();
    EQ(f.page.draft().graph().nodes().size(), std::size_t{1});
}

void theSeedWaitsForTheRadioToOpen() {
    Fx f(pc::Graph{}, true);
    f.st.flags = FOXAPI_RX_DEVICE_OPENING;
    f.tickBump();
    CHECK(!f.page.seeded() && f.page.draft().graph().nodes().empty());
    EQ(f.sub.sent.size(), std::size_t{0});
    f.st.flags = 0;
    f.st.sourceFlags = FOXAPI_SRC_CARD_OPENING;
    f.tickBump();
    CHECK(!f.page.seeded());
    f.st.sourceFlags = 0;
    f.tickBump();
    CHECK(f.page.seeded());
    // no radio of the receiver's open: the generator (nothing listed), and no centre to copy
    const pc::Node& n = at(f.page.draft().graph().nodes(), 0);
    EQ(n.device, std::string("siggen"));
    CHECK(n.freqHz == 0.0 && !n.centreChosen);
}

void theSeedCopiesNoCentreItWasNotTold() {
    // a radio open but the receiver never tuned (fact.patch.receiverCentre empty): no centre
    Fx f(pc::Graph{}, true);
    f.st.flags = FOXAPI_RX_DEVICE_OPEN;
    f.st.centreHz = 99.0e6;
    f.t.facts["fact.patch.receiverCentre"] = "";
    f.tickBump();
    CHECK(f.page.seeded());
    const pc::Node& n = at(f.page.draft().graph().nodes(), 0);
    CHECK(n.freqHz == 0.0 && !n.centreChosen);
    // tuned, but no device open: no centre either
    Fx g(pc::Graph{}, true);
    g.st.flags = 0;
    g.st.centreHz = 99.0e6;
    g.t.facts["fact.patch.receiverCentre"] = "99000000";
    g.tickBump();
    const pc::Node& m = at(g.page.draft().graph().nodes(), 0);
    CHECK(m.freqHz == 0.0 && !m.centreChosen);
    // the engine already holds a graph: the seed adds nothing to it
    Fx h(chain().g);
    EQ(h.page.draft().graph().nodes().size(), std::size_t{4});
    CHECK(h.page.seeded());
}

void theSavedPatchIsRestored() {
    // the config's saved patch: a document, with the pan and zoom it was saved at
    const Chain c = chain(true);
    pg::View saved;
    saved.pan = (pg::Vec2{40.0f, -25.0f});
    saved.zoom = 0.5f;
    Fx f(pc::Graph{}, true);
    f.cfg.patchText = documentOf(c.g, saved);
    f.tick();
    // sent as a DOCUMENT, once
    EQ(f.sub.sent.size(), std::size_t{1});
    CHECK(fc(cmdAt(f.sub, 0)).op == kPatchSetGraph && fc(cmdAt(f.sub, 0)).ival[0] == FOXAPP_PATCH_GRAPH_DOCUMENT && fc(cmdAt(f.sub, 0)).ival[1] == 0);
    CHECK(pc::graphsEqual(f.page.draft().graph(), pc::parse(f.cfg.patchText).graph));
    EQ(f.page.draft().graph().nodes().size(), c.g.nodes().size());
    CHECK(f.page.canvas().ui.view.pan.x == 40.0f && f.page.canvas().ui.view.pan.y == -25.0f && f.page.canvas().ui.view.zoom == 0.5f);
    // a restored, non-empty patch is not seeded over
    CHECK(f.page.seeded());
    f.tickBump();
    EQ(f.page.draft().graph().nodes().size(), c.g.nodes().size());
    EQ(f.sub.count(kPatchSetGraph), std::size_t{1});
    // the restore is tried once: ticking again does not send the document again
    f.tickBump();
    EQ(f.sub.sent.size(), std::size_t{1});
}

void theRestoreIsRetriedUntilTaken() {
    const Chain c = chain();
    Fx f(pc::Graph{}, true);
    f.cfg.patchText = documentOf(c.g, pg::View{});
    f.sub.refuse = true;
    f.tick();
    EQ(f.sub.sent.size(), std::size_t{1});
    CHECK(!f.page.seeded());
    f.sub.refuse = false;
    f.tickBump();
    CHECK(f.sub.sent.size() >= 2 && fc(cmdAt(f.sub, 1)).ival[0] == FOXAPP_PATCH_GRAPH_DOCUMENT);
    CHECK(f.page.seeded());
}

void aSavedPatchThatIsNotOneIsSeededOver() {
    Fx f(pc::Graph{}, true);
    f.cfg.patchText = "this is not a patch document";
    f.tick();
    // no document went; the starter radio did
    EQ(f.sub.count(kPatchSetGraph), std::size_t{1});
    CHECK(fc(cmdAt(f.sub, 0)).ival[0] == 0);
    CHECK(f.page.seeded() && f.page.draft().graph().nodes().size() == 1u);
    // an EMPTY saved patch is no restore either
    Fx g(pc::Graph{}, true);
    g.cfg.patchText = documentOf(pc::Graph{}, pg::View{});
    g.tick();
    // the document of an empty graph is loaded: nothing arrived, so nothing is seeded yet (a starter radio over the document in flight would be sent over it)
    CHECK(g.sub.count(kPatchSetGraph) == 1 && fc(cmdAt(g.sub, 0)).ival[0] == FOXAPP_PATCH_GRAPH_DOCUMENT);
    CHECK(!g.page.seeded() && g.page.draft().graph().nodes().empty());
    // ...and once the engine has taken it, the user who saved an empty patch gets the one-radio starter, as a fresh install does
    g.engineTakes();
    g.tickBump();
    CHECK(g.page.seeded());
    EQ(g.page.draft().graph().nodes().size(), std::size_t{1});
    CHECK(at(g.page.draft().graph().nodes(), 0).kind == NodeKind::Radio);
}

void aRestoredPatchThatLostWiresIsStillLoaded() {
    const Chain c = chain();
    Fx f(pc::Graph{}, true);
    f.cfg.patchText = documentOf(c.g, pg::View{}) + "wire 1 0 99 0\n";   // a wire to a node that is not there: dropped, the rest stands
    CHECK(pc::parse(f.cfg.patchText).dropped > 0);
    f.tick();
    EQ(f.sub.sent.size(), std::size_t{1});
    EQ(f.page.draft().graph().wires().size(), c.g.wires().size());
    CHECK(f.page.seeded());
}

void theConfigKeepsTheDocument() {
    Fx f(chain().g);
    f.page.draft().dirty = false;
    f.cfg.patchSaves = 0;
    f.tick();
    EQ(f.cfg.patchSaves, 0);                     // nothing changed: nothing written
    f.page.zoomStep(true);                       // a zoom
    f.tick();
    EQ(f.cfg.patchSaves, 1);
    const pc::LoadResult lr = pc::parse(f.cfg.patchText);
    CHECK(lr.ok && lr.zoom == f.page.canvas().ui.view.zoom && lr.zoom > 1.0f);
    EQ(f.cfg.patchText, documentOf(f.page.draft().graph(), f.page.canvas().ui.view));
    f.tick();
    EQ(f.cfg.patchSaves, 1);                     // once per change
    f.page.canvasWheel(100, 100, 0, 1, false, false);   // a pan
    f.tick();
    EQ(f.cfg.patchSaves, 2);
    // an edit of a node
    nm(f.page.draft().graph(), 1).name = "Moved";
    f.page.draft().dirty = true;
    f.tick();
    EQ(f.cfg.patchSaves, 3);
    CHECK(has(f.cfg.patchText, "Moved"));
}

}  // namespace

namespace {

// ================================================================================================================================
// the canvas: every gesture
// ================================================================================================================================

// radio R (40,40) -> channel C (320,40), a demod D (40,260), a speaker S (320,260); only R -> C is wired.
struct G {
    pc::Graph g;
    pc::NodeId r = 0, c = 0, d = 0, s = 0;
};
G gestureGraph() {
    G x;
    x.r = x.g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 40, 40);
    x.g.mutableNode(x.r)->device = "siggen";
    x.c = x.g.addNode(NodeKind::Channel, "Channel", PortType::Iq, 320, 40);
    x.d = x.g.addNode(NodeKind::Demod, "Demod", PortType::Iq, 40, 260);
    x.s = x.g.addNode(NodeKind::Sink, "Speaker", PortType::Audio, 320, 260);
    x.g.connect(x.r, 0, x.c, 0);
    return x;
}
pg::Vec2 outPort(const pc::Graph& g, pc::NodeId id, unsigned port = 0) { return pg::outputPortPos(*g.find(id), port); }
pg::Vec2 inPort(const pc::Graph& g, pc::NodeId id, unsigned port = 0) { return pg::inputPortPos(*g.find(id), port); }

// A drag from one port to another and its release; the page's refusal afterwards.
void dragPorts(Fx& f, pg::Vec2 from, pg::Vec2 to) {
    f.page.canvasPressed(from.x, from.y, 0);
    f.page.canvasMoved((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f);
    f.page.canvasMoved(to.x, to.y);
    f.page.canvasReleased(to.x, to.y, 0);
}

void pressOnAHeaderDragsTheNode() {
    const G x = gestureGraph();
    Fx f(x.g);
    f.page.draft().dirty = false;
    f.page.canvasPressed(100, 50, 0);
    EQ(f.page.canvas().ui.selected, x.r);
    EQ(f.page.canvas().ui.dragNode, x.r);
    CHECK(f.page.canvas().ui.grab == (pg::Vec2{60.0f, 10.0f}));
    CHECK(!f.page.canvas().ui.wireSelected && !f.page.canvas().ui.wiring && !f.page.canvas().ui.panning);
    CHECK(f.page.canvas().buttonDown() && f.page.canvas().interacting());
    // the node goes to the pointer less the grab, and the config follows
    f.page.canvasMoved(150, 90);
    CHECK(nd(f.g(), x.r).x == 90.0f && nd(f.g(), x.r).y == 80.0f);
    CHECK(f.page.draft().dirty);
    // ... while the engine's copy is a pass behind: the draft is committed at the tick, one command in flight
    f.sub.clear();
    f.tick();
    EQ(f.sub.count(kPatchSetGraph), std::size_t{1});
    f.page.canvasMoved(200, 100);
    f.tick();
    EQ(f.sub.count(kPatchSetGraph), std::size_t{1});   // not a second until the answer
    CHECK(nd(f.g(), x.r).x == 140.0f);
    f.page.canvasReleased(200, 100, 0);
    EQ(f.page.canvas().ui.dragNode, pc::NodeId{0});
    CHECK(!f.page.canvas().interacting() && !f.page.canvas().buttonDown());
    f.page.canvasMoved(300, 300);                        // no drag any more
    CHECK(nd(f.g(), x.r).x == 140.0f);
    EQ(f.page.canvas().ui.selected, x.r);

    // under a zoom and a pan: the pointer is in canvas pixels, the node in world units
    Fx h(x.g);
    h.page.canvas().setView(pg::View{(pg::Vec2{30.0f, 40.0f}), 2.0f});
    h.page.canvasPressed(130, 140, 0);                   // world (50, 50): R's header
    EQ(h.page.canvas().ui.dragNode, x.r);
    CHECK(h.page.canvas().ui.grab == (pg::Vec2{10.0f, 10.0f}));
    h.page.canvasMoved(230, 240);                        // world (100, 100)
    CHECK(nd(h.g(), x.r).x == 90.0f && nd(h.g(), x.r).y == 90.0f);
}

void pressOnABodySelectsOnly() {
    const G x = gestureGraph();
    Fx f(x.g);
    f.page.canvasPressed(100, 100, 0);
    EQ(f.page.canvas().ui.selected, x.r);
    EQ(f.page.canvas().ui.dragNode, pc::NodeId{0});
    EQ(f.page.canvas().ui.resizeNode, pc::NodeId{0});
    CHECK(!f.page.canvas().ui.panning && !f.page.canvas().ui.wiring);
    f.page.canvasMoved(300, 300);
    CHECK(nd(f.g(), x.r).x == 40.0f);
    f.page.canvasReleased(300, 300, 0);
    // a press on another node selects that
    f.click(100, 300);
    EQ(f.page.canvas().ui.selected, x.d);
    // LATER IS ON TOP: where two nodes overlap the later one takes the press
    nm(f.g(), x.s).x = 60.0f;
    nm(f.g(), x.s).y = 280.0f;
    f.click(100, 300);
    EQ(f.page.canvas().ui.selected, x.s);
}

void pressOnTheCloseKeyRemovesTheNode() {
    const G x = gestureGraph();
    Fx f(x.g);
    f.page.draft().dirty = false;
    f.select(x.r);
    f.page.canvasPressed(261, 51, 0);                   // the 14 px key at the header's right end
    CHECK(f.g().find(x.r) == nullptr);
    EQ(f.g().wires().size(), std::size_t{0});            // its wires go with it
    EQ(f.page.canvas().ui.selected, pc::NodeId{0});      // it was selected
    CHECK(!f.page.canvas().ui.wireSelected && f.page.draft().dirty);
    CHECK(!f.page.canvas().ui.panning && !f.page.canvas().ui.wiring && f.page.canvas().ui.dragNode == 0);
    // another node's selection survives a close
    Fx h(x.g);
    h.select(x.d);
    h.page.canvasPressed(261, 51, 0);
    EQ(h.page.canvas().ui.selected, x.d);
    CHECK(h.g().find(x.r) == nullptr);
    // the next tick commits it
    h.sub.clear();
    h.tick();
    EQ(h.sub.count(kPatchSetGraph), std::size_t{1});
}

void pressOnTheGripResizes() {
    const G x = gestureGraph();
    Fx f(x.g);
    f.page.canvasPressed(265, 160, 0);                  // the 16 px corner of R (40..272 x 40..168)
    EQ(f.page.canvas().ui.selected, x.r);
    EQ(f.page.canvas().ui.resizeNode, x.r);
    CHECK(f.page.canvas().ui.grab == (pg::Vec2{-7.0f, -8.0f}));
    EQ(f.page.canvas().ui.dragNode, pc::NodeId{0});
    f.page.draft().dirty = false;
    f.page.canvasMoved(300, 200);
    CHECK(nd(f.g(), x.r).w == 267.0f && nd(f.g(), x.r).h == 168.0f);
    CHECK(f.page.draft().dirty);
    // the floor a part may not go below
    f.page.canvasMoved(0, 0);
    pc::Node expected = *x.g.find(x.r);
    pg::resizeNodeTo(expected, (pg::Vec2{7.0f, 8.0f}));
    CHECK(nd(f.g(), x.r).w == expected.w && nd(f.g(), x.r).h == expected.h);
    CHECK(nd(f.g(), x.r).w == pc::kMinNodeW);
    f.page.canvasMoved(5000, 5000);
    CHECK(nd(f.g(), x.r).w == pg::kMaxNodeW && nd(f.g(), x.r).h == pg::kMaxNodeH);
    f.page.canvasReleased(5000, 5000, 0);
    EQ(f.page.canvas().ui.resizeNode, pc::NodeId{0});
    EQ(f.page.canvas().ui.selected, x.r);
}

void aWireIsMadeFromAPort() {
    const G x = gestureGraph();
    Fx f(x.g);
    f.page.draft().dirty = false;
    const pg::Vec2 a = outPort(x.g, x.r), b = inPort(x.g, x.d);
    f.page.canvasPressed(a.x, a.y, 0);
    CHECK(f.page.canvas().ui.wiring && f.page.canvas().ui.wireFrom.found && !f.page.canvas().ui.wireFrom.input && f.page.canvas().ui.wireFrom.node == x.r);
    // THE PORT BEATS THE NODE UNDER IT: the output port sits on R's right edge
    EQ(f.page.canvas().ui.selected, pc::NodeId{0});
    EQ(f.page.canvas().ui.dragNode, pc::NodeId{0});
    // the pointer within the 9 unit grab radius still finds the port
    f.page.canvasReleased(0, 0, 0);
    f.page.canvasPressed(a.x - 8.0f, a.y + 1.0f, 0);
    CHECK(f.page.canvas().ui.wiring);
    f.page.canvasReleased(0, 0, 0);
    f.page.canvasPressed(a.x + 12.0f, a.y, 0);           // out of the radius and beyond the node's edge: the wire there, not a port
    CHECK(!f.page.canvas().ui.wiring);
    f.page.canvasReleased(0, 0, 0);

    f.page.draft().dirty = false;
    dragPorts(f, a, b);
    CHECK(f.g().wires().size() == 2u);
    CHECK(std::find(f.g().wires().begin(), f.g().wires().end(), (pc::Wire{x.r, 0, x.d, 0})) != f.g().wires().end());
    CHECK(f.page.draft().dirty);
    CHECK(!f.page.canvas().ui.wiring);
    CHECK(f.page.canvasModel().refusal.shown == false);
    CHECK(f.page.canvasModel().ghost.empty());

    // a wire drawn from the INPUT end runs the same way: whichever end is the output is the source
    Fx h(x.g);
    h.page.canvasPressed(inPort(x.g, x.d).x, inPort(x.g, x.d).y, 0);
    CHECK(h.page.canvas().ui.wiring && h.page.canvas().ui.wireFrom.input);
    h.page.canvasReleased(a.x, a.y, 0);
    CHECK(std::find(h.g().wires().begin(), h.g().wires().end(), (pc::Wire{x.r, 0, x.d, 0})) != h.g().wires().end());

    // released on nothing: no wire, no refusal
    Fx k(x.g);
    k.page.canvasPressed(a.x, a.y, 0);
    k.page.canvasReleased(700, 450, 0);
    CHECK(k.g().wires().size() == 1u && !k.page.canvasModel().refusal.shown && !k.page.canvas().ui.wiring);
}

void theGhostWireFollowsThePointer() {
    const G x = gestureGraph();
    Fx f(x.g);
    CHECK(f.page.canvasModel().ghost.empty());
    const pg::Vec2 a = outPort(x.g, x.r);
    f.page.canvasPressed(a.x, a.y, 0);
    f.page.canvasMoved(200.0f, 220.0f);
    pg::Vec2 c1, c2;
    pg::wireCurve(a, (pg::Vec2{200.0f, 220.0f}), c1, c2);
    char want[200];
    std::snprintf(want, sizeof(want), "M %.2f %.2f C %.2f %.2f %.2f %.2f %.2f %.2f", a.x, a.y, c1.x, c1.y, c2.x, c2.y, 200.0f, 220.0f);
    EQ(f.page.canvasModel().ghost, std::string(want));
    f.page.canvasReleased(200, 220, 0);
    CHECK(f.page.canvasModel().ghost.empty());
    // from an input the curve runs from the pointer to the port, the direction it will finally run
    const pg::Vec2 b = inPort(x.g, x.d);
    f.page.canvasPressed(b.x, b.y, 0);
    f.page.canvasMoved(10.0f, 220.0f);
    pg::wireCurve((pg::Vec2{10.0f, 220.0f}), b, c1, c2);
    std::snprintf(want, sizeof(want), "M %.2f %.2f C %.2f %.2f %.2f %.2f %.2f %.2f", 10.0f, 220.0f, c1.x, c1.y, c2.x, c2.y, b.x, b.y);
    EQ(f.page.canvasModel().ghost, std::string(want));
}

// What one drag between two ports gives: the refusal sentence and where its bubble is, and the wire count.
struct Drag {
    std::string text;
    bool shown = false;
    float x = 0, y = 0;
    std::size_t wires = 0;
};
Drag dragged(pc::Graph g, pg::Vec2 from, pg::Vec2 to) {
    Fx f(g);
    dragPorts(f, from, to);
    const CanvasModel m = f.page.canvasModel();
    return Drag{m.refusal.text, m.refusal.shown, m.refusal.x, m.refusal.y, f.g().wires().size()};
}

void everyRefusalSaysWhy() {
    const G x = gestureGraph();
    // TypeMismatch: an I/Q output into an audio input
    Drag d = dragged(x.g, outPort(x.g, x.r), inPort(x.g, x.s));
    CHECK(d.shown && d.text == "those two carry different things" && d.wires == 1u);
    // the bubble sits at the pointer + (12, 10)
    CHECK(d.x == inPort(x.g, x.s).x + 12.0f && d.y == inPort(x.g, x.s).y + 10.0f);
    // two outputs, two inputs: not a connection at all
    d = dragged(x.g, outPort(x.g, x.r), outPort(x.g, x.c));
    CHECK(d.shown && d.text == "those two carry different things" && d.wires == 1u);
    d = dragged(x.g, inPort(x.g, x.d), inPort(x.g, x.s));
    CHECK(d.shown && d.text == "those two carry different things");
    // AlreadyWired
    d = dragged(x.g, outPort(x.g, x.r), inPort(x.g, x.c));
    CHECK(d.shown && d.text == "those are already wired together" && d.wires == 1u);
    // SelfLoop
    d = dragged(x.g, outPort(x.g, x.c), inPort(x.g, x.c));
    CHECK(d.shown && d.text == "a node cannot feed itself");
    // InputOccupied: a second radio into the channel that has one
    pc::Graph two = x.g;
    const pc::NodeId r2 = two.addNode(NodeKind::Radio, "Two", PortType::Iq, 40, 420);
    d = dragged(two, outPort(two, r2), inPort(two, x.c));
    CHECK(d.shown && d.text == "that input already has a wire" && d.wires == 1u);
    // WouldCycle: two channels in a row and the second back into the first's free input
    pc::Graph loop;
    const pc::NodeId c1 = loop.addNode(NodeKind::Channel, "One", PortType::Iq, 40, 40);
    const pc::NodeId c2 = loop.addNode(NodeKind::Channel, "Two", PortType::Iq, 320, 40);
    loop.connect(c1, 0, c2, 0);
    d = dragged(loop, outPort(loop, c2), inPort(loop, c1));
    CHECK(d.shown && d.text == "that would make a loop" && d.wires == 1u);
    // a wire that is accepted says nothing
    d = dragged(x.g, outPort(x.g, x.r), inPort(x.g, x.d));
    CHECK(!d.shown && d.text.empty() && d.wires == 2u);
}

void theRefusalBubbleLastsTwoAndAHalfSeconds() {
    const G x = gestureGraph();
    Fx f(x.g);
    dragPorts(f, outPort(x.g, x.r), inPort(x.g, x.s));
    CHECK(f.page.canvasModel().refusal.shown);
    // 76 ticks at the 33 ms tick: shown while now - refusedAt < 76
    for (int i = 1; i < kRefusalTicks; ++i) {
        f.tick();
        if (i == 1 || i == kRefusalTicks - 1) { CHECK(f.page.canvasModel().refusal.shown); }
    }
    CHECK(f.page.canvasModel().refusal.shown);       // 75 ticks later
    f.tick();
    CHECK(!f.page.canvasModel().refusal.shown);      // 76
    // a new gesture of the same kind shows it again, and the pan moves it with the view
    dragPorts(f, outPort(x.g, x.r), inPort(x.g, x.s));
    const float before = f.page.canvasModel().refusal.x;
    CHECK(f.page.canvasModel().refusal.shown);
    f.page.canvasWheel(10, 10, 1, 0, false, false);   // a pan of 48 px
    EQ(f.page.canvasModel().refusal.x, before + 48.0f);
    // a drag that is accepted clears the bubble at once
    f.page.canvas().setView(pg::View{});
    dragPorts(f, outPort(x.g, x.r), inPort(x.g, x.d));
    CHECK(!f.page.canvasModel().refusal.shown);
}

void aWireIsSelectedAndCut() {
    const G x = gestureGraph();
    Fx f(x.g);
    f.page.draft().dirty = false;
    f.select(x.d);
    f.page.canvasPressed(296, 76, 0);                    // between R's output and C's input
    CHECK(f.page.canvas().ui.wireSelected);
    EQ(f.page.canvas().ui.selected, pc::NodeId{0});
    CHECK(f.page.canvas().ui.selectedWire == (pc::Wire{x.r, 0, x.c, 0}));
    CHECK(!f.page.canvas().ui.panning);
    f.page.canvasReleased(296, 76, 0);
    const CanvasModel m = f.page.canvasModel();
    EQ(m.wires.size(), std::size_t{1});
    CHECK(at(m.wires, 0).selected);
    // Delete cuts the wire, not a node
    f.page.canvasDelete();
    EQ(f.g().wires().size(), std::size_t{0});
    CHECK(f.g().nodes().size() == 4u && !f.page.canvas().ui.wireSelected && f.page.draft().dirty);
    // a second Delete has nothing to remove
    f.page.draft().dirty = false;
    f.page.canvasDelete();
    CHECK(f.g().nodes().size() == 4u && !f.page.draft().dirty);
    // a press off the wire by more than 7 px does not select it
    Fx h(x.g);
    h.page.canvasPressed(296, 76 + 10, 0);
    CHECK(!h.page.canvas().ui.wireSelected);
    h.page.canvasReleased(296, 86, 0);
    h.page.canvasPressed(296, 76 + 6, 0);
    CHECK(h.page.canvas().ui.wireSelected);
}

void deleteRemovesTheSelectedNode() {
    const G x = gestureGraph();
    Fx f(x.g);
    f.page.canvasDelete();                               // nothing selected
    EQ(f.g().nodes().size(), std::size_t{4});
    f.select(x.c);
    f.page.draft().dirty = false;
    f.page.canvasDelete();
    CHECK(f.g().find(x.c) == nullptr && f.g().wires().empty());
    EQ(f.page.canvas().ui.selected, pc::NodeId{0});
    CHECK(f.page.draft().dirty);
}

void aDragOnEmptyCanvasPans() {
    const G x = gestureGraph();
    Fx f(x.g);
    f.select(x.r);
    f.page.draft().dirty = false;
    f.page.canvasPressed(600, 450, 0);
    EQ(f.page.canvas().ui.selected, pc::NodeId{0});
    CHECK(!f.page.canvas().ui.wireSelected && f.page.canvas().ui.panning && f.page.canvas().ui.panButton == 0);
    f.page.canvasMoved(620, 470);
    CHECK(f.page.canvas().ui.view.pan == (pg::Vec2{20.0f, 20.0f}));
    CHECK(f.page.draft().dirty);
    f.page.canvasMoved(610, 500);
    CHECK(f.page.canvas().ui.view.pan == (pg::Vec2{10.0f, 50.0f}));
    EQ(nd(f.g(), x.r).x, 40.0f);                        // no node moved
    f.page.canvasReleased(610, 500, 0);
    CHECK(!f.page.canvas().ui.panning);
    f.page.canvasMoved(0, 0);
    CHECK(f.page.canvas().ui.view.pan == (pg::Vec2{10.0f, 50.0f}));
}

void theMiddleButtonPansAnywhere() {
    const G x = gestureGraph();
    Fx f(x.g);
    f.page.canvasPressed(100, 50, 2);                    // on a node's header
    CHECK(f.page.canvas().ui.panning && f.page.canvas().ui.panButton == 2);
    EQ(f.page.canvas().ui.selected, pc::NodeId{0});      // nothing was selected or dragged
    EQ(f.page.canvas().ui.dragNode, pc::NodeId{0});
    f.page.canvasMoved(130, 70);
    CHECK(f.page.canvas().ui.view.pan == (pg::Vec2{30.0f, 20.0f}));
    f.page.canvasReleased(130, 70, 0);                   // the left button's release does not end it
    CHECK(f.page.canvas().ui.panning);
    f.page.canvasMoved(140, 70);
    CHECK(f.page.canvas().ui.view.pan == (pg::Vec2{40.0f, 20.0f}));
    f.page.canvasReleased(140, 70, 2);
    CHECK(!f.page.canvas().ui.panning);
    // the right button does nothing
    f.page.canvasPressed(100, 50, 1);
    CHECK(!f.page.canvas().ui.panning && f.page.canvas().ui.selected == 0 && f.page.canvas().ui.dragNode == 0 && !f.page.canvas().ui.wiring);
    f.page.canvasReleased(100, 50, 1);
}

void theWheelPansAndZooms() {
    const G x = gestureGraph();
    Fx f(x.g);
    f.page.draft().dirty = false;
    // a plain wheel pans 48 px a notch, both axes (a touchpad), fractional notches too
    f.page.canvasWheel(100, 100, 0, 1, false, false);
    CHECK(f.page.canvas().ui.view.pan == (pg::Vec2{0.0f, 48.0f}));
    CHECK(f.page.draft().dirty);
    f.page.canvasWheel(100, 100, 2, -0.5f, false, false);
    CHECK(f.page.canvas().ui.view.pan == (pg::Vec2{96.0f, 24.0f}));
    // Shift: the vertical notches go sideways
    f.page.canvasWheel(100, 100, 0, 1, false, true);
    CHECK(f.page.canvas().ui.view.pan == (pg::Vec2{144.0f, 24.0f}));
    // Ctrl: zoom 1.12 a notch about the pointer; the point under it stays
    Fx h(x.g);
    const pg::Vec2 at100{100.0f, 100.0f};
    const pg::Vec2 before = pg::screenToWorld(h.page.canvas().ui.view, at100);
    h.page.canvasWheel(100, 100, 0, 2, true, false);
    CHECK_NEAR(h.page.canvas().ui.view.zoom, 1.12f * 1.12f, 1e-4);
    const pg::Vec2 after = pg::screenToWorld(h.page.canvas().ui.view, at100);
    CHECK_NEAR(after.x, before.x, 1e-3);
    CHECK_NEAR(after.y, before.y, 1e-3);
    // one event at most four notches
    Fx k(x.g);
    k.page.canvasWheel(0, 0, 0, 10, true, false);
    CHECK_NEAR(k.page.canvas().ui.view.zoom, std::pow(1.12f, 4.0f), 1e-4);
    // zoom held to 0.35 .. 2.5
    for (int i = 0; i < 20; ++i) { k.page.canvasWheel(0, 0, 0, 4, true, false); }
    CHECK(k.page.canvas().ui.view.zoom == 2.5f);
    for (int i = 0; i < 40; ++i) { k.page.canvasWheel(0, 0, 0, -4, true, false); }
    CHECK(k.page.canvas().ui.view.zoom == 0.35f);
    // ctrl with only a sideways notch does nothing
    Fx m(x.g);
    m.page.canvasWheel(0, 0, 1, 0, true, false);
    CHECK(m.page.canvas().ui.view.zoom == 1.0f && m.page.canvas().ui.view.pan == (pg::Vec2{}));
}

void theCanvasKeys() {
    const G x = gestureGraph();
    Fx f(x.g);
    f.page.draft().dirty = false;
    f.page.zoomStep(true);
    CHECK_NEAR(f.page.canvas().ui.view.zoom, 1.25f, 1e-5);
    CHECK(f.page.draft().dirty);
    // about the canvas centre (400, 250): the world point under it stays
    const pg::Vec2 c0 = pg::screenToWorld(pg::View{}, (pg::Vec2{400.0f, 250.0f}));
    const pg::Vec2 c1 = pg::screenToWorld(f.page.canvas().ui.view, (pg::Vec2{400.0f, 250.0f}));
    CHECK_NEAR(c1.x, c0.x, 1e-3);
    CHECK_NEAR(c1.y, c0.y, 1e-3);
    f.page.zoomStep(false);
    CHECK_NEAR(f.page.canvas().ui.view.zoom, 1.0f, 1e-5);
    f.page.zoomStep(false);
    CHECK_NEAR(f.page.canvas().ui.view.zoom, 0.8f, 1e-5);
    // FIT: every node in view with the window's margin, never past 1:1
    f.page.fit();
    const pg::View want = pg::fitView(f.g(), 800.0f, 500.0f, 1.0f);
    CHECK(f.page.canvas().ui.view.zoom == want.zoom && f.page.canvas().ui.view.pan == want.pan);
    CHECK(want.zoom == 1.0f && want.pan.x == 104.0f);     // never past 1:1, and centred
    // an empty patch goes to View{}
    Fx e(x.g);
    for (const pc::NodeId id : {x.r, x.c, x.d, x.s}) { e.g().removeNode(id); }
    e.page.canvas().setView(pg::View{(pg::Vec2{9.0f, 9.0f}), 2.0f});
    e.page.fit();
    CHECK(e.page.canvas().ui.view.zoom == 1.0f && e.page.canvas().ui.view.pan == (pg::Vec2{}));
}

void theHotKeysFollowThePointer() {
    const G x = gestureGraph();
    Fx f(x.g);
    CanvasModel m = f.page.canvasModel();
    for (const CanvasNode& n : m.nodes) { CHECK(!n.hot_close && !n.hot_grip); }
    CHECK(!m.resize_cursor);
    f.page.canvasMoved(261, 51);                         // R's close key
    m = f.page.canvasModel();
    CHECK(at(m.nodes, 0).hot_close && !at(m.nodes, 0).hot_grip && !at(m.nodes, 1).hot_close);
    f.page.canvasMoved(265, 160);                        // R's grip
    m = f.page.canvasModel();
    CHECK(!at(m.nodes, 0).hot_close && at(m.nodes, 0).hot_grip);
    CHECK(m.resize_cursor);
    f.page.canvasMoved(100, 100);
    m = f.page.canvasModel();
    CHECK(!at(m.nodes, 0).hot_grip && !m.resize_cursor);
    // a resize in progress keeps its grip lit and the cursor, wherever the pointer is
    f.page.canvasMoved(265, 160);
    f.page.canvasPressed(265, 160, 0);
    f.page.canvasMoved(400, 400);
    m = f.page.canvasModel();
    CHECK(at(m.nodes, 0).hot_grip && m.resize_cursor);
    f.page.canvasReleased(400, 400, 0);
    // the pointer leaving the canvas: nothing is hot
    f.page.canvasMoved(261, 51);
    f.page.canvasLeft();
    m = f.page.canvasModel();
    for (const CanvasNode& n : m.nodes) { CHECK(!n.hot_close && !n.hot_grip); }
    CHECK(!m.resize_cursor);
    // no resize cursor while panning or wiring
    f.page.canvasPressed(600, 450, 0);
    f.page.canvasMoved(265, 160);
    CHECK(!f.page.canvasModel().resize_cursor);
    f.page.canvasReleased(265, 160, 0);
}

void aNewPatchForgetsTheInteraction() {
    const G x = gestureGraph();
    Fx f(x.g);
    f.page.canvasPressed(100, 50, 0);                    // a drag of node 1 under way, the button held
    EQ(f.page.canvas().ui.dragNode, x.r);
    // another epoch: a patch of another file; its ids mean other nodes
    pc::Graph other;
    other.addNode(NodeKind::Radio, "Other", PortType::Iq, 5, 5);
    f.publish(other, true);
    f.tick();
    EQ(f.page.canvas().ui.selected, pc::NodeId{0});
    EQ(f.page.canvas().ui.dragNode, pc::NodeId{0});
    CHECK(!f.page.canvas().ui.wiring && !f.page.canvas().ui.wireSelected);
    CHECK(f.page.draft().graph().nodes().size() == 1u);
    f.page.canvasReleased(100, 50, 0);
    // a wire in the air is forgotten the same way
    Fx w(x.g);
    const pg::Vec2 a = outPort(x.g, x.r);
    w.page.canvasPressed(a.x, a.y, 0);
    CHECK(w.page.canvas().ui.wiring);
    w.publish(other, true);
    w.tick();
    CHECK(!w.page.canvas().ui.wiring);
    w.page.canvasReleased(400, 400, 0);
    CHECK(w.page.draft().graph().wires().empty());

    // a node the engine removed: the selection of it is forgotten, and the model never names it
    Fx h(x.g);
    h.select(x.s);
    pc::Graph shorter = x.g;
    shorter.removeNode(x.s);
    h.publish(shorter);
    h.tick();
    EQ(h.page.canvas().ui.selected, pc::NodeId{0});
    for (const CanvasNode& n : h.page.canvasModel().nodes) { CHECK(n.id != static_cast<int>(x.s)); }
    EQ(h.page.canvasModel().nodes.size(), std::size_t{3});

    // a document loaded by the page itself forgets too
    Fx k(x.g);
    k.select(x.r);
    pc::Graph doc;
    doc.addNode(NodeKind::Radio, "Doc", PortType::Iq, 0, 0);
    CHECK(k.page.draft().loadDocument(doc, k.sub));
    k.tick();
    EQ(k.page.canvas().ui.selected, pc::NodeId{0});
}

void theModelIsTheGeometryItDrewFrom() {
    const G x = gestureGraph();
    Fx f(x.g);
    f.page.canvas().setView(pg::View{(pg::Vec2{10.0f, 20.0f}), 0.5f});
    f.select(x.c);
    CanvasModel m = f.page.canvasModel();
    CHECK(m.zoom == 0.5f);
    EQ(m.nodes.size(), std::size_t{4});
    const CanvasNode& r = at(m.nodes, 0);
    CHECK(r.id == 1 && r.kind == 0 && r.name == "Radio" && r.kind_caption == "RADIO");
    // the plate: tl = world * zoom + pan, size = the world size * zoom
    CHECK(r.x == 40.0f * 0.5f + 10.0f && r.y == 40.0f * 0.5f + 20.0f && r.w == 232.0f * 0.5f && r.h == 128.0f * 0.5f);
    CHECK(!r.selected && at(m.nodes, 1).selected && at(m.nodes, 1).kind_caption == "CHANNEL");
    // the ports: where the hit test looks, mapped
    EQ(r.inputs.size(), std::size_t{0});
    EQ(r.outputs.size(), std::size_t{1});
    const pg::Vec2 op = pg::outputPortPos(*x.g.find(x.r), 0);
    CHECK(at(r.outputs, 0).x == op.x * 0.5f + 10.0f && at(r.outputs, 0).y == op.y * 0.5f + 20.0f && at(r.outputs, 0).tone == 0);
    const CanvasNode& s = at(m.nodes, 3);
    EQ(s.inputs.size(), std::size_t{1});
    EQ(at(s.inputs, 0).tone, 1);                          // audio
    // the wire: "M a C c1 c2 b" in canvas pixels, tone = the source output's type
    EQ(m.wires.size(), std::size_t{1});
    pg::Vec2 c1, c2;
    const pg::Vec2 a = pg::outputPortPos(*x.g.find(x.r), 0), b = pg::inputPortPos(*x.g.find(x.c), 0);
    pg::wireCurve(a, b, c1, c2);
    const auto px = [](pg::Vec2 p) { return (pg::Vec2{p.x * 0.5f + 10.0f, p.y * 0.5f + 20.0f}); };
    float v[8] = {0};
    CHECK(std::sscanf(at(m.wires, 0).path.c_str(), "M %f %f C %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8);
    CHECK_NEAR(v[0], px(a).x, 0.01); CHECK_NEAR(v[1], px(a).y, 0.01);
    CHECK_NEAR(v[2], px(c1).x, 0.01); CHECK_NEAR(v[3], px(c1).y, 0.01);
    CHECK_NEAR(v[4], px(c2).x, 0.01); CHECK_NEAR(v[5], px(c2).y, 0.01);
    CHECK_NEAR(v[6], px(b).x, 0.01); CHECK_NEAR(v[7], px(b).y, 0.01);
    EQ(at(m.wires, 0).tone, 0);
    CHECK(!at(m.wires, 0).selected);
    // a wire of another type takes that type's tone
    pc::Graph g = x.g;
    g.connect(x.d, 0, x.s, 0);
    Fx h(g);
    EQ(at(h.page.canvasModel().wires, 1).tone, 1);

    // the grid: 26 world units * zoom, none below 6 px; the lines at pan mod step
    m = f.page.canvasModel();
    CHECK_NEAR(m.grid_step, 13.0f, 1e-4);
    CHECK_NEAR(m.grid_ox, 10.0f, 1e-3);
    CHECK_NEAR(m.grid_oy, 7.0f, 1e-3);
    f.page.canvas().setView(pg::View{(pg::Vec2{-10.0f, 0.0f}), 1.0f});
    m = f.page.canvasModel();
    CHECK_NEAR(m.grid_step, 26.0f, 1e-4);
    CHECK_NEAR(m.grid_ox, 16.0f, 1e-3);                   // -10 mod 26
    CHECK_NEAR(m.grid_oy, 0.0f, 1e-3);
    f.page.canvas().setView(pg::View{(pg::Vec2{0.0f, 0.0f}), 0.2f});
    EQ(f.page.canvasModel().grid_step, 0.0f);             // 5.2 px: a wash of lines
    f.page.canvas().setView(pg::View{(pg::Vec2{0.0f, 0.0f}), 6.0f / 26.0f + 0.001f});
    CHECK(f.page.canvasModel().grid_step >= 6.0f);
    // faces are live from 0.6
    f.page.canvas().setView(pg::View{(pg::Vec2{}), 0.6f});
    CHECK(f.page.canvasModel().faces_live);
    f.page.canvas().setView(pg::View{(pg::Vec2{}), 0.59f});
    CHECK(!f.page.canvasModel().faces_live);
    CHECK(f.page.canvasModel().zoom == 0.59f);
}

void theKeysNeedRoom() {
    const G x = gestureGraph();
    Fx f(x.g);
    // 28 + 28 + Fit (44) + two gaps (8) + two insets (20) = 128 wide; 24 + 20 = 44 high
    f.page.setCanvasSize(128.0f, 44.0f);
    CHECK(f.page.canvasModel().keys_shown);
    f.page.setCanvasSize(127.0f, 44.0f);
    CHECK(!f.page.canvasModel().keys_shown);
    f.page.setCanvasSize(128.0f, 43.0f);
    CHECK(!f.page.canvasModel().keys_shown);
    f.page.setCanvasSize(800.0f, 500.0f);
    CHECK(f.page.canvasModel().keys_shown);
    const CanvasModel m = f.page.canvasModel();
    EQ(m.zoom_out_tooltip, std::string("Zoom out (Ctrl+scroll also zooms)"));
    EQ(m.zoom_in_tooltip, std::string("Zoom in (Ctrl+scroll also zooms)"));
    EQ(m.fit_tooltip, std::string("Show every node on the canvas"));
    EQ(m.fit_label, std::string("Fit"));
}

void blockedFaultAndActiveMarks() {
    const Chain c = chain();
    Fx f(c.g);
    f.t.add(FOXAPI_LIST_PATCH_PROBLEMS, c.channel, "2", "");                     // NoFrequency: blocking
    f.t.add(FOXAPI_LIST_PATCH_PROBLEMS, c.speaker, "5", "");                     // NothingListens: advisory only
    f.t.add(FOXAPI_LIST_PATCH_RADIOS, c.radio, "siggen", "", kFault, 0, 0, 0);
    f.tickBump();
    const CanvasModel m = f.page.canvasModel();
    CHECK(at(m.nodes, 0).fault && !at(m.nodes, 1).fault);
    CHECK(!at(m.nodes, 0).blocked && at(m.nodes, 1).blocked && !at(m.nodes, 3).blocked);
    // `active` is the node's own switch: a radio that is on
    CHECK(at(m.nodes, 0).active && !at(m.nodes, 1).active);
    nm(f.g(), c.radio).on = false;
    CHECK(!at(f.page.canvasModel().nodes, 0).active);
}

}  // namespace

namespace {

// ================================================================================================================================
// the band
// ================================================================================================================================

void theTransportReads() {
    const Chain c = chain();
    Fx f(c.g);
    PatchModel m = f.page.bandModel();
    CHECK(!m.running && m.run_label == "START" && !m.run_line_lit);
    EQ(m.run_tooltip, std::string("Start the patch: every radio switched on opens. The receiver\nhands its radio over and runs on the signal generator."));
    EQ(m.run_line, std::string("STOPPED - 1 of 1 radio switched on"));
    EQ(m.all_off_tooltip, std::string("Switch every radio off and stop the patch. START switches\nthem all on again."));
    EQ(m.presets_tooltip, std::string("Save the patch on the canvas under a name, or load one you saved.\nLoading stops a running patch; the one loaded waits for START."));
    CHECK(!m.presets_open);
    f.page.setPresetsOpen(true);
    CHECK(f.page.bandModel().presets_open);
    // the commands
    Command k = f.page.runClicked();
    CHECK(fc(k).op == 0x0902u && fc(k).ival[0] == 1);
    CHECK(fc(f.page.allOffClicked()).op == 0x0903u);

    // RUNNING: one radio, the table says whether it is open
    f.st.flags = FOXAPI_RX_PATCH_RUNNING;
    f.tickBump();
    m = f.page.bandModel();
    CHECK(m.running && m.run_label == "STOP" && m.run_line_lit);
    EQ(m.run_tooltip, std::string("Stop the patch: every patch radio closes, recordings are\nfinished, and the receiver gets its radio back."));
    EQ(m.run_line, std::string("RUNNING - 0 of 1 radio open"));
    k = f.page.runClicked();
    CHECK(fc(k).op == 0x0902u && fc(k).ival[0] == 0);
    f.t.add(FOXAPI_LIST_PATCH_RADIOS, c.radio, "siggen", "", kActive, 2.0e6, 100.0e6, 0);
    f.tickBump();
    EQ(f.page.bandModel().run_line, std::string("RUNNING - 1 of 1 radio open"));

    // PLURAL: two radios (a count of 0 radios reads "radios" too: only 1 is singular)
    pc::Graph two = c.g;
    const pc::NodeId r2 = two.addNode(NodeKind::Radio, "Second", PortType::Iq, 60, 400);
    two.mutableNode(r2)->device = "airspy|serial=9";
    f.publish(two);
    f.tickBump();
    EQ(f.page.bandModel().run_line, std::string("RUNNING - 1 of 2 radios open"));
    f.st.flags = 0;
    f.tickBump();
    EQ(f.page.bandModel().run_line, std::string("STOPPED - 2 of 2 radios switched on"));
    nm(f.g(), r2).on = false;
    EQ(f.page.bandModel().run_line, std::string("STOPPED - 1 of 2 radios switched on"));
    pc::Graph none;
    none.addNode(NodeKind::Channel, "x", PortType::Iq, 0, 0);
    Fx z(none);
    EQ(z.page.bandModel().run_line, std::string("STOPPED - 0 of 0 radios switched on"));
}

void theTabChip() {
    Fx f(chain().g);
    PatchModel m = f.page.bandModel();
    EQ(m.tab_chip, std::string("4 NODES"));
    CHECK(m.tab_chip_tone == Tone::Neutral);
    f.st.flags = FOXAPI_RX_PATCH_RUNNING;
    f.tickBump();
    m = f.page.bandModel();
    EQ(m.tab_chip, std::string("RUNNING"));
    CHECK(m.tab_chip_tone == Tone::Good);
    pc::Graph one;
    one.addNode(NodeKind::Channel, "x", PortType::Iq, 0, 0);
    Fx g(one);
    EQ(g.page.bandModel().tab_chip, std::string("1 NODE"));
    // an empty patch says so (the seed adds a starter only once the page is shown and in step: nothing here to seed over a graph deleted by hand)
    g.g().removeNode(1);
    EQ(g.page.bandModel().tab_chip, std::string("EMPTY"));
    // running wins over an empty patch
    g.st.flags = FOXAPI_RX_PATCH_RUNNING;
    g.tickBump();
    EQ(g.page.bandModel().tab_chip, std::string("RUNNING"));
}

void thePartKeys() {
    Fx f(chain().g);
    const PatchModel m = f.page.bandModel();
    EQ(m.parts.size(), std::size_t{7});
    const char* labels[7] = {"Radio", "Channel", "Demod", "Spectrum", "Speaker", "Text out", "Map"};
    for (std::size_t i = 0; i < 7; ++i) {
        EQ(at(m.parts, i).label, std::string(labels[i]));
        EQ(at(m.parts, i).kind, static_cast<int>(i));
        CHECK(at(m.parts, i).enabled && at(m.parts, i).tooltip.empty());
    }
    // AT MOST FIVE RADIOS: the Radio key goes dim at five and still says why
    pc::Graph g = chain().g;
    for (int i = 0; i < 3; ++i) { g.addNode(NodeKind::Radio, "More", PortType::Iq, 0, 0); }
    Fx h(g);
    CHECK(at(h.page.bandModel().parts, 0).enabled && at(h.page.bandModel().parts, 0).tooltip.empty());   // four
    g.addNode(NodeKind::Radio, "Fifth", PortType::Iq, 0, 0);
    Fx k(g);
    const PatchModel five = k.page.bandModel();
    CHECK(!at(five.parts, 0).enabled);
    EQ(at(five.parts, 0).tooltip, std::string("A patch has at most 5 radios."));
    CHECK(at(five.parts, 1).enabled && at(five.parts, 1).tooltip.empty());
}

void thePartKeysPlaceNodes() {
    Fx f(chain().g);
    f.sub.clear();
    const std::size_t nodes0 = f.g().nodes().size();
    const char* names[7] = {"Radio", "Channel", "Demod", "Spectrum", "Speaker", "Text out", "Map"};
    const NodeKind kinds[7] = {NodeKind::Radio, NodeKind::Channel, NodeKind::Demod, NodeKind::Display, NodeKind::Sink, NodeKind::Sink, NodeKind::Map};
    const PortType feeds[7] = {PortType::Iq, PortType::Iq, PortType::Iq, PortType::Iq, PortType::Audio, PortType::Text, PortType::Track};
    for (int k = 1; k < 7; ++k) {
        f.page.draft().dirty = false;
        f.page.partPressed(k, f.sub);
        EQ(f.g().nodes().size(), nodes0 + static_cast<std::size_t>(k));
        const pc::Node& n = lastNode(f.g());
        CHECK(n.kind == kinds[k] && n.name == names[k]);
        CHECK(!n.inputs.empty() && n.inputs[0] == feeds[k]);
        CHECK(f.page.draft().dirty);
    }
    // each press went to the engine at once, as the window commits after a part press: the first, then one in flight
    EQ(f.sub.count(kPatchSetGraph), std::size_t{1});
    // THE NAME is the English label, stored in the patch as data (never the translation)
    const std::string was = cascade::i18n::current();
    cascade::i18n::setLanguage("de");
    f.page.partPressed(1, f.sub);
    EQ(lastNode(f.g()).name, std::string("Channel"));
    cascade::i18n::setLanguage(was);
    // the nodes land inside the canvas in view, staggered by the count placed (newPartPosition)
    pc::Graph e;
    Fx p(e);
    p.g().removeNode(1);
    const pg::Vec2 at0 = pg::newPartPosition(pg::drawView(p.page.canvas().ui.view, 1.0f), 800.0f, 500.0f, 0);
    p.page.partPressed(2, p.sub);
    CHECK(lastNode(p.g()).x == at0.x && lastNode(p.g()).y == at0.y);
    p.page.partPressed(2, p.sub);
    const pg::Vec2 at1 = pg::newPartPosition(pg::drawView(p.page.canvas().ui.view, 1.0f), 800.0f, 500.0f, 1);
    CHECK(lastNode(p.g()).x == at1.x && lastNode(p.g()).y == at1.y && at1.x != at0.x);
    // a pan moves where they land
    p.page.canvas().setView(pg::View{(pg::Vec2{-300.0f, 0.0f}), 1.0f});
    p.page.partPressed(2, p.sub);
    const pg::Vec2 at2 = pg::newPartPosition(pg::drawView(p.page.canvas().ui.view, 1.0f), 800.0f, 500.0f, 2);
    CHECK(lastNode(p.g()).x == at2.x && at2.x > at1.x + 200.0f);
}

void theRadioPartTakesTheDeviceAfterTheScan() {
    const Chain c = chain();
    Fx f(c.g);
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 1, "Signal generator", "siggen");
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 2, "NESDR", "rtlsdr|serial=1");
    f.tickBump();
    f.sub.clear();
    f.page.partPressed(0, f.sub);
    // the scan FIRST (its ticket kept), then the commit
    EQ(f.sub.sent.size(), std::size_t{2});
    CHECK(fc(cmdAt(f.sub, 0)).op == 0x8406u && fc(cmdAt(f.sub, 1)).op == kPatchSetGraph);
    const pc::Node& n = lastNode(f.g());
    CHECK(n.kind == NodeKind::Radio && n.name == "Radio" && n.device.empty());
    CHECK(f.page.draft().waits().size() == 1u && f.page.draft().waits()[0].node == n.id && f.page.draft().waits()[0].scan_ticket == 100u);
    // the scan has not answered: still none (the commit is taken, so the draft is in step again)
    f.engineTakes();
    f.tickBump();
    CHECK(lastNode(f.g()).device.empty());
    // answered: the first free radio of the list as it is now
    f.page.onAnswer(f.answer(100, 0x8406));
    f.tickBump();
    EQ(lastNode(f.g()).device, std::string("rtlsdr|serial=1"));
    CHECK(f.page.draft().waits().empty());
    // a session that would not take the scan: the list as it is, at once
    Fx h(c.g);
    h.t.add(FOXAPI_LIST_PATCH_DEVICES, 1, "NESDR", "rtlsdr|serial=1");
    h.tickBump();
    h.sub.refuse = true;
    h.page.partPressed(0, h.sub);
    EQ(lastNode(h.g()).device, std::string("rtlsdr|serial=1"));
    CHECK(h.page.draft().waits().empty());
    // the sixth radio is not made
    pc::Graph g5 = c.g;
    for (int i = 0; i < 4; ++i) { g5.addNode(NodeKind::Radio, "More", PortType::Iq, 0, 0); }
    Fx k(g5);
    const std::size_t n5 = k.g().nodes().size();
    k.sub.clear();
    k.page.partPressed(0, k.sub);
    EQ(k.g().nodes().size(), n5);
    EQ(k.sub.count(0x8406), std::size_t{0});
    // two presses before the answer: each takes a different device
    Fx m(c.g);
    m.t.add(FOXAPI_LIST_PATCH_DEVICES, 1, "NESDR", "rtlsdr|serial=1");
    m.t.add(FOXAPI_LIST_PATCH_DEVICES, 2, "Airspy", "airspy|serial=9");
    m.tickBump();
    m.page.partPressed(0, m.sub);
    m.page.partPressed(0, m.sub);
    m.engineTakes();                                       // the first commit is taken (the second press waited behind it)
    m.engineTakes();                                       // ...and the second, which the first one's settling sent: the waits are chosen once the draft is in step
    m.page.onAnswer(m.answer(102, 0x8406));                // the later scan's answer covers both
    m.tickBump();
    EQ(nodeFromEnd(m.g(), 1).device, std::string("rtlsdr|serial=1"));
    EQ(nodeFromEnd(m.g(), 0).device, std::string("airspy|serial=9"));
}

void theDecoderKeys() {
    Fx f(chain().g);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars.dll", 0, 1, 0, 0);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 2, "ADS-B", "adsb.dll", 0, 0, 2.0e6, 1);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 3, "Multi", "m.dll", 0, 0, 0, 0);          // I/Q
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 4, "Multi", "m.dll", 0, 1, 48000, 0);      // its audio twin: hidden
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 5, "SSTV", "s.dll#image", 0, 1, 12000, 0);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 6, "Dup", "d.dll", 0, 0, 0, 0);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 7, "Dup", "d.dll", 0, 0, 0, 0);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 8, "Aud", "a2.dll", 0, 1, 0, 0);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 9, "Aud", "a2.dll", 0, 1, 0, 0);
    f.tickBump();
    const PatchModel m = f.page.bandModel();
    EQ(m.no_decoders, std::string(""));
    std::vector<int> index;
    std::vector<std::string> label;
    for (const DecoderKey& k : m.decoders) { index.push_back(k.index); label.push_back(k.label); }
    EQ(index, std::vector<int>({0, 1, 2, 4, 5, 6, 7, 8}));          // row 3 (the audio twin of an I/Q decoder) is not offered
    EQ(label, Names({"ACARS", "ADS-B", "Multi", "SSTV (picture)", "Dup (I/Q)", "Dup (I/Q)", "Aud (audio)", "Aud (audio)"}));
    // the four whole sentences, %s = the part's key
    EQ(at(m.decoders, 0).tooltip, std::string("acars.dll\nAudio input, any rate\nWire it from a demodulator."));
    EQ(at(m.decoders, 1).tooltip, std::string("adsb.dll\nI/Q input, needs a fixed rate\nWire it from a channel (or the radio itself)."));
    EQ(at(m.decoders, 2).tooltip, std::string("m.dll\nI/Q input, any rate\nWire it from a channel (or the radio itself)."));
    EQ(at(m.decoders, 3).tooltip, std::string("s.dll#image\nAudio input, needs a fixed rate\nWire it from a demodulator."));
    // a picture decoder never has a twin; the key is the part's whole key
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 10, "SSTV", "s.dll#image", 0, 1, 12000, 0);
    f.tickBump();
    EQ(f.page.bandModel().decoders.size(), std::size_t{9});
    CHECK(at(f.page.bandModel().decoders, 3).label == "SSTV (picture)" && at(f.page.bandModel().decoders, 8).label == "SSTV (picture)" && at(f.page.bandModel().decoders, 8).index == 9);

    // NO PLUGIN: the sentence that says where to get one
    Fx g(chain().g);
    const PatchModel e = g.page.bandModel();
    EQ(e.decoders.size(), std::size_t{0});
    EQ(e.no_decoders, std::string("- none installed. Add decoder plugins from the plugin store in DECODE."));
}

void aDecoderKeyPlacesTheDecoder() {
    Fx f(chain().g);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars.dll", 0, 1, 0, 0);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 2, "SSTV", "s.dll#image", 0, 1, 12000, 0);
    f.tickBump();
    f.sub.clear();
    f.page.draft().dirty = false;
    f.page.decoderPressed(0, f.sub);
    const pc::Node& n = lastNode(f.g());
    CHECK(n.kind == NodeKind::Decoder && n.name == "ACARS" && n.plugin == "acars.dll" && n.inputs.size() == 1u && n.inputs[0] == PortType::Audio);
    CHECK(f.page.draft().dirty);
    EQ(f.sub.count(kPatchSetGraph), std::size_t{1});
    f.page.decoderPressed(1, f.sub);
    EQ(lastNode(f.g()).plugin, std::string("s.dll#image"));     // a picture part's key keeps its suffix
    EQ(lastNode(f.g()).name, std::string("SSTV"));
    f.page.decoderPressed(7, f.sub);                                  // no such row
    EQ(lastNode(f.g()).name, std::string("SSTV"));
    f.page.decoderPressed(-1, f.sub);
    EQ(lastNode(f.g()).name, std::string("SSTV"));
}

void theBlueprintsAreOfferedWhenTheyCanBe() {
    // no plugin, no receiver position, no airport group: nothing to pick from
    Fx f(chain().g);
    CHECK(!f.page.bandModel().blueprints_shown);
    // a plugin
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars.dll", 0, 1, 0, 0);
    f.tickBump();
    CHECK(f.page.bandModel().blueprints_shown);
    // ...a receiver position (the tower list)
    Fx g(chain().g);
    g.st.flags = FOXAPI_RX_POSITION_SET;
    g.tickBump();
    CHECK(g.page.bandModel().blueprints_shown);
    // ...a bookmark group
    Fx h(chain().g);
    h.t.add(FOXAPI_LIST_BOOKMARKS, 1, "x", "KORD Chicago");
    h.tickBump();
    CHECK(h.page.bandModel().blueprints_shown);
    // a bookmark with no group does not count
    Fx k(chain().g);
    k.t.add(FOXAPI_LIST_BOOKMARKS, 1, "x", "");
    k.tickBump();
    CHECK(!k.page.bandModel().blueprints_shown);
    CHECK(k.page.bandModel().blueprint_placeholder == "Pick a plugin to place its whole workflow");
}

const cc::Airport* firstTower() {
    for (const cc::Airport& a : cc::airbandTable()) {
        for (const cc::AirbandFreq& q : a.freqs) { if (q.type == "TWR") { return &a; } }
    }
    return nullptr;
}

void theBlueprintListIsBuiltWhenOpened() {
    Fx f(chain().g);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars.dll", 0, 1, 0, 0);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 2, "ADS-B", "adsb.dll", 0, 0, 2.0e6, 1);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 3, "SSTV", "s.dll#image", 0, 1, 12000, 0);
    f.t.add(FOXAPI_LIST_PRESETS, 1, "", "ACARS", 0, 131.55e6, 3, 0, 0);               // no label of its own: the plugin's name
    f.t.add(FOXAPI_LIST_PRESETS, 2, "Ground", "ACARS", 0, 131.725e6, 3, 0, 0);
    f.t.add(FOXAPI_LIST_PRESETS, 3, "ADS-B", "ADS-B", 0, 1090.0e6, 0, 0, 2.4e6);
    f.tickBump();
    CHECK(f.page.bandModel().blueprints.empty());       // nothing is read until the list is opened
    f.page.blueprintsOpened(f.t);
    const PatchModel m = f.page.bandModel();
    Names labels;
    for (const SelectItem& i : m.blueprints) { labels.push_back(i.label); }
    // sorted by title (case-blind): the plugin, its preset, the next plugin, the picture decoder
    EQ(labels, Names({"ACARS", "ACARS: Ground", "ADS-B", "SSTV (picture)"}));
    EQ(at(m.blueprints, 0).note, std::string("131.5500 MHz"));
    EQ(at(m.blueprints, 1).note, std::string("131.7250 MHz"));
    EQ(at(m.blueprints, 2).note, std::string("1090.0000 MHz"));
    EQ(at(m.blueprints, 3).note, std::string(""));
    for (std::size_t i = 0; i < m.blueprints.size(); ++i) { CHECK(at(m.blueprints, i).enabled); }

    // the towers: one per airport named in the frequency list, then the six nearest a known position; each once; the channels are the TWR ones
    const cc::Airport* a = firstTower();
    CHECK(a != nullptr);
    if (a == nullptr) { return; }
    Fx t(chain().g);
    t.t.add(FOXAPI_LIST_BOOKMARKS, 1, "x", cc::airportGroupName(*a).c_str());
    t.tickBump();
    t.page.blueprintsOpened(t.t);
    const PatchModel tm = t.page.bandModel();
    EQ(tm.blueprints.size(), std::size_t{1});
    EQ(at(tm.blueprints, 0).label, "Listen to the tower: " + cc::airportGroupName(*a));
    std::string want;
    for (const cc::AirbandFreq& q : a->freqs) {
        if (q.type != "TWR") { continue; }
        char b[64];
        std::snprintf(b, sizeof(b), "%s %.3f", q.type.c_str(), static_cast<double>(q.channelKhz) / 1000.0);
        if (!want.empty()) { want += '\n'; }
        want += b;
    }
    EQ(at(tm.blueprints, 0).note, want);
    CHECK(!want.empty());
    // a position known: the nearest six are added after the looked-up one, each once (the looked-up airport is not repeated)
    Fx n(chain().g);
    n.t.add(FOXAPI_LIST_BOOKMARKS, 1, "x", cc::airportGroupName(*a).c_str());
    n.st.flags = FOXAPI_RX_POSITION_SET;
    n.st.rxLatDeg = a->latDeg;
    n.st.rxLonDeg = a->lonDeg;
    n.tickBump();
    n.page.blueprintsOpened(n.t);
    Names titles;
    for (const SelectItem& i : n.page.bandModel().blueprints) { titles.push_back(i.label); }
    Names expect = {"Listen to the tower: " + cc::airportGroupName(*a)};
    for (const auto& [ap, km] : cc::nearestAirports(cc::airbandTable(), a->latDeg, a->lonDeg, 6)) {
        (void)km;
        if (ap == a) { continue; }
        bool hasTower = false, hasCtaf = false;
        for (const cc::AirbandFreq& q : ap->freqs) { hasTower = hasTower || q.type == "TWR"; hasCtaf = hasCtaf || q.type == "CTAF"; }
        if (!hasTower && !hasCtaf) { continue; }
        expect.push_back("Listen to the tower: " + cc::airportGroupName(*ap));
    }
    EQ(titles, expect);
    CHECK(titles.size() >= 2);
}

// the nodes this pick placed (and a reused radio), copied, as the window fits them
pg::View viewOfPlaced(const pc::Graph& g, const std::vector<pc::NodeId>& ids, float w, float h) {
    pc::Graph only;
    for (const pc::NodeId id : ids) {
        const pc::Node* n = g.find(id);
        if (n == nullptr) { continue; }
        if (pc::Node* c = only.mutableNode(only.addNode(n->kind, n->name))) { c->x = n->x; c->y = n->y; c->w = n->w; c->h = n->h; }
    }
    return pg::fitView(only, w, h, 1.0f);
}

void aBlueprintIsPlaced() {
    // a patch with its radio wired: a NEW radio is needed, so the scan is asked for first
    const Chain c = chain();
    Fx f(c.g);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars.dll", 0, 1, 0, 0);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 2, "ADS-B", "adsb.dll", 0, 0, 2.0e6, 1);
    f.t.add(FOXAPI_LIST_PRESETS, 1, "ADS-B", "ADS-B", 0, 1090.0e6, 0, 0, 2.4e6);
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 1, "NESDR", "rtlsdr|serial=1");
    f.tickBump();
    f.page.blueprintsOpened(f.t);
    // the list is: ACARS (no preset), ADS-B
    EQ(f.page.bandModel().blueprints.size(), std::size_t{2});
    f.sub.clear();
    const std::size_t before = f.g().nodes().size();
    f.select(c.channel);
    f.page.blueprintPicked(1, f.sub);                                      // ADS-B: an I/Q decoder with a preset
    EQ(f.sub.sent.size(), std::size_t{2});
    CHECK(fc(cmdAt(f.sub, 0)).op == 0x8406u && fc(cmdAt(f.sub, 1)).op == kPatchSetGraph);
    EQ(f.page.draft().graph().count(NodeKind::Radio), std::size_t{2});
    CHECK(f.g().nodes().size() > before);
    // a new radio: no device until the scan answers
    pc::NodeId newRadio = 0;
    for (const pc::Node& n : f.g().nodes()) { if (n.kind == NodeKind::Radio && n.id != c.radio) { newRadio = n.id; } }
    CHECK(newRadio != 0 && nd(f.g(), newRadio).device.empty());
    CHECK(f.page.draft().waits().size() == 1u && f.page.draft().waits()[0].scan_ticket == 100u);
    // the preset's frequency and rate were set; the decoder is the one named
    CHECK(nd(f.g(), newRadio).freqHz == 1090.0e6 && nd(f.g(), newRadio).centreChosen && nd(f.g(), newRadio).rateHz == 2.4e6);
    bool decoder = false;
    for (const pc::Node& n : f.g().nodes()) { decoder = decoder || (n.kind == NodeKind::Decoder && n.plugin == "adsb.dll"); }
    CHECK(decoder);
    // NOTHING IS SELECTED (a selection opens the inspector over the end of the chain just placed), and the view fits what was placed
    EQ(f.page.canvas().ui.selected, pc::NodeId{0});
    std::vector<pc::NodeId> added;
    for (const pc::Node& n : f.g().nodes()) { if (n.id > c.speaker) { added.push_back(n.id); } }
    const pg::View want = viewOfPlaced(f.g(), added, 800.0f, 500.0f);
    CHECK(f.page.canvas().ui.view.zoom == want.zoom && f.page.canvas().ui.view.pan == want.pan);
    EQ(f.page.blueprintNote(), std::string("Placed \"ADS-B\". Press START to run it."));
    CHECK(f.page.bandModel().blueprint_note == "Placed \"ADS-B\". Press START to run it.");
    // the scan answers: the device is chosen
    f.engineTakes();
    f.page.onAnswer(f.answer(100, 0x8406));
    f.tickBump();
    EQ(nd(f.g(), newRadio).device, std::string("rtlsdr|serial=1"));

    // a plugin with no preset frequency: the other note
    Fx h(c.g);
    h.t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars.dll", 0, 1, 0, 0);
    h.tickBump();
    h.page.blueprintsOpened(h.t);
    h.page.blueprintPicked(0, h.sub);
    EQ(h.page.blueprintNote(), std::string("Placed \"ACARS\". It has no preset frequency - type one in before pressing START."));
    // a bad index changes nothing
    const std::size_t sz = h.g().nodes().size();
    h.page.blueprintPicked(5, h.sub);
    h.page.blueprintPicked(-1, h.sub);
    EQ(h.g().nodes().size(), sz);
}

void aBlueprintReusesAnUnwiredRadio() {
    pc::Graph g;
    const pc::NodeId r = g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 60, 80);
    g.mutableNode(r)->device = "siggen";
    Fx f(g);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars.dll", 0, 1, 0, 0);
    f.tickBump();
    f.page.blueprintsOpened(f.t);
    f.sub.clear();
    f.page.blueprintPicked(0, f.sub);
    // no scan: no new radio is needed
    EQ(f.sub.count(0x8406), std::size_t{0});
    EQ(f.sub.count(kPatchSetGraph), std::size_t{1});
    EQ(f.g().count(NodeKind::Radio), std::size_t{1});
    CHECK(f.page.draft().waits().empty());
    EQ(nd(f.g(), r).device, std::string("siggen"));
    // the view fits the placed nodes and the reused radio
    std::vector<pc::NodeId> ids;
    for (const pc::Node& n : f.g().nodes()) { ids.push_back(n.id); }
    const pg::View want = viewOfPlaced(f.g(), ids, 800.0f, 500.0f);
    CHECK(f.page.canvas().ui.view.zoom == want.zoom && f.page.canvas().ui.view.pan == want.pan);
}

void aBlueprintFindsNoRoomForAnotherRadio() {
    // five radios, each wired: every radio is in use
    pc::Graph g;
    for (int i = 0; i < 5; ++i) {
        const pc::NodeId r = g.addNode(NodeKind::Radio, "R", PortType::Iq, 20.0f + 10.0f * i, 20.0f);
        g.mutableNode(r)->device = "dev" + std::to_string(i);
        const pc::NodeId ch = g.addNode(NodeKind::Channel, "C", PortType::Iq, 20.0f + 10.0f * i, 300.0f);
        g.connect(r, 0, ch, 0);
    }
    Fx f(g);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars.dll", 0, 1, 0, 0);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 2, "SSTV", "s.dll#image", 0, 1, 12000, 0);
    f.tickBump();
    f.page.blueprintsOpened(f.t);
    f.sub.clear();
    const std::size_t before = f.g().nodes().size();
    const pg::View view = f.page.canvas().ui.view;
    f.page.blueprintPicked(0, f.sub);
    EQ(f.g().nodes().size(), before);
    EQ(f.sub.sent.size(), std::size_t{0});                // no scan, no commit
    EQ(f.page.blueprintNote(), std::string("Every radio is in use, and a patch has at most 5. Free a radio to place \"ACARS\"."));
    CHECK(f.page.canvas().ui.view.zoom == view.zoom && f.page.canvas().ui.view.pan == view.pan);
    // a picture decoder's title says so in the note
    f.page.blueprintPicked(1, f.sub);
    EQ(f.page.blueprintNote(), std::string("Every radio is in use, and a patch has at most 5. Free a radio to place \"SSTV (picture)\"."));
}

void aTowerBlueprintCountsTheFrequenciesItLeftOut() {
    // an airport whose tower frequencies do not all fit one radio's band at 2 MS/s
    const cc::Airport* wide = nullptr;
    for (const cc::Airport& a : cc::airbandTable()) {
        pc::Blueprint bp;
        for (const cc::AirbandFreq& q : a.freqs) { if (q.type == "TWR") { bp.listen.push_back({q.hz, "x"}); } }
        if (bp.listen.size() < 2) { continue; }
        pc::Graph scratch;
        const pc::Placed p = pc::placeBlueprint(scratch, bp, "", kBlueprintDefaultRateHz);
        if (p.listenDropped > 0) { wide = &a; break; }
    }
    CHECK(wide != nullptr);
    if (wide == nullptr) { return; }
    Fx f(chain().g);
    f.t.add(FOXAPI_LIST_BOOKMARKS, 1, "x", cc::airportGroupName(*wide).c_str());
    f.tickBump();
    f.page.blueprintsOpened(f.t);
    CHECK(f.page.bandModel().blueprints.size() == 1u);
    f.page.blueprintPicked(0, f.sub);
    const std::string note = f.page.blueprintNote();
    const std::string title = "Listen to the tower: " + cc::airportGroupName(*wide);
    CHECK(note.rfind("Placed \"" + title + "\". It has no preset frequency", 0) == 0 || note.rfind("Placed \"" + title + "\". Press START to run it.", 0) == 0);
    CHECK(std::regex_search(note, std::regex(R"re( \d+ of its tower frequencies are outside this radio's band and were left out\.$)re")));
}

}  // namespace

namespace {

// ================================================================================================================================
// the information pane
// ================================================================================================================================

void fillPlan(StubTable& t, const Chain& c) {
    t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars_1.dll", 0, 1, 0, 0);
    t.add(FOXAPI_LIST_PATCH_PLAN, c.channel, "", "channel", 0, 100000.0, 48000.0, 42.0, static_cast<double>(c.radio));
    if (c.decoder != 0) { t.add(FOXAPI_LIST_PATCH_PLAN, c.decoder, "acars_1.dll", "decoder:audio", 0, 48000.0, 96000.0, 0.0, static_cast<double>(c.radio)); }
}

void theVerdictAndThePlan() {
    const Chain c = chain();
    Fx f(c.g);
    InfoModel m = f.page.infoModel();
    CHECK(!m.runnable);
    EQ(m.verdict, std::string("This patch cannot run yet."));
    EQ(m.plan_line, std::string(""));
    EQ(m.level_line, std::string(""));
    f.t.facts["fact.patch.runnable"] = "1";
    fillPlan(f.t, c);
    f.tickBump();
    m = f.page.infoModel();
    CHECK(m.runnable);
    EQ(m.verdict, std::string("This patch can run."));
    EQ(m.plan_line, std::string("1 channel(s), decimate by 42"));
    // the first channel's decimation, however many there are
    f.t.add(FOXAPI_LIST_PATCH_PLAN, 99, "", "channel", 0, 0, 12000, 7, 1);
    f.tickBump();
    EQ(f.page.infoModel().plan_line, std::string("2 channel(s), decimate by 42"));
}

void withNothingSelectedItHelps() {
    Fx f(chain().g);
    const InfoModel m = f.page.infoModel();
    CHECK(m.none_selected);
    EQ(m.help.size(), std::size_t{3});
    EQ(at(m.help, 0), std::string("Nothing selected."));
    EQ(at(m.help, 1), std::string("Click a node to set what it does. Drag from one port to another to wire them; drag a node by its title bar to move it. Delete removes whatever is selected."));
    EQ(at(m.help, 2), std::string("Drag empty canvas or scroll to move round the patch. Ctrl+scroll or the + and - keys zoom; Fit shows every node."));
    EQ(m.selection, std::string(""));
    EQ(m.folded_note, std::string(""));
    CHECK(m.problems.empty());
    // the inspector is not shown with nothing selected
    CHECK(!f.page.bandModel().inspector_shown);
    // a selection that is not a node (an id nobody holds) is nothing selected
    f.select(77);
    CHECK(f.page.infoModel().none_selected);
}

void aSelectedNodeIsNamedAndItsProblemsListed() {
    const Chain c = chain();
    Fx f(c.g);
    f.t.add(FOXAPI_LIST_PATCH_PROBLEMS, c.channel, "2", "");      // no frequency set: blocking
    f.t.add(FOXAPI_LIST_PATCH_PROBLEMS, c.channel, "5", "");      // nothing is listening to this: advisory
    f.t.add(FOXAPI_LIST_PATCH_PROBLEMS, c.demod, "1", "");
    f.tickBump();
    f.select(c.channel);
    InfoModel m = f.page.infoModel();
    CHECK(!m.none_selected && m.help.empty());
    EQ(m.selection, std::string("CHANNEL  Tower"));
    EQ(m.problems.size(), std::size_t{2});
    CHECK(at(m.problems, 0).text == "no frequency set" && !at(m.problems, 0).advisory);
    CHECK(at(m.problems, 1).text == "nothing is listening to this" && at(m.problems, 1).advisory);
    EQ(m.folded_note, std::string(""));
    CHECK(f.page.bandModel().inspector_shown && !f.page.bandModel().inspector_folded);
    // FOLDED: the settings are away, and the pane says where
    f.page.inspectorFold();
    CHECK(f.page.folded());
    m = f.page.infoModel();
    EQ(m.folded_note, std::string("Its settings are folded away at the right of the canvas - press << to open them."));
    CHECK(f.page.bandModel().inspector_shown && f.page.bandModel().inspector_folded);
    f.page.inspectorFold();
    CHECK(!f.page.folded() && f.page.infoModel().folded_note.empty());
    // a node with no problem lists none
    f.select(c.radio);
    EQ(f.page.infoModel().selection, std::string("RADIO  Radio"));
    CHECK(f.page.infoModel().problems.empty());
    // every problem line is the core's own sentence
    const std::pair<int, const char*> lines[] = {
        {0, "nothing feeds this"}, {1, "no radio upstream of this"}, {2, "no frequency set"}, {3, "outside the band the radio is receiving"},
        {4, "no whole division of this sample rate lands in the audio band"}, {5, "nothing is listening to this"}, {6, "no plugin chosen for this decoder"},
        {7, "that plugin is not installed or did not load"}, {8, "that plugin takes a different input from the one wired here"},
        {9, "that plugin needs a faster sample rate than its source produces"}, {10, "the plugin refused to start - it may need a different rate"},
        {11, "no device chosen for this radio"}, {12, "another radio in this patch already uses that device - one device, one radio"},
        {13, "the device would not open"}, {14, "this decoder puts nothing on a map"}};
    Fx h(c.g);
    for (const auto& l : lines) { h.t.add(FOXAPI_LIST_PATCH_PROBLEMS, c.radio, std::to_string(l.first).c_str(), ""); }
    h.tickBump();
    h.select(c.radio);
    const InfoModel pm = h.page.infoModel();
    EQ(pm.problems.size(), std::size_t{15});
    for (std::size_t i = 0; i < 15; ++i) { EQ(at(pm.problems, i).text, std::string(lines[i].second)); }
    CHECK(at(pm.problems, 5).advisory && at(pm.problems, 14).advisory && !at(pm.problems, 0).advisory && !at(pm.problems, 13).advisory);
}

void theChannelLevelIsSaidOnlyForAChannel() {
    const Chain c = chain(true);
    Fx f(c.g);
    fillPlan(f.t, c);
    f.t.add(FOXAPI_LIST_PATCH_LIVE, c.channel, "", "", 0, -61.5, 0, 3);
    f.t.add(FOXAPI_LIST_PATCH_LIVE, c.decoder, "", "a line", 0, 4, 0, 4);
    f.tickBump();
    f.select(c.channel);
    EQ(f.page.infoModel().level_line, std::string("-61.5 dB on this channel"));
    // THE WINDOW'S OWN LOOP TESTS ONLY THE NODE ID and prints "0.0 dB on this channel" for a running decoder: not ported
    f.select(c.decoder);
    EQ(f.page.infoModel().level_line, std::string(""));
    f.select(c.demod);
    EQ(f.page.infoModel().level_line, std::string(""));
    // a channel with no level row (its radio is not running) says nothing
    Fx g(c.g);
    fillPlan(g.t, c);
    g.tickBump();
    g.select(c.channel);
    EQ(g.page.infoModel().level_line, std::string(""));
    // a channel that is not planned has no level to say
    Fx h(c.g);
    h.t.add(FOXAPI_LIST_PATCH_LIVE, c.channel, "", "", 0, -61.0, 0, 3);
    h.tickBump();
    h.select(c.channel);
    EQ(h.page.infoModel().level_line, std::string(""));
}

// ================================================================================================================================
// the faces
// ================================================================================================================================

const CanvasNode& nodeOf(const CanvasModel& m, pc::NodeId id) {
    static const CanvasNode none{};
    for (const CanvasNode& n : m.nodes) { if (n.id == static_cast<int>(id)) { return n; } }
    return none;
}
std::string lineTexts(const std::vector<FaceLine>& v) {
    std::string s;
    for (const FaceLine& l : v) { s += "[" + std::to_string(l.tone) + ":" + l.text + "]"; }
    return s;
}

void theRadioFace() {
    pc::Graph g;
    const pc::NodeId r = g.addNode(NodeKind::Radio, "Radio", PortType::Iq, 60, 80);
    pc::Node* n = g.mutableNode(r);
    n->device = "rtlsdr|serial=1";
    n->freqHz = 100.0e6;
    n->centreChosen = true;
    Fx f(g);
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 1, "NESDR Smart", "rtlsdr|serial=1");
    f.tickBump();
    FaceModel fm = nodeOf(f.page.canvasModel(), r).face;
    EQ(fm.device_label, std::string("NESDR Smart"));
    EQ(fm.centre_text, std::string("100.000000"));
    EQ(fm.radio_switch, 1);                                           // on, and waiting for the patch
    EQ(fm.radio_switch_label, std::string("ON"));
    EQ(fm.radio_switch_tooltip, std::string("This radio is switched on. Press to close it alone - the rest\nof the patch keeps running."));
    EQ(fm.centre_tooltip, std::string("The radio's centre. Type a frequency and press Enter."));
    EQ(lineTexts(fm.lines), std::string("[1:Ready - press START.]"));
    CHECK(fm.centre_note.empty() && lineTexts(fm.lines_after).empty());

    // THE ROW OPEN: the rate in amber, the switch lit
    f.t.add(FOXAPI_LIST_PATCH_RADIOS, r, "rtlsdr|serial=1", "", kActive, 2.4e6, 100.0e6, 0);
    f.st.flags = FOXAPI_RX_PATCH_RUNNING;
    f.tickBump();
    fm = nodeOf(f.page.canvasModel(), r).face;
    EQ(fm.radio_switch, 2);
    EQ(lineTexts(fm.lines), std::string("[2:2.400 MS/s]"));
    // switched off while its row is still open (the engine closes it a pass later): the switch shows OFF at once, as the window's does
    nm(f.g(), r).on = false;
    fm = nodeOf(f.page.canvasModel(), r).face;
    EQ(fm.radio_switch, 0);
    EQ(fm.radio_switch_label, std::string("OFF"));
    EQ(fm.radio_switch_tooltip, std::string("This radio is switched off. Press to switch it on; it opens\nwhile the patch is running."));
    nm(f.g(), r).on = true;

    // PENDING: its device is opening
    f.t.lists[FOXAPI_LIST_PATCH_RADIOS].clear();
    f.t.add(FOXAPI_LIST_PATCH_RADIOS, r, "rtlsdr|serial=1", "", 0, 0, 0, 1);
    f.tickBump();
    fm = nodeOf(f.page.canvasModel(), r).face;
    EQ(fm.radio_switch, 1);
    EQ(lineTexts(fm.lines), std::string("[1:opening...]"));
    // no device
    f.t.lists[FOXAPI_LIST_PATCH_RADIOS].clear();
    f.tickBump();
    nm(f.g(), r).device.clear();
    fm = nodeOf(f.page.canvasModel(), r).face;
    EQ(fm.device_label, std::string("No device chosen"));
    EQ(lineTexts(fm.lines), std::string("[1:Choose a device on the right.]"));
    nm(f.g(), r).device = "rtlsdr|serial=1";
    // switched off
    nm(f.g(), r).on = false;
    EQ(lineTexts(nodeOf(f.page.canvasModel(), r).face.lines), std::string("[1:Switched off.]"));
    nm(f.g(), r).on = true;
    // the patch not running: "Ready"; running: nothing (or the device sentence)
    f.st.flags = 0;
    f.tickBump();
    EQ(lineTexts(nodeOf(f.page.canvasModel(), r).face.lines), std::string("[1:Ready - press START.]"));
    f.st.flags = FOXAPI_RX_PATCH_RUNNING;
    f.tickBump();
    EQ(lineTexts(nodeOf(f.page.canvasModel(), r).face.lines), std::string(""));
    f.t.add(FOXAPI_LIST_PATCH_PROBLEMS, r, "12", "");        // DeviceTwice
    f.t.add(FOXAPI_LIST_PATCH_PROBLEMS, r, "5", "");         // another problem says nothing here
    f.tickBump();
    EQ(lineTexts(nodeOf(f.page.canvasModel(), r).face.lines), std::string("[4:Another radio here already uses this device.]"));
    // not running, the same problem is not said
    f.st.flags = 0;
    f.tickBump();
    EQ(lineTexts(nodeOf(f.page.canvasModel(), r).face.lines), std::string("[1:Ready - press START.]"));
    // THE ROW'S NOTE in rust, in every case, after the rest
    f.t.add(FOXAPI_LIST_PATCH_RADIOS, r, "rtlsdr|serial=1", "the device would not open", kFault, 0, 0, 0);
    f.tickBump();
    EQ(lineTexts(nodeOf(f.page.canvasModel(), r).face.lines), std::string("[1:Ready - press START.][4:the device would not open]"));
    CHECK(nodeOf(f.page.canvasModel(), r).fault);
    f.t.lists[FOXAPI_LIST_PATCH_RADIOS].clear();
    f.t.add(FOXAPI_LIST_PATCH_RADIOS, r, "rtlsdr|serial=1", "tuned to 99.0 MHz", kActive, 2.0e6, 99.0e6, 0);
    f.tickBump();
    EQ(lineTexts(nodeOf(f.page.canvasModel(), r).face.lines), std::string("[2:2.000 MS/s][4:tuned to 99.0 MHz]"));
}

void theChannelDemodAndSinkFaces() {
    const Chain c = chain(true);
    Fx f(c.g);
    fillPlan(f.t, c);
    f.t.add(FOXAPI_LIST_PATCH_LIVE, c.channel, "", "", 0, -61.7, 0, 3);
    f.tickBump();
    CanvasModel m = f.page.canvasModel();
    // CHANNEL: its frequency (the reading is the canvas's own line: "-62 dB" in amber)
    const CanvasNode& ch = nodeOf(m, c.channel);
    EQ(ch.face.freq_text, std::string("100.100000"));
    EQ(ch.face.freq_tooltip, std::string("Type a frequency and press Enter."));
    EQ(ch.reading, std::string("-62 dB"));
    EQ(ch.reading_tone, 2);
    EQ(ch.reading2, std::string(""));
    // a channel with no level row, or not planned, has no reading
    CHECK(nodeOf(m, c.radio).reading.empty() && nodeOf(m, c.demod).reading.empty() && nodeOf(m, c.speaker).reading.empty());

    // DEMOD
    pc::Node& dn = nm(f.g(), c.demod);
    dn.mode = 4;
    dn.squelch = true;
    dn.squelchDb = -50.0f;
    FaceModel fm = nodeOf(f.page.canvasModel(), c.demod).face;
    EQ(fm.mode_index, 4);
    CHECK(fm.squelch_on);
    CHECK_NEAR(fm.squelch_norm, 70.0 / 120.0, 1e-6);
    EQ(fm.squelch_text, std::string("-50 dB"));
    CHECK(!fm.gate_shown);
    EQ(fm.squelch_tooltip, std::string("Silences this demodulator's sound while the channel is quieter\nthan the threshold - so a speaker or a recording hears signals,\nnot the noise between them. Decoders are still given every sample."));
    nm(f.g(), c.demod).mode = -3;
    EQ(nodeOf(f.page.canvasModel(), c.demod).face.mode_index, 0);
    nm(f.g(), c.demod).mode = 12;
    EQ(nodeOf(f.page.canvasModel(), c.demod).face.mode_index, 7);
    // the gate: the level it judges and whether it is open
    f.t.add(FOXAPI_LIST_PATCH_LIVE, c.demod, "", "", kActive, -62.6, 0, 1);
    f.tickBump();
    fm = nodeOf(f.page.canvasModel(), c.demod).face;
    CHECK(fm.gate_shown && fm.gate_lit);
    EQ(fm.gate_text, std::string("-63 dB"));
    EQ(fm.gate_word, std::string("open"));
    f.t.lists[FOXAPI_LIST_PATCH_LIVE].back().flags = 0;
    f.tickBump();
    fm = nodeOf(f.page.canvasModel(), c.demod).face;
    CHECK(fm.gate_shown && !fm.gate_lit);
    EQ(fm.gate_word, std::string("closed"));
    // no squelch: the gate is always "open" in effect, and says it is not gating
    nm(f.g(), c.demod).squelch = false;
    fm = nodeOf(f.page.canvasModel(), c.demod).face;
    CHECK(!fm.squelch_on && fm.gate_shown && fm.gate_lit);
    EQ(fm.gate_word, std::string("no squelch"));
    nm(f.g(), c.demod).squelch = true;
    nm(f.g(), c.demod).squelchDb = -120.0f;
    CHECK(nodeOf(f.page.canvasModel(), c.demod).face.squelch_norm == 0.0f);
    nm(f.g(), c.demod).squelchDb = 0.0f;
    CHECK(nodeOf(f.page.canvasModel(), c.demod).face.squelch_norm == 1.0f);
    EQ(nodeOf(f.page.canvasModel(), c.demod).face.squelch_text, std::string("0 dB"));
}

void theSpeakerFace() {
    const Chain c = chain(true);
    Fx f(c.g);
    fillPlan(f.t, c);
    f.tickBump();
    // WIRED TO NO DEMODULATOR: one sentence and nothing more
    pc::Graph bare;
    const pc::NodeId s0 = bare.addNode(NodeKind::Sink, "Speaker", PortType::Audio, 300, 300);
    Fx b(bare);
    FaceModel fm = nodeOf(b.page.canvasModel(), s0).face;
    EQ(lineTexts(fm.lines), std::string("[1:Wire a demodulator here to listen.]"));
    CHECK(!fm.level_shown && fm.lines_after.empty());
    // fed by a channel: its name in phosphor; its radio not running yet
    fm = nodeOf(f.page.canvasModel(), c.speaker).face;
    EQ(lineTexts(fm.lines), std::string("[3:Tower][1:Waiting for its radio.]"));
    CHECK(!fm.level_shown && fm.lines_after.empty());
    // running: where it plays, the level bar, the error
    f.t.add(FOXAPI_LIST_PATCH_LIVE, c.speaker, "The speakers", "", kActive, 0.4, 96000, 2);
    f.tickBump();
    fm = nodeOf(f.page.canvasModel(), c.speaker).face;
    EQ(lineTexts(fm.lines), std::string("[3:Tower][1:The speakers]"));
    CHECK(fm.level_shown);
    CHECK_NEAR(fm.level_norm, 0.4, 1e-6);
    CHECK(fm.lines_after.empty());
    f.t.lists[FOXAPI_LIST_PATCH_LIVE].back().detail[0] = 'o';
    f.t.lists[FOXAPI_LIST_PATCH_LIVE].back().detail[1] = 'h';
    f.t.lists[FOXAPI_LIST_PATCH_LIVE].back().detail[2] = '\0';
    f.t.lists[FOXAPI_LIST_PATCH_LIVE].back().value[0] = 1.7;
    f.tickBump();
    fm = nodeOf(f.page.canvasModel(), c.speaker).face;
    EQ(lineTexts(fm.lines_after), std::string("[4:oh]"));
    CHECK(fm.level_norm == 1.0f);
    f.t.lists[FOXAPI_LIST_PATCH_LIVE].back().value[0] = -0.2;
    f.tickBump();
    CHECK(nodeOf(f.page.canvasModel(), c.speaker).face.level_norm == 0.0f);
    // an output that could not be made: why, after the rest
    f.t.add(FOXAPI_LIST_PATCH_LIVE, c.speaker, "", "No such device", 0, 0, 0, 5);
    f.tickBump();
    fm = nodeOf(f.page.canvasModel(), c.speaker).face;
    EQ(lineTexts(fm.lines_after), std::string("[4:oh][4:No such device]"));
    // without the output row, the failure is still said (after "Waiting for its radio.")
    Fx k(c.g);
    fillPlan(k.t, c);
    k.t.add(FOXAPI_LIST_PATCH_LIVE, c.speaker, "", "No such device", 0, 0, 0, 5);
    k.tickBump();
    fm = nodeOf(k.page.canvasModel(), c.speaker).face;
    EQ(lineTexts(fm.lines), std::string("[3:Tower][1:Waiting for its radio.]"));
    EQ(lineTexts(fm.lines_after), std::string("[4:No such device]"));
    CHECK(!fm.level_shown);
}

void theTextOutFace() {
    const Chain c = chain(true);
    Fx f(c.g);
    fillPlan(f.t, c);
    f.tickBump();
    EQ(lineTexts(nodeOf(f.page.canvasModel(), c.textOut).face.lines), std::string("[1:Decoded lines appear here.]"));
    // the lines the engine's decoder said, routed to the Text out it is wired to: newest at the bottom, in phosphor
    for (int i = 1; i <= 8; ++i) {
        const std::string text = "line " + std::to_string(i);
        f.t.add(FOXAPI_LIST_DECODER_LINES, static_cast<std::uint64_t>(i), "ACARS", text.c_str(), 0, static_cast<double>(c.decoder));
    }
    f.tickBump();
    const CanvasModel cm = f.page.canvasModel();
    const CanvasNode& n = nodeOf(cm, c.textOut);
    // fit = max(1, int((h - 40) / 13.1) - 1) = int(76 / 13.1) - 1 = 4 for the default 116 high
    EQ(lineTexts(n.face.lines), std::string("[3:ACARS: line 5][3:ACARS: line 6][3:ACARS: line 7][3:ACARS: line 8]"));
    // a taller part shows more, and every line is kept (200 at most)
    nm(f.g(), c.textOut).h = 300.0f;
    EQ(nodeOf(f.page.canvasModel(), c.textOut).face.lines.size(), std::size_t{8});          // int(260/13.1) - 1 = 18: all eight
    nm(f.g(), c.textOut).h = 56.0f;                                                          // the smallest: int(16/13.1) - 1 = 0 -> one line
    EQ(lineTexts(nodeOf(f.page.canvasModel(), c.textOut).face.lines), std::string("[3:ACARS: line 8]"));
    // a line from a decoder wired to no Text out goes nowhere; the cursor moves on (a line is routed once)
    f.t.add(FOXAPI_LIST_DECODER_LINES, 9, "ACARS", "stray", 0, 999);
    f.t.add(FOXAPI_LIST_DECODER_LINES, 10, "Receiver", "own decoder", 0, 0);             // node 0: the receiver's own
    f.tickBump();
    nm(f.g(), c.textOut).h = 116.0f;
    EQ(lineTexts(nodeOf(f.page.canvasModel(), c.textOut).face.lines), std::string("[3:ACARS: line 5][3:ACARS: line 6][3:ACARS: line 7][3:ACARS: line 8]"));
    f.tickBump();
    f.tickBump();
    EQ(f.page.sinkLines().routedId(), std::uint64_t{10});
    EQ(queueOf(f.page, c.textOut).size(), std::size_t{8});
}

void textOutQueuesAreBoundedAndForgotten() {
    const Chain c = chain(true);
    Fx f(c.g);
    fillPlan(f.t, c);
    f.tickBump();
    for (int i = 1; i <= 250; ++i) { f.t.add(FOXAPI_LIST_DECODER_LINES, static_cast<std::uint64_t>(i), "ACARS", "x", 0, static_cast<double>(c.decoder)); }
    f.tickBump();
    EQ(queueOf(f.page, c.textOut).size(), kSinkLinesCap);
    // two Text outs on one decoder both get it; a sink's queue goes with the sink
    pc::Graph g = c.g;
    const pc::NodeId t2 = g.addNode(NodeKind::Sink, "Text two", PortType::Text, 960, 500);
    g.connect(c.decoder, 0, t2, 0);
    f.publish(g);
    f.tickBump();
    f.t.add(FOXAPI_LIST_DECODER_LINES, 251, "ACARS", "both", 0, static_cast<double>(c.decoder));
    f.tickBump();
    EQ(queueOf(f.page, t2).size(), std::size_t{1});
    EQ(backOf(queueOf(f.page, c.textOut)), std::string("ACARS: both"));
    f.g().removeNode(t2);
    f.tickBump();
    EQ(f.page.sinkLines().queues().count(t2), std::size_t{0});
    CHECK(f.page.sinkLines().queues().count(c.textOut) == 1u);
}

void theDecoderFaceReadsLinesAndPicture() {
    const Chain c = chain(true);
    Fx f(c.g);
    fillPlan(f.t, c);
    f.tickBump();
    // a planned decoder says how many lines it has said, even none
    CanvasNode n = nodeOf(f.page.canvasModel(), c.decoder);
    EQ(n.reading, std::string("0 lines"));
    EQ(n.reading_tone, 2);
    EQ(n.reading2, std::string(""));
    f.t.add(FOXAPI_LIST_PATCH_LIVE, c.decoder, "", "the newest line", 0, 1, 0, 4);
    f.tickBump();
    n = nodeOf(f.page.canvasModel(), c.decoder);
    EQ(n.reading, std::string("1 line"));
    EQ(n.reading2, std::string("the newest line"));
    f.t.lists[FOXAPI_LIST_PATCH_LIVE].back().value[0] = 12;
    f.tickBump();
    EQ(nodeOf(f.page.canvasModel(), c.decoder).reading, std::string("12 lines"));
    // a refused decoder says nothing
    f.t.add(FOXAPI_LIST_PATCH_NODES, c.decoder, "ACARS", "decoder", kRefused);
    f.tickBump();
    EQ(nodeOf(f.page.canvasModel(), c.decoder).reading, std::string(""));
    // the picture: a picture decoder's newest image id, taken once
    pc::Graph g = c.g;
    const pc::NodeId pic = g.addNode(NodeKind::Decoder, "SSTV", PortType::Audio, 660, 500);
    g.mutableNode(pic)->plugin = "s.dll#image";
    f.publish(g);
    f.t.add(FOXAPI_LIST_PATCH_LIVE, pic, "caption", "", kActive, 7, 1234, 6);
    f.tickBump();
    EQ(f.page.picturesToRead().size(), std::size_t{1});
    CHECK(at(f.page.picturesToRead(), 0).node == pic && at(f.page.picturesToRead(), 0).image_id == 7u && at(f.page.picturesToRead(), 0).bytes == 1234u);
    CHECK(!nodeOf(f.page.canvasModel(), pic).face.have_picture);
    f.page.pictureTaken(pic, 7);
    CHECK(f.page.picturesToRead().empty());
    FaceModel fm = nodeOf(f.page.canvasModel(), pic).face;
    CHECK(fm.have_picture && fm.picture_id == 7u);
    // a newer picture: read again
    f.t.lists[FOXAPI_LIST_PATCH_LIVE].back().value[0] = 8;
    f.tickBump();
    CHECK(f.page.picturesToRead().size() == 1u && at(f.page.picturesToRead(), 0).image_id == 8u);
    // the row goes (the node is no longer a picture's): the picture is dropped
    f.t.lists[FOXAPI_LIST_PATCH_LIVE].pop_back();
    f.tickBump();
    CHECK(!nodeOf(f.page.canvasModel(), pic).face.have_picture);
    f.t.add(FOXAPI_LIST_PATCH_LIVE, pic, "caption", "", kActive, 8, 1234, 6);
    f.tickBump();
    CHECK(!nodeOf(f.page.canvasModel(), pic).face.have_picture);          // it was dropped, so it is read afresh
    EQ(f.page.picturesToRead().size(), std::size_t{1});
    // a node the draft no longer holds drops its picture too
    f.page.pictureTaken(pic, 8);
    CHECK(nodeOf(f.page.canvasModel(), pic).face.have_picture);
    f.g().removeNode(pic);
    f.tickBump();
    CHECK(f.page.picturesToRead().empty());
    // picturesWanted itself: only decoder nodes with a kind-6 row
    std::vector<LiveRow> rows;
    LiveRow r6;
    r6.node = 1;
    r6.kind = 6;
    r6.v0 = 3;
    r6.v1 = 99;
    rows.push_back(r6);
    pc::Graph pg1;
    pg1.addNode(NodeKind::Decoder, "D", PortType::Audio, 0, 0);
    std::map<pc::NodeId, std::uint64_t> held;
    EQ(picturesWanted(pg1, rows, held).size(), std::size_t{1});
    held[1] = 3;
    EQ(picturesWanted(pg1, rows, held).size(), std::size_t{0});
    rows[0].kind = 4;
    held.clear();
    EQ(picturesWanted(pg1, rows, held).size(), std::size_t{0});
}

void theDisplayAndMapFaces() {
    const Chain c = chain(true);
    pc::Graph g = c.g;
    const pc::NodeId disp = g.addNode(NodeKind::Display, "Spectrum", PortType::Iq, 40, 400);
    const pc::NodeId disp2 = g.addNode(NodeKind::Display, "Spectrum 2", PortType::Iq, 340, 400);
    const pc::NodeId disp3 = g.addNode(NodeKind::Display, "Spectrum 3", PortType::Iq, 640, 400);
    g.connect(c.radio, 0, disp, 0);
    g.connect(c.channel, 0, disp2, 0);
    const pc::NodeId mapn = g.addNode(NodeKind::Map, "Map", PortType::Track, 700, 20);
    Fx f(g);
    fillPlan(f.t, c);
    f.tickBump();
    // the radio is not running: a Radio feeder with no running rate is not a spectrum yet
    EQ(lineTexts(nodeOf(f.page.canvasModel(), disp).face.lines), std::string("[1:Wire the radio or a channel here to see its spectrum.]"));
    // a planned channel, its radio not open
    EQ(lineTexts(nodeOf(f.page.canvasModel(), disp2).face.lines), std::string("[1:No spectrum yet - its radio is not running.]"));
    // nothing wired
    EQ(lineTexts(nodeOf(f.page.canvasModel(), disp3).face.lines), std::string("[1:Wire the radio or a channel here to see its spectrum.]"));
    // the radio is open: the deferred face draws nothing but its plate
    f.t.add(FOXAPI_LIST_PATCH_RADIOS, c.radio, "siggen", "", kActive, 2.0e6, 100.0e6, 0);
    f.tickBump();
    EQ(lineTexts(nodeOf(f.page.canvasModel(), disp).face.lines), std::string(""));
    EQ(lineTexts(nodeOf(f.page.canvasModel(), disp2).face.lines), std::string(""));
    EQ(lineTexts(nodeOf(f.page.canvasModel(), disp3).face.lines), std::string("[1:Wire the radio or a channel here to see its spectrum.]"));
    // an unplanned channel is not a spectrum source
    f.t.lists[FOXAPI_LIST_PATCH_PLAN].clear();
    f.tickBump();
    EQ(lineTexts(nodeOf(f.page.canvasModel(), disp2).face.lines), std::string("[1:Wire the radio or a channel here to see its spectrum.]"));

    // THE MAP: the sentence while nothing is wired; "no targets yet" once something is; the counts of its own sources' targets
    EQ(lineTexts(nodeOf(f.page.canvasModel(), mapn).face.lines), std::string("[1:Wire a decoder's map output here - ADS-B, AIS, APRS - up to 5.]"));
    pc::Graph g2 = g;
    const pc::NodeId adsb = g2.addNode(NodeKind::Decoder, "ADS-B", PortType::Iq, 400, 300);
    g2.mutableNode(adsb)->plugin = "adsb.dll";
    g2.connect(c.radio, 0, adsb, 0);
    g2.connect(adsb, 1, mapn, 0);
    f.publish(g2);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 2, "ADS-B", "adsb.dll", 0, 0, 0, 1);
    f.tickBump();
    EQ(lineTexts(nodeOf(f.page.canvasModel(), mapn).face.lines), std::string("[2:no targets yet]"));
    f.page.setTracks({{"ADS-B", CASCADE_TRACK_AIRCRAFT}, {"ADS-B", CASCADE_TRACK_AIRCRAFT}, {"ADS-B", CASCADE_TRACK_VESSEL}, {"Other", CASCADE_TRACK_AIRCRAFT}});
    EQ(lineTexts(nodeOf(f.page.canvasModel(), mapn).face.lines), std::string("[2:2 aircraft  1 vessels  0 stations]"));
    f.page.setTracks({{"ADS-B", CASCADE_TRACK_STATION}, {"ADS-B", 99u}});
    EQ(lineTexts(nodeOf(f.page.canvasModel(), mapn).face.lines), std::string("[2:0 aircraft  0 vessels  1 stations  +]"));
}

}  // namespace

namespace {

// ================================================================================================================================
// the inspector
// ================================================================================================================================

void theInspectorOfAnythingNamesTheNode() {
    const Chain c = chain();
    Fx f(c.g);
    CHECK(!f.page.inspectorModel().shown);
    f.select(c.demod);
    InspectorModel m = f.page.inspectorModel();
    CHECK(m.shown && !m.folded);
    EQ(m.node, static_cast<int>(c.demod));
    EQ(m.kind, 2);
    EQ(m.kind_caption, std::string("DEMOD"));
    EQ(m.name, std::string("Demod"));
    EQ(m.fold_tooltip, std::string("Fold the inspector away to give the canvas its width"));
    f.page.inspectorFold();
    m = f.page.inspectorModel();
    CHECK(m.shown && m.folded);
    EQ(m.fold_tooltip, std::string("Open the inspector: the selected node's settings"));
    // NAME: applied as typed, cut to 63 bytes on a character boundary
    f.page.inspectorFold();
    f.page.draft().dirty = false;
    f.page.inspectorName("My demodulator");
    EQ(nd(f.g(), c.demod).name, std::string("My demodulator"));
    CHECK(f.page.draft().dirty);
    EQ(f.page.inspectorModel().name, std::string("My demodulator"));
    f.page.inspectorName(std::string(80, 'x'));
    EQ(nd(f.g(), c.demod).name, std::string(63, 'x'));
    f.page.inspectorName(std::string(62, 'y') + "\xC3\xA9");
    EQ(nd(f.g(), c.demod).name, std::string(62, 'y'));
    // nothing selected: no name to set
    f.select(0);
    f.page.inspectorName("zzz");
    EQ(nd(f.g(), c.demod).name, std::string(62, 'y'));
}

void theRadioInspectorLists() {
    pc::Graph g;
    const pc::NodeId r1 = g.addNode(NodeKind::Radio, "Alpha", PortType::Iq, 40, 40);
    const pc::NodeId r2 = g.addNode(NodeKind::Radio, "Beta", PortType::Iq, 40, 300);
    g.mutableNode(r1)->device = "rtlsdr|serial=1";
    g.mutableNode(r1)->freqHz = 100.0e6;
    g.mutableNode(r1)->centreChosen = true;
    g.mutableNode(r2)->device = "airspy|serial=9";
    Fx f(g);
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 1, "Signal generator", "siggen");
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 2, "NESDR Smart", "rtlsdr|serial=1");
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 3, "Airspy", "airspy|serial=9");
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 4, "Recording a", "iqfile|path=C:\\a.cu8", 0, 2.4e6);
    f.t.facts["fact.patch.receiverRadio"] = "rtlsdr|serial=1";
    f.tickBump();
    f.select(r1);
    InspectorRadio m = f.page.inspectorModel().radio;
    EQ(m.device_items.size(), std::size_t{4});
    EQ(at(m.device_items, 0).label, std::string("Signal generator"));
    EQ(at(m.device_items, 0).id, std::string("siggen"));
    CHECK(at(m.device_items, 0).enabled);
    EQ(at(m.device_items, 1).label, std::string("NESDR Smart"));        // its own device: not "used by" itself
    CHECK(at(m.device_items, 1).enabled);
    EQ(at(m.device_items, 2).label, std::string("Airspy  (used by Beta)"));
    CHECK(!at(m.device_items, 2).enabled);
    EQ(at(m.device_items, 2).id, std::string("airspy|serial=9"));
    EQ(at(m.device_items, 3).id, std::string("iqfile|path=C:\\a.cu8"));
    EQ(m.device_index, 1);
    EQ(m.device_label, std::string("NESDR Smart"));
    CHECK(!m.looking && m.look_enabled);
    EQ(m.centre_text, std::string("100.000000"));
    // another radio's device is "used by" it, receiver's or not; the node's own is plain
    f.st.flags = FOXAPI_RX_DEVICE_OPEN;
    f.tickBump();
    f.select(r2);
    m = f.page.inspectorModel().radio;
    EQ(at(m.device_items, 1).label, std::string("NESDR Smart  (used by Alpha)"));
    CHECK(!at(m.device_items, 1).enabled);
    EQ(at(m.device_items, 2).label, std::string("Airspy"));             // the node's own device
    CHECK(at(m.device_items, 2).enabled);
    EQ(m.device_index, 2);
    // the receiver's own radio is lent when the patch starts - said only while a radio of the receiver's is open, and on the row of a device nobody else holds
    f.select(r1);
    m = f.page.inspectorModel().radio;
    EQ(at(m.device_items, 1).label, std::string("NESDR Smart  (the receiver's - lent to the patch when it starts)"));
    nm(f.g(), r2).device = "rtlsdr|serial=1";
    m = f.page.inspectorModel().radio;
    EQ(at(m.device_items, 1).label, std::string("NESDR Smart  (used by Beta)"));
    CHECK(!at(m.device_items, 1).enabled);
    nm(f.g(), r2).device = "airspy|serial=9";
    // a device the list does not hold: no row is current, the label says what the key says
    nm(f.g(), r1).device = "rtlsdr|serial=77";
    m = f.page.inspectorModel().radio;
    EQ(m.device_index, -1);
    EQ(m.device_label, std::string("rtlsdr 77 (not listed)"));
    nm(f.g(), r1).device = "rtlsdr|serial=1";
    // a scan under way: Look for radios is dead and says so
    f.st.sourceFlags = FOXAPI_SRC_SCAN_PENDING;
    f.tickBump();
    m = f.page.inspectorModel().radio;
    CHECK(m.looking && !m.look_enabled);
    EQ(f.page.inspectorLook().size(), std::size_t{0});
    f.st.sourceFlags = 0;
    f.tickBump();
    EQ(f.page.inspectorLook().size(), std::size_t{3});
    EQ(f.page.inspectorDeviceOpened().size(), std::size_t{2});
}

void theRadioInspectorRatesAndLines() {
    pc::Graph g;
    const pc::NodeId r = g.addNode(NodeKind::Radio, "Alpha", PortType::Iq, 40, 40);
    g.mutableNode(r)->device = "rtlsdr|serial=1";
    g.mutableNode(r)->freqHz = 145.5e6;
    g.mutableNode(r)->centreChosen = true;
    Fx f(g);
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 1, "NESDR Smart", "rtlsdr|serial=1");
    f.tickBump();
    f.select(r);
    InspectorRadio m = f.page.inspectorModel().radio;
    // the eleven rates, "%.3f MS/s"; none set = the default 2 MS/s
    EQ(m.rate_items.size(), std::size_t{11});
    const char* want[11] = {"1.000 MS/s", "1.024 MS/s", "2.000 MS/s", "2.048 MS/s", "2.400 MS/s", "2.500 MS/s", "3.000 MS/s", "4.000 MS/s", "6.000 MS/s", "8.000 MS/s", "10.000 MS/s"};
    for (std::size_t i = 0; i < 11; ++i) { EQ(at(m.rate_items, i).label, std::string(want[i])); }
    EQ(m.rate_index, 2);
    EQ(m.rate_label, std::string("2.000 MS/s"));
    CHECK(!m.rate_fixed);
    nm(f.g(), r).rateHz = 2.4e6;
    m = f.page.inspectorModel().radio;
    EQ(m.rate_index, 4);
    EQ(m.rate_label, std::string("2.400 MS/s"));
    nm(f.g(), r).rateHz = 2.4e6 + 0.5;                                         // within 1 Hz
    EQ(f.page.inspectorModel().radio.rate_index, 4);
    nm(f.g(), r).rateHz = 2.4e6 + 5.0;
    EQ(f.page.inspectorModel().radio.rate_index, -1);
    nm(f.g(), r).rateHz = 2.4e6;
    // the lines: nothing open; the last is always the count sentence
    const std::string tail = "Up to 5 radios, each its own device. While this page is open the receiver runs on the signal generator; closing it gives the radio back.";
    EQ(lineTexts(f.page.inspectorModel().radio.lines), "[1:" + tail + "]");
    // open: its rate and where it is
    f.t.add(FOXAPI_LIST_PATCH_RADIOS, r, "rtlsdr|serial=1", "", kActive, 2.4e6, 145.5e6, 0);
    f.tickBump();
    EQ(lineTexts(f.page.inspectorModel().radio.lines), "[2:running 2.400 MS/s][2:at 145.500000 MHz][1:" + tail + "]");
    // pending
    f.t.lists[FOXAPI_LIST_PATCH_RADIOS].clear();
    f.t.add(FOXAPI_LIST_PATCH_RADIOS, r, "rtlsdr|serial=1", "", 0, 0, 0, 1);
    f.tickBump();
    EQ(lineTexts(f.page.inspectorModel().radio.lines), "[1:opening the device...][1:" + tail + "]");
    // the row's note in rust, before the tail
    f.t.lists[FOXAPI_LIST_PATCH_RADIOS].clear();
    f.t.add(FOXAPI_LIST_PATCH_RADIOS, r, "rtlsdr|serial=1", "the device would not open", kFault, 0, 0, 0);
    f.tickBump();
    EQ(lineTexts(f.page.inspectorModel().radio.lines), "[4:the device would not open][1:" + tail + "]");

    // A RECORDING sets its own rate: shown, not offered
    nm(f.g(), r).device = "iqfile|path=C:\\rec\\a.cu8";
    nm(f.g(), r).rateHz = 2.4e6;
    m = f.page.inspectorModel().radio;
    CHECK(m.rate_fixed);
    EQ(m.rate_fixed_text, std::string("2.400 MS/s"));
    nm(f.g(), r).rateHz = 48000.0;
    EQ(f.page.inspectorModel().radio.rate_fixed_text, std::string("48.000 kS/s"));
    EQ(f.page.inspectorModel().radio.device_label, std::string("Recording: a.cu8"));
}

void theChannelDemodSinkAndMapInspectors() {
    const Chain c = chain(true);
    pc::Graph g = c.g;
    const pc::NodeId disp = g.addNode(NodeKind::Display, "Spectrum", PortType::Iq, 0, 400);
    const pc::NodeId mapn = g.addNode(NodeKind::Map, "Map", PortType::Track, 700, 20);
    const pc::NodeId adsb = g.addNode(NodeKind::Decoder, "ADS-B", PortType::Iq, 400, 300);
    g.mutableNode(adsb)->plugin = "adsb.dll";
    g.connect(c.radio, 0, adsb, 0);
    g.connect(adsb, 1, mapn, 0);
    Fx f(g);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ADS-B", "adsb.dll", 0, 0, 0, 1);
    f.t.add(FOXAPI_LIST_AUDIO_DEVICES, 1, "Speakers (Realtek)", "");
    f.t.add(FOXAPI_LIST_AUDIO_DEVICES, 2, "HDMI", "");
    f.t.facts["fact.recorder.dir"] = "C:\\Rec";
    f.tickBump();
    // CHANNEL
    f.select(c.channel);
    InspectorModel m = f.page.inspectorModel();
    EQ(m.freq_text, std::string("100.100000"));
    EQ(m.kind_caption, std::string("CHANNEL"));
    // DEMOD
    f.select(c.demod);
    nm(f.g(), c.demod).mode = 3;
    nm(f.g(), c.demod).squelchDb = -80.0f;
    m = f.page.inspectorModel();
    EQ(m.mode_index, 3);
    CHECK(m.squelch_on);
    CHECK_NEAR(m.squelch_norm, 40.0 / 120.0, 1e-6);
    EQ(m.squelch_text, std::string("-80 dB"));
    CHECK(!m.gate_shown);
    f.t.add(FOXAPI_LIST_PATCH_LIVE, c.demod, "", "", kActive, -70.2, 0, 1);
    f.tickBump();
    m = f.page.inspectorModel();
    CHECK(m.gate_shown && m.gate_lit);
    EQ(m.gate_text, std::string("-70 dB"));
    EQ(m.gate_word, std::string("open"));

    // MAP
    f.select(mapn);
    m = f.page.inspectorModel();
    EQ(m.map_wired, std::string("1 of 5 inputs wired"));
    EQ(m.map_sources.size(), std::size_t{1});
    EQ(at(m.map_sources, 0).name, std::string("ADS-B"));
    EQ(at(m.map_sources, 0).count, std::string("0"));
    f.page.setTracks({{"ADS-B", CASCADE_TRACK_AIRCRAFT}, {"ADS-B", CASCADE_TRACK_AIRCRAFT}, {"Other", CASCADE_TRACK_AIRCRAFT}});
    EQ(at(f.page.inspectorModel().map_sources, 0).count, std::string("2"));
    EQ(f.page.inspectorModel().map_help, std::string("Wire the map output of up to 5 decoders here - aircraft from one radio, ships from another - and they share this map. Drag it to pan; zoom with the + and - keys in its corner, or the mouse wheel."));

    // SINK (audio)
    f.select(c.speaker);
    m = f.page.inspectorModel();
    CHECK(m.audio_sink);
    const std::vector<SelectItem>& items = m.sink.items;
    EQ(items.size(), std::size_t{5});
    EQ(at(items, 0).label, std::string("A WAV file"));
    EQ(at(items, 0).id, std::string("wav"));
    CHECK(at(items, 0).enabled);
    EQ(at(items, 1).label, std::string(cc::kMp3Supported ? "An MP3 file" : "An MP3 file (needs Windows)"));
    EQ(at(items, 1).id, std::string("mp3"));
    CHECK(at(items, 1).enabled == cc::kMp3Supported);
    EQ(at(items, 2).label, std::string("The speakers"));
    EQ(at(items, 2).id, std::string("speakers"));
    EQ(at(items, 3).label, std::string("Speakers (Realtek)"));
    EQ(at(items, 3).id, std::string("audio:Speakers (Realtek)"));
    EQ(at(items, 4).id, std::string("audio:HDMI"));
    // no device set: a file
    EQ(m.sink.index, 0);
    EQ(m.sink.label, std::string("A WAV file"));
    EQ(m.sink.files_note, std::string("Files go in C:\\Rec, one per speaker, named after it."));
    EQ(lineTexts(m.sink.lines), std::string(""));
    nm(f.g(), c.speaker).device = "mp3";
    m = f.page.inspectorModel();
    EQ(m.sink.index, 1);
    EQ(m.sink.label, std::string("An MP3 file"));
    EQ(m.sink.files_note, std::string("Files go in C:\\Rec, one per speaker, named after it."));
    nm(f.g(), c.speaker).device = "speakers";
    m = f.page.inspectorModel();
    EQ(m.sink.index, 2);
    EQ(m.sink.label, std::string("The speakers"));
    EQ(m.sink.files_note, std::string(""));
    nm(f.g(), c.speaker).device = "audio:HDMI";
    m = f.page.inspectorModel();
    EQ(m.sink.index, 4);
    EQ(m.sink.label, std::string("HDMI"));
    nm(f.g(), c.speaker).device = "audio:Gone";
    m = f.page.inspectorModel();
    EQ(m.sink.index, -1);
    EQ(m.sink.label, std::string("Gone"));
    // the output's own words: where it plays, how long it has written, why it failed
    f.t.add(FOXAPI_LIST_PATCH_LIVE, c.speaker, "HDMI out", "stalled", kActive, 0.2, 144000, 2);
    f.t.add(FOXAPI_LIST_PATCH_LIVE, c.speaker, "", "No such device", 0, 0, 0, 5);
    f.tickBump();
    m = f.page.inspectorModel();
    EQ(lineTexts(m.sink.lines), std::string("[2:HDMI out][2:3.0 s][4:stalled][4:No such device]"));

    // SINK (text out)
    f.select(c.textOut);
    m = f.page.inspectorModel();
    CHECK(!m.audio_sink);
    EQ(m.help, std::string("Shows the lines from every decoder wired to it, and sends them to the Decoder output window."));
    // DISPLAY, and anything else
    f.select(disp);
    m = f.page.inspectorModel();
    EQ(m.help, std::string("This node has nothing to set yet."));
    EQ(m.kind_caption, std::string("DISPLAY"));
    // RADIO has no help of that kind
    f.select(c.radio);
    EQ(f.page.inspectorModel().help, std::string(""));
}

void theDecoderInspector() {
    const Chain c = chain(true);
    Fx f(c.g);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars_1.dll", 0, 1, 0, 0);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 2, "ADS-B", "adsb.dll", 0, 0, 2.0e6, 1);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 3, "VDL", "vdl.dll", 0, 1, 0, 0);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 4, "ACARS", "acars_1.dll", 0, 0, 0, 0);    // the same module, I/Q
    f.t.add(FOXAPI_LIST_PATCH_PLAN, c.decoder, "acars_1.dll", "decoder:audio", 0, 48000.0, 96000.0, 0.0, static_cast<double>(c.radio));
    f.tickBump();
    f.select(c.decoder);
    InspectorDecoder m = f.page.inspectorModel().decoder;
    // the plugins that take THIS node's input (audio): names, row ids, module keys
    EQ(m.plugin_items.size(), std::size_t{2});
    CHECK(at(m.plugin_items, 0).label == "ACARS" && at(m.plugin_items, 0).id == "0" && at(m.plugin_items, 0).note == "acars_1.dll");
    CHECK(at(m.plugin_items, 1).label == "VDL" && at(m.plugin_items, 1).id == "2" && at(m.plugin_items, 1).note == "vdl.dll");
    EQ(m.plugin_index, 0);
    EQ(m.plugin_label, std::string("ACARS"));                   // the part's name for this module and this input
    EQ(m.feed_note, std::string("Audio - wire it from a demodulator input."));
    // what the plan hands it
    EQ(m.fed, Names({"fed 48000 Hz (from 96000)"}));
    CHECK(m.shown_note.empty());                                 // it is wired to a Text out
    // not wired to a Text out: the sentence
    pc::Graph g = c.g;
    g.removeNode(c.textOut);
    f.publish(g);
    f.tickBump();
    EQ(f.page.inspectorModel().decoder.shown_note, std::string("Wire its output to a Text out part to read what it decodes in the Decoder output window."));
    // the same rate in and out
    f.t.lists[FOXAPI_LIST_PATCH_PLAN].clear();
    f.t.add(FOXAPI_LIST_PATCH_PLAN, c.decoder, "acars_1.dll", "decoder:channel", 0, 48000.0, 48000.0, 100.1e6, static_cast<double>(c.radio));
    f.tickBump();
    EQ(f.page.inspectorModel().decoder.fed, Names({"fed 48000 Hz", "centred on 100.100000 MHz"}));
    f.t.lists[FOXAPI_LIST_PATCH_PLAN].clear();
    f.t.add(FOXAPI_LIST_PATCH_PLAN, c.decoder, "acars_1.dll", "decoder:radio", 0, 2.0e6, 2.0e6, 100.0e6, static_cast<double>(c.radio));
    f.tickBump();
    EQ(f.page.inspectorModel().decoder.fed, Names({"fed 2000000 Hz", "centred on 100.000000 MHz"}));
    f.t.lists[FOXAPI_LIST_PATCH_PLAN].clear();
    f.tickBump();
    CHECK(f.page.inspectorModel().decoder.fed.empty());
    // no plugin chosen: "(none)", and no row is current; a key no part has is shown as it is
    nm(f.g(), c.decoder).plugin.clear();
    m = f.page.inspectorModel().decoder;
    EQ(m.plugin_label, std::string("(none)"));
    EQ(m.plugin_index, -1);
    nm(f.g(), c.decoder).plugin = "old_0.9.dll";
    m = f.page.inspectorModel().decoder;
    EQ(m.plugin_label, std::string("old_0.9.dll"));
    EQ(m.plugin_index, -1);
    // an I/Q decoder offers the I/Q plugins, with its own wording
    pc::Graph iq = c.g;
    const pc::NodeId d2 = iq.addNode(NodeKind::Decoder, "ADS-B", PortType::Iq, 400, 400);
    iq.mutableNode(d2)->plugin = "adsb.dll";
    iq.connect(c.radio, 0, d2, 0);
    Fx h(iq);
    h.t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars_1.dll", 0, 1, 0, 0);
    h.t.add(FOXAPI_LIST_PATCH_PARTS, 2, "ADS-B", "adsb.dll", 0, 0, 2.0e6, 1);
    h.t.add(FOXAPI_LIST_PATCH_PARTS, 3, "ACARS", "acars_1.dll", 0, 0, 0, 0);
    h.tickBump();
    h.select(d2);
    m = h.page.inspectorModel().decoder;
    EQ(m.plugin_items.size(), std::size_t{2});
    CHECK(at(m.plugin_items, 0).id == "1" && at(m.plugin_items, 1).id == "2");
    EQ(m.plugin_index, 0);
    EQ(m.feed_note, std::string("I/Q - wire it from a channel, or from the radio for the whole band input."));
}

void theDecoderInspectorHoldsTheKitsForm() {
    const Chain c = chain(true);
    Fx f(c.g);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars_1.dll", 0, 1, 0, 0);
    f.tickBump();
    f.select(c.decoder);
    foxsdr::slintkit::KitLists kit;
    foxsdr::slintkit::PluginRow row;
    row.name = "ACARS";
    row.file = "acars_1.dll";
    row.loaded = true;
    kit.rows.push_back(row);
    foxsdr::slintkit::SettingsSet set;
    set.file = "acars_1.dll";
    foxsdr::slintkit::SettingSpec spec;
    spec.id = 5;
    spec.key = "freq";
    spec.label = "Frequency";
    set.fields.push_back(spec);
    kit.settings.push_back(set);
    // no kit given: no form
    CHECK(!f.page.inspectorModel().decoder.has_settings);
    f.page.setKit(&kit);
    InspectorDecoder m = f.page.inspectorModel().decoder;
    CHECK(m.has_settings);
    EQ(m.settings_module, std::string("acars_1.dll"));
    EQ(m.settings.size(), std::size_t{1});
    EQ(at(m.settings, 0).id, std::uint64_t{5});
    // a module that is not loaded has no form
    kit.rows[0].loaded = false;
    CHECK(!f.page.inspectorModel().decoder.has_settings);
    kit.rows[0].loaded = true;
    // a module with no fields has none either
    kit.settings[0].fields.clear();
    CHECK(!f.page.inspectorModel().decoder.has_settings);
    kit.settings[0].fields.push_back(spec);
    // a picture decoder's module key loses its suffix
    nm(f.g(), c.decoder).plugin = "acars_1.dll#image";
    m = f.page.inspectorModel().decoder;
    CHECK(m.has_settings);
    EQ(m.settings_module, std::string("acars_1.dll"));
    // an empty plugin: none; a key that IS the suffix is kept whole and finds nothing
    nm(f.g(), c.decoder).plugin.clear();
    CHECK(!f.page.inspectorModel().decoder.has_settings);
    nm(f.g(), c.decoder).plugin = "#image";
    CHECK(!f.page.inspectorModel().decoder.has_settings);
    f.page.setKit(nullptr);
}

// ================================================================================================================================
// what the controls do
// ================================================================================================================================

void theFacesEditTheDraft() {
    const Chain c = chain();
    Fx f(c.g);
    f.page.draft().dirty = false;
    // the radio's own switch
    f.page.faceSwitch(static_cast<int>(c.radio));
    CHECK(!nd(f.g(), c.radio).on && f.page.draft().dirty);
    f.page.faceSwitch(static_cast<int>(c.radio));
    CHECK(nd(f.g(), c.radio).on);
    f.page.faceSwitch(static_cast<int>(c.channel));                       // not a radio
    CHECK(nd(f.g(), c.channel).on);                                      // untouched (the default is on)
    f.page.faceSwitch(999);
    // a Channel's frequency
    f.page.draft().dirty = false;
    f.page.faceFreq(static_cast<int>(c.channel), "121.5");
    CHECK(nd(f.g(), c.channel).freqHz == 121.5e6 && f.page.draft().dirty);
    f.page.draft().dirty = false;
    f.page.faceFreq(static_cast<int>(c.channel), "garbage");
    CHECK(nd(f.g(), c.channel).freqHz == 121.5e6 && !f.page.draft().dirty);
    f.page.faceFreq(static_cast<int>(c.channel), "-5");                    // no validation: the channel's problems say what is wrong
    CHECK(nd(f.g(), c.channel).freqHz == -5.0e6);
    f.page.faceFreq(static_cast<int>(c.demod), "100");                     // not a channel
    CHECK(nd(f.g(), c.demod).freqHz == 0.0);
    // the mode, clamped to the eight
    f.page.faceMode(static_cast<int>(c.demod), 5);
    EQ(nd(f.g(), c.demod).mode, 5);
    f.page.faceMode(static_cast<int>(c.demod), 99);
    EQ(nd(f.g(), c.demod).mode, 7);
    f.page.faceMode(static_cast<int>(c.demod), -4);
    EQ(nd(f.g(), c.demod).mode, 0);
    f.page.faceMode(static_cast<int>(c.channel), 3);
    EQ(nd(f.g(), c.channel).mode, 0);
    // the squelch: on / off, and the slider's norm over -120..0 dB, always clamped
    f.page.faceSquelchOn(static_cast<int>(c.demod), false);
    CHECK(!nd(f.g(), c.demod).squelch);
    f.page.faceSquelchOn(static_cast<int>(c.demod), true);
    CHECK(nd(f.g(), c.demod).squelch);
    f.page.faceSquelch(static_cast<int>(c.demod), 0.5f);
    CHECK(nd(f.g(), c.demod).squelchDb == -60.0f);
    f.page.faceSquelch(static_cast<int>(c.demod), 0.0f);
    CHECK(nd(f.g(), c.demod).squelchDb == -120.0f);
    f.page.faceSquelch(static_cast<int>(c.demod), 1.0f);
    CHECK(nd(f.g(), c.demod).squelchDb == 0.0f);
    f.page.faceSquelch(static_cast<int>(c.demod), 3.0f);
    CHECK(nd(f.g(), c.demod).squelchDb == 0.0f);
    f.page.faceSquelch(static_cast<int>(c.demod), -2.0f);
    CHECK(nd(f.g(), c.demod).squelchDb == -120.0f);
    f.page.faceSquelch(static_cast<int>(c.channel), 0.5f);
    CHECK(nd(f.g(), c.channel).squelchDb == -50.0f);
    // each of them reaches the engine at the tick
    f.sub.clear();
    f.tick();
    EQ(f.sub.count(kPatchSetGraph), std::size_t{1});
}

void theInspectorEditsTheSelectedNode() {
    const Chain c = chain(true);
    Fx f(c.g);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 1, "ACARS", "acars_1.dll", 0, 1, 0, 0);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 2, "ADS-B", "adsb.dll", 0, 0, 2.0e6, 1);
    f.t.add(FOXAPI_LIST_PATCH_PARTS, 3, "VDL", "vdl.dll", 0, 1, 0, 0);
    f.t.add(FOXAPI_LIST_AUDIO_DEVICES, 1, "HDMI", "");
    f.tickBump();
    f.page.draft().dirty = false;
    // CHANNEL: frequency
    f.select(c.channel);
    f.page.inspectorFreq("88.5");
    CHECK(nd(f.g(), c.channel).freqHz == 88.5e6 && f.page.draft().dirty);
    f.page.inspectorFreq("x");
    CHECK(nd(f.g(), c.channel).freqHz == 88.5e6);
    // DEMOD
    f.select(c.demod);
    f.page.inspectorMode(6);
    EQ(nd(f.g(), c.demod).mode, 6);
    f.page.inspectorMode(8);
    EQ(nd(f.g(), c.demod).mode, 7);
    f.page.inspectorSquelchOn(false);
    CHECK(!nd(f.g(), c.demod).squelch);
    f.page.inspectorSquelchOn(true);
    f.page.inspectorSquelch(0.25f);
    CHECK(nd(f.g(), c.demod).squelchDb == -90.0f);
    f.page.inspectorSquelch(9.0f);
    CHECK(nd(f.g(), c.demod).squelchDb == 0.0f);
    // DECODER: the item's key; its index is the row of the filtered list
    f.select(c.decoder);
    f.page.inspectorPlugin(1);                                       // items: ACARS (row 0), VDL (row 2)
    EQ(nd(f.g(), c.decoder).plugin, std::string("vdl.dll"));
    f.page.inspectorPlugin(0);
    EQ(nd(f.g(), c.decoder).plugin, std::string("acars_1.dll"));
    f.page.inspectorPlugin(5);
    f.page.inspectorPlugin(-1);
    EQ(nd(f.g(), c.decoder).plugin, std::string("acars_1.dll"));
    // SINK: the item's id; a device the list holds
    f.select(c.speaker);
    f.page.inspectorSink(2);
    EQ(nd(f.g(), c.speaker).device, std::string("speakers"));
    f.page.inspectorSink(3);
    EQ(nd(f.g(), c.speaker).device, std::string("audio:HDMI"));
    f.page.inspectorSink(0);
    EQ(nd(f.g(), c.speaker).device, std::string("wav"));
    f.page.inspectorSink(1);
    EQ(nd(f.g(), c.speaker).device, std::string(cc::kMp3Supported ? "mp3" : "wav"));
    f.page.inspectorSink(40);
    EQ(nd(f.g(), c.speaker).device, std::string(cc::kMp3Supported ? "mp3" : "wav"));
    // a Text out has no sink choice
    f.select(c.textOut);
    f.page.inspectorSink(2);
    EQ(nd(f.g(), c.textOut).device, std::string(""));
    // SIZE: smaller, larger, default
    f.select(c.demod);
    const float w0 = nd(f.g(), c.demod).w, h0 = nd(f.g(), c.demod).h;
    f.page.draft().dirty = false;
    f.page.inspectorSize(0);
    pc::Node expect = *c.g.find(c.demod);
    pg::scaleNodeSize(expect, 0.8f);
    CHECK(nd(f.g(), c.demod).w == expect.w && nd(f.g(), c.demod).h == expect.h && expect.w < w0 && expect.h < h0);
    CHECK(f.page.draft().dirty);
    f.page.inspectorSize(2);
    CHECK(nd(f.g(), c.demod).w == w0 && nd(f.g(), c.demod).h == h0);
    f.page.inspectorSize(1);
    pc::Node big = *c.g.find(c.demod);
    pg::scaleNodeSize(big, 1.25f);
    CHECK(nd(f.g(), c.demod).w == big.w && nd(f.g(), c.demod).h == big.h && big.w > w0);
    f.page.inspectorSize(7);
    CHECK(nd(f.g(), c.demod).w == big.w);
    // REMOVE
    f.select(c.demod);
    f.page.draft().dirty = false;
    f.page.inspectorRemove();
    CHECK(f.g().find(c.demod) == nullptr && f.page.draft().dirty);
    EQ(f.page.canvas().ui.selected, pc::NodeId{0});
    CHECK(!f.page.bandModel().inspector_shown);
    f.page.inspectorRemove();                                        // nothing selected: nothing
    EQ(f.g().nodes().size(), std::size_t{5});
}

void theDeviceChoiceIsOneRadioOneDevice() {
    pc::Graph g;
    const pc::NodeId r1 = g.addNode(NodeKind::Radio, "Alpha", PortType::Iq, 40, 40);
    const pc::NodeId r2 = g.addNode(NodeKind::Radio, "Beta", PortType::Iq, 40, 300);
    g.mutableNode(r1)->device = "rtlsdr|serial=1";
    g.mutableNode(r2)->device = "airspy|serial=9";
    Fx f(g);
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 1, "Signal generator", "siggen");
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 2, "NESDR Smart", "rtlsdr|serial=1");
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 3, "Airspy", "airspy|serial=9");
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 4, "Recording a", "iqfile|path=C:\\a.cu8", 0, 2.4e6);
    f.t.add(FOXAPI_LIST_PATCH_DEVICES, 5, "Recording b", "iqfile|path=C:\\b.cu8", 0, 0.0);
    f.tickBump();
    f.select(r1);
    f.page.draft().dirty = false;
    // one held by another radio: nothing happens
    f.page.inspectorDevice(2);
    EQ(nd(f.g(), r1).device, std::string("rtlsdr|serial=1"));
    CHECK(!f.page.draft().dirty);
    // a free one: taken
    f.page.inspectorDevice(0);
    EQ(nd(f.g(), r1).device, std::string("siggen"));
    CHECK(f.page.draft().dirty);
    // a recording sets the rate the file was made at
    nm(f.g(), r1).rateHz = 3.0e6;
    f.page.inspectorDevice(3);
    EQ(nd(f.g(), r1).device, std::string("iqfile|path=C:\\a.cu8"));
    CHECK(nd(f.g(), r1).rateHz == 2.4e6);
    // ...unless the list knows no rate for it
    nm(f.g(), r1).rateHz = 3.0e6;
    f.page.inspectorDevice(4);
    EQ(nd(f.g(), r1).device, std::string("iqfile|path=C:\\b.cu8"));
    CHECK(nd(f.g(), r1).rateHz == 3.0e6);
    // out of range, or nothing selected
    f.page.inspectorDevice(9);
    f.page.inspectorDevice(-1);
    EQ(nd(f.g(), r1).device, std::string("iqfile|path=C:\\b.cu8"));
    f.select(0);
    f.page.inspectorDevice(0);
    EQ(nd(f.g(), r1).device, std::string("iqfile|path=C:\\b.cu8"));
    // the rate of the eleven; none for a recording
    f.select(r2);
    f.page.inspectorRate(6);
    CHECK(nd(f.g(), r2).rateHz == 3.0e6);
    f.page.inspectorRate(11);
    f.page.inspectorRate(-1);
    CHECK(nd(f.g(), r2).rateHz == 3.0e6);
    nm(f.g(), r2).device = "iqfile|path=C:\\a.cu8";
    nm(f.g(), r2).rateHz = 2.4e6;
    f.page.inspectorRate(0);
    CHECK(nd(f.g(), r2).rateHz == 2.4e6);                        // a recording's rate is not a setting
}

void theCentreIsJudgedByTheRadioBehindTheConverter() {
    pc::Graph g;
    const pc::NodeId r = g.addNode(NodeKind::Radio, "Alpha", PortType::Iq, 40, 40);
    g.mutableNode(r)->device = "rtlsdr|serial=1";
    g.mutableNode(r)->freqHz = 100.0e6;
    g.mutableNode(r)->centreChosen = true;
    Fx f(g);
    f.select(r);
    f.page.draft().dirty = false;
    // no converter: a centre must be above 0 Hz
    f.page.faceCentre(static_cast<int>(r), "145.5");
    CHECK(nd(f.g(), r).freqHz == 145.5e6 && nd(f.g(), r).centreChosen && f.page.draft().dirty);
    EQ(at(f.page.canvasModel().nodes, 0).face.centre_note, std::string(""));
    f.page.draft().dirty = false;
    f.page.faceCentre(static_cast<int>(r), "0");
    CHECK(nd(f.g(), r).freqHz == 145.5e6 && !f.page.draft().dirty);
    EQ(nodeOf(f.page.canvasModel(), r).face.centre_note, "A radio cannot tune to " + cc::converterHzText(0.0) + " - type a centre above 0 Hz.");
    f.page.inspectorCentre("-3");
    EQ(f.page.inspectorModel().radio.centre_note, "A radio cannot tune to " + cc::converterHzText(-3.0e6) + " - type a centre above 0 Hz.");
    // a text that is not a number changes nothing, and says nothing new
    f.page.inspectorCentre("garbage");
    CHECK(nd(f.g(), r).freqHz == 145.5e6);
    EQ(f.page.inspectorModel().radio.centre_note, "A radio cannot tune to " + cc::converterHzText(-3.0e6) + " - type a centre above 0 Hz.");
    // an accepted one erases the note
    f.page.inspectorCentre("99");
    CHECK(nd(f.g(), r).freqHz == 99.0e6);
    EQ(f.page.inspectorModel().radio.centre_note, std::string(""));
    // THROUGH AN UP-CONVERTER (125 MHz): a centre below 0 Hz on the air is 124.7 MHz at the radio, so it is taken; one that would have the radio at 0 Hz or below is not
    f.t.add(FOXAPI_LIST_PATCH_NODES, r, "Alpha", "radio");
    f.t.facts["fact.patch.conv." + std::to_string(r)] = "1 125000000 0";
    f.tickBump();
    f.page.inspectorCentre("-0.28364");
    CHECK_NEAR(nd(f.g(), r).freqHz, -283640.0, 1e-6);
    CHECK(nd(f.g(), r).centreChosen);
    const double kept = nd(f.g(), r).freqHz;
    cc::ConverterSetting up;
    up.mode = cc::ConverterMode::Up;
    up.loHz = 125.0e6;
    f.page.faceCentre(static_cast<int>(r), "-130");
    CHECK(nd(f.g(), r).freqHz == kept);
    EQ(nodeOf(f.page.canvasModel(), r).face.centre_note,
       cc::converterHzText(-130.0e6) + " is out of reach through the " + converterNameText(up) + ": the radio would have to tune to 0 Hz or below.");
    EQ(converterNameText(up), std::string("125 MHz up-converter"));
    // the converter speaks for the device the ENGINE has the node on: a device chosen a moment ago and not applied has none, and is judged as a radio with no converter
    nm(f.g(), r).device = "airspy|serial=9";
    f.page.faceCentre(static_cast<int>(r), "-1");
    CHECK(nd(f.g(), r).freqHz == kept);
    EQ(nodeOf(f.page.canvasModel(), r).face.centre_note, "A radio cannot tune to " + cc::converterHzText(-1.0e6) + " - type a centre above 0 Hz.");
    // a typed centre for something that is not a radio changes nothing
    f.page.faceCentre(static_cast<int>(f.g().nodes().size() + 50), "100");
}

void notesGoWithTheirNodes() {
    pc::Graph g;
    const pc::NodeId r = g.addNode(NodeKind::Radio, "Alpha", PortType::Iq, 40, 40);
    g.mutableNode(r)->device = "rtlsdr|serial=1";
    Fx f(g);
    f.page.faceCentre(static_cast<int>(r), "-1");
    EQ(nodeOf(f.page.canvasModel(), r).face.centre_note, "A radio cannot tune to " + cc::converterHzText(-1.0e6) + " - type a centre above 0 Hz.");
    // a new patch: the note is of the old one
    pc::Graph other;
    other.addNode(NodeKind::Radio, "Other", PortType::Iq, 5, 5);
    f.publish(other, true);
    f.tick();
    EQ(nodeOf(f.page.canvasModel(), r).face.centre_note, std::string(""));
}

}  // namespace

namespace {

// ================================================================================================================================
// the presets page
// ================================================================================================================================

std::string docOfOne(const char* name, float x, float pan, float zoom) {
    pc::Graph g;
    const pc::NodeId r = g.addNode(NodeKind::Radio, name, PortType::Iq, x, 20.0f);
    g.mutableNode(r)->device = "siggen";
    const pc::NodeId c = g.addNode(NodeKind::Channel, "Chan", PortType::Iq, x + 300.0f, 20.0f);
    g.connect(r, 0, c, 0);
    return documentOf(g, pg::View{(pg::Vec2{pan, pan + 1.0f}), zoom});
}

void thePresetsPageStartsEmpty() {
    Fx f(chain().g);
    PatchPresetsModel m = f.page.presetsModel();
    CHECK(m.rows.empty() && m.empty_shown);
    EQ(m.empty_text, std::string("No presets yet. SAVE AS... keeps the patch on the canvas under a name."));
    CHECK(!m.load_enabled && !m.rename_enabled && !m.delete_enabled);
    EQ(m.edit, 0);
    CHECK(m.name_draft.empty() && m.note.empty() && !m.note_good && !m.ask_replace && !m.ask_delete);
    // keys with nothing selected do nothing
    RecSub sub;
    f.page.presetsLoad(sub);
    f.page.presetsRename();
    f.page.presetsDelete();
    EQ(sub.sent.size(), std::size_t{0});
    m = f.page.presetsModel();
    CHECK(m.edit == 0 && !m.ask_delete);
}

void thePresetsAreReadFromTheConfig() {
    Fx f(chain().g, true);
    f.cfg.presetList = {{"zeta", docOfOne("Z", 10, 1, 1.0f)}, {"Airband", docOfOne("A", 20, 2, 1.0f)}, {"mid", docOfOne("M", 30, 3, 1.0f)}};
    f.cfg.previousText = docOfOne("Old", 40, 4, 1.0f);
    f.tick();
    PatchPresetsModel m = f.page.presetsModel();
    // the previous slot first and muted, then the list sorted by name without regard to case
    EQ(m.rows.size(), std::size_t{4});
    CHECK(at(m.rows, 0).previous && at(m.rows, 0).text == "(previous patch)" && !at(m.rows, 0).selected);
    CHECK(!at(m.rows, 1).previous && at(m.rows, 1).text == "Airband");
    EQ(at(m.rows, 2).text, std::string("mid"));
    EQ(at(m.rows, 3).text, std::string("zeta"));
    CHECK(!m.empty_shown);
    EQ(m.previous_tooltip, std::string("The patch the last load replaced. Load it to undo that load."));
    // selecting
    f.page.presetsRow(2);
    m = f.page.presetsModel();
    CHECK(at(m.rows, 2).selected && !at(m.rows, 1).selected && !at(m.rows, 0).selected);
    CHECK(m.load_enabled && m.rename_enabled && m.delete_enabled);
    f.page.presetsRow(0);                                  // the previous row: loadable, never renamed or deleted
    m = f.page.presetsModel();
    CHECK(at(m.rows, 0).selected && !at(m.rows, 2).selected);
    CHECK(m.load_enabled && !m.rename_enabled && !m.delete_enabled);
    f.page.presetsRow(9);                                  // no such row
    f.page.presetsRow(-1);
    CHECK(at(f.page.presetsModel().rows, 0).selected);
    // the previous slot's keys: rename and delete do nothing
    f.page.presetsRename();
    f.page.presetsDelete();
    m = f.page.presetsModel();
    CHECK(m.edit == 0 && !m.ask_delete);
    // with no previous slot the first row is the first preset
    Fx g(chain().g, true);
    g.cfg.presetList = {{"only", docOfOne("O", 10, 1, 1.0f)}};
    g.tick();
    g.page.presetsRow(0);
    m = g.page.presetsModel();
    EQ(m.rows.size(), std::size_t{1});
    CHECK(at(m.rows, 0).selected && !at(m.rows, 0).previous && at(m.rows, 0).text == "only");
    // a selection whose row has gone is forgotten (the previous slot's too, once there is none)
    g.cfg.presetList.clear();
    Fx h(chain().g, true);
    h.cfg.previousText = docOfOne("Old", 40, 4, 1.0f);
    h.tick();
    h.page.presetsRow(0);
    CHECK(h.page.presetsModel().load_enabled);
}

void saveAsKeepsThePatchUnderAName() {
    Fx f(chain().g);
    f.page.zoomStep(true);                                 // a view to keep with it
    f.tick();
    // SAVE AS...: a row that takes a name, the keyboard focus in it
    f.page.presetsSaveAs();
    PatchPresetsModel m = f.page.presetsModel();
    EQ(m.edit, 1);
    CHECK(m.name_draft.empty() && m.focus_name);
    f.page.presetsName("  Mine  ");
    EQ(f.page.presetsModel().name_draft, std::string("  Mine  "));
    f.page.presetsGo();
    m = f.page.presetsModel();
    EQ(m.note, std::string("Saved \"Mine\"."));
    CHECK(m.note_good && m.edit == 0 && !m.ask_replace);
    EQ(m.rows.size(), std::size_t{1});
    CHECK(at(m.rows, 0).text == "Mine" && at(m.rows, 0).selected);
    CHECK(m.load_enabled && m.rename_enabled && m.delete_enabled);
    // the text is the document of the canvas now, with the view
    EQ(f.cfg.presetList.size(), std::size_t{1});
    EQ(at(f.cfg.presetList, 0).name, std::string("Mine"));
    EQ(at(f.cfg.presetList, 0).text, documentOf(f.page.draft().graph(), f.page.canvas().ui.view));
    CHECK(has(at(f.cfg.presetList, 0).text, "view 0 0 1.25") || pc::parse(at(f.cfg.presetList, 0).text).zoom == 1.25f);
    EQ(f.cfg.presetSaves, 1);
    // the name row opens again with the selected preset's name
    f.page.presetsSaveAs();
    EQ(f.page.presetsModel().name_draft, std::string("Mine"));
    f.page.presetsCancel();
    EQ(f.page.presetsModel().edit, 0);
    // A NAME TAKEN (case-blind) ASKS: not a note
    f.page.presetsSaveAs();
    f.page.presetsName("MINE");
    f.page.presetsGo();
    m = f.page.presetsModel();
    CHECK(m.ask_replace && m.edit == 1 && m.note != "Another preset already has that name.");
    EQ(m.replace_text, std::string("A preset called \"MINE\" already exists.\nReplace it with the patch on the canvas?"));
    EQ(f.cfg.presetList.size(), std::size_t{1});
    // KEEP: closes the question, nothing saved
    f.page.presetsReplace(false);
    m = f.page.presetsModel();
    CHECK(!m.ask_replace && m.edit == 1);
    EQ(f.cfg.presetSaves, 1);
    // REPLACE: saved over, the row closes and selects it
    f.page.canvasWheel(1, 1, 0, 1, false, false);          // the canvas moved since
    f.page.presetsGo();
    CHECK(f.page.presetsModel().ask_replace);
    f.page.presetsReplace(true);
    m = f.page.presetsModel();
    CHECK(!m.ask_replace && m.edit == 0);
    EQ(m.note, std::string("Saved \"MINE\"."));
    CHECK(m.note_good);
    EQ(m.rows.size(), std::size_t{1});
    EQ(f.cfg.presetSaves, 2);
    EQ(at(f.cfg.presetList, 0).text, documentOf(f.page.draft().graph(), f.page.canvas().ui.view));
    // a second preset, with a view of its own
    f.page.presetsSaveAs();
    f.page.presetsName("another");
    f.page.presetsGo();
    EQ(f.page.presetsModel().rows.size(), std::size_t{2});
    EQ(at(f.page.presetsModel().rows, 0).text, std::string("another"));        // sorted without regard to case
    EQ(f.cfg.presetSaves, 3);
}

void saveAsRefusalsAreSaid() {
    Fx f(chain().g);
    // an empty name (spaces and control characters are not a name)
    f.page.presetsSaveAs();
    f.page.presetsName("  \t ");
    f.page.presetsGo();
    PatchPresetsModel m = f.page.presetsModel();
    EQ(m.note, std::string("Type a name for the preset first."));
    CHECK(!m.note_good && m.edit == 1 && !m.ask_replace);
    EQ(f.cfg.presetSaves, 0);
    // the previous slot's own name, in any case
    f.page.presetsName("(Previous Patch)");
    f.page.presetsGo();
    EQ(f.page.presetsModel().note, std::string("That name is kept for the previous patch. Choose another."));
    CHECK(f.page.presetsModel().edit == 1);
    // a name is cut to 64 bytes on a character boundary and cleaned
    f.page.presetsName(std::string(70, 'n'));
    f.page.presetsGo();
    EQ(at(f.cfg.presetList, 0).name, std::string(64, 'n'));
    EQ(f.page.presetsModel().note, "Saved \"" + std::string(64, 'n') + "\".");
    // a list that is full
    Fx g(chain().g, true);
    for (int i = 0; i < 100; ++i) { g.cfg.presetList.push_back({"p" + std::to_string(1000 + i), docOfOne("P", 10, 1, 1.0f)}); }
    g.tick();
    g.page.presetsSaveAs();
    g.page.presetsName("one more");
    g.page.presetsGo();
    EQ(g.page.presetsModel().note, std::string("There are already 100 presets. Delete one to make room."));
    CHECK(!g.page.presetsModel().note_good && g.page.presetsModel().edit == 1);
    EQ(g.cfg.presetList.size(), std::size_t{100});
    // a patch too large to keep
    Fx h(chain().g);
    for (int i = 0; i < 3500; ++i) { h.g().addNode(NodeKind::Channel, std::string(60, 'c') + std::to_string(i % 10), PortType::Iq, 10.0f * static_cast<float>(i), 5.0f); }
    CHECK(documentOf(h.g(), pg::View{}).size() > cc::kMaxPatchPresetTextBytes);
    h.page.presetsSaveAs();
    h.page.presetsName("huge");
    h.page.presetsGo();
    EQ(h.page.presetsModel().note, std::string("This patch is too large to keep as a preset (over 256 KB)."));
    CHECK(!h.page.presetsModel().note_good && h.cfg.presetList.empty() && h.page.presetsModel().edit == 1);
}

void renameAndDelete() {
    Fx f(chain().g, true);
    f.cfg.presetList = {{"alpha", docOfOne("A", 10, 1, 1.0f)}, {"beta", docOfOne("B", 20, 2, 1.0f)}};
    f.tick();
    f.page.presetsRow(0);
    f.page.presetsRename();
    PatchPresetsModel m = f.page.presetsModel();
    EQ(m.edit, 2);
    EQ(m.name_draft, std::string("alpha"));
    CHECK(m.focus_name);
    // a name another preset has is never an overwrite
    f.page.presetsName("Beta");
    f.page.presetsGo();
    m = f.page.presetsModel();
    EQ(m.note, std::string("Another preset already has that name."));
    CHECK(!m.note_good && m.edit == 2 && !m.ask_replace);
    EQ(f.cfg.presetSaves, 0);
    // empty and reserved
    f.page.presetsName("");
    f.page.presetsGo();
    EQ(f.page.presetsModel().note, std::string("Type a name for the preset first."));
    f.page.presetsName("(previous patch)");
    f.page.presetsGo();
    EQ(f.page.presetsModel().note, std::string("That name is kept for the previous patch. Choose another."));
    // renamed: a change of case alone is a rename, the row closes, the new name is selected
    f.page.presetsName("ALPHA");
    f.page.presetsGo();
    m = f.page.presetsModel();
    EQ(m.note, std::string("Renamed to \"ALPHA\"."));
    CHECK(m.note_good && m.edit == 0);
    EQ(at(m.rows, 0).text, std::string("ALPHA"));
    CHECK(at(m.rows, 0).selected);
    EQ(f.cfg.presetSaves, 1);
    EQ(at(f.cfg.presetList, 0).name, std::string("ALPHA"));
    f.page.presetsRename();
    f.page.presetsName("gamma");
    f.page.presetsGo();
    m = f.page.presetsModel();
    EQ(at(m.rows, 1).text, std::string("gamma"));                         // sorted: ALPHA is gone, beta, gamma
    EQ(at(m.rows, 0).text, std::string("beta"));
    CHECK(at(m.rows, 1).selected);
    EQ(f.cfg.presetSaves, 2);

    // DELETE: a question first, with the name
    f.page.presetsDelete();
    m = f.page.presetsModel();
    CHECK(m.ask_delete);
    EQ(m.delete_text, std::string("Delete the preset \"gamma\"? This cannot be undone."));
    f.page.presetsDeleteConfirm(false);
    m = f.page.presetsModel();
    CHECK(!m.ask_delete);
    EQ(m.rows.size(), std::size_t{2});
    EQ(f.cfg.presetSaves, 2);
    f.page.presetsDelete();
    f.page.presetsDeleteConfirm(true);
    m = f.page.presetsModel();
    CHECK(!m.ask_delete);
    EQ(m.note, std::string("Deleted \"gamma\"."));
    CHECK(m.note_good);
    EQ(m.rows.size(), std::size_t{1});
    EQ(f.cfg.presetSaves, 3);
    EQ(f.cfg.presetList.size(), std::size_t{1});
    // the selection of a row that has gone is forgotten
    f.tick();
    m = f.page.presetsModel();
    CHECK(!at(m.rows, 0).selected && !m.load_enabled && !m.delete_enabled);
}

void loadingAPresetLoadsADocument() {
    const std::string airband = docOfOne("Air", 100, 5, 0.5f);
    Fx f(chain().g, true);
    f.cfg.presetList = {{"Airband", airband}};
    f.tick();
    f.page.canvas().setView(pg::View{(pg::Vec2{11.0f, 12.0f}), 1.5f});
    const std::string outgoing = documentOf(f.page.draft().graph(), f.page.canvas().ui.view);
    f.select(2);
    f.page.presetsRow(0);
    f.sub.clear();
    f.page.presetsLoad(f.sub);
    // STOP first (a load always leaves the patch stopped), then the document
    EQ(f.sub.sent.size(), std::size_t{2});
    CHECK(fc(cmdAt(f.sub, 0)).op == 0x0902u && fc(cmdAt(f.sub, 0)).ival[0] == 0);
    CHECK(fc(cmdAt(f.sub, 1)).op == kPatchSetGraph && fc(cmdAt(f.sub, 1)).ival[0] == FOXAPP_PATCH_GRAPH_DOCUMENT && fc(cmdAt(f.sub, 1)).ival[1] == 0);
    const pc::LoadResult pr = pc::parse(airband);
    EQ(textOf(cmdAt(f.sub, 1)), pc::graphCommandText(pr.graph));
    CHECK(pc::graphsEqual(f.page.draft().graph(), pr.graph));
    CHECK(f.page.canvas().ui.view.pan == (pg::Vec2{pr.panX, pr.panY}) && f.page.canvas().ui.view.zoom == 0.5f);
    EQ(f.page.canvas().ui.selected, pc::NodeId{0});
    CHECK(f.page.seeded());
    PatchPresetsModel m = f.page.presetsModel();
    EQ(m.note, std::string("Loaded \"Airband\". Press START to run it."));
    CHECK(m.note_good);
    // THE PREVIOUS SLOT holds what was replaced, taken NOW, and is written to the config
    EQ(f.cfg.previousText, outgoing);
    CHECK(f.cfg.presetSaves >= 1);
    CHECK(at(m.rows, 0).previous);
    // load it back: the patch the load replaced; the slot then holds the preset's
    f.page.presetsRow(0);
    f.sub.clear();
    f.page.presetsLoad(f.sub);
    EQ(f.sub.sent.size(), std::size_t{2});
    EQ(f.page.presetsModel().note, std::string("Loaded \"(previous patch)\". Press START to run it."));
    CHECK(f.page.canvas().ui.view.zoom == 1.5f && f.page.canvas().ui.view.pan == (pg::Vec2{11.0f, 12.0f}));
    EQ(f.page.draft().graph().nodes().size(), std::size_t{4});
    // a double click loads
    f.page.presetsRow(1);
    f.sub.clear();
    f.page.presetsRowActivated(1, f.sub);
    EQ(f.sub.sent.size(), std::size_t{2});
    EQ(f.page.draft().graph().nodes().size(), std::size_t{2});
    // a load drops what the old patch's nodes remembered
    CHECK(f.page.sinkLines().queues().empty());
}

void aLoadThatCannotHappenChangesNothing() {
    Fx f(chain().g, true);
    f.cfg.presetList = {{"broken", "this is not a patch"}, {"fine", docOfOne("F", 10, 1, 1.0f)}};
    f.tick();
    const std::string before = documentOf(f.page.draft().graph(), f.page.canvas().ui.view);
    f.sub.clear();
    f.page.presetsRow(0);
    f.page.presetsLoad(f.sub);
    // PARSED FIRST: nothing is sent, nothing changes - not a stop, not the slot
    EQ(f.sub.sent.size(), std::size_t{0});
    EQ(documentOf(f.page.draft().graph(), f.page.canvas().ui.view), before);
    PatchPresetsModel m = f.page.presetsModel();
    EQ(m.note, std::string("That preset is not a patch this version can read."));
    CHECK(!m.note_good);
    CHECK(f.cfg.previousText.empty());
    // a STOP the session would not take is a load that did not happen
    f.page.presetsRow(1);
    f.sub.refuse = true;
    f.page.presetsLoad(f.sub);
    EQ(f.sub.sent.size(), std::size_t{1});
    EQ(documentOf(f.page.draft().graph(), f.page.canvas().ui.view), before);
    EQ(f.page.presetsModel().note, std::string("That preset is not a patch this version can read."));
    CHECK(f.cfg.previousText.empty());
    // a document the session would not take after the STOP did go: the canvas is untouched
    f.sub.refuse = false;
    f.sub.refuseNth = 1;
    f.sub.calls = 0;
    f.page.presetsLoad(f.sub);
    EQ(documentOf(f.page.draft().graph(), f.page.canvas().ui.view), before);
    CHECK(f.cfg.previousText.empty());
    EQ(f.page.presetsModel().note, std::string("That preset is not a patch this version can read."));
    // a preset that went between the click and the load
    f.sub.refuseNth = -1;
    f.page.presetsRow(1);
    f.page.presetsDelete();
    f.page.presetsDeleteConfirm(true);
    f.sub.clear();
    f.page.presetsLoad(f.sub);
    EQ(f.page.presetsModel().note, std::string("That preset is no longer there."));
    CHECK(f.sub.sent.empty());
}

void nothingAboutAPresetIsLogged() {
    // the application log's tail rides in a crash report and PRIVACY.md promises presets never leave the machine
    const std::string mine = slurp(srcPath("slint/patch_logic.cpp"));
    const std::size_t from = mine.find("// ---- THE PRESETS ----");
    const std::size_t to = mine.find("// ---- END OF THE PRESETS ----");
    CHECK(from != std::string::npos && to != std::string::npos && from < to);
    if (from == std::string::npos || to == std::string::npos || from >= to) { return; }
    const std::string section = mine.substr(from, to - from);
    CHECK(section.find("diagLogf") == std::string::npos && section.find("diagWarnf") == std::string::npos && section.find("fprintf") == std::string::npos);
    CHECK(section.size() > 2000);
}

void theRadioSwitchIsLogged() {
    const std::string mine = slurp(srcPath("slint/patch_logic.cpp"));
    CHECK(has(mine, "patch: radio node %u switched %s"));
    const std::string app = slurp(srcPath("gui/app_window_patch_radios.cpp"));
    CHECK(has(app, "patch: radio node %u switched %s"));
}

// ================================================================================================================================
// the answers the page polls
// ================================================================================================================================

void answersGoWhereTheyBelong() {
    const Chain c = chain();
    Fx f(c.g);
    nm(f.page.draft().graph(), c.radio).x = 200.0f;
    f.sub.clear();
    f.tick();
    EQ(f.sub.count(kPatchSetGraph), std::size_t{1});
    EQ(f.page.draft().send().ticket, std::uint64_t{100});
    // the engine takes it as the next version
    pc::Graph engine = c.g;
    engine.mutableNode(c.radio)->x = 200.0f;
    f.publish(engine);
    f.page.onAnswer(f.answer(100, kPatchSetGraph, FOXAPI_OK, static_cast<double>(f.version)));
    f.tick();
    CHECK(f.page.draft().send().state == PatchDraft::Send::State::Idle);
    EQ(f.page.draft().version(), f.version);
    // an answer for another op is not the commit's
    nm(f.page.draft().graph(), c.radio).x = 300.0f;
    f.sub.clear();
    f.tick();
    EQ(f.sub.count(kPatchSetGraph), std::size_t{1});
    f.page.onAnswer(f.answer(f.page.draft().send().ticket, 0x0902u, FOXAPI_OK, 5));
    CHECK(f.page.draft().send().state == PatchDraft::Send::State::Sent);
    // a refused commit: the page shows the patch as the engine holds it
    f.page.onAnswer(f.answer(f.page.draft().send().ticket, kPatchSetGraph, FOXAPI_FAILED, 0));
    f.tick();
    CHECK(nd(f.page.draft().graph(), c.radio).x == 200.0f);
    CHECK(!f.page.draft().adoptWanted() && f.page.draft().send().state == PatchDraft::Send::State::Idle);
    // the scan's answer raises the mark, whatever it was
    f.page.onAnswer(f.answer(55, 0x8406));
    EQ(f.page.draft().scanDone(), std::uint64_t{55});
}

void theInspectorsWidth() {
    // none, the 24 px strip folded, 190 px open (the artboard's)
    EQ(inspectorWidth(false, false), 0.0f);
    EQ(inspectorWidth(false, true), 0.0f);
    EQ(inspectorWidth(true, false), 190.0f);
    EQ(inspectorWidth(true, true), 24.0f);
    Fx f(chain().g);
    EQ(f.page.inspectorWidthNow(), 0.0f);
    f.select(1);
    EQ(f.page.inspectorWidthNow(), 190.0f);
    f.page.inspectorFold();
    EQ(f.page.inspectorWidthNow(), 24.0f);
    // the canvas keeps the rest of the page: patchCanvasWidth's own arithmetic, with the gap
    EQ(canvasWidthFor(1000.0f, 190.0f), pg::patchCanvasWidth(1000.0f, 190.0f));
    EQ(canvasWidthFor(1000.0f, 0.0f), 1000.0f);
    EQ(infoWidthFor(1000.0f), pg::patchInfoPaneWidth(1000.0f, 1.0f));
    EQ(infoWidthFor(1000.0f), 420.0f);
    EQ(infoWidthFor(500.0f), 250.0f);                   // never more than half the page
}

}  // namespace

int main() {
#if defined(_MSC_VER)
    // A failed assertion of the C++ runtime must print to stderr and stop the run, never open a modal dialog on the desktop of whoever runs it.
    for (const int kind : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        (void)kind;
        _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not eat the lines before it
    RUN(everySentenceIsADrawers);
    RUN(sourcesStateWhatWeDo);
    RUN(listsAreRead);
    RUN(liveIsRead);
    RUN(lookIsRead);
    RUN(smallRules);
    RUN(opsAreTheWindows);
    RUN(draftAdoptsTheFirstLook);
    RUN(draftCommitsAnEdit);
    RUN(draftSettlesAnAnswer);
    RUN(draftRefusedAnswerAdoptsTheEngine);
    RUN(draftDropsAnEditOfAnOldPatch);
    RUN(draftFollowsTheEngine);
    RUN(draftLoadsADocument);
    RUN(draftChoosesTheRadiosOfWaitingParts);
    RUN(defaultDeviceOrder);
    RUN(viewShownIsOffered);
    RUN(notShowingBuildsNothing);
    RUN(theSeedIsTheReceiversRadio);
    RUN(theSeedWaitsForTheRadioToOpen);
    RUN(theSeedCopiesNoCentreItWasNotTold);
    RUN(theSavedPatchIsRestored);
    RUN(theRestoreIsRetriedUntilTaken);
    RUN(aSavedPatchThatIsNotOneIsSeededOver);
    RUN(aRestoredPatchThatLostWiresIsStillLoaded);
    RUN(theConfigKeepsTheDocument);
    RUN(pressOnAHeaderDragsTheNode);
    RUN(pressOnABodySelectsOnly);
    RUN(pressOnTheCloseKeyRemovesTheNode);
    RUN(pressOnTheGripResizes);
    RUN(aWireIsMadeFromAPort);
    RUN(theGhostWireFollowsThePointer);
    RUN(everyRefusalSaysWhy);
    RUN(theRefusalBubbleLastsTwoAndAHalfSeconds);
    RUN(aWireIsSelectedAndCut);
    RUN(deleteRemovesTheSelectedNode);
    RUN(aDragOnEmptyCanvasPans);
    RUN(theMiddleButtonPansAnywhere);
    RUN(theWheelPansAndZooms);
    RUN(theCanvasKeys);
    RUN(theHotKeysFollowThePointer);
    RUN(aNewPatchForgetsTheInteraction);
    RUN(theModelIsTheGeometryItDrewFrom);
    RUN(theKeysNeedRoom);
    RUN(blockedFaultAndActiveMarks);
    RUN(theTransportReads);
    RUN(theTabChip);
    RUN(thePartKeys);
    RUN(thePartKeysPlaceNodes);
    RUN(theRadioPartTakesTheDeviceAfterTheScan);
    RUN(theDecoderKeys);
    RUN(aDecoderKeyPlacesTheDecoder);
    RUN(theBlueprintsAreOfferedWhenTheyCanBe);
    RUN(theBlueprintListIsBuiltWhenOpened);
    RUN(aBlueprintIsPlaced);
    RUN(aBlueprintReusesAnUnwiredRadio);
    RUN(aBlueprintFindsNoRoomForAnotherRadio);
    RUN(aTowerBlueprintCountsTheFrequenciesItLeftOut);
    RUN(theVerdictAndThePlan);
    RUN(withNothingSelectedItHelps);
    RUN(aSelectedNodeIsNamedAndItsProblemsListed);
    RUN(theChannelLevelIsSaidOnlyForAChannel);
    RUN(theRadioFace);
    RUN(theChannelDemodAndSinkFaces);
    RUN(theSpeakerFace);
    RUN(theTextOutFace);
    RUN(textOutQueuesAreBoundedAndForgotten);
    RUN(theDecoderFaceReadsLinesAndPicture);
    RUN(theDisplayAndMapFaces);
    RUN(theInspectorOfAnythingNamesTheNode);
    RUN(theRadioInspectorLists);
    RUN(theRadioInspectorRatesAndLines);
    RUN(theChannelDemodSinkAndMapInspectors);
    RUN(theDecoderInspector);
    RUN(theDecoderInspectorHoldsTheKitsForm);
    RUN(theFacesEditTheDraft);
    RUN(theInspectorEditsTheSelectedNode);
    RUN(theDeviceChoiceIsOneRadioOneDevice);
    RUN(theCentreIsJudgedByTheRadioBehindTheConverter);
    RUN(notesGoWithTheirNodes);
    RUN(thePresetsPageStartsEmpty);
    RUN(thePresetsAreReadFromTheConfig);
    RUN(saveAsKeepsThePatchUnderAName);
    RUN(saveAsRefusalsAreSaid);
    RUN(renameAndDelete);
    RUN(loadingAPresetLoadsADocument);
    RUN(aLoadThatCannotHappenChangesNothing);
    RUN(nothingAboutAPresetIsLogged);
    RUN(theRadioSwitchIsLogged);
    RUN(answersGoWhereTheyBelong);
    RUN(theInspectorsWidth);
    return testSummary("test_slint_host_patch");
}
