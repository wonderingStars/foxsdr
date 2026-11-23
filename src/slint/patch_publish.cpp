// patch_publish.cpp - the PATCH page's publisher (patch_publish.hpp): patch_logic.hpp's models as the window's generated structs, the window's gestures as calls on the PatchPage
// that name them. SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "patch_publish.hpp"
#include "safe_text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/health_events.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "patch_faces.hpp"
#include "signatures.hpp"

namespace foxsdr::slintpatch {

namespace cc = cascade::core;
using cascade::i18n::tr;

namespace {

slint::SharedString ss(const std::string& s) { return foxsdr::slintsafe::shared(s, __FILE__); }

::ChipTone tone(Tone t) {
    switch (t) {
        case Tone::Good: return ::ChipTone::Good;
        case Tone::Warn: return ::ChipTone::Warn;
        case Tone::Err: return ::ChipTone::Err;
        default: return ::ChipTone::Neutral;
    }
}

template <class T> std::shared_ptr<slint::Model<T>> model(std::vector<T> v) { return std::make_shared<slint::VectorModel<T>>(std::move(v)); }

std::vector<::SelectItem> items(const std::vector<SelectItem>& v) {
    std::vector<::SelectItem> out;
    out.reserve(v.size());
    for (const SelectItem& s : v) {
        ::SelectItem o{};
        o.label = ss(s.label);
        o.id = ss(s.id);
        o.enabled = s.enabled;
        o.note = ss(s.note);
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<slint::SharedString> strings(const std::vector<std::string>& v) {
    std::vector<slint::SharedString> out;
    out.reserve(v.size());
    for (const std::string& s : v) { out.push_back(ss(s)); }
    return out;
}

std::vector<::FaceLine> faceLines(const std::vector<FaceLine>& v) {
    std::vector<::FaceLine> out;
    out.reserve(v.size());
    for (const FaceLine& l : v) {
        ::FaceLine o{};
        o.text = ss(l.text);
        o.tone = l.tone;
        out.push_back(std::move(o));
    }
    return out;
}

// ---- signatures: the content of a model as one string, so a model whose content did not move is not set again (a set list recreates every repeater item behind it) ----
struct Sig {
    std::string s;
    Sig& operator<<(const std::string& v) { s += v; s += '\x1f'; return *this; }
    Sig& operator<<(const char* v) { s += v; s += '\x1f'; return *this; }
    Sig& operator<<(long long v) { s += std::to_string(v); s += '\x1f'; return *this; }
    Sig& operator<<(int v) { return *this << static_cast<long long>(v); }
    Sig& operator<<(unsigned v) { return *this << static_cast<long long>(v); }
    Sig& operator<<(std::uint64_t v) { return *this << static_cast<long long>(v); }
    Sig& operator<<(bool v) { s += v ? '1' : '0'; s += '\x1f'; return *this; }
    Sig& operator<<(float v) { char b[32]; std::snprintf(b, sizeof(b), "%.2f", static_cast<double>(v)); s += b; s += '\x1f'; return *this; }
    Sig& operator<<(double v) { return *this << static_cast<float>(v); }
    void sep() { s += '\x1e'; }
};

Sig& operator<<(Sig& g, const std::vector<FaceLine>& v) {
    for (const FaceLine& l : v) { g << l.text << l.tone; }
    g.sep();
    return g;
}
Sig& operator<<(Sig& g, const std::vector<SelectItem>& v) {
    for (const SelectItem& i : v) { g << i.label << i.id << i.enabled << i.note; }
    g.sep();
    return g;
}
Sig& operator<<(Sig& g, const std::vector<std::string>& v) {
    for (const std::string& i : v) { g << i; }
    g.sep();
    return g;
}

std::string sigBand(const PatchModel& m) {
    Sig g;
    g << m.running << m.run_label << m.run_tooltip << m.run_line << m.run_line_lit << m.presets_open << m.tab_chip << static_cast<int>(m.tab_chip_tone);
    for (const PartKey& k : m.parts) { g << k.label << k.kind << k.enabled << k.tooltip; }
    g.sep();
    for (const DecoderKey& k : m.decoders) { g << k.label << k.index << k.tooltip; }
    g.sep();
    g << m.no_decoders << m.blueprints_shown << m.blueprints << m.blueprint_note << m.inspector_shown << m.inspector_folded;
    g << m.info.runnable << m.info.verdict << m.info.level_line << m.info.plan_line << m.info.none_selected << m.info.help << m.info.selection << m.info.folded_note;
    for (const ProblemLine& p : m.info.problems) { g << p.text << p.advisory; }
    return g.s;
}

void sigFace(Sig& g, const FaceModel& f) {
    g << f.radio_switch << f.radio_switch_label << f.device_label << f.centre_text << f.centre_note << f.freq_text << f.mode_index << f.squelch_on << f.squelch_norm << f.squelch_text
      << f.gate_shown << f.gate_text << f.gate_word << f.gate_lit << f.level_shown << f.level_norm << f.picture_id << f.have_picture << f.lines << f.lines_after;
}

std::string sigNode(const CanvasNode& n) {
    Sig g;
    g << n.id << n.kind << n.kind_caption << n.name << n.x << n.y << n.w << n.h << n.selected << n.blocked << n.fault << n.active << n.hot_close << n.hot_grip;
    for (const PortDot& p : n.inputs) { g << p.x << p.y << p.tone; }
    g.sep();
    for (const PortDot& p : n.outputs) { g << p.x << p.y << p.tone; }
    g.sep();
    g << n.reading << n.reading_tone << n.reading2;
    sigFace(g, n.face);
    return g.s;
}

std::string sigInspector(const InspectorModel& m, const std::string& settingsSig) {
    Sig g;
    g << m.shown << m.folded << m.node << m.kind << m.kind_caption << m.name << m.freq_text << m.mode_index << m.squelch_on << m.squelch_norm << m.squelch_text << m.gate_shown
      << m.gate_text << m.gate_word << m.gate_lit << m.map_wired << m.map_help << m.audio_sink << m.help;
    const InspectorRadio& r = m.radio;
    g << r.device_items << r.device_index << r.device_label << r.looking << r.look_enabled << r.centre_text << r.centre_note << r.rate_items << r.rate_index << r.rate_label << r.rate_fixed
      << r.rate_fixed_text << r.lines;
    const InspectorDecoder& d = m.decoder;
    g << d.plugin_items << d.plugin_index << d.plugin_label << d.feed_note << d.fed << d.shown_note << d.has_settings << d.settings_module << settingsSig;
    for (const InspectorMapSource& s : m.map_sources) { g << s.count << s.name; }
    g.sep();
    const InspectorSink& k = m.sink;
    g << k.items << k.index << k.label << k.lines << k.files_note;
    return g.s;
}

std::string sigPresets(const PatchPresetsModel& m) {
    Sig g;
    for (const PresetRow& r : m.rows) { g << r.text << r.previous << r.selected; }
    g.sep();
    g << m.empty_shown << m.empty_text << m.load_enabled << m.rename_enabled << m.delete_enabled << m.edit << m.name_draft << m.note << m.note_good << m.ask_replace << m.replace_text
      << m.ask_delete << m.delete_text;
    return g.s;
}

// ---- the mappings ----
::FaceModel mapFace(const FaceModel& f, const slint::Image& picture) {
    ::FaceModel o{};
    o.radio_switch = f.radio_switch;
    o.radio_switch_label = ss(f.radio_switch_label);
    o.device_label = ss(f.device_label);
    o.centre_text = ss(f.centre_text);
    o.centre_note = ss(f.centre_note);
    o.freq_text = ss(f.freq_text);
    o.mode_index = f.mode_index;
    o.squelch_on = f.squelch_on;
    o.squelch_norm = f.squelch_norm;
    o.squelch_text = ss(f.squelch_text);
    o.gate_shown = f.gate_shown;
    o.gate_text = ss(f.gate_text);
    o.gate_word = ss(f.gate_word);
    o.gate_lit = f.gate_lit;
    o.level_shown = f.level_shown;
    o.level_norm = f.level_norm;
    o.picture = picture;
    o.have_picture = f.have_picture;
    o.lines = model(faceLines(f.lines));
    o.lines_after = model(faceLines(f.lines_after));
    return o;
}

std::vector<::PortDot> dots(const std::vector<PortDot>& v) {
    std::vector<::PortDot> out;
    out.reserve(v.size());
    for (const PortDot& p : v) { out.push_back(::PortDot{p.x, p.y, p.tone}); }
    return out;
}

::CanvasNode mapNode(const CanvasNode& n, const slint::Image& picture) {
    ::CanvasNode o{};
    o.id = n.id;
    o.kind = n.kind;
    o.kind_caption = ss(n.kind_caption);
    o.name = ss(n.name);
    o.x = n.x;
    o.y = n.y;
    o.w = n.w;
    o.h = n.h;
    o.selected = n.selected;
    o.blocked = n.blocked;
    o.fault = n.fault;
    o.active = n.active;
    o.hot_close = n.hot_close;
    o.hot_grip = n.hot_grip;
    o.inputs = model(dots(n.inputs));
    o.outputs = model(dots(n.outputs));
    o.reading = ss(n.reading);
    o.reading_tone = n.reading_tone;
    o.reading2 = ss(n.reading2);
    o.face = mapFace(n.face, picture);
    return o;
}

::PatchModel mapBand(const PatchModel& m) {
    ::PatchModel o{};
    o.running = m.running;
    o.run_label = ss(m.run_label);
    o.run_tooltip = ss(m.run_tooltip);
    o.run_line = ss(m.run_line);
    o.run_line_lit = m.run_line_lit;
    o.presets_open = m.presets_open;
    o.tab_chip = ss(m.tab_chip);
    o.tab_chip_tone = tone(m.tab_chip_tone);
    std::vector<::PartKey> parts;
    for (const PartKey& k : m.parts) { parts.push_back(::PartKey{ss(k.label), k.kind, k.enabled, ss(k.tooltip)}); }
    o.parts = model(std::move(parts));
    std::vector<::DecoderKey> decoders;
    for (const DecoderKey& k : m.decoders) { decoders.push_back(::DecoderKey{ss(k.label), k.index, ss(k.tooltip)}); }
    o.decoders = model(std::move(decoders));
    o.no_decoders = ss(m.no_decoders);
    o.blueprints_shown = m.blueprints_shown;
    o.blueprints = model(items(m.blueprints));
    o.blueprint_note = ss(m.blueprint_note);
    std::vector<::ProblemLine> problems;
    for (const ProblemLine& p : m.info.problems) { problems.push_back(::ProblemLine{ss(p.text), p.advisory}); }
    o.info.runnable = m.info.runnable;
    o.info.verdict = ss(m.info.verdict);
    o.info.level_line = ss(m.info.level_line);
    o.info.plan_line = ss(m.info.plan_line);
    o.info.none_selected = m.info.none_selected;
    o.info.help = model(strings(m.info.help));
    o.info.selection = ss(m.info.selection);
    o.info.folded_note = ss(m.info.folded_note);
    o.info.problems = model(std::move(problems));
    o.inspector_shown = m.inspector_shown;
    o.inspector_folded = m.inspector_folded;
    return o;
}

::PatchPresetsModel mapPresets(const PatchPresetsModel& m) {
    ::PatchPresetsModel o{};
    std::vector<::PresetRow> rows;
    for (const PresetRow& r : m.rows) { rows.push_back(::PresetRow{ss(r.text), r.previous, r.selected}); }
    o.rows = model(std::move(rows));
    o.empty_shown = m.empty_shown;
    o.load_enabled = m.load_enabled;
    o.rename_enabled = m.rename_enabled;
    o.delete_enabled = m.delete_enabled;
    o.edit = m.edit;
    o.name_draft = ss(m.name_draft);
    o.note = ss(m.note);
    o.note_good = m.note_good;
    o.ask_replace = m.ask_replace;
    o.replace_text = ss(m.replace_text);
    o.ask_delete = m.ask_delete;
    o.delete_text = ss(m.delete_text);
    return o;
}

constexpr int kProbeGapTicks = 8;           // ticks between the probe's steps (a commit and its answer round-trip)
constexpr int kProbeWaitMax = 120;          // a probe step that waits longer than this gives up
constexpr std::size_t kAnswerLog = 8;       // the last answers the summary names

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// the seams of patch_logic.hpp, over this window's duties and kit

struct PatchPublisher::Impl {
    // The settings file's half: the saved patch, the presets, the previous-patch slot (the Interface half HostDuties holds).
    class Config final : public PatchConfig {
    public:
        Config(foxsdr::slinthost::HostDuties& d, bool bounded) : d_(d), bounded_(bounded) {}
        // FOXSDR_SLINT_PATCH_DOC (a bounded run only; the drive header's patchdoc=): a patch document the page restores in place of the profile's saved patch, so a script can start from a
        // known graph (docs/multi-radio-records/patch-faces.md). Nothing is ever written to it.
        std::string patch() const override {
            if (bounded_) {
                if (const char* f = std::getenv("FOXSDR_SLINT_PATCH_DOC"); f != nullptr && *f != '\0') {
                    std::ifstream in(f, std::ios::binary);
                    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    if (!text.empty()) { return text; }
                }
            }
            return d_.interfaceHalf().patch;
        }
        std::vector<cc::PatchPreset> presets() const override { return d_.interfaceHalf().patchPresets; }
        std::string previous() const override { return d_.interfaceHalf().patchPresetPrevious; }
        void savePatch(const std::string& text) override {
            cc::AppConfig half = d_.interfaceHalf();
            half.patch = text;
            (void)d_.saveInterfaceHalf(half);
        }
        // A SAVED PATCH THAT LOST CONNECTIONS on the way back in, and the page went on with the rest: counted, anonymously, as the classic window counts it (the ledger holds it until
        // reporting is decided).
        void patchLostConnections(int dropped) override {
            if (dropped > 0) { cc::health::noteRecovered(cc::health::Recovered::PatchLoad); }
        }
        void savePresets(const std::vector<cc::PatchPreset>& list, const std::string& previous) override {
            cc::AppConfig half = d_.interfaceHalf();
            half.patchPresets = list;
            half.patchPresetPrevious = previous;
            (void)d_.saveInterfaceHalf(half);
        }

    private:
        foxsdr::slinthost::HostDuties& d_;
        bool bounded_ = false;
    };
    // What a command's ticket costs: the kit's one submit.
    class Sub final : public Submitter {
    public:
        explicit Sub(Impl& im) : im_(im) {}
        std::uint64_t submit(const Command& c) override { return im_.send(c); }
        std::uint64_t offer(const Command& c) override { return im_.send(c); }

    private:
        Impl& im_;
    };
    // A picture a decoder node holds, as the window's image.
    struct Pic {
        slint::Image image;
        std::uint64_t id = 0;
    };
    struct Probe {
        int step = 0, wait = 0, waited = 0;
        bool done = false;
        std::string note;
    };

    PatchPublisher& self;
    foxsdr::slintkit::LiveTable table;
    Config config;
    Sub sub;
    PatchPage page;
    foxsdr::slinthost::Signatures sigs;
    std::shared_ptr<slint::VectorModel<::CanvasNode>> nodes = std::make_shared<slint::VectorModel<::CanvasNode>>();
    std::vector<std::string> nodeSigs;
    std::shared_ptr<slint::VectorModel<::CanvasWire>> wires = std::make_shared<slint::VectorModel<::CanvasWire>>();
    std::vector<std::string> wireSigs;
    std::map<std::string, std::shared_ptr<slint::VectorModel<::SettingField>>> fieldModels;   // one per module's form, updated IN PLACE (a replaced model loses what is being typed)
    std::map<pc::NodeId, Pic> pictures;
    std::string settingsFile;                     // the module whose form the inspector shows (the last publish's): where inspector-setting goes
    std::vector<foxsdr::slintkit::SettingSpec> settingsSpecs;
    std::uint32_t flags = 0;
    bool haveBand = false;
    Probe probe;
    std::map<std::uint32_t, int> sentByOp, withheldByOp;
    std::vector<std::string> answers;
    std::int64_t tickNo = 0;

    Impl(PatchPublisher& p, foxsdr::slinthost::HostDuties& duties) : self(p), table(p.api_, p.session_), config(duties, p.bounded_), sub(*this) {
        page.setConfig(&config);
        page.setKit(&p.kit_.lists());
    }

    void say(const std::string& line) const { std::fprintf(stderr, "foxsdr-slint: patch: %s\n", line.c_str()); }

    // ---- commands ----
    // A bounded run lets through only the two ops that touch the engine's graph and nothing outside it: the draft's SET_GRAPH and the view-shown flag. Every other op is said once
    // by its code and NOT sent (ticket 0: the logic then does what the window does with a refusal).
    // FOXSDR_SLINT_PATCH_RUN=1 (a bounded run only; patch faces, docs/multi-radio-records/patch-faces.md): START and ALL OFF go through too, so a drive script can run a patch whose radio is
    // the signal generator (a scratch run has no other device) and read the faces back. Nothing but the engine's own generator is ever opened by it.
    static bool patchRunSeam() {
        const char* e = std::getenv("FOXSDR_SLINT_PATCH_RUN");
        return e != nullptr && e[0] == '1';
    }
    static bool boundedMaySend(std::uint32_t op) {
        return op == FOXAPP_OP_PATCH_SET_GRAPH || op == FOXAPP_OP_SET_PATCH_PAGE_OPEN || (patchRunSeam() && (op == FOXAPI_OP_PATCH_RUN || op == FOXAPI_OP_PATCH_ALL_OFF));
    }

    std::uint64_t send(const Command& c) {
        if (!c.valid) { return 0u; }
        const std::uint32_t op = c.q.c.op;
        if (self.bounded_ && !boundedMaySend(op)) {
            ++self.withheld_;
            if (withheldByOp[op]++ == 0) { say("bounded run - op 0x" + hex(op) + " not sent"); }
            return 0u;
        }
        const std::uint64_t ticket = self.kit_.submit(c.q);
        if (ticket != 0u) {
            ++self.sent_;
            ++sentByOp[op];
        }
        if (self.bounded_ && op == FOXAPP_OP_PATCH_SET_GRAPH) {
            say("SET_GRAPH " + std::string(c.q.c.ival[0] == FOXAPP_PATCH_GRAPH_DOCUMENT ? "document" : "edit") + " from version " + std::to_string(c.q.c.ival[1]) + ", " +
                std::to_string(c.q.longText.empty() ? std::strlen(c.q.c.text) : c.q.longText.size()) + " bytes, ticket " + std::to_string(ticket));
        } else if (self.bounded_ && sentByOp[op] == 1) {
            say("op 0x" + hex(op) + " sent, ticket " + std::to_string(ticket));
        }
        return ticket;
    }
    void sendAll(const std::vector<Command>& cs) {
        for (const Command& c : cs) { (void)send(c); }
    }
    static std::string hex(std::uint32_t v) {
        char b[16];
        std::snprintf(b, sizeof(b), "%04x", static_cast<unsigned>(v));
        return b;
    }

    // ---- the pictures ----
    // read_image for each decoder node whose newest picture the page does not hold; one a tick (a whole picture is a few hundred KB), whole or not at all.
    void readPictures() {
        const std::vector<PictureWant> wants = page.picturesToRead();
        if (wants.empty() || self.api_ == nullptr || !FOXAPI_HAS(self.api_, read_image)) { return; }
        const PictureWant& w = wants.front();
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::max<std::uint64_t>(w.bytes, 1u)));
        FoxImageInfo info{};
        info.structSize = sizeof(info);
        std::int32_t got = self.api_->read_image(self.session_, w.image_id, &info, bytes.data(), bytes.size());
        if (got > 0 && static_cast<std::size_t>(got) > bytes.size()) {   // the picture grew between the list and the read: ask again with room
            bytes.resize(static_cast<std::size_t>(got));
            got = self.api_->read_image(self.session_, w.image_id, &info, bytes.data(), bytes.size());
        }
        if (got <= 0 || static_cast<std::size_t>(got) > bytes.size() || !pictureWhole(info.format, info.width, info.height, static_cast<std::uint64_t>(got))) { return; }
        const std::size_t count = static_cast<std::size_t>(info.width) * info.height;
        slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(info.width, info.height);
        slint::Rgba8Pixel* px = buf.begin();
        const bool rgb = info.format == CASCADE_IMAGE_RGB24;
        for (std::size_t i = 0; i < count; ++i) {
            if (rgb) {
                px[i] = {bytes[3 * i], bytes[3 * i + 1], bytes[3 * i + 2], 255};
            } else {
                px[i] = {bytes[i], bytes[i], bytes[i], 255};
            }
        }
        Pic p;
        p.image = slint::Image(buf);
        p.id = w.image_id;
        pictures[w.node] = std::move(p);
        page.pictureTaken(w.node, w.image_id);
    }
    slint::Image pictureOf(pc::NodeId node, std::uint64_t imageId, bool have) const {
        if (!have) { return slint::Image(); }
        const auto it = pictures.find(node);
        return it != pictures.end() && it->second.id == imageId ? it->second.image : slint::Image();
    }
    void dropGonePictures() {
        for (auto it = pictures.begin(); it != pictures.end();) {
            it = page.draft().graph().find(it->first) == nullptr ? pictures.erase(it) : std::next(it);
        }
    }

    // ---- the plugin settings form of a decoder node: the kit's rows, updated in place ----
    // ---- THE LIVE FACES (patch_faces.hpp; docs/multi-radio-records/patch-faces.md) ----
    // A Spectrum part keeps a well (its two pictures, painted by the receiver's own painters) and the images the window was last given; a radio's frame is read once a tick however many
    // parts hang off it. A Map part keeps a view and a compositor of its own (the map pages' compositor), its targets and what MapPane places. All of it is touched only while the PATCH
    // view is showing (publish() does not call into it otherwise) and exists only for the parts the draft holds.
    struct SpecFace {
        SpectrumWell well;
        std::uint64_t pushedSeq = 0;
        bool pushed = false, live = false;
        slint::Image trace, waterfall;
        std::uint64_t revision = 0;
        std::vector<SpectrumMarker> markers;
        SpectrumSlice slice;
    };
    struct RadioFrame {
        std::vector<float> bins;
        std::uint64_t seq = 0;
        bool have = false;
    };
    struct MapFace {
        foxsdr::slintmap::TileCompositor comp;
        MapFaceView view;
        int w = 0, h = 0;
        bool sourceSet = false, placed = false, zoomIn = true, zoomOut = true;
        std::string attribution, hover, scaleText;
        float scaleFrac = 0.0f;
        std::array<slint::SharedPixelBuffer<slint::Rgba8Pixel>, 2> buffers;
        int which = 0;
        slint::Image base;
        std::vector<MapFaceTarget> targets;
        std::vector<std::string> targetIds;
        std::size_t sources = 0;
        std::uint64_t revision = 0;
    };
    std::map<pc::NodeId, SpecFace> specs;
    std::map<pc::NodeId, RadioFrame> radioFrames;
    std::map<pc::NodeId, std::unique_ptr<MapFace>> maps;
    std::shared_ptr<slint::VectorModel<::FaceLive>> lives = std::make_shared<slint::VectorModel<::FaceLive>>();
    std::vector<std::string> liveSigs;
    std::vector<float> frameScratch = std::vector<float>(16384);
    mutable std::map<std::string, std::string> lastLook;      // faceState "moved": what the pictures looked like the last time it was read
    mutable std::map<std::string, std::uint64_t> lastFrames;  // faceState "framesdelta": the frame count the last time it was read

    static cc::TrackPresentation presentOf(const CascadeTrack& t) { return cc::trackPresentation(t); }

    // One radio's newest frame (read_node_spectrum with the held sequence as the cursor: NO_CHANGE keeps the frame); NOT_FOUND drops it. The buffer is the engine's own bin count.
    void readRadioFrame(pc::NodeId radio) {
        RadioFrame& rf = radioFrames[radio];
        if (frameScratch.size() < 4096u) { frameScratch.resize(4096u); }
        FoxSpectrumInfo info{};
        info.structSize = sizeof(info);
        std::int32_t r = self.api_->read_node_spectrum(self.session_, radio, rf.have ? rf.seq : 0u, &info, frameScratch.data(), static_cast<std::uint32_t>(frameScratch.size()));
        if (r == FOXAPI_OK && info.binCount > frameScratch.size()) {
            frameScratch.resize(info.binCount);
            r = self.api_->read_node_spectrum(self.session_, radio, 0u, &info, frameScratch.data(), static_cast<std::uint32_t>(frameScratch.size()));
        }
        if (r == FOXAPI_OK) {
            rf.bins.assign(frameScratch.begin(), frameScratch.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(info.copied, frameScratch.size())));
            rf.seq = info.seq;
            rf.have = !rf.bins.empty();
        } else if (r != FOXAPI_NO_CHANGE) {
            rf.have = false;
            rf.bins.clear();
            rf.seq = 0;
        }
    }

