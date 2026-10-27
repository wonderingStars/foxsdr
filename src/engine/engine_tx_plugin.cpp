// engine_tx_plugin.cpp - A PLUGIN MAY KEY THE TRANSMITTER, in the engine (stub campaign B13, TX-PLUGIN; master 0.99.74's CASCADE_CAP_TX_AUDIO).
//
// SAFETY-CRITICAL, AND READ AS SUCH. The sentence in core/transmitter.hpp used to be "only a hand can key it"; on the owner's written approval of 2026-10-12 (a
// Winlink-compatible radio link, which needs a plugin to send audio while the radio is keyed) it is "a hand, or a plugin the operator has GRANTED and ARMED".
// Everything here exists to keep the second half as hard to satisfy by accident as the first half always was. The RULES are master's and are not touched:
//   - core/tx_audio.hpp's pluginMayKey(): the plugin that would hold the transmitter has the grant, the operator's window is open, the TRANSMIT page is live, the control
//     loop is ticking, the plugin's own active() is fresh and yes, a radio is open and has not faulted, no hand has a key - and no fault has been seen since the window opened;
//   - core::TxArming: 5, 15 or 60 minutes, session-only, closing itself;
//   - core::Transmitter::setPluginKey: the cap (a plugin's key is never held longer than kLatchTimeout), the fault latch, one source (a hand's key takes the audio with it)
//     and the TX thread's dead-man (the control tick going stale opens the key whatever the caller is doing).
// WHAT MOVED IS WHO DECIDES. In master the window decided, once a frame (AppWindow::applyPluginTransmitKey), and a window that stopped stopped deciding - which, with the
// plugin's key a request the frame loop rebuilt every frame and the transmitter's dead-man watching the tick, was the safe direction. The engine runs without a window now,
// and a window that stopped must not leave a plugin able to key. So the ENGINE decides, once a key drive (ApiHostImpl::txPass, between the hands' levels and
// Transmitter::tick()), from state it holds itself:
//   - THE GRANT (pluginTransmitAllowed_) is changed only by FOXAPP_OP_PLUGIN_TX_GRANT and THE WINDOW (txArming_) only by FOXAPP_OP_TX_PLUGIN_ARM: LOCAL ops, grant TRANSMIT, not
//     on the web profile, refused to every REMOTE session at submit (three ways) and again where they are applied. The grant has an op of its own because FOXAPI_OP_PLUGIN_GRANT
//     IS on the web profile: nothing transmit-related rides on it (its kind 3 is still "no such grant").
//   - THE WINDOW HAS A LEASE: it belongs to the session that opened it and lives while that session is open, keeps its heartbeat (Transmitter::kKeyAliveWait) and keeps saying
//     its TRANSMIT page is on screen (ARM -1, every frame the page is drawn). A window that crashed, froze, or closed its page lets the lease go and the window closes with it.
//   - THE REQUEST IS REBUILT FROM NOTHING EVERY DRIVE and honoured only while the grant AND the arming are current: revoking either unkeys in the same drive (the stage 5d
//     second-drive precedent: ApiHostImpl::applyTail runs the drive again after the internal tier).
//   - EVERY CLOSING REASON UNKEYS, AND THE PLUGIN ASKING AGAIN DOES NOT RE-KEY until a person arms a new window: the page closed, the radio closed or changed, OFF, the grant
//     withdrawn, a fault, the cap, the window's own time, a configuration loaded, the receiver stopped, the engine stopping, the window that armed it gone or silent - and
//     (a hardening master lacked, added here on purpose) THE KEYED PLUGIN GOING DORMANT, BEING STOPPED, UNLOADED OR REBUILT, which unkeys at once and closes the window.
// NOTHING HERE HAS BEEN RUN INTO AN ANTENNA. Every test of it keys a ledger fake (a sink that records what it was given) and no real transmitter has been keyed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/plugin_runner.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"

