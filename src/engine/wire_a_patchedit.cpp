// wire_a_patchedit.cpp - WIRING LANE wire-a, second pass: the patch page's edit history (undo / redo), selection, copy / paste and bypass, over the graph the engine holds (see the operations' comment in core/wire_a_ops.hpp).
//
// HOW. The page edits a DRAFT and sends the whole graph (PATCH_SET_GRAPH); the engine's copy is replaced as a new version. So a "step" of history is a version of that graph: each pass compares the graph with the one the
// history last saw, and a change that is more than the running radios' own state (on/off, the centre they learnt, the rate) pushes the old graph on the undo stack. Undo and redo hand a stored graph back as a new version, so
// a page that edits from an older version is merged over it (rebaseDraft) like any other edit. Copy, paste and bypass change the graph here and the next pass records the step. Nothing is written to disk.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/patch_draft.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"

namespace cascade::engine {

namespace pc = cascade::core::patch;
using nlohmann::json;

namespace {

// Two graphs that differ only in what the running radios keep up to date are the same edit.
bool sameEdit(const pc::Graph& a, const pc::Graph& b) {
    pc::Graph x = a, y = b;
    auto flat = [](pc::Graph& g) {
        const pc::Graph snap = g;
        for (const pc::Node& n : snap.nodes()) {
            if (n.kind != pc::NodeKind::Radio) { continue; }
            if (pc::Node* m = g.mutableNode(n.id)) { m->on = true; m->freqHz = 0.0; m->rateHz = 0.0; m->centreChosen = false; }
        }
    };
    flat(x);
    flat(y);
    return pc::graphsEqual(x, y);
}

bool parseIds(const std::string& text, std::vector<pc::NodeId>& out, std::string& why) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("ids") || !j["ids"].is_array()) { why = "the text is a JSON object with an \"ids\" list"; return false; }
    for (const json& v : j["ids"]) {
        if (!v.is_number_unsigned() || v.get<std::uint64_t>() == 0 || v.get<std::uint64_t>() > 0xFFFFFFFEull) { why = "an id is a whole number above 0"; return false; }
        out.push_back(static_cast<pc::NodeId>(v.get<std::uint64_t>()));
    }
    return true;
}

}  // namespace

void WireA::patchApply(pc::Graph g) {
    g.reserveIds(e_.patchGraph_.nextId());
    // the radios keep their own state: what the running radios learnt is not part of an undo
    for (const pc::Node& n : e_.patchGraph_.nodes()) {
        if (n.kind != pc::NodeKind::Radio) { continue; }
        if (pc::Node* m = g.mutableNode(n.id)) { m->on = n.on; m->freqHz = n.freqHz; m->rateHz = n.rateHz; m->centreChosen = n.centreChosen; }
    }
    e_.patchGraph_ = std::move(g);
    e_.syncPatchGraphVersion();
    patchCur_ = e_.patchGraph_;
    patchSeenVersion_ = e_.patchGraphVersion_;
}

void WireA::patchHistoryPass() {
    e_.syncPatchGraphVersion();
    if (!patchHistInit_) {
        patchCur_ = e_.patchGraph_;
        patchSeenVersion_ = e_.patchGraphVersion_;
        patchHistInit_ = true;
        return;
    }
    if (e_.patchGraphVersion_ == patchSeenVersion_) { return; }
    if (!sameEdit(patchCur_, e_.patchGraph_)) {
        if (patchUndo_.size() >= kPatchHistoryDepth) { patchUndo_.erase(patchUndo_.begin()); }
        patchUndo_.push_back(patchCur_);
        patchRedo_.clear();
        bump();
    }
    patchCur_ = e_.patchGraph_;
    patchSeenVersion_ = e_.patchGraphVersion_;
    // the selection never names a part that is gone
    patchSel_.erase(std::remove_if(patchSel_.begin(), patchSel_.end(), [&](pc::NodeId id) { return e_.patchGraph_.find(id) == nullptr; }), patchSel_.end());
}