    static slint::Image imageOf(const std::vector<foxsdr::slinthost::Rgba>& px, std::uint32_t w, std::uint32_t h) {
        slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(w, h);
        if (!px.empty() && px.size() == static_cast<std::size_t>(w) * h) { std::memcpy(buf.begin(), px.data(), sizeof(slint::Rgba8Pixel) * px.size()); }
        return slint::Image(buf);
    }

    // ONCE A TICK WHILE THE VIEW SHOWS: every wired Spectrum part takes its radio's newest frame (a new row), is reshaped to its node's size and slice, and hands the window two new pictures.
    void readSpectra() {
        const pc::Graph& g = page.draft().graph();
        std::set<pc::NodeId> displays;
        for (const pc::Node& n : g.nodes()) {
            if (n.kind == pc::NodeKind::Display) { displays.insert(n.id); }
        }
        for (auto it = specs.begin(); it != specs.end();) { it = displays.count(it->first) != 0u ? std::next(it) : specs.erase(it); }
        std::map<pc::NodeId, SpectrumSlice> slices;
        std::set<pc::NodeId> wanted;
        for (const pc::NodeId id : displays) {
            slices[id] = spectrumSliceOf(g, id, page.lists());
            const SpectrumSlice& s = slices[id];
            if (s.wired && s.radio_open && s.radio != pc::kNoNode) { wanted.insert(s.radio); }
        }
        for (auto it = radioFrames.begin(); it != radioFrames.end();) { it = wanted.count(it->first) != 0u ? std::next(it) : radioFrames.erase(it); }
        const bool canRead = self.api_ != nullptr && self.session_ != nullptr && FOXAPI_HAS(self.api_, read_node_spectrum);
        std::set<pc::NodeId> have;
        for (const pc::NodeId radio : wanted) {
            if (canRead) { readRadioFrame(radio); }
            const auto it = radioFrames.find(radio);
            if (it != radioFrames.end() && it->second.have) { have.insert(radio); }
        }
        page.setSpectrumHave(have);
        for (const pc::NodeId id : displays) {
            SpecFace& sf = specs[id];
            const SpectrumSlice& s = slices[id];
            const pc::Node* node = g.find(id);
            const pg::Vec2 sz = node != nullptr ? pg::nodeSize(*node) : pg::Vec2{0.0f, 0.0f};
            const WellSize ws = wellSizeOf(sz.x, sz.y);
            sf.slice = s;
            sf.markers = s.wired ? spectrumMarkersOf(g, id, page.lists(), s) : std::vector<SpectrumMarker>{};
            if (!s.wired || have.count(s.radio) == 0u || !ws.usable()) {
                if (sf.live) { ++sf.revision; }
                sf.live = false;
                sf.pushed = false;
                continue;
            }
            if (sf.well.shape(ws, s.lo_hz, s.hi_hz, s.rate_hz)) { sf.pushed = false; }
            const RadioFrame& rf = radioFrames[s.radio];
            if (!sf.pushed || sf.pushedSeq != rf.seq) {
                if (sf.well.push(rf.bins)) {
                    sf.trace = imageOf(sf.well.trace(), sf.well.size().w, sf.well.size().traceH);
                    sf.waterfall = imageOf(sf.well.waterfall(), sf.well.size().w, sf.well.size().wfH);
                    sf.pushed = true;
                    sf.pushedSeq = rf.seq;
                    ++sf.revision;
                }
            }
            if (!sf.live && sf.pushed) { ++sf.revision; }
            sf.live = sf.pushed;
        }
    }