namespace cascade::engine {

namespace cc = cascade::core;

// The facts' bits are the table's words; the runner's own constants are what they are built from.
static_assert(FOXAPP_TXBIT_DECLARED == cc::PluginRunner::kTxDeclared, "fact.plugin.<file>.tx: declared");
static_assert(FOXAPP_TXBIT_ASKED == cc::PluginRunner::kTxAsked, "fact.plugin.<file>.tx: asked");
static_assert(FOXAPP_TXBIT_HOLDER == cc::PluginRunner::kTxHolder, "fact.plugin.<file>.tx: holder");
static_assert(FOXAPP_TXBIT_WAITING == cc::PluginRunner::kTxWaiting, "fact.plugin.<file>.tx: waiting");
static_assert(FOXAPP_TXBIT_STOPPED == cc::PluginRunner::kTxStopped, "fact.plugin.<file>.tx: stopped");

namespace {

bool listHas(const std::vector<std::string>& list, const std::string& key) {
    return std::find(list.begin(), list.end(), key) != list.end();
}

FoxCommandResult resultFor(const FoxCommand& c) {
    FoxCommandResult r;
    std::memset(&r, 0, sizeof(r));
    r.structSize = static_cast<std::uint32_t>(sizeof(FoxCommandResult));
    r.status = FOXAPI_OK;
    r.op = c.op;
    return r;
}

FoxCommandResult refused(FoxCommandResult r, std::int32_t status, const char* why) {
    r.status = status;
    r.flags |= FOXAPI_RESULT_REFUSED;
    std::snprintf(r.message, sizeof(r.message), "%s", why);
    return r;
}

}  // namespace

// --- the grant ---------------------------------------------------------------------------------------------------------------------

void Engine::applyPluginTransmitGrants() {
    // Every entry is a module file name (a PluginUi::tuneKey-style key). The API core holds the live copy a plugin's get_state reads (CASCADE_STATE_TX_GRANTED); the runner holds the
    // one it pulls by - a module that is not in it is polled (so it can be said to have ASKED) and never pulled from.
    for (const std::string& k : pluginTransmitAllowed_) {
        if (!k.empty()) { pluginApi_->setTxGranted(k, true); }
    }
    pluginRunner_.setTxGranted(pluginTransmitAllowed_);
}

std::int32_t Engine::applyPluginTxGrantOp(const std::string& moduleFile, bool grant, std::string& why) {
    if (moduleFile.empty()) {
        why = "no module named";
        return FOXAPI_BAD_ARGUMENT;
    }
    if (grant) {
        // A grant is for a module that can use it. (A grant for anything else would be a permission nobody could ever see on a plate.)
        bool found = false;
        for (const cc::LoadedPlugin& p : pluginHost_.plugins()) {
            if (p.loaded && p.txAudio != nullptr && cc::pluginKey(p) == moduleFile) {
                found = true;
                break;
            }
        }
        if (!found) {
            why = "no loaded plugin of that file name declares transmit audio";
            return FOXAPI_NOT_FOUND;
        }
        if (!listHas(pluginTransmitAllowed_, moduleFile)) { pluginTransmitAllowed_.push_back(moduleFile); }
        pluginApi_->setTxGranted(moduleFile, true);
        // An ARM refusal that said "no plugin has been granted TRANSMIT" is answered by this.
        if (txArmRefusal_.load(std::memory_order_seq_cst) == cc::TxArming::Refusal::NoGrant) { txArmRefusal_.store(cc::TxArming::Refusal::None, std::memory_order_seq_cst); }
    } else {
        // Any name: a grant held for a module that has since gone is still one the operator can take back.
        const auto it = std::find(pluginTransmitAllowed_.begin(), pluginTransmitAllowed_.end(), moduleFile);
        if (it != pluginTransmitAllowed_.end()) { pluginTransmitAllowed_.erase(it); }
        pluginApi_->setTxGranted(moduleFile, false);
    }
    // The runner's copy in the same breath: a grant taken back takes the holder with it at once (PluginRunner::setTxGranted), not at the next audio block. The key itself
    // goes in THIS pass's drive (txPass runs at the end of beginPass and again after the internal tier), which reads the durable list above.
    pluginRunner_.setTxGranted(pluginTransmitAllowed_);
    return FOXAPI_OK;
}

bool Engine::txAnyGrant() const {
    for (const cc::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded || p.txAudio == nullptr) { continue; }
        if (listHas(pluginTransmitAllowed_, cc::pluginKey(p))) { return true; }
    }
    return false;
}

