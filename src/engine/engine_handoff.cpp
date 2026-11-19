// engine_handoff.cpp - ONE RADIO, TWO FOXSDR WINDOWS (field fix A2, 2026-11-19; the channel is core/radio_handoff.hpp, the words are its sheetFor()).
//
// TWO SIDES.
//   THE ASKER (the window whose open failed "in use by another program"): offerHandoff() is called ONCE for a failed open (the start-up restore's first failure, or a radio the user picked - never
//   a try inside the saved-radio retry series). A worker looks for another FoxSDR window under this user that holds that radio's key. Found: the state is "ask", the saved-radio retry is cancelled
//   (no loop runs behind the question) and fact.radio.handoff carries it for the window to show; not found: "other", the plain sentence, and the retry carries on as before (the updater's old copy
//   still letting go is the case that retry exists for). The window answers with FOXAPP_OP_RADIO_HANDOFF: 1 asks the holder to release (a worker, bounded), then the radio is opened here through the
//   ordinary open; 0 puts the question away. A refusal or a silence is said and changes nothing. None of it is a failure of the radio: no report is filed for the move or its refusal, and an open that
//   then fails is an ordinary failed open (reported once, like any).
//   THE HOLDER (the window that has the radio): the pipe's thread calls holds()/release(); release() hands the request to THIS file's pump on the control pass and waits up to kHolderWait for it. The pass
//   refuses while the radio's I/Q or audio take is running or starting (a take belongs to one run of one source: the app ends it for a source the USER chose, but a stranger's request is not a user's
//   choice at this window and cannot be asked in the time), and otherwise puts the radio's receiver on the signal generator exactly as the user's own pick of the generator does, and leaves a notice.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

#include "core/diag_log.hpp"
#include "core/radio_handoff.hpp"
#include "core/receiver_tables.hpp"
#include "engine/engine.hpp"

namespace ho = cascade::core::handoff;
using namespace cascade::gui;