    // ---- the Map faces ----
    MapFace* mapFor(pc::NodeId node, bool create) {
        const pc::Node* n = page.draft().graph().find(node);
        if (n == nullptr || n->kind != pc::NodeKind::Map) { return nullptr; }
        auto it = maps.find(node);
        if (it == maps.end()) {
            if (!create) { return nullptr; }
            it = maps.emplace(node, std::make_unique<MapFace>()).first;
        }
        return it->second.get();
    }
    std::vector<cc::HostTrack> tracksOf(pc::NodeId node) const {
        return tracksOfSources(self.kit_.tracks(), pc::mapSources(page.draft().graph(), node, page.lists().parts));
    }
    static std::string hoverTextOf(const cc::HostTrack& tr) {
        std::string s = tr.t.label[0] != '\0' ? std::string(tr.t.label) : std::string(tr.t.id);
        char b[48];
        if (std::isfinite(tr.t.altM)) { std::snprintf(b, sizeof(b), " \xC2\xB7 %.0f ft", tr.t.altM * 3.28084); s += b; }
        if (std::isfinite(tr.t.speedMps)) { std::snprintf(b, sizeof(b), " \xC2\xB7 %.0f kt", tr.t.speedMps * 1.94384); s += b; }
        return s;
    }
    static slint::Color kindColour(std::uint32_t kind) {   // kit_publish.cpp's colours for a target's kind
        switch (kind) {
            case CASCADE_TRACK_AIRCRAFT: return slint::Color::from_argb_uint8(255, 0xf5, 0xa5, 0x24);
            case CASCADE_TRACK_VESSEL: return slint::Color::from_argb_uint8(255, 0x7a, 0xd7, 0xff);
            case CASCADE_TRACK_STATION: return slint::Color::from_argb_uint8(255, 0x3d, 0xdc, 0x84);
            case CASCADE_TRACK_SATELLITE: return slint::Color::from_argb_uint8(255, 0xe6, 0xe8, 0xec);
            default: return slint::Color::from_argb_uint8(255, 0xc9, 0xce, 0xd6);
        }
    }