std::string Engine::txFirstGrantName() const {
    for (const cc::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded || p.txAudio == nullptr) { continue; }
        if (listHas(pluginTransmitAllowed_, cc::pluginKey(p))) { return p.name; }
    }
    return std::string();
}

// --- the window --------------------------------------------------------------------------------------------------------------------

void Engine::txPluginDisarm(const char* why) {
    // ANY THREAD (see the lease's note in engine.hpp): the window under its lock, then the owner, an atomic store. The order is the old one; the closed window is what every
    // reader tests first, so a reader between the two sees a closed window with a stale owner, and that is inert.
    if (txArming_.disarm(why)) { cc::diagLogf("tx: disarmed (%s)", why != nullptr ? why : "no reason given"); }
    txArmOwner_.store(0, std::memory_order_seq_cst);
}

void Engine::txPluginEnd(const char* why) {
    // The key first (an atomic store: the TX thread plays the tail by itself within a block), then the window.
    transmitter_.setPluginKey(false);
    txPluginDisarm(why);
}

void Engine::txPluginModuleGoing(const std::string& moduleFile, const char* why) {
    // Only when a plugin's key is down or asked for: a module going to sleep that is not the one on the air changes nothing about the key (the window, if the grant went with
    // it, closes at the next drive by the ordinary rule). An empty name is "every module" (the runner cleared or rebuilt).
    if (!transmitter_.pluginKeyRequested() && !transmitter_.pluginKeyed()) { return; }
    if (!moduleFile.empty() && !txKeyedKey_.empty() && txKeyedKey_ != moduleFile) { return; }
    txPluginEnd(why);
}

