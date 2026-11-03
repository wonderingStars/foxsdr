// patch_logic.cpp - the PATCH page's pure logic (patch_logic.hpp): the ImGui window's rules and sentences over the table's lists and facts, with no window and no engine. Every
// sentence is the drawer's own, through cascade::i18n::tr(); a format is composed with formatText / formatUtf8 into a std::string, never into a fixed buffer. Each callback does
// what the ImGui control does: an edit of the draft, or a command, or both (patch_logic.hpp has the table).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "patch_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "core/airband_data.hpp"
#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/mp3_support.hpp"
#include "core/patch_audio_rate.hpp"
#include "core/patch_device_label.hpp"
#include "core/plugin_abi.h"
#include "core/receiver_tables.hpp"
#include "core/tune_control.hpp"
#include "core/utf8_text.hpp"

namespace foxsdr::slintpatch {

using cascade::i18n::tr;
namespace cc = cascade::core;
namespace cmd = cascade::core::cmd;

namespace {

std::string factOf(TableReader& t, const std::string& key) {
    std::string s;
    if (!t.fact(key, s)) { s.clear(); }
    return s;
}

std::int32_t listSize(TableReader& t, std::uint32_t list) { return std::max<std::int32_t>(0, t.readList(list, 0, nullptr)); }

// Every entry of a list, in order; `got <= i` is the end of a list that changed under the read (what was read stands).
template <typename F>
void eachItem(TableReader& t, std::uint32_t list, F&& f) {
    const std::int32_t n = listSize(t, list);
    for (std::int32_t i = 0; i < n; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (t.readList(list, static_cast<std::uint32_t>(i), &it) <= i) { break; }
        f(i, it);
    }
}

Command make(const cmd::QueuedCommand& q) {
    Command c;
    c.q = q;
    c.grant = foxsdr::slintsignal::grantFor(q.c.op);
    c.valid = true;
    return c;
}
Command make(const FoxCommand& f) {
    cmd::QueuedCommand q;
    q.c = f;
    return make(q);
}

std::uint64_t number(const std::string& t) { return static_cast<std::uint64_t>(std::strtoull(t.c_str(), nullptr, 10)); }



bool endsWith(const std::string& s, const std::string& suffix) { return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0; }

}  // namespace

// =================================================================================================================================
// THE READS
// =================================================================================================================================

void readLists(TableReader& table, std::uint64_t listSeq, PatchLists& c) {
    if (c.list_seq == listSeq) { return; }
    c.list_seq = listSeq;

    // THE PARTS BIN: a picture decoder is the key's suffix (kImageKeySuffix); the rest is the entry's.
    c.parts.clear();
    eachItem(table, FOXAPI_LIST_PATCH_PARTS, [&](int, const FoxListItem& it) {
        pc::DecoderInfo d;
        d.key = it.detail;
        d.name = it.name;
        d.feed = static_cast<pc::PortType>(static_cast<int>(it.value[0]));
        d.requiredRateHz = it.value[1];
        d.tracks = it.value[2] != 0.0;
        d.image = endsWith(d.key, pc::kImageKeySuffix);
        c.parts.push_back(std::move(d));
    });

    // THE PLAN: a channel is an entry whose detail is "channel"; a decoder's detail says what feeds it.
    c.plan = pc::Plan{};
    eachItem(table, FOXAPI_LIST_PATCH_PLAN, [&](int, const FoxListItem& it) {
        if (std::strcmp(it.detail, "channel") == 0) {
            pc::ChannelPlan ch;
            ch.node = static_cast<pc::NodeId>(it.id);
            ch.offsetHz = it.value[0];
            ch.outRateHz = it.value[1];
            ch.decimation = static_cast<unsigned>(it.value[2]);
            ch.radio = static_cast<pc::NodeId>(it.value[3]);
            c.plan.channels.push_back(ch);
        } else {
            pc::DecoderPlan dp;
            dp.node = static_cast<pc::NodeId>(it.id);
            dp.source = std::strcmp(it.detail, "decoder:radio") == 0     ? pc::DecoderSource::Radio
                        : std::strcmp(it.detail, "decoder:channel") == 0 ? pc::DecoderSource::Channel
                                                                         : pc::DecoderSource::Audio;
            dp.rateHz = it.value[0];
            dp.inRateHz = it.value[1];
            dp.centreHz = it.value[2];
            dp.radio = static_cast<pc::NodeId>(it.value[3]);
            c.plan.decoders.push_back(dp);
        }
    });
    // ...its problems (the code is the core's Problem, a decimal; a code from a newer engine is not one this window can word, and is left out) and the verdict.
    eachItem(table, FOXAPI_LIST_PATCH_PROBLEMS, [&](int, const FoxListItem& it) {
        const long code = std::strtol(it.name, nullptr, 10);
        if (code < 0 || code > static_cast<long>(pc::Problem::NoTracks)) { return; }
        c.plan.problems.push_back({static_cast<pc::NodeId>(it.id), static_cast<pc::Problem>(code)});
    });
    c.plan.runnable = factOf(table, "fact.patch.runnable") == "1";

    // THE RADIOS.
    c.radios.clear();
    c.radios_open = 0;
    eachItem(table, FOXAPI_LIST_PATCH_RADIOS, [&](int, const FoxListItem& it) {
        RadioRow r;
        r.device = it.name;
        r.note = it.detail;
        r.rate_hz = it.value[0];
        r.centre_hz = it.value[1];
        r.pending = it.value[2] != 0.0;
        r.open = (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
        r.fault = (it.flags & FOXAPI_ITEM_FAULT) != 0u;
        if (r.open) { ++c.radios_open; }
        c.radios[static_cast<pc::NodeId>(it.id)] = std::move(r);
    });

    // THE NODES the engine refused to run, and each Radio node's converter.
    c.refused.clear();
    c.conv.clear();
    eachItem(table, FOXAPI_LIST_PATCH_NODES, [&](int, const FoxListItem& it) {
        const auto id = static_cast<pc::NodeId>(it.id);
        if ((it.flags & FOXAPI_ITEM_REFUSED) != 0u) { c.refused.push_back(id); }
        if (std::strcmp(it.detail, "radio") != 0) { return; }
        const std::string fact = factOf(table, "fact.patch.conv." + std::to_string(id));
        int mode = 0;
        double lo = 0.0;
        int inverted = 0;
        if (std::sscanf(fact.c_str(), "%d %lf %d", &mode, &lo, &inverted) == 3 && mode >= 0 && mode <= 2) {
            cc::ConverterSetting s;
            s.mode = static_cast<cc::ConverterMode>(mode);
            s.loHz = lo;
            s.inverted = inverted != 0;
            c.conv[id] = s;
        }
    });

    // THE DEVICES A RADIO CAN BE SET TO, in the Device combo's order, and the receiver's own radio.
    c.devices.clear();
    eachItem(table, FOXAPI_LIST_PATCH_DEVICES, [&](int, const FoxListItem& it) {
        DeviceRow d;
        d.label = it.name;
        d.key = it.detail;
        // A key the item cut (159 bytes) is read whole.
        if (std::strlen(it.detail) >= sizeof(it.detail) - 1u) { d.key = factOf(table, "fact.patch.device." + std::to_string(it.id)); }
        d.rate_hz = it.value[0];
        c.devices.push_back(std::move(d));
    });
    c.receiver_radio = factOf(table, "fact.patch.receiverRadio");
    c.receiver_tuned = !factOf(table, "fact.patch.receiverCentre").empty();

    // What the page also reads: the frequency list's groups (the tower blueprints), the output devices, where the recorder writes.
    c.bookmark_groups.clear();
    eachItem(table, FOXAPI_LIST_BOOKMARKS, [&](int, const FoxListItem& it) {
        const std::string group = it.detail;
        if (!group.empty() && std::find(c.bookmark_groups.begin(), c.bookmark_groups.end(), group) == c.bookmark_groups.end()) { c.bookmark_groups.push_back(group); }
    });
    c.audio_devices.clear();
    eachItem(table, FOXAPI_LIST_AUDIO_DEVICES, [&](int, const FoxListItem& it) { c.audio_devices.push_back(it.name); });
    c.recorder_dir = factOf(table, "fact.recorder.dir");
}

std::vector<LiveRow> readLive(TableReader& table) {
    std::vector<LiveRow> rows;
    eachItem(table, FOXAPI_LIST_PATCH_LIVE, [&](int, const FoxListItem& it) {
        LiveRow r;
        r.node = static_cast<pc::NodeId>(it.id);
        r.kind = static_cast<int>(it.value[2]);
        r.v0 = it.value[0];
        r.v1 = it.value[1];
        r.open = (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
        r.name = it.name;
        r.detail = it.detail;
        rows.push_back(std::move(r));
    });
    return rows;
}

const LiveRow* liveRow(const std::vector<LiveRow>& rows, int kind, pc::NodeId node) {
    for (const LiveRow& r : rows) {
        if (r.kind == kind && r.node == node) { return &r; }
    }
    return nullptr;
}

const PatchLook& readLook(TableReader& table, std::uint64_t listSeq, PatchLook& c) {
    if (c.list_seq == listSeq) { return c; }
    c.scratch = factOf(table, "fact.patch.version");
    if (c.scratch.empty()) {
        // No block has been published (the first tick of a window whose session was opened before any pass).
        c.valid = false;
        c.list_seq = listSeq;
        return c;
    }
    const std::uint64_t version = number(c.scratch);
    const std::uint64_t epoch = number(factOf(table, "fact.patch.epoch"));
    if (!c.valid || version != c.version || epoch != c.epoch) {
        c.text = factOf(table, "fact.patch.graph");
        if (number(factOf(table, "fact.patch.version")) != version) {
            c.valid = false;
            c.list_seq = ~std::uint64_t{0};   // a publish between the reads: look again
            return c;
        }
        pc::Graph g;
        std::string why;
        if (!pc::graphFromCommandText(c.text, g, why)) {
            cc::diagWarnf("patch: the engine's graph could not be read back (%s)", why.c_str());
            c.valid = false;
            c.list_seq = listSeq;
            return c;
        }
        c.graph = std::move(g);
    }
    c.version = version;
    c.epoch = epoch;
    c.valid = true;
    c.list_seq = listSeq;
    return c;
}

std::string defaultDeviceKey(const pc::Graph& draft, const PatchLists& lists) {
    const auto taken = [&draft](const std::string& key) {
        for (const pc::Node& n : draft.nodes()) {
            if (n.kind == pc::NodeKind::Radio && pc::sameDevice(n.device, key)) { return true; }
        }
        return false;
    };
    // The receiver's own radio first (the one lent to the patch, else the one it has open: fact.patch.receiverRadio), then the engine's list of devices in its order (PATCH_DEVICES).
    std::vector<std::string> candidates;
    if (!lists.receiver_radio.empty()) { candidates.push_back(lists.receiver_radio); }
    for (const DeviceRow& c : lists.devices) {
        // Radios only: a new Radio is never started on a recording nobody chose - the generator is the stand-in when no radio is free.
        if (!pc::isGeneratorKey(c.key) && !pc::isIqFileKey(c.key)) { candidates.push_back(c.key); }
    }
    for (const std::string& k : candidates) {
        if (!taken(k)) { return k; }
    }
    return pc::kGeneratorKey;
}

// =================================================================================================================================
// THE SMALL RULES
// =================================================================================================================================

const char* kindCaption(pc::NodeKind k) {
    switch (k) {
        case pc::NodeKind::Radio: return tr("RADIO");
        case pc::NodeKind::Channel: return tr("CHANNEL");
        case pc::NodeKind::Demod: return tr("DEMOD");
        case pc::NodeKind::Decoder: return tr("DECODER");
        case pc::NodeKind::Display: return tr("DISPLAY");
        case pc::NodeKind::Sink: return tr("SINK");
        case pc::NodeKind::Map: return tr("MAP");
    }
    return tr("NODE");
}

const char* refusalText(pc::Connect why) {
    switch (why) {
        case pc::Connect::Ok: return "";
        case pc::Connect::UnknownNode: return tr("that node is gone");
        case pc::Connect::NoSuchPort: return tr("there is no port there");
        case pc::Connect::TypeMismatch: return tr("those two carry different things");
        case pc::Connect::InputOccupied: return tr("that input already has a wire");
        case pc::Connect::AlreadyWired: return tr("those are already wired together");
        case pc::Connect::SelfLoop: return tr("a node cannot feed itself");
        case pc::Connect::WouldCycle: return tr("that would make a loop");
    }
    return tr("that connection was refused");
}

int portTone(pc::PortType t) {
    switch (t) {
        case pc::PortType::Iq: return 0;
        case pc::PortType::Audio: return 1;
        case pc::PortType::Text: return 2;
        case pc::PortType::Control: return 3;
        case pc::PortType::Track: return 4;
    }
    return 0;
}

namespace {
// The seven fixed parts, in the order of app_window.cpp kParts: the English label (stored in the patch as data), the kind and the feed.
struct PartSpec {
    const char* label;
    pc::NodeKind kind;
    pc::PortType feed;
};
const PartSpec kParts[7] = {
    {FOX_TR_NOOP("Radio"), pc::NodeKind::Radio, pc::PortType::Iq},       {FOX_TR_NOOP("Channel"), pc::NodeKind::Channel, pc::PortType::Iq},
    {FOX_TR_NOOP("Demod"), pc::NodeKind::Demod, pc::PortType::Iq},       {FOX_TR_NOOP("Spectrum"), pc::NodeKind::Display, pc::PortType::Iq},
    {FOX_TR_NOOP("Speaker"), pc::NodeKind::Sink, pc::PortType::Audio},   {FOX_TR_NOOP("Text out"), pc::NodeKind::Sink, pc::PortType::Text},
    {FOX_TR_NOOP("Map"), pc::NodeKind::Map, pc::PortType::Track},
};
}  // namespace

std::string partLabelEnglish(int kind) { return kind >= 0 && kind < 7 ? std::string(kParts[kind].label) : std::string(); }

std::string converterNameText(const cc::ConverterSetting& s) {
    const std::string lo = cc::converterHzText(s.loHz);
    std::string out;
    if (s.mode == cc::ConverterMode::Down) {
        cc::formatUtf8(out, s.inverted ? tr("%s down-converter, spectrum inverted") : tr("%s down-converter"), lo.c_str());
    } else {
        cc::formatUtf8(out, s.inverted ? tr("%s up-converter, spectrum inverted") : tr("%s up-converter"), lo.c_str());
    }
    return out;
}

bool parseMhz(const std::string& text, double& mhz) {
    std::size_t lo = 0, hi = text.size();
    while (lo < hi && std::isspace(static_cast<unsigned char>(text[lo]))) { ++lo; }
    while (hi > lo && std::isspace(static_cast<unsigned char>(text[hi - 1]))) { --hi; }
    if (lo == hi) { return false; }
    const std::string body = text.substr(lo, hi - lo);
    char* end = nullptr;
    const double v = std::strtod(body.c_str(), &end);
    if (end == body.c_str() || *end != '\0' || !std::isfinite(v)) { return false; }
    mhz = v;
    return true;
}

std::string cutBytes(const std::string& s, std::size_t bytes) {
    if (s.size() <= bytes) { return s; }
    return s.substr(0, cc::utf8Floor(s.c_str(), bytes));
}

std::string documentOf(const pc::Graph& g, const pg::View& view) { return pc::serialise(g, view.pan.x, view.pan.y, view.zoom); }

// =================================================================================================================================
// THE EFFECTS THAT ARE ONE OP
// =================================================================================================================================

Command opRun(std::uint32_t receiverFlags) { return make(cmd::makeInt(FOXAPI_OP_PATCH_RUN, (receiverFlags & FOXAPI_RX_PATCH_RUNNING) != 0u ? 0 : 1)); }
Command opAllOff() { return make(cmd::make(FOXAPI_OP_PATCH_ALL_OFF)); }
Command opScanNativeOnly() { return make(cmd::make(FOXAPP_OP_SCAN_NATIVE_ONLY)); }
std::vector<Command> opDeviceOpened() {
    // The pass applies commands in the order sent: the wish for the lists, then the recordings.
    return {make(cmd::make(FOXAPP_OP_PATCH_RADIO_LIST_OPENED)), make(cmd::make(FOXAPP_OP_PATCH_LIST_RECORDINGS))};
}
std::vector<Command> opLook(std::uint32_t sourceFlags) {
    if ((sourceFlags & FOXAPI_SRC_SCAN_PENDING) != 0u) { return {}; }
    return {make(cmd::make(FOXAPP_OP_PATCH_LOOK_FOR_RADIOS)), make(cmd::make(FOXAPI_OP_SCAN_DEVICES)), make(cmd::make(FOXAPP_OP_PATCH_LIST_RECORDINGS))};
}
Command opViewShown(bool shown) { return make(cmd::makeInt(FOXAPP_OP_SET_PATCH_PAGE_OPEN, shown ? 1 : 0)); }

std::vector<PictureWant> picturesWanted(const pc::Graph& draft, const std::vector<LiveRow>& live, const std::map<pc::NodeId, std::uint64_t>& held) {
    std::vector<PictureWant> out;
    for (const LiveRow& r : live) {
        if (r.kind != 6) { continue; }
        const pc::Node* n = draft.find(r.node);
        if (n == nullptr || n->kind != pc::NodeKind::Decoder) { continue; }
        const auto id = static_cast<std::uint64_t>(r.v0);
        const auto it = held.find(r.node);
        if (it != held.end() && it->second == id) { continue; }
        out.push_back({r.node, id, static_cast<std::uint64_t>(r.v1)});
    }
    return out;
}

bool pictureWhole(std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint64_t bytesGot) {
    std::uint64_t bpp = 0;
    if (format == CASCADE_IMAGE_RGB24) { bpp = 3; }
    else if (format == CASCADE_IMAGE_GRAY8) { bpp = 1; }
    if (bpp == 0 || width == 0 || height == 0) { return false; }
    return static_cast<std::uint64_t>(width) * height * bpp == bytesGot;
}

// =================================================================================================================================
// THE TEXT OUT QUEUES
// =================================================================================================================================

void SinkLines::read(TableReader& table, const pc::Graph& graph) {
    // Only the rows above the highest routed are taken, so a line is routed once (newest rows are at the end of the list).
    const std::int32_t n = listSize(table, FOXAPI_LIST_DECODER_LINES);
    std::vector<FoxListItem> fresh;
    for (std::int32_t i = n - 1; i >= 0; --i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (table.readList(FOXAPI_LIST_DECODER_LINES, static_cast<std::uint32_t>(i), &it) <= i) { continue; }
        if (it.id <= routed_id_) { break; }
        fresh.push_back(it);
    }
    for (auto row = fresh.rbegin(); row != fresh.rend(); ++row) {
        routed_id_ = row->id;
        const auto node = static_cast<pc::NodeId>(row->value[0]);
        if (node == 0) { continue; }
        for (const pc::Wire& w : graph.wires()) {
            if (w.from != node) { continue; }
            const pc::Node* dst = graph.find(w.to);
            if (dst == nullptr || dst->kind != pc::NodeKind::Sink) { continue; }
            std::deque<std::string>& q = queues_[w.to];
            q.push_back(std::string(row->name) + ": " + row->detail);
            while (q.size() > kSinkLinesCap) { q.pop_front(); }
        }
    }
}

void SinkLines::dropGone(const pc::Graph& graph) {
    for (auto it = queues_.begin(); it != queues_.end();) {
        it = graph.find(it->first) == nullptr ? queues_.erase(it) : std::next(it);
    }
}

// =================================================================================================================================
// THE DRAFT MACHINE (app_window.cpp 12220-12251 and 12522-12711)
// =================================================================================================================================

// A NEW PATCH (another epoch), or a document this page loaded itself: whatever the page was doing - a drag, a resize, a wire in the air, a selection - was of a node in the OLD one,
// and its id may now name a different node altogether. (The canvas forgets when it sees forgetSerial move.)
// THE DRAFT BECOMES THE ENGINE'S GRAPH, as one publish showed it: the graph, the version it is and the epoch it belongs to.
void PatchDraft::adopt(const PatchLook& l, bool documentChanged) {
    const bool followed = in_step_;
    if (in_step_ && epoch_ != l.epoch) {
        ++forget_serial_;
        // A Radio part waiting for its scan was of the old patch.
        waits_.clear();
    }
    draft_ = l.graph;
    base_ = l.graph;
    version_ = l.version;
    epoch_ = l.epoch;
    in_step_ = true;
    // The engine changed it (a loaded patch, START or ALL OFF, a centre the radio reported): the config's document follows. (The first adoption is not a change, and neither is the
    // answer to a document this page loaded itself: it set the document with it.)
    if (followed && documentChanged) { dirty = true; }
}

// THE COMMIT'S ANSWER, ONCE THE PUBLISH THAT SHOWS IT HAS BEEN READ. The draft is the engine's graph as it now is (the engine merged what this edit did not know about) with whatever
// the page changed since it SENT on top: a node still being dragged keeps the pointer's place, and nothing snaps back. A document is adopted whole.
void PatchDraft::settle(const PatchLook& l) {
    if (send_.state != Send::State::Applied) { return; }
    if (!l.valid || l.version < send_.new_version) { return; }   // the publish that shows it is not read yet
    if (send_.document || l.epoch != epoch_) {
        // A document, or a NEW PATCH loaded under this edit: the edit was of the old one and belongs to nothing in the new one. It goes.
        if (!send_.document) { cc::diagLogf("patch: an edit of the previous patch was dropped - a new patch was loaded"); }
        const bool ownDocument = send_.document;
        send_ = Send{};
        adopt(l, !ownDocument);
        return;
    }
    draft_ = pc::rebaseDraft(send_.sent, draft_, l.graph);
    base_ = l.graph;
    version_ = l.version;
    epoch_ = l.epoch;
    send_ = Send{};
    dirty = true;
}

void PatchDraft::onAnswer(const FoxCommandResult& r, const PatchLook& l) {
    if (send_.state != Send::State::Sent || r.ticket != send_.ticket) { return; }
    if (r.status != FOXAPI_OK) {
        cc::diagWarnf("patch: the page's edit was not taken (%s); the page shows the patch as it is", r.message);
        send_ = Send{};
        adopt_wanted_ = true;
        return;
    }
    send_.state = Send::State::Applied;
    send_.new_version = static_cast<std::uint64_t>(r.applied[0]);
    send_.merged = r.applied[1] == 1.0;
    settle(l);
}

// A Radio part's device, once its scan has been answered (see PatchPage::partPressed). In the order the parts were pressed, each taking the first free radio of the list as it is now -
// and the draft already holds the device the part before it took, so two parts never start on the same radio.
void PatchDraft::finishRadioParts(const PatchLists& lists) {
    if (waits_.empty() || !in_step_) { return; }
    std::size_t done = 0;
    for (; done < waits_.size(); ++done) {
        if (waits_[done].scan_ticket > scan_done_) { break; }
        if (pc::Node* n = draft_.mutableNode(waits_[done].node)) {
            if (n->device.empty()) {
                n->device = defaultDeviceKey(draft_, lists);
                dirty = true;
            }
        }
    }
    waits_.erase(waits_.begin(), waits_.begin() + static_cast<std::ptrdiff_t>(done));
}

void PatchDraft::sync(const PatchLook& l, const PatchLists& lists, bool interacting) {
    settle(l);
    // NEVER A DEFAULT, and never a graph that was not published: before the first publish has been read the draft is not in step with anything, and nothing can be committed from it.
    if (!l.valid) { return; }
    if (adopt_wanted_ && send_.state == Send::State::Idle) {
        adopt_wanted_ = false;
        adopt(l);
        finishRadioParts(lists);
        return;
    }
    // A COMMIT IS IN FLIGHT: the published graph is the old one until the pass has applied it, and adopting it now would put a dragged node back where it was. The answer decides.
    if (send_.state != Send::State::Idle) { return; }
    if (!in_step_ || l.epoch != epoch_) {
        adopt(l);
        finishRadioParts(lists);
        return;
    }
    if (l.version != version_ && !interacting) {
        // The engine's graph changed, and not by this draft: a loaded patch, START or ALL OFF, a device or centre the patch runtime set. An edit not yet committed would be kept, on top.
        // (Never mid-drag: the node under the pointer would jump back for a frame. What the engine did meanwhile is merged by the engine itself when the drag's own commit arrives -
        // the draft names the version it was edited from.)
        const bool pending = !pc::graphsEqual(draft_, base_);
        if (pending) {
            draft_ = pc::rebaseDraft(base_, draft_, l.graph);
            base_ = l.graph;
            version_ = l.version;
            dirty = true;
        } else {
            adopt(l);
        }
    }
    finishRadioParts(lists);
}

void PatchDraft::commit(const PatchLook& l, Submitter& sub) {
    settle(l);
    // Nothing has ever been drawn from this draft, so nothing in it is an edit - and an empty graph sent now would wipe the engine's.
    if (!in_step_) { return; }
    if (adopt_wanted_) {
        // The engine refused the last edit: the page shows the patch as it is, and does not send it again.
        if (l.valid && send_.state == Send::State::Idle) {
            adopt_wanted_ = false;
            adopt(l);
        }
        return;
    }
    // ONE COMMIT IN FLIGHT: edits made meanwhile stay in the draft and go with the next, from the new base.
    if (send_.state != Send::State::Idle) { return; }
    // Nothing edited: no text written.
    if (pc::graphsEqual(draft_, base_)) { return; }
    if (!l.valid) { return; }
    if (epoch_ != l.epoch) {
        // A NEW PATCH was loaded under this edit: the edit was of the old one, and belongs to nothing in the new one. It goes, and adopting the new patch drops the drag with it.
        cc::diagLogf("patch: an edit of the previous patch was dropped - a new patch was loaded");
        adopt(l);
        return;
    }
    // THE WHOLE DRAFT, WITH THE VERSION IT WAS EDITED FROM. The engine merges it over the graph as it is when the command is applied (START's switches, a centre the runtime learnt,
    // another session's edit in the same pass are kept), so the page no longer rebases against a copy that may be a pass old.
    const Command c = make(cmd::makeText(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(draft_), 0, static_cast<std::int64_t>(version_)));
    const std::uint64_t ticket = sub.submit(c);
    if (ticket == 0u) { return; }   // not taken (the session is full): the edit stays in the draft, and goes next tick
    send_ = Send{};
    send_.state = Send::State::Sent;
    send_.ticket = ticket;
    send_.sent = draft_;
}

bool PatchDraft::loadDocument(const pc::Graph& g, Submitter& sub) {
    // A NEW PATCH: nothing of the old one is in this page's hands any more (a drag, a selection, a part waiting for a device), and the draft is the document at once. The engine's answer -
    // and the publish that shows it - settles the epoch, the version and whatever the engine added (settle adopts it whole).
    ++forget_serial_;
    waits_.clear();
    const Command c = make(cmd::makeText(FOXAPP_OP_PATCH_SET_GRAPH, pc::graphCommandText(g), FOXAPP_PATCH_GRAPH_DOCUMENT));
    const std::uint64_t ticket = sub.submit(c);
    if (ticket == 0u) { return false; }
    send_ = Send{};
    send_.state = Send::State::Sent;
    send_.ticket = ticket;
    send_.document = true;
    adopt_wanted_ = false;
    draft_ = g;
    base_ = g;
    version_ = 0;
    in_step_ = true;
    return true;
}

// =================================================================================================================================
// THE CANVAS: the pointer stream (gui/patch_view.cpp 395-525)
// =================================================================================================================================

bool PatchCanvas::interacting() const { return down_ && (ui.dragNode != pc::kNoNode || ui.resizeNode != pc::kNoNode || ui.wiring); }

pg::Vec2 PatchCanvas::pointerWorld() const { return pg::screenToWorld(drawView(), pointer_); }

void PatchCanvas::forget() {
    ui.dragNode = pc::kNoNode;
    ui.resizeNode = pc::kNoNode;
    ui.wiring = false;
    ui.selected = pc::kNoNode;
    ui.wireSelected = false;
}

void PatchCanvas::reconcile(const PatchDraft& d) {
    if (d.forgetSerial() != seen_serial_) {
        seen_serial_ = d.forgetSerial();
        forget();
    }
    // A drag, a resize, a wire or a selection of a node that vanished (the engine's graph changed under it) is forgotten.
    const pc::Graph& g = d.graph();
    if (ui.dragNode != pc::kNoNode && g.find(ui.dragNode) == nullptr) { ui.dragNode = pc::kNoNode; }
    if (ui.resizeNode != pc::kNoNode && g.find(ui.resizeNode) == nullptr) { ui.resizeNode = pc::kNoNode; }
    if (ui.selected != pc::kNoNode && g.find(ui.selected) == nullptr) { ui.selected = pc::kNoNode; }
    if (ui.wiring && g.find(ui.wireFrom.node) == nullptr) { ui.wiring = false; }
    if (ui.wireSelected && std::find(g.wires().begin(), g.wires().end(), ui.selectedWire) == g.wires().end()) { ui.wireSelected = false; }
}

void PatchCanvas::press(PatchDraft& d, float x, float y, int button, std::int64_t now) {
    (void)now;
    reconcile(d);
    pointer_ = pg::Vec2{x, y};
    last_ = pointer_;
    pointer_known_ = true;
    // A PAN IS HELD BY THE BUTTON THAT STARTED IT: the middle button anywhere on the canvas, or - for a touchpad with no middle button - the LEFT button pressed on empty canvas.
    if (button == 2) {
        ui.panning = true;
        ui.panButton = 2;
        return;
    }
    if (button != 0) { return; }
    down_ = true;
    pc::Graph& g = d.graph();
    const pg::View dv = drawView();
    const pg::Vec2 world = pg::screenToWorld(dv, pointer_);
    // WHAT THE PRESS LANDED ON decides what the drag does, in one place (gui::patch::pressTarget).
    const pg::PressOn on = pg::pressTarget(g, world, dv.zoom, kWireGrabPx);
    const pc::NodeId hit = pg::nodeAt(g, world);
    const pc::Node* n = hit != pc::kNoNode ? g.find(hit) : nullptr;
    switch (on) {
        case pg::PressOn::Port:
            ui.wiring = true;
            ui.wireFrom = pg::portAt(g, world);
            break;
        case pg::PressOn::CloseKey:
            // Closing takes the node's wires with it (removeNode).
            g.removeNode(hit);
            if (ui.selected == hit) { ui.selected = pc::kNoNode; }
            ui.wireSelected = false;
            d.dirty = true;
            break;
        case pg::PressOn::Grip:
            if (n != nullptr) {
                ui.selected = hit;
                ui.wireSelected = false;
                // `grab` is the pointer's offset from the CORNER, so the corner does not jump to the pointer.
                const pg::Vec2 s = pg::nodeSize(*n);
                ui.resizeNode = hit;
                ui.grab = pg::Vec2{world.x - (n->x + s.x), world.y - (n->y + s.y)};
            }
            break;
        case pg::PressOn::Header:
            if (n != nullptr) {
                ui.selected = hit;
                ui.wireSelected = false;
                ui.dragNode = hit;
                ui.grab = pg::Vec2{world.x - n->x, world.y - n->y};
            }
            break;
        case pg::PressOn::Body:
            ui.selected = hit;
            ui.wireSelected = false;
            break;
        case pg::PressOn::Wire:
            // Near a wire: selects it.
            ui.selected = pc::kNoNode;
            ui.wireSelected = false;
            for (const pc::Wire& w : g.wires()) {
                const pg::WireEnds e = pg::wireEnds(g, w);
                if (!e.found) { continue; }
                if (pg::distanceToWire(e.from, e.to, world) * dv.zoom <= kWireGrabPx) {
                    ui.selectedWire = w;
                    ui.wireSelected = true;
                    break;
                }
            }
            break;
        case pg::PressOn::Empty:
            // Empty canvas: the selection clears, and holding the button down drags the view (pressPans).
            ui.selected = pc::kNoNode;
            ui.wireSelected = false;
            break;
    }
    if (pg::pressPans(on)) {
        ui.panning = true;
        ui.panButton = 0;
    }
}

void PatchCanvas::move(PatchDraft& d, float x, float y) {
    reconcile(d);
    const pg::Vec2 delta{x - last_.x, y - last_.y};
    pointer_ = pg::Vec2{x, y};
    last_ = pointer_;
    pointer_known_ = true;
    if (ui.panning && (delta.x != 0.0f || delta.y != 0.0f)) {
        ui.view = pg::panBy(ui.view, delta);
        d.dirty = true;
    }
    const pg::Vec2 world = pg::screenToWorld(drawView(), pointer_);
    if (ui.dragNode != pc::kNoNode) {
        if (pc::Node* n = d.graph().mutableNode(ui.dragNode)) {
            n->x = world.x - ui.grab.x;
            n->y = world.y - ui.grab.y;
            d.dirty = true;
        }
    }
    if (ui.resizeNode != pc::kNoNode) {
        if (pc::Node* n = d.graph().mutableNode(ui.resizeNode)) {
            pg::resizeNodeTo(*n, pg::Vec2{world.x - ui.grab.x, world.y - ui.grab.y});
            d.dirty = true;
        }
    }
}

void PatchCanvas::release(PatchDraft& d, float x, float y, int button, std::int64_t now) {
    reconcile(d);
    pointer_ = pg::Vec2{x, y};
    last_ = pointer_;
    pointer_known_ = true;
    if (button == 2) {
        if (ui.panning && ui.panButton == 2) { ui.panning = false; }
        return;
    }
    if (button != 0) { return; }
    down_ = false;
    if (ui.panning && ui.panButton == 0) { ui.panning = false; }
    ui.dragNode = pc::kNoNode;
    ui.resizeNode = pc::kNoNode;
    if (!ui.wiring) { return; }
    pc::Graph& g = d.graph();
    const pg::Vec2 world = pg::screenToWorld(drawView(), pointer_);
    const pg::PortHit drop = pg::portAt(g, world);
    ui.refusal = pc::Connect::Ok;
    if (drop.found && drop.node != pc::kNoNode) {
        // A wire may be drawn from either end; whichever end is the OUTPUT is the source.
        const pg::PortHit& out = ui.wireFrom.input ? drop : ui.wireFrom;
        const pg::PortHit& in = ui.wireFrom.input ? ui.wireFrom : drop;
        if (!out.input && in.input) {
            const pc::Connect r = g.connect(out.node, out.port, in.node, in.port);
            if (r == pc::Connect::Ok) { d.dirty = true; }
            if (r != pc::Connect::Ok) {
                ui.refusal = r;
                refused_tick_ = now;
                ui.refusedNear = world;
            }
        } else {
            // Two inputs or two outputs: there is no sense in which that is a connection, and saying so is better than silence.
            ui.refusal = pc::Connect::TypeMismatch;
            refused_tick_ = now;
            ui.refusedNear = world;
        }
    }
    ui.wiring = false;
}

void PatchCanvas::left() { pointer_known_ = false; }

void PatchCanvas::wheel(PatchDraft& d, float x, float y, float notchesX, float notchesY, bool ctrl, bool shift) {
    reconcile(d);
    pointer_ = pg::Vec2{x, y};
    pointer_known_ = true;
    pg::WheelInput w;
    w.x = notchesX;
    w.y = notchesY;
    w.ctrl = ctrl;
    w.shift = shift;
    ui.view = pg::wheelView(ui.view, pointer_, w);
    d.dirty = true;
}

void PatchCanvas::zoomStep(PatchDraft& d, bool in) {
    ui.view = pg::zoomStepView(ui.view, canvas_w, canvas_h, in);
    d.dirty = true;
}

void PatchCanvas::fit(PatchDraft& d) {
    // The page's own graph, which holds every edit (the fit is of what is on the canvas, not of the engine's copy a pass behind).
    ui.view = pg::fitView(d.graph(), canvas_w, canvas_h, 1.0f);
    d.dirty = true;
}

// Delete removes whichever ONE thing is selected.
void PatchCanvas::deleteKey(PatchDraft& d) {
    reconcile(d);
    if (ui.wireSelected) {
        d.graph().disconnect(ui.selectedWire);
        ui.wireSelected = false;
        d.dirty = true;
    } else if (ui.selected != pc::kNoNode) {
        d.graph().removeNode(ui.selected);
        ui.selected = pc::kNoNode;
        d.dirty = true;
    }
}

// =================================================================================================================================
// THE PAGE: the tick
// =================================================================================================================================

PatchPage::PatchPage() = default;

void PatchPage::setCanvasSize(float w, float h) { canvas_.setSize(w, h); }

pc::Node* PatchPage::selectedNode() {
    return canvas_.ui.selected == pc::kNoNode ? nullptr : draft_.graph().mutableNode(canvas_.ui.selected);
}
const pc::Node* PatchPage::selectedNode() const {
    return canvas_.ui.selected == pc::kNoNode ? nullptr : draft_.graph().find(canvas_.ui.selected);
}

void PatchPage::tick(TableReader& table, const FoxReceiverState& state, Submitter& sub, bool showing) {
    ++now_;
    // THE ENGINE IS TOLD WHETHER THE VIEW IS SHOWING (FOXAPP_OP_SET_PATCH_PAGE_OPEN): OFFERED, not submitted - a refusal is no drop, the flag is offered again next tick.
    if (want_shown_ != shown_taken_) {
        if (sub.offer(opViewShown(want_shown_)) != 0u) { shown_taken_ = want_shown_; }
    }
    flags_ = state.flags;
    source_flags_ = state.sourceFlags;
    centre_hz_ = state.centreHz;
    rx_lat_ = state.rxLatDeg;
    rx_lon_ = state.rxLonDeg;
    listseq_ = state.listSeq;
    if (!store_loaded_ && config_ != nullptr) {
        store_.assign(config_->presets(), config_->previous());
        store_loaded_ = true;
    }

    // 1 syncDraft(interacting)   2 readLive   3 the restore and the seed   4 (the models are built on demand)   5 commitDraft
    readLook(table, state.listSeq, look_);
    if (showing || !draft_.waits().empty()) { readLists(table, state.listSeq, lists_); }
    canvas_.reconcile(draft_);
    draft_.sync(look_, lists_, canvas_.interacting());
    canvas_.reconcile(draft_);
    if (showing) { live_ = readLive(table); }
    restoreAndSeed(sub, state, showing);
    forgetGone(showing);
    if (showing) { sink_lines_.read(table, draft_.graph()); }
    draft_.commit(look_, sub);
    // THE DOCUMENT THE CONFIG KEEPS: the page's own graph, which holds every edit (the engine's may be a pass behind it), with the view.
    if (draft_.dirty) {
        draft_.dirty = false;
        if (config_ != nullptr) { config_->savePatch(documentOf(draft_.graph(), canvas_.ui.view)); }
    }
}

void PatchPage::onAnswer(const FoxCommandResult& r) {
    if (r.op == FOXAPP_OP_PATCH_SET_GRAPH) { draft_.onAnswer(r, look_); }
    if (r.op == FOXAPP_OP_SCAN_NATIVE_ONLY) { draft_.noteScanAnswer(r.ticket); }
}

void PatchPage::restoreAndSeed(Submitter& sub, const FoxReceiverState& state, bool showing) {
    (void)state;
    if (!look_.valid || !draft_.inStep()) { return; }
    // THE RESTORE: the config's saved patch, once, as a NEW DOCUMENT. A text that is not a patch is given up; a document the session would not take is tried again next tick.
    if (!restored_) {
        const std::string text = config_ != nullptr ? config_->patch() : std::string();
        if (text.empty()) {
            restored_ = true;
        } else {
            const pc::LoadResult pr = pc::parse(text);
            if (!pr.ok) {
                restored_ = true;
            } else {
                if (!draft_.loadDocument(pr.graph, sub)) { return; }
                canvas_.ui.view.pan = pg::Vec2{pr.panX, pr.panY};
                canvas_.ui.view.zoom = pr.zoom;
                seeded_ = !pr.graph.nodes().empty();
                if (pr.dropped > 0) { cc::diagLogf("patch: the saved patch lost %d connection(s) on loading", pr.dropped); }
                restored_ = true;
                canvas_.reconcile(draft_);
            }
        }
    }
    // THE SEED (seedPatchIfNeeded 12713-12751): the patch a page opens with when the user has none, only once the receiver's radio has finished opening, only while the page is shown,
    // and not while a document is on its way.
    if (!showing || seeded_) { return; }
    if ((flags_ & FOXAPI_RX_DEVICE_OPENING) != 0u || (source_flags_ & FOXAPI_SRC_CARD_OPENING) != 0u) { return; }
    if (!draft_.inStep() || draft_.send().state != PatchDraft::Send::State::Idle) { return; }
    seeded_ = true;
    pc::Graph& g = draft_.graph();
    if (g.nodes().empty()) {
        // NOT WRAPPED: this is the default value handed to a new node's NAME field, which is stored in the graph as plain data from this point on.
        g.addNode(pc::NodeKind::Radio, "Radio", pc::PortType::Iq, 60.0f, 80.0f);
    }
    // The starter radio is the receiver's own radio or, with none, the generator - named now, because the patch no longer starts (and so takes the receiver's radio) the moment the
    // page opens.
    std::vector<pc::NodeId> ids;
    for (const pc::Node& n0 : g.nodes()) { ids.push_back(n0.id); }
    for (const pc::NodeId id : ids) {
        pc::Node* n = g.mutableNode(id);
        if (n == nullptr || n->kind != pc::NodeKind::Radio || !n->device.empty()) { continue; }
        n->device = defaultDeviceKey(g, lists_);
        // The receiver's AIR centre, which may be below 0 Hz through a converter (radioCentreSet) or be exactly 0 Hz - marked chosen, never judged by its value.
        if (!pc::radioCentreSet(*n) && (flags_ & FOXAPI_RX_DEVICE_OPEN) != 0u && lists_.receiver_tuned) {
            n->freqHz = centre_hz_;
            n->centreChosen = true;
        }
    }
    // The starter counts as a change, or it would be rebuilt from scratch on every launch and the first node the user drags would be the only thing that ever persisted.
    draft_.dirty = true;
    draft_.commit(look_, sub);
}

// What a node that is gone (or a patch that is new) takes with it: its notes, its picture, its lines.
void PatchPage::forgetGone(bool showing) {
    if (draft_.forgetSerial() != forget_seen_) {
        forget_seen_ = draft_.forgetSerial();
        centre_notes_.clear();
        pictures_.clear();
        sink_lines_.clear();
    }
    const pc::Graph& g = draft_.graph();
    for (auto it = centre_notes_.begin(); it != centre_notes_.end();) {
        it = g.find(it->first) == nullptr ? centre_notes_.erase(it) : std::next(it);
    }
    for (auto it = pictures_.begin(); it != pictures_.end();) {
        // ...and so does a picture the engine has let go of (a node it removed, a new patch document): its row is gone.
        const bool dead = g.find(it->first) == nullptr || (showing && liveRow(live_, 6, it->first) == nullptr);
        it = dead ? pictures_.erase(it) : std::next(it);
    }
    sink_lines_.dropGone(g);
    // A selection whose row has gone is forgotten rather than pointing at nothing.
    if (!preset_sel_.empty() && store_.find(preset_sel_) < 0) { preset_sel_.clear(); }
    if (preset_sel_previous_ && store_.previous().empty()) { preset_sel_previous_ = false; }
}

// =================================================================================================================================
// THE BAND
// =================================================================================================================================

PatchModel PatchPage::bandModel() const {
    PatchModel m;
    const bool running = (flags_ & FOXAPI_RX_PATCH_RUNNING) != 0u;
    m.running = running;
    m.run_label = running ? tr("STOP") : tr("START");
    m.run_tooltip = running ? tr("Stop the patch: every patch radio closes, recordings are\nfinished, and the receiver gets its radio back.")
                            : tr("Start the patch: every radio switched on opens. The receiver\nhands its radio over and runs on the signal generator.");
    m.all_off_tooltip = tr("Switch every radio off and stop the patch. START switches\nthem all on again.");
    m.presets_tooltip = tr("Save the patch on the canvas under a name, or load one you saved.\nLoading stops a running patch; the one loaded waits for START.");
    m.presets_open = presets_open_;

    // What is running, in words. Singular and plural as whole keys: an English "s" handed in by %s is a word no catalogue can translate.
    std::size_t radios = 0, on = 0;
    for (const pc::Node& n : draft_.graph().nodes()) {
        if (n.kind != pc::NodeKind::Radio) { continue; }
        ++radios;
        if (n.on) { ++on; }
    }
    if (running) {
        cc::formatUtf8(m.run_line, radios == 1 ? tr("RUNNING - %zu of %zu radio open") : tr("RUNNING - %zu of %zu radios open"), static_cast<std::size_t>(lists_.radios_open), radios);
        m.run_line_lit = true;
    } else {
        cc::formatUtf8(m.run_line, radios == 1 ? tr("STOPPED - %zu of %zu radio switched on") : tr("STOPPED - %zu of %zu radios switched on"), on, radios);
        m.run_line_lit = false;
    }

    // THE RAIL'S CHIP (drawPatchSection).
    const std::size_t nodes = draft_.graph().nodes().size();
    if (running) {
        m.tab_chip = tr("RUNNING");
        m.tab_chip_tone = Tone::Good;
    } else if (nodes == 0) {
        m.tab_chip = tr("EMPTY");
    } else {
        cc::formatUtf8(m.tab_chip, nodes == 1 ? tr("%zu NODE") : tr("%zu NODES"), nodes);
    }

    // THE SEVEN FIXED PARTS. AT MOST FIVE RADIOS: the key goes grey at five and says why.
    for (int i = 0; i < 7; ++i) {
        PartKey k;
        k.label = tr(kParts[i].label);
        k.kind = i;
        const bool full = kParts[i].kind == pc::NodeKind::Radio && draft_.graph().count(pc::NodeKind::Radio) >= pc::kMaxRadios;
        k.enabled = !full;
        if (full) { cc::formatUtf8(k.tooltip, tr("A patch has at most %zu radios."), pc::kMaxRadios); }
        m.parts.push_back(std::move(k));
    }

    // THE DECODER ROW: one part per installed decoder plugin.
    if (lists_.parts.empty()) { m.no_decoders = tr("- none installed. Add decoder plugins from the plugin store in DECODE."); }
    for (std::size_t i = 0; i < lists_.parts.size(); ++i) {
        const pc::DecoderInfo& info = lists_.parts[i];
        // A DECODER THAT DOES NOT NEED SOUND DOES NOT OFFER IT: a module that can decode straight from the radio's I/Q is offered only that way; its audio twin is left out of the bin.
        if (!info.image && info.feed == pc::PortType::Audio && pc::hasIqTwin(lists_.parts, i)) { continue; }
        // The input is in the label only when it disambiguates - a module that is both an audio and an I/Q decoder shows two keys.
        bool twin = false;
        for (std::size_t j = 0; j < lists_.parts.size(); ++j) {
            if (j == i || lists_.parts[j].key != info.key) { continue; }
            // A twin that is hidden above is not a key to tell this from.
            const bool hidden = !lists_.parts[j].image && lists_.parts[j].feed == pc::PortType::Audio && pc::hasIqTwin(lists_.parts, j);
            if (!hidden) { twin = true; }
        }
        // A picture decoder always says so - it is a different kind of part from a text decoder of the same name.
        if (info.image) { twin = false; }
        // One whole phrase per variant, so a translation can place the name where its language wants it.
        const char* shownFmt = info.image ? tr("%s (picture)") : !twin ? "%s" : info.feed == pc::PortType::Iq ? "%s (I/Q)" : tr("%s (audio)");
        DecoderKey k;
        cc::formatUtf8(k.label, shownFmt, info.name.c_str());
        k.index = static_cast<int>(i);
        // Four whole sentences rather than one built from fragments, so each can be translated as a sentence.
        const bool iqIn = info.feed == pc::PortType::Iq;
        const bool fixedRate = info.requiredRateHz > 0.0;
        const char* tipFmt = iqIn ? (fixedRate ? tr("%s\nI/Q input, needs a fixed rate\nWire it from a channel (or the radio itself).")
                                               : tr("%s\nI/Q input, any rate\nWire it from a channel (or the radio itself)."))
                                  : (fixedRate ? tr("%s\nAudio input, needs a fixed rate\nWire it from a demodulator.")
                                               : tr("%s\nAudio input, any rate\nWire it from a demodulator."));
        cc::formatUtf8(k.tooltip, tipFmt, info.key.c_str());
        m.decoders.push_back(std::move(k));
    }

    // THE BLUEPRINTS: offered when there is a plugin to build round, or a position or a group the tower list can use.
    m.blueprints_shown = !(lists_.parts.empty() && (flags_ & FOXAPI_RX_POSITION_SET) == 0u && lists_.bookmark_groups.empty());
    m.blueprint_placeholder = tr("Pick a plugin to place its whole workflow");
    for (std::size_t i = 0; i < blueprints_.size(); ++i) {
        const pc::Blueprint& bp = blueprints_[i];
        SelectItem it;
        it.label = bp.decoder.image && bp.listen.empty() ? cc::formatText(tr("%s (picture)"), bp.title.c_str()) : bp.title;
        it.id = std::to_string(i);
        if (bp.tune.freqHz > 0.0) {
            cc::formatUtf8(it.note, "%.4f MHz", bp.tune.freqHz / 1.0e6);
        } else if (!bp.listen.empty()) {
            // A tower's channels, one a line, as they will be named.
            for (const pc::ListenChannel& c : bp.listen) {
                if (!it.note.empty()) { it.note += '\n'; }
                it.note += c.name;
            }
        }
        m.blueprints.push_back(std::move(it));
    }
    m.blueprint_note = blueprint_note_;
    m.info = infoModel();
    m.inspector_shown = selectedNode() != nullptr;
    m.inspector_folded = folded_;
    return m;
}

InfoModel PatchPage::infoModel() const {
    InfoModel m;
    m.runnable = lists_.plan.runnable;
    // WHAT THE PATCH AS A WHOLE IS DOING, first, because it is the question someone opening this page is actually asking.
    m.verdict = m.runnable ? tr("This patch can run.") : tr("This patch cannot run yet.");
    const pc::Node* sel = selectedNode();
    // The selected channel's own level, spelled out: only for a node the plan holds as a channel and whose level the engine reports. (The window's own loop tests only the node id and
    // prints "0.0 dB on this channel" for a running decoder: not ported.)
    if (sel != nullptr && pc::findChannel(lists_.plan, sel->id) != nullptr) {
        if (const LiveRow* r = liveRow(live_, 3, sel->id)) { cc::formatUtf8(m.level_line, tr("%.1f dB on this channel"), r->v0); }
    }
    if (!lists_.plan.channels.empty()) { cc::formatUtf8(m.plan_line, tr("%zu channel(s), decimate by %u"), lists_.plan.channels.size(), lists_.plan.channels[0].decimation); }
    if (sel == nullptr) {
        m.none_selected = true;
        m.help.push_back(tr("Nothing selected."));
        m.help.push_back(tr("Click a node to set what it does. Drag from one port to another to wire them; drag a node by its title bar to move it. Delete removes whatever is selected."));
        // HOW TO MOVE ROUND IT: the wheel pans, and a touchpad user has no middle button - say what does work.
        m.help.push_back(tr("Drag empty canvas or scroll to move round the patch. Ctrl+scroll or the + and - keys zoom; Fit shows every node."));
        return m;
    }
    m.none_selected = false;
    // The node, by kind and name; its settings are in the drawer.
    cc::formatUtf8(m.selection, "%s  %s", kindCaption(sel->kind), sel->name.c_str());
    if (folded_) { m.folded_note = tr("Its settings are folded away at the right of the canvas - press << to open them."); }
    // THE NODE'S PROBLEMS, here where they are read: an advisory is worth saying and not worth alarming about.
    for (const pc::Problem p : pc::problemsFor(lists_.plan, sel->id)) { m.problems.push_back({tr(pc::problemText(p)), pc::isAdvisory(p)}); }
    return m;
}

// =================================================================================================================================
// THE CANVAS MODEL
// =================================================================================================================================

namespace {
std::string pathOf(const pg::View& dv, pg::Vec2 a, pg::Vec2 b) {
    pg::Vec2 c1, c2;
    pg::wireCurve(a, b, c1, c2);
    const pg::Vec2 p0 = pg::worldToScreen(dv, a), p1 = pg::worldToScreen(dv, c1), p2 = pg::worldToScreen(dv, c2), p3 = pg::worldToScreen(dv, b);
    return cc::formatText("M %.2f %.2f C %.2f %.2f %.2f %.2f %.2f %.2f", static_cast<double>(p0.x), static_cast<double>(p0.y), static_cast<double>(p1.x), static_cast<double>(p1.y),
                          static_cast<double>(p2.x), static_cast<double>(p2.y), static_cast<double>(p3.x), static_cast<double>(p3.y));
}
}  // namespace

CanvasModel PatchPage::canvasModel() const {
    CanvasModel m;
    const pg::View dv = canvas_.drawView();
    const pc::Graph& g = draft_.graph();
    const pg::Interaction& ui = canvas_.ui;
    m.zoom = ui.view.zoom;
    m.faces_live = ui.view.zoom >= kFacesMinZoom;
    // THE GRID, in world units so it scrolls and scales with the patch; below 6 px it is just a wash of lines.
    const float step = kGridWorld * dv.zoom;
    if (step >= kGridMinStepPx) {
        m.grid_step = step;
        m.grid_ox = dv.pan.x - std::floor(dv.pan.x / step) * step;
        m.grid_oy = dv.pan.y - std::floor(dv.pan.y / step) * step;
    }
    const pg::Vec2 world = canvas_.pointerKnown() ? canvas_.pointerWorld() : pg::Vec2{};
    for (const pc::Node& n : g.nodes()) {
        CanvasNode c;
        c.id = static_cast<int>(n.id);
        c.kind = static_cast<int>(n.kind);
        c.kind_caption = kindCaption(n.kind);
        c.name = n.name;
        const pg::Vec2 tl = pg::worldToScreen(dv, pg::Vec2{n.x, n.y});
        const pg::Vec2 size = pg::nodeSize(n);
        c.x = tl.x;
        c.y = tl.y;
        c.w = size.x * dv.zoom;
        c.h = size.y * dv.zoom;
        c.selected = ui.selected == n.id;
        c.blocked = pc::hasBlockingProblem(lists_.plan, n.id);
        const auto row = lists_.radios.find(n.id);
        c.fault = row != lists_.radios.end() && row->second.fault;
        c.active = n.kind == pc::NodeKind::Radio && n.on;
        if (canvas_.pointerKnown()) {
            c.hot_close = pg::pointInCloseKey(n, world);
            c.hot_grip = ui.resizeNode == n.id || pg::pointInResizeGrip(n, world);
        } else {
            c.hot_grip = ui.resizeNode == n.id;
        }
        for (pc::PortIndex i = 0; i < static_cast<pc::PortIndex>(n.inputs.size()); ++i) {
            const pg::Vec2 p = pg::worldToScreen(dv, pg::inputPortPos(n, i));
            c.inputs.push_back({p.x, p.y, portTone(n.inputs[i])});
        }
        for (pc::PortIndex i = 0; i < static_cast<pc::PortIndex>(n.outputs.size()); ++i) {
            const pg::Vec2 p = pg::worldToScreen(dv, pg::outputPortPos(n, i));
            c.outputs.push_back({p.x, p.y, portTone(n.outputs[i])});
        }
        readingOf(n, c.reading, c.reading_tone, c.reading2);
        c.face = faceOf(n);
        m.nodes.push_back(std::move(c));
    }
    // THE WIRES, under the nodes: a wire with a missing end is skipped.
    for (const pc::Wire& w : g.wires()) {
        const pg::WireEnds e = pg::wireEnds(g, w);
        if (!e.found) { continue; }
        const pc::Node* src = g.find(w.from);
        CanvasWire cw;
        cw.path = pathOf(dv, e.from, e.to);
        cw.tone = portTone(src->outputs[w.fromPort]);
        cw.selected = ui.wireSelected && ui.selectedWire == w;
        m.wires.push_back(std::move(cw));
    }
    // The wire being dragged, following the pointer, drawn in the direction it will finally run.
    if (ui.wiring && ui.wireFrom.found) {
        if (const pc::Node* n = g.find(ui.wireFrom.node)) {
            const pg::Vec2 a = ui.wireFrom.input ? pg::inputPortPos(*n, ui.wireFrom.port) : pg::outputPortPos(*n, ui.wireFrom.port);
            m.ghost = ui.wireFrom.input ? pathOf(dv, world, a) : pathOf(dv, a, world);
        }
    }
    // THE REFUSAL: the sentence, 2.5 s, at the pointer + (12, 10).
    if (ui.refusal != pc::Connect::Ok && now_ - canvas_.refusedTick() < kRefusalTicks) {
        const pg::Vec2 at = pg::worldToScreen(dv, ui.refusedNear);
        m.refusal.shown = true;
        m.refusal.text = refusalText(ui.refusal);
        m.refusal.x = at.x + 12.0f;
        m.refusal.y = at.y + 10.0f;
    }
    // THE THREE VIEW KEYS need the room of three keys and a margin (drawPatchViewKeys).
    const float totalW = kStepKeyW * 2.0f + kFitKeyMinW + kKeyGap * 2.0f;
    m.keys_shown = canvas_.canvas_w >= totalW + kKeyInset * 2.0f && canvas_.canvas_h >= kKeyH + kKeyInset * 2.0f;
    m.zoom_out_tooltip = tr("Zoom out (Ctrl+scroll also zooms)");
    m.zoom_in_tooltip = tr("Zoom in (Ctrl+scroll also zooms)");
    m.fit_tooltip = tr("Show every node on the canvas");
    m.fit_label = tr("Fit");
    // THE RESIZE CURSOR, while one is being dragged and while the pointer rests on the grip of the part that is on top there.
    if (ui.resizeNode != pc::kNoNode) {
        m.resize_cursor = true;
    } else if (canvas_.pointerKnown() && !ui.panning && !ui.wiring && ui.dragNode == pc::kNoNode) {
        const pc::NodeId over = pg::nodeAt(g, world);
        const pc::Node* on = over != pc::kNoNode ? g.find(over) : nullptr;
        m.resize_cursor = on != nullptr && pg::pointInResizeGrip(*on, world);
    }
    return m;
}

// ---- the canvas callbacks ----
void PatchPage::canvasPressed(float x, float y, int button) { canvas_.press(draft_, x, y, button, now_); }
void PatchPage::canvasMoved(float x, float y) { canvas_.move(draft_, x, y); }
void PatchPage::canvasReleased(float x, float y, int button) { canvas_.release(draft_, x, y, button, now_); }
void PatchPage::canvasLeft() { canvas_.left(); }
void PatchPage::canvasWheel(float x, float y, float notchesX, float notchesY, bool ctrl, bool shift) { canvas_.wheel(draft_, x, y, notchesX, notchesY, ctrl, shift); }
void PatchPage::zoomStep(bool in) { canvas_.zoomStep(draft_, in); }
void PatchPage::fit() { canvas_.fit(draft_); }
void PatchPage::canvasDelete() { canvas_.deleteKey(draft_); }

// =================================================================================================================================
// THE FACES (drawPatchFaces, app_window.cpp 11459-12117) and the readings the canvas draws itself (patch_view.cpp 330-358)
// =================================================================================================================================

namespace {

FaceLine fl(const std::string& text, int tone) { return FaceLine{text, tone}; }

// A demodulator's squelch as the face and the inspector both say it.
struct SquelchWords {
    int mode_index = 0;
    bool on = false;
    float norm = 0.0f;
    std::string text;
    bool gate_shown = false;
    std::string gate_text, gate_word;
    bool gate_lit = false;
};
SquelchWords squelchWords(const pc::Node& n, const std::vector<LiveRow>& live) {
    SquelchWords w;
    w.mode_index = std::clamp(n.mode, 0, 7);
    w.on = n.squelch;
    w.norm = (n.squelchDb + 120.0f) / 120.0f;
    cc::formatUtf8(w.text, "%.0f dB", static_cast<double>(n.squelchDb));
    // WHERE TO SET IT: the level the gate is judging, and whether it is open.
    if (const LiveRow* gate = liveRow(live, 1, n.id)) {
        w.gate_shown = true;
        cc::formatUtf8(w.gate_text, "%.0f dB", gate->v0);
        if (!n.squelch || gate->open) {
            w.gate_lit = true;
            w.gate_word = n.squelch ? tr("open") : tr("no squelch");
        } else {
            w.gate_lit = false;
            w.gate_word = tr("closed");
        }
    }
    return w;
}

std::string megaText(double hz) { return cc::formatText("%.6f", hz / 1e6); }

}  // namespace

std::vector<FaceLine> PatchPage::radioLines(const pc::Node& n) const {
    std::vector<FaceLine> out;
    const auto it = lists_.radios.find(n.id);
    const RadioRow* row = it != lists_.radios.end() ? &it->second : nullptr;
    if (row != nullptr && row->open) {
        out.push_back(fl(cc::formatText("%.3f MS/s", row->rate_hz / 1e6), kToneAmber));
    } else if (row != nullptr && row->pending) {
        out.push_back(fl(tr("opening..."), kToneMuted));
    } else if (n.device.empty()) {
        out.push_back(fl(tr("Choose a device on the right."), kToneMuted));
    } else if (!n.on) {
        out.push_back(fl(tr("Switched off."), kToneMuted));
    } else if ((flags_ & FOXAPI_RX_PATCH_RUNNING) == 0u) {
        out.push_back(fl(tr("Ready - press START."), kToneMuted));
    } else {
        // ONE DEVICE, ONE RADIO, said on the face of the radio that is not running because of it - not only by its edge.
        for (const pc::Problem pr : pc::problemsFor(lists_.plan, n.id)) {
            if (pr == pc::Problem::DeviceTwice) { out.push_back(fl(tr("Another radio here already uses this device."), kToneRust)); }
        }
    }
    if (row != nullptr && !row->note.empty()) { out.push_back(fl(row->note, kToneRust)); }
    return out;
}

void PatchPage::readingOf(const pc::Node& n, std::string& reading, int& tone, std::string& reading2) const {
    reading.clear();
    reading2.clear();
    tone = 0;
    // Numbers are AMBER, because the palette reserves amber for numbers; decoded text is PHOSPHOR, because it is what the radio actually received.
    if (pc::findChannel(lists_.plan, n.id) != nullptr) {
        if (const LiveRow* r = liveRow(live_, 3, n.id)) {
            cc::formatUtf8(reading, "%.0f dB", r->v0);
            tone = 2;
        }
        return;
    }
    // Every RUNNING decoder gets a face, including one that has said nothing yet: "0 lines" on a decoder that is being fed is itself the answer to "is it running".
    for (const pc::DecoderPlan& dp : lists_.plan.decoders) {
        if (dp.node != n.id) { continue; }
        if (std::find(lists_.refused.begin(), lists_.refused.end(), dp.node) != lists_.refused.end()) { return; }
        const LiveRow* face = liveRow(live_, 4, n.id);
        const auto lines = face != nullptr ? static_cast<std::size_t>(face->v0) : std::size_t{0};
        // Singular and plural as whole keys: an English "s" handed in by %s is a word no catalogue can translate.
        cc::formatUtf8(reading, lines == 1 ? tr("%zu line") : tr("%zu lines"), lines);
        tone = 2;
        if (face != nullptr) { reading2 = face->detail; }
        return;
    }
}

FaceModel PatchPage::faceOf(const pc::Node& n) const {
    FaceModel f;
    const pc::Graph& g = draft_.graph();
    switch (n.kind) {
        case pc::NodeKind::Radio: {
            const auto it = lists_.radios.find(n.id);
            const bool open = it != lists_.radios.end() && it->second.open;
            // The switch shows what a press made at once, as the window's does: off first, then lit while open, amber while waiting.
            f.radio_switch = !n.on ? 0 : open ? 2 : 1;
            f.radio_switch_label = n.on ? tr("ON") : tr("OFF");
            f.radio_switch_tooltip = n.on ? tr("This radio is switched on. Press to close it alone - the rest\nof the patch keeps running.")
                                          : tr("This radio is switched off. Press to switch it on; it opens\nwhile the patch is running.");
            f.device_label = pc::deviceLabelFor(lists_.devices, n.device);
            f.centre_text = megaText(n.freqHz);
            f.centre_tooltip = tr("The radio's centre. Type a frequency and press Enter.");
            const auto note = centre_notes_.find(n.id);
            if (note != centre_notes_.end()) { f.centre_note = note->second; }
            f.lines = radioLines(n);
            break;
        }
        case pc::NodeKind::Channel:
            f.freq_text = megaText(n.freqHz);
            f.freq_tooltip = tr("Type a frequency and press Enter.");
            break;
        case pc::NodeKind::Demod: {
            const SquelchWords w = squelchWords(n, live_);
            f.mode_index = w.mode_index;
            f.squelch_on = w.on;
            f.squelch_norm = w.norm;
            f.squelch_text = w.text;
            f.gate_shown = w.gate_shown;
            f.gate_text = w.gate_text;
            f.gate_word = w.gate_word;
            f.gate_lit = w.gate_lit;
            f.squelch_tooltip = tr("Silences this demodulator's sound while the channel is quieter\nthan the threshold - so a speaker or a recording hears signals,\nnot the noise between them. Decoders are still given every sample.");
            break;
        }
        case pc::NodeKind::Sink: {
            const bool text = !n.inputs.empty() && n.inputs[0] == pc::PortType::Text;
            if (!text) {
                // A speaker: what it is playing, by the channel's name, and WHERE IT GOES - with its level.
                const pc::Node* ch = g.find(pc::channelFeeding(g, n.id));
                const LiveRow* dest = liveRow(live_, 2, n.id);
                if (ch == nullptr) {
                    f.lines.push_back(fl(tr("Wire a demodulator here to listen."), kToneMuted));
                    break;
                }
                f.lines.push_back(fl(ch->name, kTonePhosphor));
                if (dest != nullptr) {
                    f.lines.push_back(fl(dest->name, kToneMuted));
                    // A level bar: the newest block's peak, on the face's width.
                    f.level_shown = true;
                    f.level_norm = std::clamp(static_cast<float>(dest->v0), 0.0f, 1.0f);
                    if (!dest->detail.empty()) { f.lines_after.push_back(fl(dest->detail, kToneRust)); }
                } else {
                    f.lines.push_back(fl(tr("Waiting for its radio."), kToneMuted));
                }
                if (const LiveRow* derr = liveRow(live_, 5, n.id)) { f.lines_after.push_back(fl(derr->detail, kToneRust)); }
                break;
            }
            // Text out: the newest lines that fit, oldest at the top.
            const auto it = sink_lines_.queues().find(n.id);
            if (it == sink_lines_.queues().end() || it->second.empty()) {
                f.lines.push_back(fl(tr("Decoded lines appear here."), kToneMuted));
                break;
            }
            const int fit = std::max(1, static_cast<int>((pg::nodeSize(n).y - 40.0f) / 13.1f) - 1);
            const std::deque<std::string>& q = it->second;
            const std::size_t start = q.size() > static_cast<std::size_t>(fit) ? q.size() - static_cast<std::size_t>(fit) : 0u;
            // One row per line, cut at the face's edge rather than wrapped, so the newest lines stay in view at the bottom.
            for (std::size_t i = start; i < q.size(); ++i) { f.lines.push_back(fl(q[i], kTonePhosphor)); }
            break;
        }
        case pc::NodeKind::Display: {
            // The deferred face: its two sentences when they apply, and the plate otherwise.
            const pc::Node* feeder = nullptr;
            for (const pc::Wire& w : g.wires()) {
                if (w.to == n.id) {
                    feeder = g.find(w.from);
                    break;
                }
            }
            // OFF ITS OWN RADIO (0.99.17): the radio this part hangs off, not the receiver's.
            const pc::NodeId radioId = pc::radioOf(g, n.id);
            const auto rrun = lists_.radios.find(radioId);
            const bool radioOpen = rrun != lists_.radios.end() && rrun->second.open;
            const double rate = radioOpen ? rrun->second.rate_hz : 0.0;
            bool ok = false;
            if (feeder != nullptr && feeder->kind == pc::NodeKind::Radio && rate > 0.0) {
                ok = true;
            } else if (feeder != nullptr && feeder->kind == pc::NodeKind::Channel && pc::findChannel(lists_.plan, feeder->id) != nullptr) {
                ok = true;
            }
            if (!ok) {
                f.lines.push_back(fl(tr("Wire the radio or a channel here to see its spectrum."), kToneMuted));
            } else if (!radioOpen) {
                f.lines.push_back(fl(tr("No spectrum yet - its radio is not running."), kToneMuted));
            }
            break;
        }
        case pc::NodeKind::Map: {
            // A MAP WITH NOTHING WIRED IS STILL A MAP: the sentence stays until a decoder is wired in.
            const std::vector<std::string> sources = pc::mapSources(g, n.id, lists_.parts);
            if (sources.empty()) {
                f.lines.push_back(fl(cc::formatText(tr("Wire a decoder's map output here - ADS-B, AIS, APRS - up to %zu."), pc::kMapInputs), kToneMuted));
                break;
            }
            std::size_t air = 0, sea = 0, fixed = 0, other = 0, total = 0;
            for (const MapTrack& t : tracks_) {
                if (std::find(sources.begin(), sources.end(), t.plugin) == sources.end()) { continue; }
                ++total;
                switch (t.kind) {
                    case CASCADE_TRACK_AIRCRAFT: ++air; break;
                    case CASCADE_TRACK_VESSEL: ++sea; break;
                    case CASCADE_TRACK_STATION: ++fixed; break;
                    default: ++other; break;
                }
            }
            if (total == 0) {
                f.lines.push_back(fl(tr("no targets yet"), kToneAmber));
            } else {
                f.lines.push_back(fl(cc::formatText(tr("%zu aircraft  %zu vessels  %zu stations%s"), air, sea, fixed, other > 0 ? "  +" : ""), kToneAmber));
            }
            break;
        }
        case pc::NodeKind::Decoder: {
            // A text decoder's face is its output - the count and latest line the canvas draws. A PICTURE decoder's face is its picture, fitted and in proportion.
            const LiveRow* live = liveRow(live_, 6, n.id);
            const auto held = pictures_.find(n.id);
            if (live != nullptr && held != pictures_.end()) {
                f.have_picture = true;
                f.picture_id = held->second;
            }
            break;
        }
    }
    return f;
}

// =================================================================================================================================
// THE INSPECTOR (drawPatchRadioInspector, drawPatchSquelch, drawPatchSinkInspector, drawPatchMapInspector and drawPatchView 13117-13318)
// =================================================================================================================================

namespace {

// The Device combo: one row per PATCH_DEVICES row; ONE DEVICE, ONE RADIO - a device another Radio already has is shown, greyed, with who has it.
std::vector<SelectItem> deviceItemsOf(const pc::Node& n, const pc::Graph& g, const PatchLists& lists, std::uint32_t flags) {
    std::vector<SelectItem> items;
    for (const DeviceRow& c : lists.devices) {
        const pc::Node* holder = nullptr;
        for (const pc::Node& other : g.nodes()) {
            if (other.id != n.id && other.kind == pc::NodeKind::Radio && pc::sameDevice(other.device, c.key)) { holder = &other; }
        }
        SelectItem it;
        it.label = c.label;
        it.id = c.key;
        if (holder != nullptr) {
            std::string buf;
            cc::formatUtf8(buf, tr("  (used by %s)"), holder->name.c_str());
            it.label += buf;
            it.enabled = false;
        } else if ((flags & FOXAPI_RX_DEVICE_OPEN) != 0u && !lists.receiver_radio.empty() && pc::sameDevice(c.key, lists.receiver_radio)) {
            // IN USE BY THE RECEIVER IS NOT "NOT AVAILABLE": the receiver lends its radio to the patch when the patch starts, so the row is offered, and says what will happen to it.
            it.label += tr("  (the receiver's - lent to the patch when it starts)");
        }
        items.push_back(std::move(it));
    }
    return items;
}

// The Sound goes to combo: a WAV file, an MP3 file, the speakers, then each output device.
std::vector<SelectItem> sinkItemsOf(const PatchLists& lists) {
    std::vector<SelectItem> items;
    items.push_back({tr("A WAV file"), "wav", true, "", ""});
    const bool mp3 = cc::kMp3Supported;
    items.push_back({mp3 ? tr("An MP3 file") : tr("An MP3 file (needs Windows)"), "mp3", mp3, "", ""});
    items.push_back({tr("The speakers"), "speakers", true, "", ""});
    for (const std::string& name : lists.audio_devices) { items.push_back({name, pc::makeOutputDeviceKey(name), true, "", ""}); }
    return items;
}

std::vector<SelectItem> pluginItemsOf(const pc::Node& n, const PatchLists& lists) {
    const pc::PortType feed = n.inputs.empty() ? pc::PortType::Iq : n.inputs[0];
    std::vector<SelectItem> items;
    for (std::size_t i = 0; i < lists.parts.size(); ++i) {
        const pc::DecoderInfo& info = lists.parts[i];
        if (info.feed != feed) { continue; }
        SelectItem it;
        it.label = info.name;
        it.id = std::to_string(i);
        it.note = info.key;
        items.push_back(std::move(it));
    }
    return items;
}

}  // namespace

InspectorModel PatchPage::inspectorModel() const {
    InspectorModel m;
    const pc::Node* sel = selectedNode();
    m.folded = folded_;
    m.fold_tooltip = folded_ ? tr("Open the inspector: the selected node's settings") : tr("Fold the inspector away to give the canvas its width");
    if (sel == nullptr) { return m; }
    const pc::Node& n = *sel;
    const pc::Graph& g = draft_.graph();
    m.shown = true;
    m.node = static_cast<int>(n.id);
    m.kind = static_cast<int>(n.kind);
    m.kind_caption = kindCaption(n.kind);
    m.name = n.name;
    switch (n.kind) {
        case pc::NodeKind::Radio: {
            InspectorRadio& r = m.radio;
            r.device_items = deviceItemsOf(n, g, lists_, flags_);
            for (std::size_t i = 0; i < lists_.devices.size(); ++i) {
                if (lists_.devices[i].key == n.device) {
                    r.device_index = static_cast<int>(i);
                    break;
                }
            }
            r.device_label = pc::deviceLabelFor(lists_.devices, n.device);
            r.looking = (source_flags_ & FOXAPI_SRC_SCAN_PENDING) != 0u;
            r.look_enabled = !r.looking;
            r.centre_text = megaText(n.freqHz);
            const auto note = centre_notes_.find(n.id);
            if (note != centre_notes_.end()) { r.centre_note = note->second; }
            for (int i = 0; i < kRateCount; ++i) {
                SelectItem it;
                cc::formatUtf8(it.label, "%.3f MS/s", kRates[i] / 1e6);
                it.id = cc::formatText("%.17g", kRates[i]);
                r.rate_items.push_back(std::move(it));
                if (r.rate_index < 0 && std::fabs(kRates[i] - cascade::gui::radioRate(n)) < 1.0) { r.rate_index = i; }
            }
            cc::formatUtf8(r.rate_label, "%.3f MS/s", cascade::gui::radioRate(n) / 1e6);
            // A RECORDING'S RATE IS NOT A SETTING: it is the rate it was made at. Shown, and said, not offered.
            r.rate_fixed = pc::deviceSetsItsOwnRate(n.device);
            if (r.rate_fixed) { r.rate_fixed_text = cascade::gui::recordingRateText(cascade::gui::radioRate(n)); }
            // What it is actually doing.
            const auto it = lists_.radios.find(n.id);
            const RadioRow* row = it != lists_.radios.end() ? &it->second : nullptr;
            if (row != nullptr && row->open) {
                r.lines.push_back(fl(cc::formatText(tr("running %.3f MS/s"), row->rate_hz / 1e6), kToneAmber));
                r.lines.push_back(fl(cc::formatText(tr("at %.6f MHz"), row->centre_hz / 1e6), kToneAmber));
            } else if (row != nullptr && row->pending) {
                r.lines.push_back(fl(tr("opening the device..."), kToneMuted));
            }
            if (row != nullptr && !row->note.empty()) { r.lines.push_back(fl(row->note, kToneRust)); }
            r.lines.push_back(fl(cc::formatText(tr("Up to %zu radios, each its own device. While this page is open the receiver runs on the signal generator; closing it gives the radio back."), pc::kMaxRadios), kToneMuted));
            break;
        }
        case pc::NodeKind::Map: {
            std::size_t wired = 0;
            for (const pc::Wire& w : g.wires()) {
                if (w.to == n.id) { ++wired; }
            }
            cc::formatUtf8(m.map_wired, tr("%zu of %zu inputs wired"), wired, pc::kMapInputs);
            for (const std::string& src : pc::mapSources(g, n.id, lists_.parts)) {
                std::size_t count = 0;
                for (const MapTrack& t : tracks_) {
                    if (t.plugin == src) { ++count; }
                }
                m.map_sources.push_back({std::to_string(count), src});
            }
            cc::formatUtf8(m.map_help, tr("Wire the map output of up to %zu decoders here - aircraft from one radio, ships from another - and they share this map. Drag it to pan; zoom with the + and - keys in its corner, or the mouse wheel."), pc::kMapInputs);
            break;
        }
        case pc::NodeKind::Sink:
            if (!n.inputs.empty() && n.inputs[0] == pc::PortType::Audio) {
                m.audio_sink = true;
                InspectorSink& s = m.sink;
                const std::string key = n.device.empty() ? std::string("wav") : n.device;
                s.items = sinkItemsOf(lists_);
                for (std::size_t i = 0; i < s.items.size(); ++i) {
                    if (s.items[i].id == key) {
                        s.index = static_cast<int>(i);
                        break;
                    }
                }
                switch (pc::outputKind(key)) {
                    case pc::OutputKind::Wav: s.label = tr("A WAV file"); break;
                    case pc::OutputKind::Mp3: s.label = tr("An MP3 file"); break;
                    case pc::OutputKind::Speakers: s.label = tr("The speakers"); break;
                    case pc::OutputKind::Device: s.label = pc::outputDeviceName(key); break;
                }
                if (const LiveRow* d = liveRow(live_, 2, n.id)) {
                    s.lines.push_back(fl(d->name, kToneAmber));
                    s.lines.push_back(fl(cc::formatText(tr("%.1f s"), d->v1 / pc::kOutRateHz), kToneAmber));
                    if (!d->detail.empty()) { s.lines.push_back(fl(d->detail, kToneRust)); }
                }
                if (const LiveRow* err = liveRow(live_, 5, n.id)) { s.lines.push_back(fl(err->detail, kToneRust)); }
                if (pc::outputKind(key) == pc::OutputKind::Wav || pc::outputKind(key) == pc::OutputKind::Mp3) {
                    cc::formatUtf8(s.files_note, tr("Files go in %s, one per speaker, named after it."), lists_.recorder_dir.c_str());
                }
            } else {
                m.help = tr("Shows the lines from every decoder wired to it, and sends them to the Decoder output window.");
            }
            break;
        case pc::NodeKind::Channel:
            // IN MHz, because that is how every other frequency in this application is entered, and stored in Hz because that is what the arithmetic wants.
            m.freq_text = megaText(n.freqHz);
            break;
        case pc::NodeKind::Demod: {
            const SquelchWords w = squelchWords(n, live_);
            m.mode_index = w.mode_index;
            m.squelch_on = w.on;
            m.squelch_norm = w.norm;
            m.squelch_text = w.text;
            m.gate_shown = w.gate_shown;
            m.gate_text = w.gate_text;
            m.gate_word = w.gate_word;
            m.gate_lit = w.gate_lit;
            break;
        }
        case pc::NodeKind::Decoder: {
            InspectorDecoder& d = m.decoder;
            // WHICH PLUGIN, chosen from those installed that take this node's input. This is also the repair path: a plugin's key is its file name, which carries its version, so after an
            // upgrade a saved node names a file that is gone and says so - and is re-pointed here in one click rather than rebuilt.
            const pc::PortType feed = n.inputs.empty() ? pc::PortType::Iq : n.inputs[0];
            d.plugin_items = pluginItemsOf(n, lists_);
            for (std::size_t i = 0; i < d.plugin_items.size(); ++i) {
                if (d.plugin_items[i].note == n.plugin) {
                    d.plugin_index = static_cast<int>(i);
                    break;
                }
            }
            d.plugin_label = n.plugin.empty() ? std::string(tr("(none)")) : n.plugin;
            for (const pc::DecoderInfo& info : lists_.parts) {
                if (info.key == n.plugin && info.feed == feed) {
                    d.plugin_label = info.name;
                    break;
                }
            }
            // Two whole sentences, not a phrase dropped into "%s input.".
            d.feed_note = feed == pc::PortType::Iq ? tr("I/Q - wire it from a channel, or from the radio for the whole band input.") : tr("Audio - wire it from a demodulator input.");
            // What the plan will actually hand it. A number, so amber.
            for (const pc::DecoderPlan& dp : lists_.plan.decoders) {
                if (dp.node != n.id) { continue; }
                std::string t;
                if (dp.rateHz != dp.inRateHz) {
                    cc::formatUtf8(t, tr("fed %.0f Hz (from %.0f)"), dp.rateHz, dp.inRateHz);
                } else {
                    cc::formatUtf8(t, tr("fed %.0f Hz"), dp.rateHz);
                }
                d.fed.push_back(t);
                if (dp.source != pc::DecoderSource::Audio) {
                    std::string c;
                    cc::formatUtf8(c, tr("centred on %.6f MHz"), dp.centreHz / 1e6);
                    d.fed.push_back(c);
                }
                break;
            }
            if (!pc::decoderIsShown(g, n.id)) { d.shown_note = tr("Wire its output to a Text out part to read what it decodes in the Decoder output window."); }
            // THE PLUGIN'S SETTINGS FORM, on the node that runs it: the kit's, for a module that is loaded and has settings.
            if (kit_ != nullptr) {
                std::string modKey = n.plugin;
                const std::string suffix = pc::kImageKeySuffix;
                if (modKey.size() > suffix.size() && endsWith(modKey, suffix)) { modKey.resize(modKey.size() - suffix.size()); }
                bool loaded = false;
                for (const foxsdr::slintkit::PluginRow& r : kit_->rows) {
                    if (r.file == modKey) {
                        loaded = r.loaded;
                        break;
                    }
                }
                const foxsdr::slintkit::SettingsSet* set = nullptr;
                for (const foxsdr::slintkit::SettingsSet& s : kit_->settings) {
                    if (s.file == modKey) {
                        set = &s;
                        break;
                    }
                }
                if (loaded && set != nullptr && !set->fields.empty()) {
                    d.has_settings = true;
                    d.settings_module = modKey;
                    d.settings = set->fields;
                }
            }
            break;
        }
        case pc::NodeKind::Display:
            m.help = tr("This node has nothing to set yet.");
            break;
    }
    return m;
}

// =================================================================================================================================
// THE CALLBACKS -> EFFECTS
// =================================================================================================================================

void PatchPage::partPressed(int kind, Submitter& sub) {
    if (kind < 0 || kind > 6) { return; }
    // A PRESS IS PLACED once the canvas has a size: a new node lands inside the canvas in view (newPartPosition), not at the world origin, which after any pan is off the screen.
    const pg::Vec2 at = pg::newPartPosition(canvas_.drawView(), canvas_.canvas_w, canvas_.canvas_h, nodes_placed_);
    pc::Graph& g = draft_.graph();
    if (kParts[kind].kind == pc::NodeKind::Radio) {
        // A Radio part is its own case. TWO PASSES, NOT ONE: the node is created NOW with no device, so the press shows, the scan is asked for, and the device is chosen when the scan's
        // answer arrives (finishRadioParts) - by which time the list exists. Two Radio presses before the answer take two different devices.
        draft_.sync(look_, lists_, canvas_.interacting());
        const pc::NodeId made = g.addNode(pc::NodeKind::Radio, partLabelEnglish(kind), kParts[kind].feed, at.x, at.y);
        if (made == pc::kNoNode) { return; }   // five radios: nothing
        const std::uint64_t scan = sub.submit(opScanNativeOnly());
        if (scan != 0u) {
            draft_.addWait(made, scan);
        } else if (pc::Node* n = g.mutableNode(made)) {
            // The session would not take the scan: the list as it is.
            n->device = defaultDeviceKey(g, lists_);
        }
    } else {
        // The name is the ENGLISH label: it is stored in the patch as data.
        g.addNode(kParts[kind].kind, partLabelEnglish(kind), kParts[kind].feed, at.x, at.y);
    }
    ++nodes_placed_;
    draft_.dirty = true;
    // To the engine before the compile, as the part always was.
    draft_.commit(look_, sub);
}

void PatchPage::decoderPressed(int index, Submitter& sub) {
    if (index < 0 || static_cast<std::size_t>(index) >= lists_.parts.size()) { return; }
    const pc::DecoderInfo& info = lists_.parts[static_cast<std::size_t>(index)];
    const pg::Vec2 at = pg::newPartPosition(canvas_.drawView(), canvas_.canvas_w, canvas_.canvas_h, nodes_placed_);
    const pc::NodeId made = draft_.graph().addNode(pc::NodeKind::Decoder, info.name, info.feed, at.x, at.y);
    if (pc::Node* n = draft_.graph().mutableNode(made)) { n->plugin = info.key; }
    ++nodes_placed_;
    draft_.dirty = true;
    draft_.commit(look_, sub);
}

void PatchPage::faceSwitch(int node) {
    pc::Node* n = draft_.graph().mutableNode(static_cast<pc::NodeId>(node));
    if (n == nullptr || n->kind != pc::NodeKind::Radio) { return; }
    n->on = !n->on;
    draft_.dirty = true;
    cc::diagLogf("patch: radio node %u switched %s", static_cast<unsigned>(n->id), n->on ? "on" : "off");
}

bool PatchPage::setRadioCentre(pc::Node& n, double airHz) {
    // THE RADIO decides, not the sign of the air figure: a node's centre is an AIR frequency and may be carried in below 0 Hz through an up-converter, so refusing everything <= 0 here
    // made such a centre impossible to type back in. What must hold is that the radio behind the node's converter is told something above 0 Hz.
    // THE CONVERTER IN FORCE for the node's device is the table's (fact.patch.conv.<node>, a pass old). It speaks for the device the ENGINE has the node on: a device chosen a moment ago
    // and not yet applied has none published, and is judged as a radio with no converter until the pass has caught up.
    cc::ConverterSetting conv;
    {
        const auto known = lists_.conv.find(n.id);
        const pc::Node* published = look_.graph.find(n.id);
        if (known != lists_.conv.end() && published != nullptr && pc::sameDevice(published->device, n.device)) { conv = known->second; }
    }
    if (!cc::radioCentreTakeable(conv, airHz)) {
        std::string msg;
        if (cc::converterActive(conv)) {
            cc::formatUtf8(msg, tr("%s is out of reach through the %s: the radio would have to tune to 0 Hz or below."), cc::converterHzText(airHz).c_str(), converterNameText(conv).c_str());
        } else {
            cc::formatUtf8(msg, tr("A radio cannot tune to %s - type a centre above 0 Hz."), cc::converterHzText(airHz).c_str());
        }
        centre_notes_[n.id] = msg;
        return false;
    }
    n.freqHz = airHz;
    n.centreChosen = true;   // 0 Hz on the air is a centre too
    centre_notes_.erase(n.id);
    draft_.dirty = true;
    return true;
}

void PatchPage::faceCentre(int node, const std::string& mhz) {
    double v = 0.0;
    pc::Node* n = draft_.graph().mutableNode(static_cast<pc::NodeId>(node));
    if (n == nullptr || n->kind != pc::NodeKind::Radio || !parseMhz(mhz, v)) { return; }
    setRadioCentre(*n, v * 1e6);
}
void PatchPage::inspectorCentre(const std::string& mhz) {
    double v = 0.0;
    pc::Node* n = selectedNode();
    if (n == nullptr || n->kind != pc::NodeKind::Radio || !parseMhz(mhz, v)) { return; }
    setRadioCentre(*n, v * 1e6);
}

namespace {
bool setFreq(pc::Node* n, const std::string& mhz, PatchDraft& d) {
    double v = 0.0;
    if (n == nullptr || n->kind != pc::NodeKind::Channel || !parseMhz(mhz, v)) { return false; }
    n->freqHz = v * 1e6;   // no validation: the channel's problems say what is wrong
    d.dirty = true;
    return true;
}
bool setMode(pc::Node* n, int index, PatchDraft& d) {
    if (n == nullptr || n->kind != pc::NodeKind::Demod) { return false; }
    n->mode = std::clamp(index, 0, 7);
    d.dirty = true;
    return true;
}
bool setSquelchOn(pc::Node* n, bool on, PatchDraft& d) {
    if (n == nullptr || n->kind != pc::NodeKind::Demod) { return false; }
    n->squelch = on;
    d.dirty = true;
    return true;
}
bool setSquelch(pc::Node* n, float norm, PatchDraft& d) {
    if (n == nullptr || n->kind != pc::NodeKind::Demod || !std::isfinite(norm)) { return false; }
    // ALWAYS CLAMPED: -120..0 dB is not just the slider's travel, it is every squelch the graph can hold (the loader and the engine refuse anything else).
    n->squelchDb = std::clamp(pc::kSquelchMinDb + norm * (pc::kSquelchMaxDb - pc::kSquelchMinDb), pc::kSquelchMinDb, pc::kSquelchMaxDb);
    d.dirty = true;
    return true;
}
}  // namespace

void PatchPage::faceFreq(int node, const std::string& mhz) { setFreq(draft_.graph().mutableNode(static_cast<pc::NodeId>(node)), mhz, draft_); }
void PatchPage::faceMode(int node, int index) { setMode(draft_.graph().mutableNode(static_cast<pc::NodeId>(node)), index, draft_); }
void PatchPage::faceSquelchOn(int node, bool on) { setSquelchOn(draft_.graph().mutableNode(static_cast<pc::NodeId>(node)), on, draft_); }
void PatchPage::faceSquelch(int node, float norm) { setSquelch(draft_.graph().mutableNode(static_cast<pc::NodeId>(node)), norm, draft_); }
void PatchPage::inspectorFreq(const std::string& mhz) { setFreq(selectedNode(), mhz, draft_); }
void PatchPage::inspectorMode(int index) { setMode(selectedNode(), index, draft_); }
void PatchPage::inspectorSquelchOn(bool on) { setSquelchOn(selectedNode(), on, draft_); }
void PatchPage::inspectorSquelch(float norm) { setSquelch(selectedNode(), norm, draft_); }

void PatchPage::inspectorName(const std::string& text) {
    pc::Node* n = selectedNode();
    if (n == nullptr) { return; }
    n->name = cutBytes(text, kNameBytes);
    draft_.dirty = true;
}

void PatchPage::inspectorDevice(int index) {
    pc::Node* n = selectedNode();
    if (n == nullptr || n->kind != pc::NodeKind::Radio || index < 0) { return; }
    const std::vector<SelectItem> items = deviceItemsOf(*n, draft_.graph(), lists_, flags_);
    if (static_cast<std::size_t>(index) >= items.size() || !items[static_cast<std::size_t>(index)].enabled) { return; }
    const DeviceRow& c = lists_.devices[static_cast<std::size_t>(index)];
    n->device = c.key;
    // A recording's rate is its own: the node takes it now, so the patch is planned at the rate it will run at.
    if (pc::isIqFileKey(c.key) && c.rate_hz > 0.0) { n->rateHz = c.rate_hz; }
    draft_.dirty = true;
}

void PatchPage::inspectorRate(int index) {
    pc::Node* n = selectedNode();
    if (n == nullptr || n->kind != pc::NodeKind::Radio || index < 0 || index >= kRateCount || pc::deviceSetsItsOwnRate(n->device)) { return; }
    n->rateHz = kRates[index];
    draft_.dirty = true;
}

void PatchPage::inspectorPlugin(int index) {
    pc::Node* n = selectedNode();
    if (n == nullptr || n->kind != pc::NodeKind::Decoder || index < 0) { return; }
    const std::vector<SelectItem> items = pluginItemsOf(*n, lists_);
    if (static_cast<std::size_t>(index) >= items.size()) { return; }
    n->plugin = items[static_cast<std::size_t>(index)].note;
    draft_.dirty = true;
}

void PatchPage::inspectorSink(int index) {
    pc::Node* n = selectedNode();
    if (n == nullptr || n->kind != pc::NodeKind::Sink || n->inputs.empty() || n->inputs[0] != pc::PortType::Audio || index < 0) { return; }
    const std::vector<SelectItem> items = sinkItemsOf(lists_);
    if (static_cast<std::size_t>(index) >= items.size() || !items[static_cast<std::size_t>(index)].enabled) { return; }
    n->device = items[static_cast<std::size_t>(index)].id;
    draft_.dirty = true;
}

void PatchPage::inspectorSize(int step) {
    pc::Node* n = selectedNode();
    if (n == nullptr) { return; }
    switch (step) {
        case 0: pg::scaleNodeSize(*n, 0.8f); break;
        case 1: pg::scaleNodeSize(*n, 1.25f); break;
        case 2: pg::resetNodeSize(*n); break;
        default: return;
    }
    draft_.dirty = true;
}

void PatchPage::inspectorRemove() {
    if (canvas_.ui.selected == pc::kNoNode || selectedNode() == nullptr) { return; }
    draft_.graph().removeNode(canvas_.ui.selected);
    canvas_.ui.selected = pc::kNoNode;
    draft_.dirty = true;
}

// =================================================================================================================================
// THE BLUEPRINTS (app_window_patch_blueprints.cpp)
// =================================================================================================================================

namespace {
// What a blueprint is called in the list: a picture decoder says so, as its key in the parts bin does.
std::string shownTitle(const pc::Blueprint& bp) {
    if (!bp.decoder.image) { return bp.title; }
    std::string out;
    cc::formatUtf8(out, tr("%s (picture)"), bp.title.c_str());
    return out;
}

// THE AIRPORTS A "LISTEN TO THE TOWER" BLUEPRINT IS OFFERED FOR: every airport the user has looked up in the Airband section - its group is in the frequency list, named airportGroupName() -
// and, with a receiver position, the kTowerNearest nearest it. Each once, looked-up first.
std::vector<const cc::Airport*> towerAirports(const std::vector<std::string>& groups, bool rxSet, double lat, double lon) {
    std::vector<const cc::Airport*> out;
    const auto have = [&out](const cc::Airport* a) { return std::find(out.begin(), out.end(), a) != out.end(); };
    for (const std::string& g : groups) {
        // The group starts with the airport's ident: "KORD Chicago O'Hare ...".
        const std::string ident = g.substr(0, g.find(' '));
        for (const cc::Airport* a : cc::findAirports(cc::airbandTable(), ident)) {
            if (cc::airportGroupName(*a) == g && !have(a)) { out.push_back(a); }
        }
    }
    if (rxSet) {
        for (const auto& [a, km] : cc::nearestAirports(cc::airbandTable(), lat, lon, kTowerNearest)) {
            (void)km;
            if (!have(a)) { out.push_back(a); }
        }
    }
    return out;
}

// An airport's tower frequencies, as a listen blueprint's channels: its TWR frequencies, or - a field with no tower - its CTAF, where the traffic there talks.
std::vector<pc::ListenChannel> towerChannels(const cc::Airport& a) {
    std::vector<pc::ListenChannel> out;
    for (const char* type : {"TWR", "CTAF"}) {
        for (const cc::AirbandFreq& f : a.freqs) {
            if (f.type != type) { continue; }
            std::string name;
            cc::formatUtf8(name, "%s %.3f", f.type.c_str(), static_cast<double>(f.channelKhz) / 1000.0);
            out.push_back({f.hz, name});
        }
        if (!out.empty()) { break; }
    }
    return out;
}
}  // namespace

std::vector<pc::Blueprint> PatchPage::buildBlueprints(TableReader& table) const {
    // Each plugin's presets, from the table's PRESETS list, keyed by the module key the catalogue names parts by. A PRESETS entry names its plugin by the plugin's NAME (detail), as the parts
    // do, so the module key of every part whose name it is gets the preset. value[0] is the frequency, value[1] the ABI demod code, value[3] the preset's own sample rate (0 when none).
    struct Row {
        std::string plugin;
        pc::BlueprintTune tune;
    };
    std::vector<Row> rows;
    eachItem(table, FOXAPI_LIST_PRESETS, [&rows](int, const FoxListItem& it) {
        Row r;
        r.plugin = it.detail;
        char label[CASCADE_PRESET_LABEL_CHARS];
        std::snprintf(label, sizeof(label), "%s", it.name);
        r.tune.label = cascade::gui::presetLabel(label, r.plugin);
        r.tune.freqHz = it.value[0];
        r.tune.mode = pc::demodIndexForAbi(static_cast<std::uint32_t>(it.value[1]));
        r.tune.rateHz = it.value[3] > 0.0 ? it.value[3] : 0.0;
        rows.push_back(std::move(r));
    });
    std::map<std::string, std::vector<pc::BlueprintTune>> tunes;
    for (const pc::DecoderInfo& d : lists_.parts) {
        std::string module = d.key;
        if (d.image && endsWith(module, pc::kImageKeySuffix)) { module.erase(module.size() - std::strlen(pc::kImageKeySuffix)); }
        if (tunes.count(module) != 0) { continue; }
        std::vector<pc::BlueprintTune>& list = tunes[module];
        for (const Row& r : rows) {
            if (r.plugin == d.name) { list.push_back(r.tune); }
        }
    }
    std::vector<pc::Blueprint> out = pc::listBlueprints(lists_.parts, tunes);
    // ...then LISTEN TO THE TOWER, one per airport, after the plugins.
    const bool rxSet = (flags_ & FOXAPI_RX_POSITION_SET) != 0u;
    for (const cc::Airport* a : towerAirports(lists_.bookmark_groups, rxSet, rx_lat_, rx_lon_)) {
        pc::Blueprint bp;
        bp.listen = towerChannels(*a);
        if (bp.listen.empty()) { continue; }
        cc::formatUtf8(bp.title, tr("Listen to the tower: %s"), cc::airportGroupName(*a).c_str());
        out.push_back(std::move(bp));
    }
    return out;
}

void PatchPage::blueprintsOpened(TableReader& table) { blueprints_ = buildBlueprints(table); }

void PatchPage::blueprintPicked(int index, Submitter& sub) {
    if (index < 0 || static_cast<std::size_t>(index) >= blueprints_.size()) { return; }
    const pc::Blueprint bp = blueprints_[static_cast<std::size_t>(index)];
    const std::string title = shownTitle(bp);
    draft_.sync(look_, lists_, canvas_.interacting());
    pc::Graph& g = draft_.graph();

    // A NEW radio starts on a device nothing else is using, as one from the parts bin does - and finding one reads the native list, so that is asked for only when no radio on the canvas is
    // free to be taken over. ON THE TABLE THE SCAN IS TWO PASSES: the radio is placed NOW with no device, the scan is asked for, and finishRadioParts gives it the first free device when
    // the answer arrives - by which time the list exists.
    std::uint64_t scanTicket = 0;
    const bool needsNewRadio = pc::unwiredRadio(g) == pc::kNoNode && g.count(pc::NodeKind::Radio) < pc::kMaxRadios;
    if (needsNewRadio) { scanTicket = sub.submit(opScanNativeOnly()); }
    const pc::Placed placed = pc::placeBlueprint(g, bp, std::string(), kBlueprintDefaultRateHz);
    if (placed.result == pc::Place::RadiosFull) {
        cc::formatUtf8(blueprint_note_, tr("Every radio is in use, and a patch has at most %zu. Free a radio to place \"%s\"."), pc::kMaxRadios, title.c_str());
        return;
    }
    if (!placed.reusedRadio && placed.radio != pc::kNoNode) {
        if (scanTicket != 0u) {
            draft_.addWait(placed.radio, scanTicket);
        } else if (pc::Node* n = g.mutableNode(placed.radio)) {
            // The session would not take the scan: the list as it is.
            n->device = defaultDeviceKey(g, lists_);
        }
    }

    // THE NEW WORKFLOW FITTED TO THE CANVAS - the Fit key's own arithmetic (fitView), asked of a graph holding only what this pick placed, so the whole chain from radio to Text out is in
    // view and the user's other nodes are not what decides the zoom. Nothing is selected: a selection opens the inspector, which would cover the right-hand end of the very chain just placed.
    if (canvas_.canvas_w > 0.0f && canvas_.canvas_h > 0.0f) {
        pc::Graph only;
        std::vector<pc::NodeId> ids = placed.added;
        if (placed.reusedRadio) { ids.push_back(placed.radio); }
        for (const pc::NodeId id : ids) {
            const pc::Node* n = g.find(id);
            if (n == nullptr) { continue; }
            if (pc::Node* c = only.mutableNode(only.addNode(n->kind, n->name))) {
                c->x = n->x;
                c->y = n->y;
                c->w = n->w;
                c->h = n->h;
            }
        }
        canvas_.ui.view = pg::fitView(only, canvas_.canvas_w, canvas_.canvas_h, 1.0f);
    }
    canvas_.ui.selected = pc::kNoNode;
    draft_.dirty = true;
    // To the engine before the compile, as the part always was.
    draft_.commit(look_, sub);
    cc::formatUtf8(blueprint_note_, placed.noFrequency ? tr("Placed \"%s\". It has no preset frequency - type one in before pressing START.") : tr("Placed \"%s\". Press START to run it."), title.c_str());
    // A tower whose frequencies span more than one radio's band: the ones left out are counted, so the user knows the patch is not all of them.
    if (placed.listenDropped > 0) {
        std::string more;
        cc::formatUtf8(more, tr("%zu of its tower frequencies are outside this radio's band and were left out."), placed.listenDropped);
        blueprint_note_ += ' ';
        blueprint_note_ += more;
    }
}

// ---- THE PRESETS ----
// (app_window_patch_presets.cpp) NOTHING ABOUT A PRESET IS LOGGED: the application log's tail rides in a crash report the user chooses to send, and PRIVACY.md promises that nothing about a
// preset leaves the machine. Every change is written to the config at once: the presets and the previous-patch slot are the Interface half of the settings.

void PatchPage::persistPresets() {
    if (config_ != nullptr) { config_->savePresets(store_.list(), store_.previous()); }
}

std::string PatchPage::presetSentence(cc::PatchPresetStatus st, const std::string& name) const {
    std::string out;
    switch (st) {
        case cc::PatchPresetStatus::Saved:
        case cc::PatchPresetStatus::Overwritten:
            cc::formatUtf8(out, tr("Saved \"%s\"."), name.c_str());
            break;
        case cc::PatchPresetStatus::Renamed:
            cc::formatUtf8(out, tr("Renamed to \"%s\"."), name.c_str());
            break;
        case cc::PatchPresetStatus::Deleted:
            cc::formatUtf8(out, tr("Deleted \"%s\"."), name.c_str());
            break;
        case cc::PatchPresetStatus::Exists:
            out = tr("Another preset already has that name.");
            break;
        case cc::PatchPresetStatus::EmptyName:
            out = tr("Type a name for the preset first.");
            break;
        case cc::PatchPresetStatus::ReservedName:
            out = tr("That name is kept for the previous patch. Choose another.");
            break;
        case cc::PatchPresetStatus::ListFull:
            cc::formatUtf8(out, tr("There are already %zu presets. Delete one to make room."), cc::kMaxPatchPresets);
            break;
        case cc::PatchPresetStatus::TooLarge:
            cc::formatUtf8(out, tr("This patch is too large to keep as a preset (over %zu KB)."), cc::kMaxPatchPresetTextBytes / 1024u);
            break;
        case cc::PatchPresetStatus::NotAPatch:
            out = tr("That preset is not a patch this version can read.");
            break;
        case cc::PatchPresetStatus::NotFound:
            out = tr("That preset is no longer there.");
            break;
    }
    return out;
}

bool PatchPage::replacePatch(const std::string& text, Submitter& sub) {
    // PARSED FIRST: a text that is not a patch must change nothing at all - not stop a running patch, not clear a face.
    const pc::LoadResult pr = pc::parse(text);
    if (!pr.ok) { return false; }
    // THE PATCH IS STOPPED THROUGH ITS OWN STOP, ahead of the document (the session's commands are applied in the order they were sent): a load always leaves the patch stopped. A STOP the
    // session cannot take is a load that did not happen: the canvas is untouched.
    if (sub.submit(opRun(FOXAPI_RX_PATCH_RUNNING)) == 0u) { return false; }
    // To the engine as a NEW DOCUMENT: its own epoch, no edit of the old patch merged into it; the draft is the document at once.
    if (!draft_.loadDocument(pr.graph, sub)) { return false; }
    canvas_.ui.view.pan = pg::Vec2{pr.panX, pr.panY};
    canvas_.ui.view.zoom = pr.zoom;
    canvas_.reconcile(draft_);
    // A fresh graph numbers its nodes from 1 again, so anything remembered by node id would attach to whichever new node took the number. Dropped with the graph it described.
    sink_lines_.clear();
    pictures_.clear();
    centre_notes_.clear();
    forget_seen_ = draft_.forgetSerial();
    // What the user asked for, empty or not: no starter radio over it.
    seeded_ = true;
    return true;
}

bool PatchPage::loadPreset(const std::string& text, Submitter& sub) {
    // What is on the canvas NOW, taken before it goes: the slot's whole point.
    const std::string outgoing = documentOf(draft_.graph(), canvas_.ui.view);
    if (!replacePatch(text, sub)) { return false; }
    store_.keepPrevious(outgoing);
    return true;
}

void PatchPage::loadSelected(Submitter& sub) {
    // Copied, not referenced: the load refills the previous-patch slot, and loading the slot itself must not read a string it is overwriting.
    std::string text;
    std::string label;
    if (preset_sel_previous_) {
        text = store_.previous();
        label = tr("(previous patch)");
    } else {
        const int i = store_.find(preset_sel_);
        if (i < 0) {
            preset_note_ = presetSentence(cc::PatchPresetStatus::NotFound, preset_sel_);
            preset_note_good_ = false;
            return;
        }
        text = store_.list()[static_cast<std::size_t>(i)].text;
        label = store_.list()[static_cast<std::size_t>(i)].name;
    }
    if (!loadPreset(text, sub)) {
        preset_note_ = presetSentence(cc::PatchPresetStatus::NotAPatch, label);
        preset_note_good_ = false;
        return;
    }
    persistPresets();
    cc::formatUtf8(preset_note_, tr("Loaded \"%s\". Press START to run it."), label.c_str());
    preset_note_good_ = true;
}

void PatchPage::presetsRow(int index) {
    const bool hasPrevious = !store_.previous().empty();
    if (index < 0) { return; }
    if (hasPrevious && index == 0) {
        // THE PREVIOUS-PATCH SLOT, first and muted: not one of the user's presets, but the one mis-click it exists to undo.
        preset_sel_previous_ = true;
        preset_sel_.clear();
        return;
    }
    const std::size_t i = static_cast<std::size_t>(index) - (hasPrevious ? 1u : 0u);
    if (i >= store_.list().size()) { return; }
    preset_sel_ = store_.list()[i].name;
    preset_sel_previous_ = false;
}

void PatchPage::presetsRowActivated(int index, Submitter& sub) {
    presetsRow(index);
    presetsLoad(sub);
}

void PatchPage::presetsLoad(Submitter& sub) {
    if (!(preset_sel_previous_ || !preset_sel_.empty())) { return; }
    loadSelected(sub);
}

void PatchPage::presetsSaveAs() {
    const bool namedSelected = !preset_sel_previous_ && !preset_sel_.empty();
    preset_edit_ = 1;
    preset_name_ = namedSelected ? preset_sel_ : std::string();
    preset_focus_name_ = true;
}

void PatchPage::presetsRename() {
    if (preset_sel_previous_ || preset_sel_.empty()) { return; }
    preset_edit_ = 2;
    preset_name_ = preset_sel_;
    preset_focus_name_ = true;
}

void PatchPage::presetsDelete() {
    if (preset_sel_previous_ || preset_sel_.empty()) { return; }
    preset_pending_ = preset_sel_;
    preset_ask_delete_ = true;
}

void PatchPage::presetsName(const std::string& text) { preset_name_ = text; }

void PatchPage::presetsCancel() { preset_edit_ = 0; }

void PatchPage::presetsGo() {
    if (preset_edit_ == 0) { return; }
    preset_focus_name_ = false;
    const std::string name = cc::cleanPatchPresetName(preset_name_);
    if (preset_edit_ == 1) {
        // Never an overwrite from here: a name already taken asks.
        const cc::PatchPresetStatus st = store_.save(name, documentOf(draft_.graph(), canvas_.ui.view), false);
        if (st == cc::PatchPresetStatus::Exists) {
            preset_pending_ = name;
            preset_ask_replace_ = true;
            return;
        }
        preset_note_ = presetSentence(st, name);
        preset_note_good_ = st == cc::PatchPresetStatus::Saved;
        if (st == cc::PatchPresetStatus::Saved) {
            preset_edit_ = 0;
            preset_sel_ = name;
            preset_sel_previous_ = false;
            persistPresets();
        }
    } else {
        const cc::PatchPresetStatus st = store_.rename(preset_sel_, name);
        preset_note_ = presetSentence(st, name);
        preset_note_good_ = st == cc::PatchPresetStatus::Renamed;
        if (st == cc::PatchPresetStatus::Renamed) {
            preset_edit_ = 0;
            preset_sel_ = name;
            persistPresets();
        }
    }
}

void PatchPage::presetsReplace(bool yes) {
    preset_ask_replace_ = false;
    if (!yes) { return; }
    const cc::PatchPresetStatus st = store_.save(preset_pending_, documentOf(draft_.graph(), canvas_.ui.view), true);
    preset_note_ = presetSentence(st, preset_pending_);
    preset_note_good_ = st == cc::PatchPresetStatus::Overwritten || st == cc::PatchPresetStatus::Saved;
    if (preset_note_good_) {
        preset_edit_ = 0;
        preset_sel_ = preset_pending_;
        preset_sel_previous_ = false;
        persistPresets();
    }
}

void PatchPage::presetsDeleteConfirm(bool yes) {
    preset_ask_delete_ = false;
    if (!yes) { return; }
    const cc::PatchPresetStatus st = store_.remove(preset_pending_);
    preset_note_ = presetSentence(st, preset_pending_);
    preset_note_good_ = st == cc::PatchPresetStatus::Deleted;
    if (st == cc::PatchPresetStatus::Deleted) { persistPresets(); }
}

PatchPresetsModel PatchPage::presetsModel() const {
    PatchPresetsModel m;
    const bool hasPrevious = !store_.previous().empty();
    // A selection whose row has gone (deleted, or renamed from under it) is forgotten rather than pointing at nothing.
    const bool selPrevious = preset_sel_previous_ && hasPrevious;
    const std::string sel = !preset_sel_.empty() && store_.find(preset_sel_) >= 0 ? preset_sel_ : std::string();
    if (hasPrevious) { m.rows.push_back({tr("(previous patch)"), true, selPrevious}); }
    for (const cc::PatchPreset& p : store_.list()) { m.rows.push_back({p.name, false, !selPrevious && cc::patchPresetNamesMatch(p.name, sel)}); }
    m.empty_shown = store_.list().empty() && !hasPrevious;
    m.empty_text = tr("No presets yet. SAVE AS... keeps the patch on the canvas under a name.");
    const bool anySelected = selPrevious || !sel.empty();
    const bool namedSelected = !selPrevious && !sel.empty();
    m.load_enabled = anySelected;
    m.rename_enabled = namedSelected;
    m.delete_enabled = namedSelected;
    m.edit = preset_edit_;
    m.name_draft = preset_name_;
    m.note = preset_note_;
    m.note_good = preset_note_good_;
    m.ask_replace = preset_ask_replace_;
    if (preset_ask_replace_) { cc::formatUtf8(m.replace_text, tr("A preset called \"%s\" already exists.\nReplace it with the patch on the canvas?"), preset_pending_.c_str()); }
    m.ask_delete = preset_ask_delete_;
    if (preset_ask_delete_) { cc::formatUtf8(m.delete_text, tr("Delete the preset \"%s\"? This cannot be undone."), preset_pending_.c_str()); }
    m.focus_name = preset_focus_name_;
    m.previous_tooltip = tr("The patch the last load replaced. Load it to undo that load.");
    return m;
}
// ---- END OF THE PRESETS ----

}  // namespace foxsdr::slintpatch