WireA::Result WireA::patchEditOp(const FoxCommand& c, const std::string& text, Result res) {
    patchHistoryPass();   // whatever changed since the last pass is a step before this one acts
    switch (c.op) {
        case FOXAPP_OP_PATCH_UNDO:
        case FOXAPP_OP_PATCH_REDO: {
            const bool undo = c.op == FOXAPP_OP_PATCH_UNDO;
            auto& from = undo ? patchUndo_ : patchRedo_;
            auto& to = undo ? patchRedo_ : patchUndo_;
            if (from.empty()) { return refused(res, FOXAPI_NO_CHANGE, undo ? "there is nothing to undo" : "there is nothing to redo"); }
            to.push_back(patchCur_);
            pc::Graph g = std::move(from.back());
            from.pop_back();
            patchApply(std::move(g));
            patchSel_.erase(std::remove_if(patchSel_.begin(), patchSel_.end(), [&](pc::NodeId id) { return e_.patchGraph_.find(id) == nullptr; }), patchSel_.end());
            res.applied[0] = static_cast<double>(e_.patchGraphVersion_);
            bump();
            return res;
        }
        case FOXAPP_OP_PATCH_SELECT: {
            std::vector<pc::NodeId> ids;
            std::string why;
            if (c.ival[0] < 0 || c.ival[0] > 4) { return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 replace, 1 add, 2 toggle, 3 clear or 4 all"); }
            if (c.ival[0] <= 2) {
                if (!parseIds(text, ids, why)) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
                for (pc::NodeId id : ids) { if (e_.patchGraph_.find(id) == nullptr) { return refused(res, FOXAPI_BAD_ARGUMENT, "a part with that number is not in the patch"); } }
            }
            std::vector<pc::NodeId> next = patchSel_;
            if (c.ival[0] == 0) { next = ids; }
            if (c.ival[0] == 1) { for (pc::NodeId id : ids) { if (std::find(next.begin(), next.end(), id) == next.end()) { next.push_back(id); } } }
            if (c.ival[0] == 2) {
                for (pc::NodeId id : ids) {
                    const auto it = std::find(next.begin(), next.end(), id);
                    if (it == next.end()) { next.push_back(id); } else { next.erase(it); }
                }
            }
            if (c.ival[0] == 3) { next.clear(); }
            if (c.ival[0] == 4) { next.clear(); for (const pc::Node& n : e_.patchGraph_.nodes()) { next.push_back(n.id); } }
            std::vector<pc::NodeId> a = next, b = patchSel_;
            std::sort(a.begin(), a.end());
            a.erase(std::unique(a.begin(), a.end()), a.end());
            std::sort(b.begin(), b.end());
            if (a == b) { return refused(res, FOXAPI_NO_CHANGE, "that is the selection already"); }
            patchSel_ = next;
            std::sort(patchSel_.begin(), patchSel_.end());
            patchSel_.erase(std::unique(patchSel_.begin(), patchSel_.end()), patchSel_.end());
            res.applied[0] = static_cast<double>(patchSel_.size());
            bump();
            return res;
        }
        case FOXAPP_OP_PATCH_COPY: {
            std::vector<pc::NodeId> sel;
            for (pc::NodeId id : patchSel_) { if (e_.patchGraph_.find(id) != nullptr) { sel.push_back(id); } }
            if (sel.empty()) { return refused(res, FOXAPI_NO_CHANGE, "nothing is selected"); }
            PatchClip clip;
            float left = 1.0e30f, top = 1.0e30f;
            for (pc::NodeId id : sel) { const pc::Node* n = e_.patchGraph_.find(id); left = std::min(left, n->x); top = std::min(top, n->y); }
            for (pc::NodeId id : sel) {
                pc::Node n = *e_.patchGraph_.find(id);
                n.x -= left;
                n.y -= top;
                clip.nodes.push_back(std::move(n));
            }
            for (const pc::Wire& w : e_.patchGraph_.wires()) {
                if (std::find(sel.begin(), sel.end(), w.from) != sel.end() && std::find(sel.begin(), sel.end(), w.to) != sel.end()) { clip.wires.push_back(w); }
            }
            patchClip_ = std::move(clip);
            res.applied[0] = static_cast<double>(patchClip_.nodes.size());
            res.applied[1] = static_cast<double>(patchClip_.wires.size());
            bump();
            return res;
        }
        case FOXAPP_OP_PATCH_PASTE: {
            if (patchClip_.nodes.empty()) { return refused(res, FOXAPI_NO_CHANGE, "the clipboard is empty"); }
            if (!std::isfinite(c.num[0]) || !std::isfinite(c.num[1])) { return refused(res, FOXAPI_BAD_ARGUMENT, "the paste place is two numbers"); }
            const pc::Graph before = e_.patchGraph_;
            std::vector<std::pair<pc::NodeId, pc::NodeId>> made;   // old id -> new id
            for (const pc::Node& n : patchClip_.nodes) {
                pc::PortType feed = pc::PortType::Iq;
                if (!n.inputs.empty()) { feed = n.inputs[0]; } else if (!n.outputs.empty()) { feed = n.outputs[0]; }
                const pc::NodeId id = e_.patchGraph_.addNode(n.kind, n.name, feed, n.x + static_cast<float>(c.num[0]), n.y + static_cast<float>(c.num[1]));
                if (id == pc::kNoNode) {
                    e_.patchGraph_ = before;   // all or nothing
                    return refused(res, FOXAPI_OUT_OF_RANGE, "the patch cannot take another of those (a patch has at most five radios)");
                }
                pc::Node* m = e_.patchGraph_.mutableNode(id);
                const pc::NodeId keep = m->id;
                const float x = m->x, y = m->y;
                *m = n;
                m->id = keep;
                m->x = x;
                m->y = y;
                made.emplace_back(n.id, id);
            }
            std::size_t refusedWires = 0;
            auto newId = [&](pc::NodeId old) { for (const auto& p : made) { if (p.first == old) { return p.second; } } return pc::kNoNode; };
            for (const pc::Wire& w : patchClip_.wires) {
                if (e_.patchGraph_.connect(newId(w.from), w.fromPort, newId(w.to), w.toPort) != pc::Connect::Ok) { ++refusedWires; }
            }
            e_.syncPatchGraphVersion();
            patchSel_.clear();
            for (const auto& p : made) { patchSel_.push_back(p.second); }
            std::sort(patchSel_.begin(), patchSel_.end());
            res.applied[0] = static_cast<double>(made.size());
            res.applied[1] = static_cast<double>(refusedWires);
            patchHistoryPass();   // one step for the whole paste
            bump();
            return res;
        }
        case FOXAPP_OP_PATCH_BYPASS: {
            if (c.ival[0] != 0 && c.ival[0] != 1) { return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 1 on or 0 off"); }
            std::vector<pc::NodeId> ids;
            std::string why;
            if (text.empty()) { ids = patchSel_; } else if (!parseIds(text, ids, why)) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
            if (ids.empty()) { return refused(res, FOXAPI_NO_CHANGE, "no part is named or selected"); }
            const bool on = c.ival[0] == 1;
            for (pc::NodeId id : ids) {
                const pc::Node* n = e_.patchGraph_.find(id);
                if (n == nullptr) { return refused(res, FOXAPI_BAD_ARGUMENT, "a part with that number is not in the patch"); }
                if (on && !pc::canBypass(*n)) { return refused(res, FOXAPI_OUT_OF_RANGE, "that part cannot be bypassed: what it takes in and what it gives out are different"); }
            }
            bool changed = false;
            for (pc::NodeId id : ids) {
                pc::Node* n = e_.patchGraph_.mutableNode(id);
                if (n->bypass != on) { n->bypass = on; changed = true; }
            }
            if (!changed) { return refused(res, FOXAPI_NO_CHANGE, on ? "those parts are bypassed already" : "those parts are in the path already"); }
            e_.syncPatchGraphVersion();
            patchHistoryPass();
            res.applied[0] = static_cast<double>(ids.size());
            bump();
            return res;
        }
        default:
            return refused(res, FOXAPI_UNSUPPORTED, "this operation is not in this build");
    }
}

void WireA::patchEditFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("patchedit.undo", std::to_string(patchUndo_.size()));
    fact("patchedit.redo", std::to_string(patchRedo_.size()));
    std::string sel;
    for (pc::NodeId id : patchSel_) { if (!sel.empty()) { sel += ','; } sel += std::to_string(id); }
    fact("patchedit.selection", sel);
    fact("patchedit.clipboard", std::to_string(patchClip_.nodes.size()));
}

}  // namespace cascade::engine