FoxCommandResult Engine::txPluginArmOp(std::uint64_t session, bool remote, bool sessionOpen, const FoxCommand& c) {
    FoxCommandResult res = resultFor(c);
    // THE FOURTH LOCK (submit refuses a REMOTE session three ways before this is reached): a remote session can never hold, refresh or open the window.
    if (remote) { return refused(res, FOXAPI_DENIED, "the application's own commands are for sessions on this machine"); }
    const std::int64_t minutes = c.ival[0];
    // OFF is in the never-BUSY class (fox_engine_api.cpp kSaferArmOff) and so is applied for a session that has closed or detached since it sent it, as every safer command is: it
    // only ever makes things safer. Anything that keeps or opens the window needs the session open.
    if (!sessionOpen && minutes != 0) { return refused(res, FOXAPI_DETACHED, "the session closed before this command was applied"); }
    const std::int64_t nowMs = cc::TxAudioLink::nowMs();

    if (minutes == -1) {
        // THE PAGE IS LIVE, said again. Only the session that holds the window can say it, and only while one is open: anything else is a no-op that says so.
        if (!txArming_.armed() || txArmOwner_.load(std::memory_order_seq_cst) != session) {
            res.status = FOXAPI_NO_CHANGE;
            return res;
        }
        txArmLiveMs_.store(nowMs, std::memory_order_seq_cst);
        return res;
    }
    if (minutes == 0) {
        // CLOSING IS ANY LOCAL SESSION'S: it only ever makes things safer. ival[1] says why (the two sentences the page lets the log carry).
        if (txArming_.armed()) { txPluginDisarm(c.ival[1] == 1 ? "the TRANSMIT page was closed" : "you pressed OFF"); }
        // OFF also answers a refusal the page was still lettering.
        txArmRefusal_.store(cc::TxArming::Refusal::None, std::memory_order_seq_cst);
        res.applied[0] = 0.0;
        return res;
    }


    // ARM. A radio that is open AND has not faulted: arming a window over a board that has already stopped answering is arming nothing.
    const bool radioOpen = transmitter_.haveSink() && !(transmitter_.sink() != nullptr && transmitter_.sink()->faulted());
    const int asked = minutes > 0 && minutes < 1000 ? static_cast<int>(minutes) : -1;   // (a number the window cannot mean is BadMinutes below)
    const cc::TxArming::Refusal r = txArming_.arm(asked, radioOpen, txAnyGrant(), cc::TxArming::Clock::now());
    txArmRefusal_.store(r, std::memory_order_seq_cst);
    res.applied[0] = static_cast<double>(static_cast<int>(r));
    switch (r) {
        case cc::TxArming::Refusal::None: break;
        case cc::TxArming::Refusal::NoRadio:
            return refused(res, FOXAPI_NO_DEVICE, "not armed: no transmitter is open");
        case cc::TxArming::Refusal::NoGrant:
            return refused(res, FOXAPI_NOT_FOUND, "not armed: no plugin has been granted TRANSMIT");
        case cc::TxArming::Refusal::BadMinutes:
            return refused(res, FOXAPI_OUT_OF_RANGE, "not armed: that is not one of the offered times");
    }
    // A NEW WINDOW IS A NEW DECISION. A plugin whose key was cut - by the cap, a fault, the frozen-control handle - stays cut until the operator arms again; this is the arming, and
    // the only place that clears it.
    transmitter_.clearPluginFault();
    txFaultsSeen_ = transmitter_.faultCount();
    // The page's stamp BEFORE the owner (both seq_cst): a drive that sees the new owner sees a fresh stamp with it, never the previous window's old one. (On the pass nothing is
    // between the two; the order only matters to a reader on another thread, and then it is the fail-safe one.)
    txArmLiveMs_.store(nowMs, std::memory_order_seq_cst);
    txArmOwner_.store(session, std::memory_order_seq_cst);
    cc::diagLogf("tx: armed %d min", asked);
    res.applied[0] = 0.0;
    return res;
}

// --- the drive ---------------------------------------------------------------------------------------------------------------------