    // ONCE A TICK WHILE THE VIEW SHOWS: each Map part composes its picture (the basemap's tiles or the coastline, the graticule, the receiver's rings) and projects the targets of the
    // decoders wired into it. The view is fitted to the targets the first time there are any, as the ImGui part's is.
    void tickMaps(const FoxReceiverState& state) {
        const pc::Graph& g = page.draft().graph();
        std::set<pc::NodeId> present;
        for (const pc::Node& n : g.nodes()) {
            if (n.kind == pc::NodeKind::Map) { present.insert(n.id); }
        }
        for (auto it = maps.begin(); it != maps.end();) { it = present.count(it->first) != 0u ? std::next(it) : maps.erase(it); }
        const bool home = (state.flags & FOXAPI_RX_POSITION_SET) != 0u;
        for (const pc::NodeId id : present) {
            MapFace& mf = *mapFor(id, true);
            const std::vector<cc::HostTrack> mine = tracksOf(id);
            mf.sources = pc::mapSources(g, id, page.lists().parts).size();
            if (mf.w < 16 || mf.h < 16) { continue; }   // the picture's size comes with the face (face-map-resized)
            if (!mf.placed) {
                // A fresh part opens on the receiver's position when it has one, else the whole planet; the first targets pull it to them (below).
                if (home) {
                    mf.view.centreLat = state.rxLatDeg;
                    mf.view.centreLon = state.rxLonDeg;
                    mf.view.spanDeg = 20.0;
                } else {
                    wholeWorld(mf.view, mf.w, mf.h, false);
                    mf.view.fitted = false;
                }
                mf.placed = true;
            }
            if (!mf.view.fitted && !mine.empty()) { (void)fitToTargets(mf.view, mine, mf.w, mf.h, &presentOf); }
            foxsdr::slintkit::BasemapClient& bm = self.kit_.basemap();
            const bool tiles = bm.active();
            if (tiles != mf.sourceSet || (tiles && bm.attribution() != mf.attribution)) {
                mf.sourceSet = tiles;
                mf.attribution = tiles ? bm.attribution() : std::string();
                if (tiles) {
                    mf.comp.setTileSource([&bm](std::uint32_t z, std::uint32_t x, std::uint32_t y, std::vector<std::uint8_t>& rgb) { return static_cast<int>(bm.rawTile(z, x, y, rgb)); }, bm.tileSize(), bm.minZoom(),
                                          bm.maxZoom());
                } else {
                    mf.comp.setTileSource(foxsdr::slintmap::TileSource{}, 256, 0, 0);
                }
                mf.comp.setMercator(tiles);
            }
            const foxsdr::slintmap::MapLook& look = self.pages_.mapLook();
            mf.comp.setSize(mf.w, mf.h);
            mf.comp.setView(mf.view.centreLat, mf.view.centreLon, mf.view.spanDeg);
            mf.comp.setLayers(!tiles, true);
            mf.comp.setTrailLook(look.trailWidthPx, look.iconPx, look.trailStyle);
            mf.comp.setHome(home, state.rxLatDeg, state.rxLonDeg);
            auto& buf = mf.buffers[static_cast<std::size_t>(mf.which)];
            if (buf.width() != static_cast<std::uint32_t>(mf.w) || buf.height() != static_cast<std::uint32_t>(mf.h)) { buf = slint::SharedPixelBuffer<slint::Rgba8Pixel>(static_cast<std::uint32_t>(mf.w), static_cast<std::uint32_t>(mf.h)); }
            bool changed = false;
            if (mf.comp.compose(reinterpret_cast<std::uint8_t*>(buf.begin()), mf.w, mf.h)) {
                mf.which ^= 1;
                auto& other = mf.buffers[static_cast<std::size_t>(mf.which)];
                if (other.width() != buf.width() || other.height() != buf.height()) { other = slint::SharedPixelBuffer<slint::Rgba8Pixel>(buf.width(), buf.height()); }
                std::memcpy(other.begin(), buf.begin(), sizeof(slint::Rgba8Pixel) * buf.width() * buf.height());
                mf.base = slint::Image(mf.buffers[static_cast<std::size_t>(mf.which ^ 1)]);
                changed = true;
            }
            const std::vector<MapFaceTarget> now = projectTargets(mine, mf.comp, look, &presentOf);
            std::string was, is;
            for (const MapFaceTarget& t : mf.targets) { was += t.id + std::to_string(t.x) + std::to_string(t.y) + std::to_string(t.alpha) + ";"; }
            for (const MapFaceTarget& t : now) { is += t.id + std::to_string(t.x) + std::to_string(t.y) + std::to_string(t.alpha) + ";"; }
            if (was != is) { changed = true; }
            mf.targets = now;
            mf.targetIds.clear();
            for (const MapFaceTarget& t : now) { mf.targetIds.push_back(t.id); }
            // the scale bar, the map pages' own steps
            std::string scale;
            float frac = 0.0f;
            const double mpp = mf.comp.metresPerPixel();
            if (mpp > 0.0) {
                static const double steps[] = {100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000, 2000000, 5000000};
                double pick = steps[0];
                for (const double st : steps) { if (st / mpp <= 0.3 * mf.w) { pick = st; } }
                frac = static_cast<float>(pick / mpp / mf.w);
                char b[32];
                std::snprintf(b, sizeof(b), pick >= 1000.0 ? "%.0f km" : "%.0f m", pick >= 1000.0 ? pick / 1000.0 : pick);
                scale = b;
            }
            const bool zi = mf.view.spanDeg > foxsdr::slintmap::TileCompositor::kMinSpanDeg, zo = mf.view.spanDeg < foxsdr::slintmap::TileCompositor::kMaxSpanDeg;
            if (scale != mf.scaleText || frac != mf.scaleFrac || zi != mf.zoomIn || zo != mf.zoomOut) { changed = true; }
            mf.scaleText = scale;
            mf.scaleFrac = frac;
            mf.zoomIn = zi;
            mf.zoomOut = zo;
            if (changed) { ++mf.revision; }
        }
    }