namespace cascade::engine {

namespace {
constexpr std::chrono::milliseconds kHolderWait{4000};     // how long the pipe thread waits for this window's pass
constexpr int kSearchBudgetMs = 1500;                       // looking for the holder
constexpr int kReleaseWaitMs = 5000;                        // the asker's patience for the holder's answer
constexpr std::chrono::milliseconds kSettleAfterRelease{300};   // the other window's handle is closed before it answers; this is margin for the driver
}  // namespace

struct Engine::HandoffImpl {
    std::mutex mu;
    std::condition_variable cv;
    ho::Server server;
    std::string prefix;          // empty: this process offers and answers nothing (a bounded run, a service, a test)
    bool closing = false;
    // holder
    std::set<std::string> held;  // the keys of the radios open here, written by the pass
    bool reqPending = false, reqDone = false;
    std::string reqKey;
    ho::ReleaseResult reqRes;
    std::string movedModel, movedLast4;
    unsigned movedSeq = 0;
    double heldRefreshedS = -1.0;
    // asker
    unsigned seq = 0;
    std::string state;           // "", "searching", "ask", "other", "moving", "refused", "timeout"
    std::string kind, args, key, model, last4, detail;
    double rateHz = 0.0;
    int row = -1;
    std::size_t slot = 0;
    ho::Holder holder;
    bool searchDone = false, searchFound = false;
    bool moveDone = false;
    ho::ReleaseResult moveRes;
};

void Engine::startHandoffServer(const std::string& windowWord) {
    if (!handoff_) { handoff_ = std::make_shared<HandoffImpl>(); }
    const std::shared_ptr<HandoffImpl> impl = handoff_;
    if (!impl->prefix.empty()) { return; }
    ho::Hooks hooks;
    hooks.holds = [impl](const std::string& key) {
        std::lock_guard<std::mutex> lk(impl->mu);
        return impl->held.count(key) != 0;
    };
    hooks.release = [impl](const std::string& key) {
        std::unique_lock<std::mutex> lk(impl->mu);
        ho::ReleaseResult r;
        if (impl->held.count(key) == 0) { r.result = ho::Release::NotHeld; return r; }
        if (impl->reqPending) { r.result = ho::Release::Refused; r.code = "busy"; return r; }
        impl->reqPending = true;
        impl->reqDone = false;
        impl->reqKey = key;
        impl->cv.wait_for(lk, kHolderWait, [&] { return impl->reqDone || impl->closing; });
        const bool done = impl->reqDone;
        impl->reqPending = false;
        if (!done) { r.result = ho::Release::Refused; r.code = "busy"; return r; }
        return impl->reqRes;
    };
    const std::string prefix = ho::defaultPrefix();
    if (impl->server.start(prefix, windowWord, hooks)) {
        std::lock_guard<std::mutex> lk(impl->mu);
        impl->prefix = prefix;
        cascade::core::diagLogf("handoff: this window can be asked to let a radio go (local pipe, this user only)");
    }
}

void Engine::stopHandoffServer() {
    if (!handoff_) { return; }
    {
        std::lock_guard<std::mutex> lk(handoff_->mu);
        handoff_->closing = true;
    }
    handoff_->cv.notify_all();
    handoff_->server.stop();
}

bool Engine::handoffActive() const { return handoff_ && !handoff_->prefix.empty(); }

void Engine::offerHandoff(const std::string& kind, const std::string& args, double requestRateHz, int row) {
    if (!handoffActive()) { return; }
    const std::shared_ptr<HandoffImpl> impl = handoff_;
    const std::string key = ho::radioKey(kind, args);
    std::string model = modelFromNativeLabel(nativeLabelFor(kind, args));
    if (model.empty() || model == args) { model = kind; }
    {
        std::lock_guard<std::mutex> lk(impl->mu);
        if (impl->state == "searching" || impl->state == "moving" || impl->state == "ask") { return; }   // one question at a time
        ++impl->seq;
        impl->state = "searching";
        impl->kind = kind;
        impl->args = args;
        impl->key = key;
        impl->model = model;
        impl->last4 = ho::lastFour(ho::serialOfKey(key));
        impl->detail.clear();
        impl->rateHz = requestRateHz;
        impl->row = row;
        impl->slot = scopedSlot();
        impl->searchDone = false;
        impl->searchFound = false;
    }
    ++handoffVersion_;
    std::thread([impl, key]() {
        ho::Holder h;
        const bool found = !key.empty() && ho::findHolder(ho::defaultPrefix(), key, kSearchBudgetMs, h);
        std::lock_guard<std::mutex> lk(impl->mu);
        impl->holder = h;
        impl->searchFound = found;
        impl->searchDone = true;
    }).detach();
}

bool Engine::handoffAnswer(bool move) {
    if (!handoff_) { return false; }
    const std::shared_ptr<HandoffImpl> impl = handoff_;
    std::unique_lock<std::mutex> lk(impl->mu);
    const bool open = impl->state == "ask" || impl->state == "other" || impl->state == "refused" || impl->state == "timeout";
    if (!open) { return false; }
    if (!move || impl->state != "ask") {
        impl->state.clear();
        lk.unlock();
        ++handoffVersion_;
        return true;
    }
    impl->state = "moving";
    impl->moveDone = false;
    const ho::Holder holder = impl->holder;
    const std::string key = impl->key;
    lk.unlock();
    ++handoffVersion_;
    std::thread([impl, holder, key]() {
        const ho::ReleaseResult r = ho::requestRelease(holder, key, kReleaseWaitMs);
        if (r.result == ho::Release::Released) { std::this_thread::sleep_for(kSettleAfterRelease); }
        std::lock_guard<std::mutex> g(impl->mu);
        impl->moveRes = r;
        impl->moveDone = true;
    }).detach();
    return true;
}

void Engine::pumpHandoff() {
    if (!handoff_) { return; }
    HandoffImpl& H = *handoff_;
    if (H.prefix.empty()) { return; }
    const double now = clockS();

    // THE HOLDER: which radios are open here (a few times a second), and a release somebody asked for.
    std::string wanted;
    {
        std::lock_guard<std::mutex> lk(H.mu);
        if (H.reqPending && !H.reqDone) { wanted = H.reqKey; }
    }
    if (wanted.empty() && now - H.heldRefreshedS < 0.25) {
        // nothing asked, and the held list is fresh enough
    } else {
        std::set<std::string> keys;
        std::size_t holderSlot = kMaxReceivers;
        for (std::size_t s = 0; s < kMaxReceivers; ++s) {
            if (!receivers_[s]) { continue; }
            const Receiver& r = *receivers_[s];
            if (r.device_ == nullptr || r.device_->deviceDead()) { continue; }
            const std::string k = ho::radioKey(r.sourceKind_, r.deviceArgs_);
            if (k.empty()) { continue; }
            keys.insert(k);
            if (!wanted.empty() && k == wanted && holderSlot == kMaxReceivers) { holderSlot = s; }
        }
        ho::ReleaseResult res;
        res.result = ho::Release::NotHeld;
        std::string model, last4;
        if (!wanted.empty() && holderSlot != kMaxReceivers) {
            SlotScope scope(this, holderSlot);
            const bool recording = rx().iqRecorder_.recording() || rx().audioRecorder_.recording() || rx().iqStart_.pending() || rx().audioStart_.pending() ||
                                   rx().iqStartQueued_ || rx().audioStartQueued_;
            if (recording) {
                res.result = ho::Release::Refused;
                res.code = "recording";
                cascade::core::diagLogf("handoff: another window asked for this window's radio; refused, a recording is running here");
            } else if (rx().deviceOpenPending_) {
                res.result = ho::Release::Refused;
                res.code = "busy";
            } else {
                model = modelFromNativeLabel(nativeLabelFor(rx().sourceKind_, rx().deviceArgs_));
                if (model.empty()) { model = rx().sourceKind_; }
                last4 = ho::lastFour(ho::serialOfKey(wanted));
                selectSource(0);   // the user's own pick of the generator: the take (none runs), the converter, the rate and the saved radio are dealt with there
                if (rx().device_ == nullptr) {
                    res.result = ho::Release::Released;
                    rx().sourceError_ = ho::sheetFor("moved", model, last4, std::string()).text;
                    cascade::core::diagLogf("handoff: a radio was moved to another FoxSDR window at its request");
                } else {
                    res.result = ho::Release::Refused;
                    res.code = "busy";
                }
            }
            keys.erase(wanted);
            if (res.result != ho::Release::Released) { keys.insert(wanted); }
        }
        {
            std::lock_guard<std::mutex> lk(H.mu);
            H.held = keys;
            H.heldRefreshedS = now;
            if (!wanted.empty() && H.reqPending && !H.reqDone) {
                H.reqRes = res;
                H.reqDone = true;
                if (res.result == ho::Release::Released) {
                    H.movedModel = model;
                    H.movedLast4 = last4;
                    ++H.movedSeq;
                }
            }
        }
        H.cv.notify_all();
        if (!wanted.empty()) { ++handoffVersion_; }
    }

    // THE ASKER: the search came back, or the release did.
    std::unique_lock<std::mutex> lk(H.mu);
    if (H.state == "searching" && H.searchDone) {
        H.state = H.searchFound ? "ask" : "other";
        const bool found = H.searchFound;
        lk.unlock();
        if (found) {
            cancelSavedRadioRetry();   // no retry loop behind the question
            cascade::core::diagLogf("handoff: the radio is held by another FoxSDR window on this PC - asking whether to move it here");
        }
        ++handoffVersion_;
    } else if (H.state == "moving" && H.moveDone) {
        const ho::ReleaseResult r = H.moveRes;
        const std::string kind = H.kind, args = H.args, model2 = H.model;
        const double rate = H.rateHz;
        const int row = H.row;
        const std::size_t slot = H.slot;
        if (r.result == ho::Release::Released || r.result == ho::Release::NotHeld) {
            H.state.clear();
            lk.unlock();
            // OPEN IT HERE through the ordinary asynchronous open, as a pick of the radio: a failure of that is an open that failed, reported once like any.
            SlotScope scope(this, slot);
            if (!rx().deviceOpenPending_ && rx().device_ == nullptr) {
                DeviceOpenResult req;
                req.kind = kind;
                req.args = args;
                req.requestRateHz = rate > 0.0 ? rate : 2048000.0;
                req.row = row;
                req.keepCenterHz = carriedAirCentre();
                cancelSavedRadioRetry();
                launchDeviceOpen(std::move(req), model2);
            }
        } else if (r.result == ho::Release::Refused) {
            H.state = "refused";
            H.detail = r.code;
            lk.unlock();
        } else {
            H.state = "timeout";   // nothing answered: both windows stay as they are
            lk.unlock();
        }
        ++handoffVersion_;
    }
}

void Engine::handoffFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    if (!handoff_) { return; }
    HandoffImpl& H = *handoff_;
    std::lock_guard<std::mutex> lk(H.mu);
    if (H.prefix.empty()) { return; }
    // "<seq>\t<state>\t<model>\t<last four of the serial>\t<detail>": the sheet's words are core::handoff::sheetFor's, written by the window.
    std::string v;
    if (H.state == "ask" || H.state == "other" || H.state == "refused" || H.state == "timeout") {
        v = std::to_string(H.seq) + "\t" + H.state + "\t" + H.model + "\t" + H.last4 + "\t" + H.detail;
    }
    fact("radio.handoff", v);
    fact("radio.handoffMoved", H.movedSeq == 0 ? std::string() : std::to_string(H.movedSeq) + "\tmoved\t" + H.movedModel + "\t" + H.movedLast4 + "\t");
}

}  // namespace cascade::engine