void Engine::txPluginStep(const TxPluginLease& lease) {
    using Clock = cc::TxArming::Clock;

    // NOTHING TO DECIDE: no window open, nothing asked for, nothing keyed or to be told about. The request is cleared anyway (an atomic store), so a drive that skipped
    // everything below could still never leave one standing.
    if (!txArming_.armed() && !transmitter_.pluginKeyRequested() && !transmitter_.pluginKeyed() && !txPluginKeyedPrev_) {
        transmitter_.setPluginKey(false);
        return;
    }

    const Clock::time_point now = Clock::now();
    const std::int64_t nowMs = cc::TxAudioLink::nowMs();

    // THE WINDOW'S OWN TIMER first: a window whose time has come is closed before anything below can count it as open.
    bool expired = false;
    txArming_.update(now, &expired);
    if (expired) {
        cc::diagLogf("tx: disarmed (%s)", txArming_.lastReason().c_str());
        txArmOwner_.store(0, std::memory_order_seq_cst);
    }

    const bool haveSink = transmitter_.haveSink();
    const bool sinkFaulted = haveSink && transmitter_.sink() != nullptr && transmitter_.sink()->faulted();
    const bool tickFresh = transmitter_.msSinceTick() <= static_cast<std::int64_t>(cc::Transmitter::kKeyAliveWait.count());
    const std::uint64_t faults = transmitter_.faultCount();
    const bool keyedNow = transmitter_.pluginKeyed();
    const std::int64_t leaseMs = static_cast<std::int64_t>(cc::Transmitter::kKeyAliveWait.count());

    // THE LEASE: the window belongs to the session that opened it. Which of its three conditions failed is the sentence the operator reads.
    const char* leaseLost = nullptr;
    if (txArming_.armed()) {
        if (!lease.sessionPresent || lease.sessionRemote) {
            leaseLost = "the window that armed this was closed";
        } else if (lease.beatAgeMs > leaseMs) {
            leaseLost = "the window stopped responding";
        } else if (nowMs - txArmLiveMs_.load(std::memory_order_seq_cst) > leaseMs) {
            leaseLost = "the TRANSMIT page was closed";
        }
    }
    const bool pageLive = txArming_.armed() && leaseLost == nullptr;

    // WHAT CLOSES THE WINDOW, as against what merely withholds the key. A fault, the page going, the radio going, the window going, the control loop having stalled, the grant
    // being taken back and the keyed plugin going each end the operator's authorisation outright - the next time the plugin may transmit is when a person arms it again.
    if (txArming_.armed()) {
        std::string faultWhy;
        const char* why = nullptr;
        if (!haveSink) {
            why = "the transmit radio was closed";
        } else if (sinkFaulted) {
            why = "the radio faulted";
        } else if (faults != txFaultsSeen_) {
            why = "the transmitter stopped on its own";
        } else if (transmitter_.pluginFault()) {
            faultWhy = transmitter_.pluginFaultReason();
            why = faultWhy.c_str();
        } else if (leaseLost != nullptr) {
            why = leaseLost;
        } else if (!tickFresh) {
            why = "the transmit control stopped responding";
        } else if (!txAnyGrant()) {
            why = "no plugin holds the TRANSMIT grant any more";
        } else if (keyedNow) {
            // THE KEYED PLUGIN MUST STILL BE THERE (the hardening): loaded, allowed to run (not stopped, not dormant) and still the runner's holder. Its instances going
            // away already withdraws its active() through the link; this does not depend on that - it unkeys and closes the window itself.
            bool runs = false;
            if (!txKeyedKey_.empty()) {
                refreshApiPluginKeys();
                const std::vector<cc::LoadedPlugin>& plugins = pluginHost_.plugins();
                for (std::size_t i = 0; i < plugins.size() && i < apiPluginKeys_.size(); ++i) {
                    if (apiPluginKeys_[i] != txKeyedKey_) { continue; }
                    runs = plugins[i].loaded && plugins[i].txAudio != nullptr && pluginShouldRunById(plugins[i], apiPluginIds_[i]) &&
                           pluginRunner_.txHolderKeyInto(txHolderScratch_) && txHolderScratch_ == txKeyedKey_;
                    break;
                }
            }
            if (!runs) { why = "the plugin that held the key stopped (dormant, stopped, unloaded or rebuilt)"; }
        }
        if (why != nullptr) { txPluginDisarm(why); }
    }
    txFaultsSeen_ = faults;

    // THE DECISION. The plugin that would hold the transmitter is the one keyed now, or else the runner's holder; its grant is checked HERE as well as in the runner, from the
    // durable list, so neither alone can let an ungranted plugin through.
    const bool armedNow = txArming_.armed();
    // The runner's lock is taken only when a window is open and nothing is keyed: with none, nothing can be keyed and the question has no use.
    const std::string* holder = &txKeyedKey_;
    if (!keyedNow) {
        holder = &txHolderScratch_;
        if (armedNow) {
            (void)pluginRunner_.txHolderKeyInto(txHolderScratch_);
        } else {
            txHolderScratch_.clear();
        }
    }
    cc::PluginKeyInputs in;
    in.granted = !holder->empty() && listHas(pluginTransmitAllowed_, *holder);
    in.armed = armedNow;
    in.pageLive = pageLive && armedNow;
    in.tickFresh = tickFresh;
    in.active = txLink_->wantsKey(nowMs);
    in.radioOk = haveSink && !sinkFaulted;
    in.handsOff = !transmitter_.pttHeld() && !transmitter_.latched() && !transmitter_.remoteKeyed();
    in.faulted = transmitter_.pluginFault();
    const bool may = cc::pluginMayKey(in) && !txPluginStopping_.load(std::memory_order_seq_cst);
    transmitter_.setPluginKey(may);
    // THE ARGUMENT, now a valid one (the review of B13, concern 2). Two variables, each side a store then a load of the OTHER one, so it is the store-buffering shape and only
    // sequential consistency can forbid "neither sees the other". Transmitter::pluginKey_ is relaxed (core/transmitter.cpp, byte-identical to master's and not ours to change), so
    // the ordering is carried by two seq_cst FENCES, one on each side, with the flag seq_cst as well:
    //   this side:  setPluginKey(may); FENCE X; load txPluginStopping_
    //   stopControlThread:  store txPluginStopping_ = true; FENCE Y; setPluginKey(false)
    // The fences are in the total order S, so X precedes Y or Y precedes X. Y before X: the flag's store is sequenced before Y and Y precedes X, so the load after X sees the flag
    // (or something later) and writes false itself. X before Y: our setPluginKey(may) is sequenced before X and X precedes Y, which is sequenced before the stopper's
    // setPluginKey(false), so the stopper's false is LATER in pluginKey_'s modification order than our write ([atomics.order]: two fences, X sequenced after the first write and
    // Y before the second, X preceding Y in S) and is what the transmitter is left holding. Either way the key ends false; there is no third case. (The pass-thread's serialisation
    // of the ops that reach this function, and the loop's last act ApiHost::engineStopping, still back it up: belt and braces, not the argument.)
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (txPluginStopping_.load(std::memory_order_seq_cst)) { transmitter_.setPluginKey(false); }

    // WHY NOT, for the log line that says why a plugin was unkeyed: the first condition that failed, in the order that tells the operator the most.
    if (may) {
        txWhyNot_.clear();
    } else if (in.faulted) {
        txWhyNot_ = transmitter_.pluginFaultReason();
    } else if (!in.radioOk) {
        txWhyNot_ = "the radio is not available";
    } else if (!in.armed) {
        txWhyNot_ = txArming_.lastReason().empty() ? std::string("the arming window is closed") : txArming_.lastReason();
    } else if (!in.pageLive) {
        txWhyNot_ = "the TRANSMIT page was closed";
    } else if (!in.tickFresh) {
        txWhyNot_ = "the transmit control stopped responding";
    } else if (!in.granted) {
        txWhyNot_ = "the TRANSMIT grant was withdrawn";
    } else if (!in.handsOff) {
        txWhyNot_ = "a hand took the key";
    } else if (!holder->empty() && (pluginRunner_.txBits(*holder) & cc::PluginRunner::kTxStopped) != 0u) {
        txWhyNot_ = "its transmit audio reported a permanent error";
    } else {
        txWhyNot_ = "the plugin let go (active() is zero)";
    }
}