    // What a node's FaceLive row says, as a signature (so a row is set only when it moved).
    std::string liveSigOf(const CanvasNode& n) {
        Sig g;
        if (n.kind == static_cast<int>(pc::NodeKind::Display)) {
            const auto it = specs.find(static_cast<pc::NodeId>(n.id));
            if (it == specs.end()) { return std::string(); }
            g << "S" << it->second.live << static_cast<long long>(it->second.revision);
            for (const SpectrumMarker& m : it->second.markers) { g << m.pos << m.label; }
        } else if (n.kind == static_cast<int>(pc::NodeKind::Map)) {
            const auto it = maps.find(static_cast<pc::NodeId>(n.id));
            if (it == maps.end()) { return std::string(); }
            g << "M" << static_cast<long long>(it->second->revision) << it->second->hover << it->second->attribution;
        }
        return g.s;
    }
    ::FaceLive makeLive(const CanvasNode& n) {
        ::FaceLive o{};
        std::vector<::FaceMarker> markers;
        std::vector<::MapTarget> targets;
        if (n.kind == static_cast<int>(pc::NodeKind::Display)) {
            const auto it = specs.find(static_cast<pc::NodeId>(n.id));
            if (it != specs.end()) {
                o.have_spectrum = it->second.live;
                if (it->second.live) {
                    o.trace = it->second.trace;
                    o.waterfall = it->second.waterfall;
                    for (const SpectrumMarker& m : it->second.markers) { markers.push_back(::FaceMarker{m.pos, ss(m.label)}); }
                }
            }
        } else if (n.kind == static_cast<int>(pc::NodeKind::Map)) {
            const auto it = maps.find(static_cast<pc::NodeId>(n.id));
            o.have_map = it != maps.end();
            if (it != maps.end()) {
                const MapFace& mf = *it->second;
                o.map_base = mf.base;
                for (const MapFaceTarget& t : mf.targets) {
                    ::MapTarget mt{};
                    mt.id = ss(t.id);
                    mt.x = t.x;
                    mt.y = t.y;
                    mt.heading = t.heading;
                    mt.kind = t.kind;
                    mt.label = ss(t.label);
                    mt.alpha = t.alpha;
                    mt.estimated = t.estimated;
                    mt.emergency = t.emergency;
                    mt.color = kindColour(static_cast<std::uint32_t>(t.kind));
                    mt.size = t.size;
                    targets.push_back(std::move(mt));
                }
                o.map_scale_text = ss(mf.scaleText);
                o.map_scale_frac = mf.scaleFrac;
                o.map_attribution = ss(mf.sourceSet ? mf.attribution : std::string(tr("Coastline: Natural Earth (public domain)")));
                o.map_zoom_in = mf.zoomIn;
                o.map_zoom_out = mf.zoomOut;
                o.map_hover = ss(mf.hover);
            }
        }
        o.markers = model(std::move(markers));
        o.map_targets = model(std::move(targets));
        return o;
    }