void Engine::txPluginAfterTick() {
    const bool keyed = transmitter_.pluginKeyed();
    if (keyed && !txPluginKeyedPrev_) {
        (void)pluginRunner_.txHolderKeyInto(txKeyedKey_);
        txKeyedName_ = pluginRunner_.txHolderName();
        cc::diagLogf("tx: plugin %s keyed", txKeyedName_.c_str());
    }
    if (!keyed && txPluginKeyedPrev_) {
        // The cap fires INSIDE the tick, after this drive's decision was made, so its reason is read from the transmitter first.
        std::string why = transmitter_.pluginFault() ? transmitter_.pluginFaultReason() : txWhyNot_;
        if (why.empty()) { why = "the key was released"; }
        cc::diagLogf("tx: plugin %s unkeyed (%s)", txKeyedName_.c_str(), why.c_str());
        txKeyedKey_.clear();
    }
    txPluginKeyedPrev_ = keyed;

    // A FAULT THAT HAPPENED INSIDE THE TICK (the cap) closes the window the same drive, so the very next decision already has it closed.
    if (transmitter_.pluginFault() && txArming_.armed()) { txPluginDisarm(transmitter_.pluginFaultReason().c_str()); }

    // THE RUNNER IS TOLD ONLY WHEN THIS CHANGES (setTxKeyed takes its lock): from the drive the transmitter is keyed for the plugin, pull() may be called; from the one it is
    // not, it will not be, and when the call returns nothing is still in flight.
    if (keyed != txRunnerKeyed_) {
        pluginRunner_.setTxKeyed(keyed);
        txRunnerKeyed_ = keyed;
    }

    // WHAT THE PLUGINS ARE TOLD (get_state's CASCADE_STATE_TX_ARMED / _TRANSMITTING): every drive while a window is open or a key is down (a plugin loaded meanwhile has to
    // learn it), and once more on the drive it all went quiet.
    const bool armed = txArming_.armed();
    if (armed || keyed || txPublishedArmed_ || !txPublishedKey_.empty()) {
        pluginApi_->publishTxState(armed, keyed ? txKeyedKey_ : std::string());
        txPublishedArmed_ = armed;
        if (keyed) {
            txPublishedKey_ = txKeyedKey_;
        } else {
            txPublishedKey_.clear();
        }
    }
}

// --- the facts ---------------------------------------------------------------------------------------------------------------------

std::uint32_t Engine::txPluginBitsOf(const std::string& moduleFile) const {
    std::uint32_t bits = pluginRunner_.txBits(moduleFile);
    if (listHas(pluginTransmitAllowed_, moduleFile)) { bits |= FOXAPP_TXBIT_GRANTED; }
    if (transmitter_.pluginKeyed() && !txKeyedKey_.empty() && txKeyedKey_ == moduleFile) { bits |= FOXAPP_TXBIT_ON_AIR; }
    return bits;
}

void Engine::buildApiListsTxPlugin(ApiListBlock& b) {
    // LOCAL ONLY, every one: what is armed and what is keyed on this machine's radio is the operator's page, not a remote session's.
    const auto put = [&b](const std::string& key, std::string value) { b.facts[key] = ApiListBlock::Fact{std::move(value), true}; };
    const bool armed = txArming_.armed();
    put("tx.plugin.armed", armed ? "1" : "0");
    put("tx.plugin.minutes", armed ? std::to_string(txArming_.minutes()) : std::string());
    put("tx.plugin.secondsLeft", armed ? std::to_string(txArming_.secondsLeft(cc::TxArming::Clock::now())) : std::string());
    put("tx.plugin.name", armed ? txFirstGrantName() : std::string());
    put("tx.plugin.keyed", transmitter_.pluginKeyed() ? txKeyedName_ : std::string());
    put("tx.plugin.refusal", std::to_string(static_cast<int>(txArmRefusal_.load(std::memory_order_seq_cst))));
    // Why the window last closed, when it did not close by the operator's own OFF (the page letters it in amber).
    put("tx.plugin.reason", (!armed && txArming_.lastReason() != "you pressed OFF") ? txArming_.lastReason() : std::string());
    put("tx.plugin.dataFlat", txDataFlat_ ? "1" : "0");
    refreshApiPluginKeys();
    const std::vector<cc::LoadedPlugin>& plugins = pluginHost_.plugins();
    for (std::size_t i = 0; i < plugins.size() && i < apiPluginKeys_.size(); ++i) {
        if (!plugins[i].loaded || plugins[i].txAudio == nullptr) { continue; }
        put("plugin." + apiPluginKeys_[i] + ".tx", std::to_string(txPluginBitsOf(apiPluginKeys_[i])));
    }
}

}  // namespace cascade::engine