    std::shared_ptr<slint::Model<::SettingField>> settingsModel(const InspectorDecoder& d, std::string& sig) {
        std::vector<::SettingField> fields;
        for (const foxsdr::slintkit::SettingSpec& s : d.settings) {
            ::SettingField f{};
            f.id = static_cast<int>(s.id);
            f.key = ss(s.key);
            f.label = ss(s.label);
            f.kind = s.kind == CASCADE_SETTING_NUMBER ? ::SettingKind::Number : s.kind == CASCADE_SETTING_BOOL ? ::SettingKind::Bool : s.kind == CASCADE_SETTING_PATH ? ::SettingKind::Path
                   : s.kind == 4u ? ::SettingKind::Password : ::SettingKind::Text;
            const std::string shown = self.kit_.edits().shown(d.settings_module, s.key, s.value);
            f.value = ss(shown);
            f.checked = shown == "1";
            f.placeholder = ss(s.placeholder);
            f.max_length = static_cast<int>(s.maxLength);
            f.error = ss(self.kit_.edits().error(d.settings_module, s.key));
            f.pending = self.kit_.edits().pending(d.settings_module, s.key);
            {   // screen lane M, row 18: named choices, as the plugin pages draw them
                std::vector<slint::SharedString> values, labels;
                for (const auto& ch : foxsdr::slintkit::parseChoices(s.choices)) { values.push_back(ss(ch.first)); labels.push_back(ss(ch.second)); }
                f.choice_values = std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(values));
                f.choice_labels = std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(labels));
                sig += "c" + s.choices + ";";
            }
            sig += s.key + "=" + shown + (f.pending ? "?" : "") + std::string(f.error) + ";";
            fields.push_back(std::move(f));
        }
        auto& cached = fieldModels[d.settings_module];
        if (!cached || cached->row_count() != fields.size()) {
            cached = std::make_shared<slint::VectorModel<::SettingField>>(std::move(fields));
            return cached;
        }
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const ::SettingField& now = fields[i];
            const auto wasRow = cached->row_data(i);
            if (!wasRow) { cached->set_row_data(i, now); continue; }
            const ::SettingField was = *wasRow;
            const auto choicesOf = [](const ::SettingField& x) { std::string c; if (x.choice_values) { for (std::size_t j = 0; j < x.choice_values->row_count(); ++j) { c += std::string(*x.choice_values->row_data(j)) + ""; } } return c; };
            if (was.id != now.id || was.value != now.value || was.error != now.error || was.pending != now.pending || was.checked != now.checked || was.label != now.label ||
                was.kind != now.kind || was.placeholder != now.placeholder || was.max_length != now.max_length || choicesOf(was) != choicesOf(now)) {
                cached->set_row_data(i, now);
            }
        }
        return cached;
    }

    // ---- the models, set when their content moved ----
    template <class Row, class Make>
    static void syncRows(const std::shared_ptr<slint::VectorModel<Row>>& m, std::vector<std::string>& sigsOf, const std::vector<std::string>& now, const Make& make) {
        for (std::size_t i = 0; i < now.size(); ++i) {
            if (i < sigsOf.size()) {
                if (sigsOf[i] != now[i]) {
                    m->set_row_data(i, make(i));
                    sigsOf[i] = now[i];
                }
            } else {
                m->push_back(make(i));
                sigsOf.push_back(now[i]);
            }
        }
        while (m->row_count() > now.size()) { m->erase(m->row_count() - 1u); }
        sigsOf.resize(now.size());
    }

    void publishBand() {
        const PatchModel m = page.bandModel();
        if (!sigs.changed("band", sigBand(m))) { return; }
        self.ui_.set_patch_model(mapBand(m));
        haveBand = true;
    }

    void publishCanvas() {
        const CanvasModel m = page.canvasModel();
        std::vector<std::string> nodeNow, wireNow;
        nodeNow.reserve(m.nodes.size());
        for (const CanvasNode& n : m.nodes) { nodeNow.push_back(sigNode(n)); }
        for (const CanvasWire& w : m.wires) { wireNow.push_back(w.path + "|" + std::to_string(w.tone) + (w.selected ? "s" : "")); }
        syncRows(nodes, nodeSigs, nodeNow, [&](std::size_t i) { return mapNode(m.nodes[i], pictureOf(static_cast<pc::NodeId>(m.nodes[i].id), m.nodes[i].face.picture_id, m.nodes[i].face.have_picture)); });
        syncRows(wires, wireSigs, wireNow, [&](std::size_t i) { return ::CanvasWire{ss(m.wires[i].path), m.wires[i].tone, m.wires[i].selected}; });
        std::vector<std::string> liveNow;
        liveNow.reserve(m.nodes.size());
        for (const CanvasNode& n : m.nodes) { liveNow.push_back(liveSigOf(n)); }
        syncRows(lives, liveSigs, liveNow, [&](std::size_t i) { return makeLive(m.nodes[i]); });
        Sig g;
        g << m.zoom << m.faces_live << m.grid_step << m.grid_ox << m.grid_oy << m.ghost << m.refusal.shown << m.refusal.text << m.refusal.x << m.refusal.y << m.keys_shown;
        for (const std::string& s : nodeNow) { g << s.size(); }
        if (!sigs.changed("canvas", g.s)) { return; }
        ::CanvasModel o{};
        o.zoom = m.zoom;
        o.faces_live = m.faces_live;
        o.grid_step = m.grid_step;
        o.grid_ox = m.grid_ox;
        o.grid_oy = m.grid_oy;
        o.nodes = nodes;
        o.wires = wires;
        o.lives = lives;
        o.ghost = ss(m.ghost);
        o.refusal = ::Refusal{m.refusal.shown, ss(m.refusal.text), m.refusal.x, m.refusal.y};
        o.keys_shown = m.keys_shown;
        self.ui_.set_canvas_model(o);
    }

    void publishInspector() {
        const InspectorModel m = page.inspectorModel();
        settingsFile = m.decoder.has_settings ? m.decoder.settings_module : std::string();
        settingsSpecs = m.decoder.has_settings ? m.decoder.settings : std::vector<foxsdr::slintkit::SettingSpec>{};
        std::string settingsSig;
        std::shared_ptr<slint::Model<::SettingField>> fields = m.decoder.has_settings ? settingsModel(m.decoder, settingsSig) : model(std::vector<::SettingField>{});
        if (!sigs.changed("inspector", sigInspector(m, settingsSig))) { return; }
        ::InspectorModel o{};
        o.shown = m.shown;
        o.folded = m.folded;
        o.node = m.node;
        o.kind = m.kind;
        o.kind_caption = ss(m.kind_caption);
        o.name = ss(m.name);
        o.freq_text = ss(m.freq_text);
        o.mode_index = m.mode_index;
        o.squelch_on = m.squelch_on;
        o.squelch_norm = m.squelch_norm;
        o.squelch_text = ss(m.squelch_text);
        o.gate_shown = m.gate_shown;
        o.gate_text = ss(m.gate_text);
        o.gate_word = ss(m.gate_word);
        o.gate_lit = m.gate_lit;
        const InspectorRadio& r = m.radio;
        o.radio.device_items = model(items(r.device_items));
        o.radio.device_index = r.device_index;
        o.radio.device_label = ss(r.device_label);
        o.radio.looking = r.looking;
        o.radio.look_enabled = r.look_enabled;
        o.radio.centre_text = ss(r.centre_text);
        o.radio.centre_note = ss(r.centre_note);
        o.radio.rate_items = model(items(r.rate_items));
        o.radio.rate_index = r.rate_index;
        o.radio.rate_label = ss(r.rate_label);
        o.radio.rate_fixed = r.rate_fixed;
        o.radio.rate_fixed_text = ss(r.rate_fixed_text);
        o.radio.lines = model(faceLines(r.lines));
        const InspectorDecoder& d = m.decoder;
        o.decoder.plugin_items = model(items(d.plugin_items));
        o.decoder.plugin_index = d.plugin_index;
        o.decoder.plugin_label = ss(d.plugin_label);
        o.decoder.feed_note = ss(d.feed_note);
        o.decoder.fed = model(strings(d.fed));
        o.decoder.shown_note = ss(d.shown_note);
        o.decoder.settings = fields;
        o.decoder.has_settings = d.has_settings;
        o.map_wired = ss(m.map_wired);
        std::vector<::InspectorMapSource> sources;
        for (const InspectorMapSource& s : m.map_sources) { sources.push_back(::InspectorMapSource{ss(s.count), ss(s.name)}); }
        o.map_sources = model(std::move(sources));
        o.map_help = ss(m.map_help);
        o.sink.items = model(items(m.sink.items));
        o.sink.index = m.sink.index;
        o.sink.label = ss(m.sink.label);
        o.sink.lines = model(faceLines(m.sink.lines));
        o.sink.files_note = ss(m.sink.files_note);
        o.audio_sink = m.audio_sink;
        o.help = ss(m.help);
        self.ui_.set_inspector_model(o);
    }

    void publishPresets() {
        const PatchPresetsModel m = page.presetsModel();
        if (!sigs.changed("presets", sigPresets(m))) { return; }
        self.pages_.setPatchPresetsModel(mapPresets(m));
    }

    // ---- the self-capture of the pointer (FOXSDR_SLINT_PATCH_PROBE=1): the same callbacks the window's mouse fires ----
    const CanvasNode* nodeOfKind(const CanvasModel& m, int kind) const {
        for (const CanvasNode& n : m.nodes) {
            if (n.kind == kind) { return &n; }
        }
        return nullptr;
    }
    bool idle() const { return page.draft().send().state == PatchDraft::Send::State::Idle; }
    void wireNodes(int fromKind, int toKind) {
        auto&& ev = self.ui_.global<::PatchEvents>();
        const CanvasModel m = page.canvasModel();
        const CanvasNode* a = nodeOfKind(m, fromKind);
        const CanvasNode* b = nodeOfKind(m, toKind);
        if (a == nullptr || b == nullptr || a->outputs.empty() || b->inputs.empty()) {
            say("probe: no ports to wire kind " + std::to_string(fromKind) + " -> " + std::to_string(toKind));
            return;
        }
        const PortDot from = a->outputs.front(), to = b->inputs.front();
        say("probe: wire kind " + std::to_string(fromKind) + " -> " + std::to_string(toKind) + " from (" + std::to_string(static_cast<int>(from.x)) + "," + std::to_string(static_cast<int>(from.y)) +
            ") to (" + std::to_string(static_cast<int>(to.x)) + "," + std::to_string(static_cast<int>(to.y)) + ")");
        ev.invoke_canvas_moved(from.x, from.y);
        ev.invoke_canvas_pressed(from.x, from.y, 0);
        ev.invoke_canvas_moved((from.x + to.x) / 2.0f, (from.y + to.y) / 2.0f);
        ev.invoke_canvas_moved(to.x, to.y);
        ev.invoke_canvas_released(to.x, to.y, 0);
    }
    void runProbe() {
        Probe& p = probe;
        if (p.done) { return; }
        if (p.wait > 0) { --p.wait; return; }
        auto&& ev = self.ui_.global<::PatchEvents>();
        const auto ready = [&] { return page.seeded() && page.draft().inStep() && idle() && page.canvas().canvas_w > 0.0f; };
        if (!ready()) {
            if (++p.waited > kProbeWaitMax) {
                say("probe: gave up at step " + std::to_string(p.step) + " (seeded=" + std::to_string(page.seeded() ? 1 : 0) + " in step=" + std::to_string(page.draft().inStep() ? 1 : 0) +
                    " idle=" + std::to_string(idle() ? 1 : 0) + " canvas=" + std::to_string(static_cast<int>(page.canvas().canvas_w)) + "x" + std::to_string(static_cast<int>(page.canvas().canvas_h)) + ")");
                p.done = true;
            }
            return;
        }
        p.waited = 0;
        switch (p.step) {
            case 0: say("probe: the Channel part key"); ev.invoke_part_pressed(1); break;
            case 1: say("probe: the Demod part key"); ev.invoke_part_pressed(2); break;
            case 2: wireNodes(0, 1); break;
            case 3: wireNodes(1, 2); break;
            case 4: {
                const CanvasModel m = page.canvasModel();
                if (const CanvasNode* n = nodeOfKind(m, 1)) {
                    say("probe: select the Channel by its header");
                    ev.invoke_canvas_moved(n->x + 20.0f, n->y + 8.0f);
                    ev.invoke_canvas_pressed(n->x + 20.0f, n->y + 8.0f, 0);
                    ev.invoke_canvas_released(n->x + 20.0f, n->y + 8.0f, 0);
                }
                break;
            }
            case 5:
                say("probe: save the preset \"Probe\"");
                ev.invoke_presets_save_as();
                ev.invoke_presets_name(ss("Probe"));
                ev.invoke_presets_go();
                break;
            default:
                p.done = true;
                say("probe: done");
                return;
        }
        ++p.step;
        p.wait = kProbeGapTicks;
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------
PatchPublisher::PatchPublisher(MainWindow& ui, foxsdr::slintkit::PluginKit& kit, foxsdr::slintkit::KitPublisher& pages, foxsdr::slinthost::HostDuties& duties, const FoxEngineApi* api,
                               FoxSession* session, bool bounded)
    : ui_(ui), kit_(kit), pages_(pages), duties_(duties), api_(api), session_(session), bounded_(bounded) {
    impl_ = std::make_unique<Impl>(*this, duties);
}

PatchPublisher::~PatchPublisher() = default;

void PatchPublisher::setViewShown(bool shown) { impl_->page.setViewShown(shown); }

void PatchPublisher::onAnswer(const FoxCommandResult& r) {
    Impl& m = *impl_;
    m.page.onAnswer(r);
    if (r.op != FOXAPP_OP_PATCH_SET_GRAPH && r.op != FOXAPP_OP_SET_PATCH_PAGE_OPEN && r.op != FOXAPP_OP_SCAN_NATIVE_ONLY) { return; }
    ++answered_;
    char line[200];
    std::snprintf(line, sizeof(line), "op 0x%04x ticket %llu status %d applied [%g, %g]%s%s", r.op, static_cast<unsigned long long>(r.ticket), r.status, r.applied[0], r.applied[1],
                  r.message[0] != '\0' ? " - " : "", r.message);
    m.answers.push_back(line);
    if (m.answers.size() > kAnswerLog) { m.answers.erase(m.answers.begin()); }
    if (bounded_) { m.say(std::string("answer: ") + line); }
}

void PatchPublisher::publish(const FoxReceiverState& state) {
    Impl& m = *impl_;
    ++m.tickNo;
    m.flags = state.flags;
    const bool showing = ui_.get_view_index() == 1;
    using View = foxsdr::slintkit::KitPublisher::PatchPresetsView;
    const View presets = pages_.patchPresetsView();
    m.page.setViewShown(showing);
    m.page.setPresetsOpen(presets != View::Closed);
    // The Map face's target counts read the kit's tracks - copied only while a Map node exists (a busy ADS-B feed is thousands of rows).
    if (showing && m.page.draft().graph().count(pc::NodeKind::Map) > 0) {
        std::vector<MapTrack> tracks;
        tracks.reserve(kit_.tracks().size());
        for (const cc::HostTrack& t : kit_.tracks()) { tracks.push_back(MapTrack{t.plugin, t.t.kind}); }
        m.page.setTracks(std::move(tracks));
    }
    m.page.tick(m.table, state, m.sub, showing);
    if (showing) {
        m.readPictures();
        m.dropGonePictures();
        m.readSpectra();
        m.tickMaps(state);
        if (bounded_ && probe_) { m.runProbe(); }
        m.publishBand();
        m.publishCanvas();
        m.publishInspector();
    }
    if (presets != View::Closed) { m.publishPresets(); }
}

// A BOUNDED RUN'S READ-BACK OF THE FACES (main.cpp: `state patchface.<s|m><k>.<what>`; s = the k-th Spectrum part, m = the k-th Map part, in the draft's order). Every figure is
// measured from the pictures and the view the faces were painted from, so a drive script reads what the eye would.
std::string PatchPublisher::faceState(const std::string& name, std::string& out) const {
    const Impl& m = *impl_;
    if (name.size() < 4 || (name[0] != 's' && name[0] != 'm')) { return "patchface.<s|m><k>.<what>, not patchface." + name; }
    const std::size_t dot = name.find('.');
    if (dot == std::string::npos || dot < 2) { return "patchface.<s|m><k>.<what>, not patchface." + name; }
    const bool spectrum = name[0] == 's';
    const std::size_t k = static_cast<std::size_t>(std::atoi(name.substr(1, dot - 1).c_str()));
    const std::string what = name.substr(dot + 1);
    const pc::Graph& g = m.page.draft().graph();
    const pc::NodeKind want = spectrum ? pc::NodeKind::Display : pc::NodeKind::Map;
    const pc::Node* node = nullptr;
    std::size_t seen = 0;
    for (const pc::Node& n : g.nodes()) {
        if (n.kind != want) { continue; }
        if (seen++ == k) { node = &n; break; }
    }
    if (node == nullptr) { return "no " + std::string(spectrum ? "Spectrum" : "Map") + " part number " + std::to_string(k); }
    const auto num = [](double v) { char b[48]; std::snprintf(b, sizeof(b), "%.4f", v); return std::string(b); };
    if (what == "id") { out = std::to_string(static_cast<long long>(node->id)); return std::string(); }
    if (what == "sentence") {
        const CanvasModel cm = m.page.canvasModel();
        out.clear();
        for (const CanvasNode& cn : cm.nodes) {
            if (cn.id != static_cast<int>(node->id)) { continue; }
            for (const FaceLine& l : cn.face.lines) { out += (out.empty() ? "" : " | ") + l.text; }
        }
        return std::string();
    }
    if (spectrum) {
        const auto it = m.specs.find(node->id);
        if (it == m.specs.end()) { return "that Spectrum part has not been looked at yet (the view is not showing?)"; }
        const Impl::SpecFace& sf = it->second;
        const SpectrumWell& w = sf.well;
        if (what == "moved") {   // 1 when the two pictures differ from what the last read of this figure saw (the first read sees an empty well)
            char b[40];
            std::snprintf(b, sizeof(b), "%016llx%016llx", static_cast<unsigned long long>(w.traceHash()), static_cast<unsigned long long>(w.waterfallHash()));
            std::string& last = m.lastLook["s" + std::to_string(k)];
            out = last != b ? "1" : "0";
            last = b;
        }
        else if (what == "framesdelta") {   // the frames the well has taken since the last read of this figure
            std::uint64_t& last = m.lastFrames["s" + std::to_string(k)];
            out = std::to_string(w.frames() >= last ? w.frames() - last : w.frames());
            last = w.frames();
        }
        else if (what == "live") { out = sf.live ? "1" : "0"; }
        else if (what == "wired") { out = sf.slice.wired ? "1" : "0"; }
        else if (what == "channel") { out = sf.slice.channel ? "1" : "0"; }
        else if (what == "frames") { out = std::to_string(w.frames()); }
        else if (what == "span") { out = num(sf.slice.hi_hz - sf.slice.lo_hz); }
        else if (what == "lo") { out = num(sf.slice.lo_hz); }
        else if (what == "hi") { out = num(sf.slice.hi_hz); }
        else if (what == "rate") { out = num(sf.slice.rate_hz); }
        else if (what == "width") { out = std::to_string(w.size().w); }
        else if (what == "traceh") { out = std::to_string(w.size().traceH); }
        else if (what == "wfh") { out = std::to_string(w.size().wfH); }
        else if (what == "peakcol") { out = std::to_string(w.peakColumn()); }
        else if (what == "peakfrac") { out = num(w.size().w > 0 && w.peakColumn() >= 0 ? (static_cast<double>(w.peakColumn()) + 0.5) / static_cast<double>(w.size().w) : -1.0); }
        else if (what == "peakdb") { out = num(w.peakDb()); }
        else if (what == "floordb") { out = num(w.floorDb()); }
        else if (what == "abovefloor") { out = num(static_cast<double>(w.peakDb()) - static_cast<double>(w.floorDb())); }
        else if (what == "rows") { out = std::to_string(w.litWaterfallRows()); }
        else if (what == "thash") { char b[24]; std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(w.traceHash())); out = b; }
        else if (what == "whash") { char b[24]; std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(w.waterfallHash())); out = b; }
        else if (what == "markers") { out = std::to_string(sf.markers.size()); }
        else if (what == "markernames") { out.clear(); for (const SpectrumMarker& mk : sf.markers) { out += (out.empty() ? "" : " | ") + mk.label; } }
        else if (what == "revision") { out = std::to_string(sf.revision); }
        else { return "patchface.s<k>.<id|sentence|moved|framesdelta|live|wired|channel|frames|span|lo|hi|rate|width|traceh|wfh|peakcol|peakfrac|peakdb|floordb|abovefloor|rows|thash|whash|markers|markernames|revision>, not " + name; }
        return std::string();
    }
    const auto it = m.maps.find(node->id);
    if (it == m.maps.end()) { return "that Map part has not been looked at yet (the view is not showing?)"; }
    const Impl::MapFace& mf = *it->second;
    if (what == "moved") {   // 1 when the chart or its targets differ from what the last read of this figure saw
        std::string look = std::to_string(mf.revision);
        std::string& last = m.lastLook["m" + std::to_string(k)];
        out = last != look ? "1" : "0";
        last = look;
    }
    else if (what == "targets") { out = std::to_string(mf.targets.size()); }
    else if (what == "sources") { out = std::to_string(mf.sources); }
    else if (what == "span") { out = num(mf.view.spanDeg); }
    else if (what == "lat") { out = num(mf.view.centreLat); }
    else if (what == "lon") { out = num(mf.view.centreLon); }
    else if (what == "fitted") { out = mf.view.fitted ? "1" : "0"; }
    else if (what == "width") { out = std::to_string(mf.w); }
    else if (what == "height") { out = std::to_string(mf.h); }
    else if (what == "revision") { out = std::to_string(mf.revision); }
    else if (what == "hover") { out = mf.hover; }
    else if (what == "scale") { out = mf.scaleText; }
    else if (what == "ids") { out.clear(); for (const std::string& id : mf.targetIds) { out += (out.empty() ? "" : " | ") + id; } }
    else if (what == "xs") { out.clear(); for (const MapFaceTarget& t : mf.targets) { out += (out.empty() ? "" : " | ") + num(t.x); } }
    else if (what == "ys") { out.clear(); for (const MapFaceTarget& t : mf.targets) { out += (out.empty() ? "" : " | ") + num(t.y); } }
    else if (what == "basehash") {   // the composed picture's fingerprint (the pixels the pane was given)
        std::uint64_t h = 1469598103934665603ull;
        const auto& buf = mf.buffers[static_cast<std::size_t>(mf.which ^ 1)];
        const slint::Rgba8Pixel* px = buf.begin();
        const std::size_t n = static_cast<std::size_t>(buf.width()) * buf.height();
        for (std::size_t i = 0; i < n; ++i) {
            h = (h ^ px[i].r) * 1099511628211ull;
            h = (h ^ px[i].g) * 1099511628211ull;
            h = (h ^ px[i].b) * 1099511628211ull;
        }
        char b[24];
        std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(h));
        out = b;
    }
    else { return "patchface.m<k>.<id|sentence|moved|targets|sources|span|lat|lon|fitted|width|height|revision|hover|scale|ids|xs|ys|basehash>, not " + name; }
    return std::string();
}

std::string PatchPublisher::summaryLine() const {
    const Impl& m = *impl_;
    const pc::Graph& g = m.page.draft().graph();
    const InfoModel info = m.page.infoModel();
    using View = foxsdr::slintkit::KitPublisher::PatchPresetsView;
    const View v = pages_.patchPresetsView();
    std::string s = "patch: nodes=" + std::to_string(g.nodes().size()) + " wires=" + std::to_string(g.wires().size()) + " running=" + ((m.flags & FOXAPI_RX_PATCH_RUNNING) ? "1" : "0") +
                    " verdict=\"" + info.verdict + "\" seeded=" + (m.page.seeded() ? "1" : "0") + " version=" + std::to_string(m.page.draft().version()) + " epoch=" +
                    std::to_string(m.page.draft().epoch()) + " selected=" + std::to_string(static_cast<long long>(m.page.selected())) + " presets page=" +
                    (v == View::Closed ? "closed" : v == View::Docked ? "docked" : "shown") + " presets=" + std::to_string(m.page.presetsModel().rows.size()) +
                    " view shown=" + (ui_.get_view_index() == 1 ? "1" : "0");
    if (probe_) { s += std::string(" probe=") + (m.probe.done ? "done" : "unfinished") + " step=" + std::to_string(m.probe.step); }
    s += "; commands sent=" + std::to_string(sent_) + " withheld=" + std::to_string(withheld_) + " answers=" + std::to_string(answered_);
    if (!m.answers.empty()) {
        s += " [";
        for (std::size_t i = 0; i < m.answers.size(); ++i) { s += (i ? "; " : "") + m.answers[i]; }
        s += "]";
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------------------
void PatchPublisher::wire() {
    Impl* m = impl_.get();
    auto&& ev = ui_.global<::PatchEvents>();
    PatchPage& page = m->page;

    // ---- the band ----
    ev.on_run_clicked([m] { (void)m->send(m->page.runClicked()); });
    ev.on_all_off_clicked([m] { (void)m->send(m->page.allOffClicked()); });
    ev.on_presets_clicked([this] {
        using View = foxsdr::slintkit::KitPublisher::PatchPresetsView;
        if (pages_.patchPresetsView() != View::Closed) { pages_.closePatchPresetsPage(); } else { pages_.openPatchPresetsPage(); }
    });
    ev.on_part_pressed([m](int kind) { m->page.partPressed(kind, m->sub); });
    ev.on_decoder_pressed([m](int index) { m->page.decoderPressed(index, m->sub); });
    ev.on_blueprints_opened([m] { m->page.blueprintsOpened(m->table); });
    ev.on_blueprint_picked([m](int index) { m->page.blueprintPicked(index, m->sub); });

    // ---- the canvas ----
    ev.on_canvas_size([&page](float w, float h) { page.setCanvasSize(w, h); });
    ev.on_canvas_pressed([&page](float x, float y, int button) { page.canvasPressed(x, y, button); });
    ev.on_canvas_moved([&page](float x, float y) { page.canvasMoved(x, y); });
    ev.on_canvas_released([&page](float x, float y, int button) { page.canvasReleased(x, y, button); });
    ev.on_canvas_left([&page] { page.canvasLeft(); });
    ev.on_canvas_wheel([&page](float x, float y, float nx, float ny, bool ctrl, bool shift) { page.canvasWheel(x, y, nx, ny, ctrl, shift); });
    ev.on_zoom_step([&page](bool in) { page.zoomStep(in); });
    ev.on_fit([&page] { page.fit(); });
    ev.on_canvas_delete([&page] { page.canvasDelete(); });

    // ---- the faces ----
    ev.on_face_switch([&page](int node) { page.faceSwitch(node); });
    ev.on_face_centre([&page](int node, slint::SharedString t) { page.faceCentre(node, std::string(t)); });
    ev.on_face_freq([&page](int node, slint::SharedString t) { page.faceFreq(node, std::string(t)); });
    ev.on_face_mode([&page](int node, int index) { page.faceMode(node, index); });
    ev.on_face_squelch_on([&page](int node, bool on) { page.faceSquelchOn(node, on); });
    ev.on_face_squelch([&page](int node, float norm) { page.faceSquelch(node, norm); });
    // the Map face (docs/multi-radio-records/patch-faces.md): the view moves as the map pages' does
    ev.on_face_map_resized([m](int node, float w, float h) {
        if (Impl::MapFace* mf = m->mapFor(node, true)) {
            mf->w = static_cast<int>(std::clamp(w, 0.0f, 2400.0f));
            mf->h = static_cast<int>(std::clamp(h, 0.0f, 2400.0f));
        }
    });
    ev.on_face_map_panned([m](int node, float dx, float dy) {
        if (Impl::MapFace* mf = m->mapFor(node, false)) { panView(mf->view, mf->comp, dx, dy); }
    });
    ev.on_face_map_zoomed([m](int node, int notches, float fx, float fy) {
        if (Impl::MapFace* mf = m->mapFor(node, false)) { zoomView(mf->view, mf->comp, std::pow(0.85, notches), fx, fy); }
    });
    ev.on_face_map_zoom_key([m](int node, int dir) {
        if (Impl::MapFace* mf = m->mapFor(node, false)) { zoomView(mf->view, mf->comp, dir > 0 ? 0.5 : 2.0, 0.5f, 0.5f); }
    });
    ev.on_face_map_hovered([m](int node, int index) {
        Impl::MapFace* mf = m->mapFor(node, false);
        if (mf == nullptr) { return; }
        mf->hover.clear();
        if (index >= 0 && index < static_cast<int>(mf->targetIds.size())) {
            for (const cc::HostTrack& t : m->tracksOf(node)) {
                if (mf->targetIds[static_cast<std::size_t>(index)] == t.t.id) {
                    mf->hover = Impl::hoverTextOf(t);
                    break;
                }
            }
        }
        ++mf->revision;
    });
    ev.on_inspector_map_fit([m] {
        Impl::MapFace* mf = m->mapFor(m->page.selected(), false);
        if (mf != nullptr && mf->w > 0 && mf->h > 0) { (void)fitToTargets(mf->view, m->tracksOf(m->page.selected()), mf->w, mf->h, &Impl::presentOf); }
    });
    ev.on_inspector_map_whole([m] {
        Impl::MapFace* mf = m->mapFor(m->page.selected(), false);
        if (mf != nullptr) { wholeWorld(mf->view, mf->w, mf->h, mf->sourceSet); }
    });

    // ---- the inspector ----
    ev.on_inspector_fold([&page] { page.inspectorFold(); });
    ev.on_inspector_name([&page](slint::SharedString t) { page.inspectorName(std::string(t)); });
    ev.on_inspector_device_opened([m] { m->sendAll(m->page.inspectorDeviceOpened()); });
    ev.on_inspector_device([&page](int index) { page.inspectorDevice(index); });
    ev.on_inspector_look([m] { m->sendAll(m->page.inspectorLook()); });
    ev.on_inspector_centre([&page](slint::SharedString t) { page.inspectorCentre(std::string(t)); });
    ev.on_inspector_rate([&page](int index) { page.inspectorRate(index); });
    ev.on_inspector_freq([&page](slint::SharedString t) { page.inspectorFreq(std::string(t)); });
    ev.on_inspector_mode([&page](int index) { page.inspectorMode(index); });
    ev.on_inspector_squelch_on([&page](bool on) { page.inspectorSquelchOn(on); });
    ev.on_inspector_squelch([&page](float norm) { page.inspectorSquelch(norm); });
    ev.on_inspector_plugin([&page](int index) { page.inspectorPlugin(index); });
    // The plugin's settings form is the kit's: the edit goes through the kit's own commit for the module the form shows (the answer lands in the kit's edits by its ticket).
    ev.on_inspector_setting([this, m](int id, slint::SharedString t) {
        if (m->settingsFile.empty()) { return; }
        for (const foxsdr::slintkit::SettingSpec& s : m->settingsSpecs) {
            if (static_cast<int>(s.id) == id) {
                kit_.editSetting(m->settingsFile, s, std::string(t));
                return;
            }
        }
    });
    ev.on_inspector_sink([&page](int index) { page.inspectorSink(index); });
    ev.on_inspector_size([&page](int step) { page.inspectorSize(step); });
    ev.on_inspector_remove([&page] { page.inspectorRemove(); });

    // ---- the presets page ----
    ev.on_presets_row([&page](int index) { page.presetsRow(index); });
    ev.on_presets_row_activated([m](int index) { m->page.presetsRowActivated(index, m->sub); });
    ev.on_presets_load([m] { m->page.presetsLoad(m->sub); });
    ev.on_presets_save_as([&page] { page.presetsSaveAs(); });
    ev.on_presets_rename([&page] { page.presetsRename(); });
    ev.on_presets_delete([&page] { page.presetsDelete(); });
    ev.on_presets_name([&page](slint::SharedString t) { page.presetsName(std::string(t)); });
    ev.on_presets_go([&page] { page.presetsGo(); });
    ev.on_presets_cancel([&page] { page.presetsCancel(); });
    ev.on_presets_replace([&page](bool yes) { page.presetsReplace(yes); });
    ev.on_presets_delete_confirm([&page](bool yes) { page.presetsDeleteConfirm(yes); });
}

}  // namespace foxsdr::slintpatch
