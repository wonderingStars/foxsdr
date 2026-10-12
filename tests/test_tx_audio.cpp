// test_tx_audio.cpp - CASCADE_CAP_TX_AUDIO: a plugin may send transmit audio, and every
// condition that stands between that plugin and a keyed radio.
//
// THE SAFETY IS THE POINT. This feature changes a rule ("only a hand can key it" became
// "a hand, or a plugin the operator has granted and armed"), so almost everything below
// is a refusal: each condition in core/tx_audio.hpp is turned off ON ITS OWN while the
// others stay on, and the answer has to be "no key". Nothing here transmits: the radio is
// a ledger (CaptureSink) and the plugins are static tables, as in test_plugin_audio.cpp.
//
//   0. THE ABI           the bit, the flags, the table, and what is refused at load; get_state's
//                        three flags and who is told which.
//   1. THE DECISION      pluginMayKey(): all 256 combinations of its eight inputs, and each
//                        single input turned off against seven that are on.
//   2. THE ARMING WINDOW the operator's 5/15/60 minutes, driven by an injected clock.
//   3. THE RING          never blocks, feeds faster than it is drained, and what the GUI is
//                        told about whether the plugin wants the key (fresh, stale, drain).
//   4. THE RUNNER        pull() only while keyed and granted, one holder at a time and
//                        sticky, a permanent error stops the plugin, the grant withdrawn.
//   5. THE TRANSMITTER   plugin audio through the NFM modulator with the pre-emphasis off,
//                        a stall sends silence, the 60 s cap (the latch's own), the fault
//                        latch, the frozen window, a hand has precedence, hostile samples
//                        (the sanitiser by itself, and the whole path end to end).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "core/plugin_api.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_runner.hpp"
#include "core/transmitter.hpp"
#include "core/tx_audio.hpp"
#include "test_check.hpp"

using cascade::core::LoadedPlugin;
using cascade::core::PluginKeyInputs;
using cascade::core::PluginRunner;
using cascade::core::Transmitter;
using cascade::core::TxArming;
using cascade::core::TxAudioLink;
using cascade::core::TxInput;
using cascade::dsp::TxMode;

namespace {

constexpr double kPi = 3.14159265358979323846;

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ===========================================================================
// 1. THE DECISION
// ===========================================================================

PluginKeyInputs allGood() {
    PluginKeyInputs in;
    in.granted = true;
    in.armed = true;
    in.pageLive = true;
    in.tickFresh = true;
    in.active = true;
    in.radioOk = true;
    in.handsOff = true;
    in.faulted = false;
    return in;
}

// ===========================================================================
// 0. THE ABI SURFACE AND ITS VALIDATION
// ===========================================================================

std::int32_t abiPull(void*, float*, std::size_t) { return 0; }
std::int32_t abiActive(void*) { return 0; }
void* abiCreate(uint32_t) { return nullptr; }
void abiProcess(void*, const float*, std::size_t) {}
std::int32_t abiPoll(void*, char*, std::size_t) { return 0; }
void abiDestroy(void*) {}
void* abiIqCreate(double, double) { return nullptr; }

void testAbiSurface() {
    std::printf("  the ABI surface: the bit, the flags, the table, and what is refused at load\n");
    static_assert(CASCADE_CAP_TX_AUDIO == 0x00004000u, "the bit is part of the contract");
    static_assert((CASCADE_CAP_ALL_KNOWN & CASCADE_CAP_TX_AUDIO) != 0u,
                  "CASCADE_CAP_ALL_KNOWN must carry the transmit-audio bit");
    static_assert(CASCADE_CAP_ALL_KNOWN == 0x00007FFFu, "the exact line from the ABI text");
    static_assert(CASCADE_STATE_TX_GRANTED == 0x00001000u, "state flag values are the contract");
    static_assert(CASCADE_STATE_TX_ARMED == 0x00002000u, "state flag values are the contract");
    static_assert(CASCADE_STATE_TRANSMITTING == 0x00004000u, "state flag values are the contract");
    static_assert(CASCADE_PLUGIN_ABI_VERSION == 3, "a new capability bit must not cost an ABI bump");
    static_assert(CASCADE_HOST_API_LEVEL == 2u, "transmit audio is host API level 2");
    // The new flags are new bits.
    static_assert(((CASCADE_STATE_TX_GRANTED | CASCADE_STATE_TX_ARMED | CASCADE_STATE_TRANSMITTING) &
                   (CASCADE_STATE_LOCATOR_KNOWN | CASCADE_STATE_STOPPED |
                    CASCADE_STATE_SETTINGS_GRANTED | CASCADE_STATE_TUNE_GRANTED)) == 0u,
                  "the new flags are new bits");
    static_assert(sizeof(void*) != 8 || sizeof(CascadeTxAudioApi) == 32u,
                  "four words and two function pointers, no padding on a 64-bit target");

    CascadeDecoderApi dec{};
    dec.structSize = static_cast<std::uint32_t>(sizeof(CascadeDecoderApi));
    dec.create = &abiCreate;
    dec.process = &abiProcess;
    dec.poll_text = &abiPoll;
    dec.destroy = &abiDestroy;
    CascadeTxAudioApi tx{};
    tx.structSize = static_cast<std::uint32_t>(sizeof(CascadeTxAudioApi));
    tx.sampleRateHz = 48000u;
    tx.channels = 1u;
    tx.flags = 0u;
    tx.pull = &abiPull;
    tx.active = &abiActive;

    // `caps` is the whole capability list; `bits` the OR the descriptor declares.
    const auto validate = [](const CascadeCapabilityEntry* caps, std::uint32_t count,
                             std::uint32_t bits) {
        CascadePluginDesc d{};
        d.structSize = static_cast<std::uint32_t>(sizeof(CascadePluginDesc));
        d.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
        d.name = "Fake";
        d.version = "1.0.0";
        d.author = "";
        d.licence = "MIT";
        d.capabilities = bits;
        d.capabilityCount = count;
        d.capabilityTables = caps;
        return cascade::core::validatePluginDesc(&d);
    };
    const auto withTx = [&](const CascadeTxAudioApi& t) {
        const CascadeCapabilityEntry caps[] = {
            {CASCADE_CAP_DECODER, static_cast<std::uint32_t>(sizeof(CascadeDecoderApi)), &dec},
            {CASCADE_CAP_TX_AUDIO, static_cast<std::uint32_t>(sizeof(CascadeTxAudioApi)), &t},
        };
        return validate(caps, 2u, CASCADE_CAP_DECODER | CASCADE_CAP_TX_AUDIO);
    };
    using cascade::core::PluginRejection;

    // The well-formed table is accepted...
    CHECK(withTx(tx) == PluginRejection::None);
    // ...and each way to be malformed is refused with ITS OWN reason.
    {
        CascadeTxAudioApi bad = tx;
        bad.structSize += 8u;
        CHECK(withTx(bad) == PluginRejection::TxAudioStructSizeMismatch);
    }
    for (const std::uint32_t rate : {0u, 8000u, 44100u, 47999u, 48001u, 96000u}) {
        CascadeTxAudioApi bad = tx;
        bad.sampleRateHz = rate;
        CHECK(withTx(bad) == PluginRejection::TxAudioBadRate);  // 48000 only: not "in range"
    }
    for (const std::uint32_t ch : {0u, 2u, 3u}) {
        CascadeTxAudioApi bad = tx;
        bad.channels = ch;
        CHECK(withTx(bad) == PluginRejection::TxAudioBadChannels);  // mono only
    }
    {
        CascadeTxAudioApi bad = tx;
        bad.flags = 1u;
        CHECK(withTx(bad) == PluginRejection::TxAudioReservedNotZero);
    }
    {
        CascadeTxAudioApi bad = tx;
        bad.pull = nullptr;
        CHECK(withTx(bad) == PluginRejection::MissingTxAudioFunction);
        bad = tx;
        bad.active = nullptr;
        CHECK(withTx(bad) == PluginRejection::MissingTxAudioFunction);
    }
    // The bit with no table at all.
    {
        const CascadeCapabilityEntry caps[] = {
            {CASCADE_CAP_DECODER, static_cast<std::uint32_t>(sizeof(CascadeDecoderApi)), &dec},
        };
        CHECK(validate(caps, 1u, CASCADE_CAP_DECODER | CASCADE_CAP_TX_AUDIO) ==
              PluginRejection::MissingTxAudioApi);
    }

    // A PLUGIN WITH THE BIT AND NO DECODER IS REFUSED, WITH THAT REASON - not loaded and silent, and
    // not "built for a newer version". Any one of the three decoder capabilities will do.
    {
        const CascadeCapabilityEntry caps[] = {
            {CASCADE_CAP_TX_AUDIO, static_cast<std::uint32_t>(sizeof(CascadeTxAudioApi)), &tx},
        };
        CHECK(validate(caps, 1u, CASCADE_CAP_TX_AUDIO) == PluginRejection::TxAudioNeedsDecoder);
        const std::string said =
            cascade::core::pluginRejectionMessage(PluginRejection::TxAudioNeedsDecoder);
        std::printf("    refused with: %s\n", said.c_str());
        CHECK(said.find("CASCADE_CAP_TX_AUDIO") != std::string::npos);
        CHECK(said.find("decoder") != std::string::npos);
    }
    {
        CascadeIqDecoderApi iq{};
        iq.structSize = static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi));
        iq.create = &abiIqCreate;
        iq.process = &abiProcess;
        iq.poll_text = &abiPoll;
        iq.destroy = &abiDestroy;
        const CascadeCapabilityEntry caps[] = {
            {CASCADE_CAP_IQ_DECODER, static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi)), &iq},
            {CASCADE_CAP_TX_AUDIO, static_cast<std::uint32_t>(sizeof(CascadeTxAudioApi)), &tx},
        };
        CHECK(validate(caps, 2u, CASCADE_CAP_IQ_DECODER | CASCADE_CAP_TX_AUDIO) ==
              PluginRejection::None);
    }

    // The numbers the author needs are in the message.
    {
        CascadeTxAudioApi bad = tx;
        bad.sampleRateHz = 44100u;
        const CascadeCapabilityEntry caps[] = {
            {CASCADE_CAP_DECODER, static_cast<std::uint32_t>(sizeof(CascadeDecoderApi)), &dec},
            {CASCADE_CAP_TX_AUDIO, static_cast<std::uint32_t>(sizeof(CascadeTxAudioApi)), &bad},
        };
        CascadePluginDesc d{};
        d.structSize = static_cast<std::uint32_t>(sizeof(CascadePluginDesc));
        d.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
        d.name = "Fake";
        d.version = "1.0.0";
        d.author = "";
        d.licence = "MIT";
        d.capabilities = CASCADE_CAP_DECODER | CASCADE_CAP_TX_AUDIO;
        d.capabilityCount = 2u;
        d.capabilityTables = caps;
        const std::string said =
            cascade::core::describePluginRejection(PluginRejection::TxAudioBadRate, &d);
        CHECK(said.find("44100") != std::string::npos);
        CHECK(said.find("48000") != std::string::npos);
    }
}

void testApiFlags() {
    std::printf("  get_state: the three flags, and who is told the last two\n");
    using cascade::core::PluginApiClient;
    using cascade::core::PluginApiCore;
    PluginApiCore core;
    PluginApiClient& tx = core.client("tx.dll", "Modem", CASCADE_CAP_DECODER | CASCADE_CAP_TX_AUDIO);
    PluginApiClient& plain = core.client("plain.dll", "Plain", CASCADE_CAP_DECODER);
    core.setLiveSet({"tx.dll", "plain.dll"});
    cascade::core::ReceiverFacts facts;
    facts.running = true;
    core.publish(facts);

    constexpr std::uint32_t kTxBits =
        CASCADE_STATE_TX_GRANTED | CASCADE_STATE_TX_ARMED | CASCADE_STATE_TRANSMITTING;
    const auto flagsOf = [&](const PluginApiClient& c) {
        CascadeReceiverState st{};
        st.structSize = static_cast<std::uint32_t>(sizeof(CascadeReceiverState));
        CHECK(core.getState(c, &st) == CASCADE_API_OK);
        return st.flags & kTxBits;
    };

    CHECK(flagsOf(tx) == 0u);     // a fresh install: not granted, not armed, not keyed
    CHECK(flagsOf(plain) == 0u);

    core.setTxGranted("tx.dll", true);
    CHECK(core.txGranted("tx.dll") && !core.txGranted("plain.dll"));
    CHECK(flagsOf(tx) == CASCADE_STATE_TX_GRANTED);
    CHECK(flagsOf(plain) == 0u);  // a grant is per plugin

    core.publishTxState(true, "");
    CHECK(flagsOf(tx) == (CASCADE_STATE_TX_GRANTED | CASCADE_STATE_TX_ARMED));
    CHECK(flagsOf(plain) == 0u);  // only a plugin that declared the capability is told the window is open

    core.publishTxState(true, "tx.dll");
    CHECK(flagsOf(tx) == kTxBits);
    CHECK(flagsOf(plain) == 0u);

    // KEYED FOR SOMEBODY ELSE: the other plugin is not told. (A plugin that never declared the
    // capability is never told anything about the transmitter, whoever is keyed.)
    core.publishTxState(true, "plain.dll");
    CHECK(flagsOf(tx) == (CASCADE_STATE_TX_GRANTED | CASCADE_STATE_TX_ARMED));
    CHECK(flagsOf(plain) == 0u);

    core.publishTxState(false, "");
    CHECK(flagsOf(tx) == CASCADE_STATE_TX_GRANTED);

    core.setTxGranted("tx.dll", false);
    CHECK(flagsOf(tx) == 0u);
    core.setTxGranted("tx.dll", true);
    core.clearGrants();  // a rescan clears the live grants; the owner re-applies them
    CHECK(flagsOf(tx) == 0u);
    CHECK(!core.txGranted("tx.dll"));
}

void testDecision() {
    std::printf("  the keying decision: every input on its own\n");
    using cascade::core::pluginMayKey;
    // Nothing on: no key. This is the state of a fresh install.
    CHECK(!pluginMayKey(PluginKeyInputs{}));
    // Everything on: a key.
    CHECK(pluginMayKey(allGood()));

    // EACH POSITIVE INPUT OFF, THE OTHER SEVEN ON. The names are the failure message.
    struct One {
        const char* name;
        void (*off)(PluginKeyInputs&);
    };
    const One ones[] = {
        {"granted", [](PluginKeyInputs& in) { in.granted = false; }},
        {"armed", [](PluginKeyInputs& in) { in.armed = false; }},
        {"pageLive", [](PluginKeyInputs& in) { in.pageLive = false; }},
        {"tickFresh", [](PluginKeyInputs& in) { in.tickFresh = false; }},
        {"active", [](PluginKeyInputs& in) { in.active = false; }},
        {"radioOk", [](PluginKeyInputs& in) { in.radioOk = false; }},
        {"handsOff", [](PluginKeyInputs& in) { in.handsOff = false; }},
        // faulted is the one input that is bad when TRUE.
        {"faulted", [](PluginKeyInputs& in) { in.faulted = true; }},
    };
    for (const One& o : ones) {
        PluginKeyInputs in = allGood();
        o.off(in);
        const bool key = pluginMayKey(in);
        if (key) { std::printf("    keyed with only %s wrong\n", o.name); }
        CHECK(!key);
    }

    // And the whole 2^8 table: exactly one combination keys.
    int keyed = 0;
    for (unsigned bits = 0; bits < 256u; ++bits) {
        PluginKeyInputs in;
        in.granted = (bits & 1u) != 0u;
        in.armed = (bits & 2u) != 0u;
        in.pageLive = (bits & 4u) != 0u;
        in.tickFresh = (bits & 8u) != 0u;
        in.active = (bits & 16u) != 0u;
        in.radioOk = (bits & 32u) != 0u;
        in.handsOff = (bits & 64u) != 0u;
        in.faulted = (bits & 128u) != 0u;
        if (pluginMayKey(in)) {
            ++keyed;
            CHECK(bits == 0x7Fu);  // all seven on, faulted off
        }
    }
    CHECK(keyed == 1);
}

// ===========================================================================
// 2. THE ARMING WINDOW
// ===========================================================================

void testArming() {
    std::printf("  the arming window: 5/15/60 minutes, refusals, expiry, never armed by default\n");
    using Clock = TxArming::Clock;
    const Clock::time_point t0 = Clock::now();

    TxArming w;
    CHECK(!w.armed());                 // a default-constructed window is CLOSED
    CHECK(w.secondsLeft(t0) == 0);
    CHECK(!w.update(t0));

    // Refusals, in the order an operator can fix them in: radio, then grant, then minutes.
    CHECK(w.arm(5, false, false, t0) == TxArming::Refusal::NoRadio);
    CHECK(w.arm(5, true, false, t0) == TxArming::Refusal::NoGrant);
    CHECK(w.arm(7, true, true, t0) == TxArming::Refusal::BadMinutes);
    CHECK(w.arm(0, true, true, t0) == TxArming::Refusal::BadMinutes);
    CHECK(w.arm(-5, true, true, t0) == TxArming::Refusal::BadMinutes);
    CHECK(w.arm(61, true, true, t0) == TxArming::Refusal::BadMinutes);
    CHECK(!w.armed());  // not one refusal opened it

    // The three lengths, each closing exactly when its time comes.
    for (const int minutes : {5, 15, 60}) {
        TxArming x;
        CHECK(x.arm(minutes, true, true, t0) == TxArming::Refusal::None);
        CHECK(x.armed() && x.minutes() == minutes);
        const auto len = std::chrono::minutes(minutes);
        CHECK(x.secondsLeft(t0) == minutes * 60);
        CHECK(x.update(t0 + len - std::chrono::milliseconds(1)));   // 1 ms before: open
        CHECK(x.secondsLeft(t0 + len - std::chrono::milliseconds(1)) == 1);  // rounded UP
        bool expired = false;
        CHECK(!x.update(t0 + len, &expired));                        // on the dot: closed
        CHECK(expired);
        CHECK(!x.armed());
        CHECK(x.lastReason() == "the arming window ran out");
        // Closed means closed, and only the call that closed it says "expired".
        expired = true;
        CHECK(!x.update(t0 + len + std::chrono::seconds(1), &expired));
        CHECK(!expired);
    }

    // A refused ARM leaves an open window exactly as it was.
    TxArming y;
    CHECK(y.arm(15, true, true, t0) == TxArming::Refusal::None);
    CHECK(y.arm(15, false, true, t0 + std::chrono::minutes(1)) == TxArming::Refusal::NoRadio);
    CHECK(y.armed() && y.minutes() == 15);
    CHECK(y.secondsLeft(t0) == 15 * 60);
    // ...and a new ARM RESTARTS it from now, replacing the length.
    CHECK(y.arm(5, true, true, t0 + std::chrono::minutes(10)) == TxArming::Refusal::None);
    CHECK(y.minutes() == 5);
    CHECK(y.secondsLeft(t0 + std::chrono::minutes(10)) == 300);

    // disarm() says why, once; a second disarm says nothing and keeps the first reason.
    CHECK(y.disarm("the TRANSMIT page was closed"));
    CHECK(!y.armed());
    CHECK(y.lastReason() == "the TRANSMIT page was closed");
    CHECK(!y.disarm("a second reason"));
    CHECK(y.lastReason() == "the TRANSMIT page was closed");
    // Arming again clears the reason.
    CHECK(y.arm(5, true, true, t0) == TxArming::Refusal::None);
    CHECK(y.lastReason().empty());

    CHECK(TxArming::validMinutes(5) && TxArming::validMinutes(15) && TxArming::validMinutes(60));
    CHECK(!TxArming::validMinutes(0) && !TxArming::validMinutes(10) && !TxArming::validMinutes(120));
}

// ===========================================================================
// 3. THE RING AND WHAT THE GUI IS TOLD
// ===========================================================================

void testRingNeverBlocks() {
    std::printf("  the ring: fed faster than it is drained, the writer never blocks or waits\n");
    TxAudioLink link;
    CHECK(link.backlog() == 0u);

    // Capacity: it accepts exactly its size and then refuses, returning at once.
    std::vector<float> chunk(1000);
    for (std::size_t i = 0; i < chunk.size(); ++i) { chunk[i] = static_cast<float>(i); }
    std::size_t accepted = 0;
    for (int i = 0; i < 100; ++i) { accepted += link.write(chunk.data(), chunk.size()); }
    CHECK(accepted == TxAudioLink::kRingFrames);
    CHECK(link.backlog() == TxAudioLink::kRingFrames);
    CHECK(link.write(chunk.data(), chunk.size()) == 0u);  // full: nothing, immediately
    link.clear();
    CHECK(link.backlog() == 0u);

    // Two threads for 300 ms: the writer pushes bursts as fast as it can without ever
    // sleeping, the reader takes 480 frames (one transmit block) every couple of milliseconds.
    // The writer must keep GOING (a blocking write would stop it at the first full ring), and
    // what the reader gets is an ordered subsequence of what was written.
    std::atomic<bool> stopWriter{false};
    std::atomic<std::size_t> offered{0};
    std::atomic<std::size_t> accepted2{0};
    std::thread writer([&] {
        std::vector<float> burst(512);
        std::size_t next = 0;
        while (!stopWriter.load(std::memory_order_acquire)) {
            for (std::size_t i = 0; i < burst.size(); ++i) { burst[i] = static_cast<float>(next + i); }
            const std::size_t put = link.write(burst.data(), burst.size());
            accepted2.fetch_add(put, std::memory_order_relaxed);
            offered.fetch_add(burst.size(), std::memory_order_relaxed);
            next += burst.size();  // moves on whether or not it was accepted: it does not wait
        }
    });
    std::vector<float> got(480);
    float last = -1.0f;
    std::size_t readTotal = 0;
    bool ordered = true;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(300)) {
        const std::size_t n = link.read(got.data(), got.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (!(got[i] > last)) { ordered = false; }
            last = got[i];
        }
        readTotal += n;
        sleepMs(2);
    }
    stopWriter.store(true, std::memory_order_release);
    writer.join();
    std::printf("    writer offered %zu frames, ring accepted %zu, reader took %zu\n",
                offered.load(), accepted2.load(), readTotal);
    CHECK(offered.load() > 20 * TxAudioLink::kRingFrames);  // it kept going, far past one ring
    CHECK(accepted2.load() < offered.load() / 2);           // and was much faster than the reader
    CHECK(readTotal > 1000u);                               // while the reader really did read
    CHECK(ordered);                                         // what got through is in order
}

void testLinkWantsKey() {
    std::printf("  the link: what the GUI is told about whether the plugin wants the key\n");
    TxAudioLink link;
    const std::int64_t t = TxAudioLink::nowMs();
    CHECK(!link.wantsKey(t));  // never published: nothing wanted

    link.publishActive(true, t);
    CHECK(link.wantsKey(t));
    CHECK(link.wantsKey(t + TxAudioLink::kActiveFreshMs));  // the edge of fresh
    // STALE: an old "yes" is not a request (the receiver stopped, the DSP thread is wedged).
    CHECK(!link.wantsKey(t + TxAudioLink::kActiveFreshMs + 1));
    CHECK(!link.wantsKey(t + 60000));

    // active() goes to zero with NOTHING queued: no key at once.
    link.publishActive(false, t + 10);
    CHECK(!link.wantsKey(t + 10));

    // active() goes to zero with audio still queued: the key stays to play it out, then goes.
    link.publishActive(true, t + 20);
    std::vector<float> audio(2000, 0.1f);
    CHECK(link.write(audio.data(), audio.size()) == audio.size());
    link.publishActive(false, t + 30);
    CHECK(link.wantsKey(t + 31));                                   // draining
    CHECK(link.wantsKey(t + 30 + TxAudioLink::kDrainMaxMs - 1));    // still inside the bound
    CHECK(!link.wantsKey(t + 30 + TxAudioLink::kDrainMaxMs));       // the bound: no key, whatever is left
    link.publishActive(false, t + 40);                               // the DSP thread keeps polling
    CHECK(link.wantsKey(t + 41));                                    // ...and that does not extend it
    std::vector<float> sink(4000);
    CHECK(link.read(sink.data(), sink.size()) == 2000u);
    CHECK(!link.wantsKey(t + 42));                                   // drained: the key goes

    // The plugin asks again while draining: the drain is over, it is just wanted.
    link.write(audio.data(), audio.size());
    link.publishActive(true, t + 50);
    link.publishActive(false, t + 60);
    link.publishActive(true, t + 70);
    CHECK(link.wantsKey(t + 71));
    link.publishActive(true, t + 5000);
    CHECK(link.wantsKey(t + 5001));  // fresh yes: no ring needed

    // publishNone (the holder is gone, or stopped for good): nothing queued is worth sending.
    link.publishNone(t + 6000);
    CHECK(!link.wantsKey(t + 6001));
    link.clear();
}

// ===========================================================================
// 4. THE RUNNER
// ===========================================================================

enum class PullMode { Tone, Nothing, Dead, Overstate };

struct FakeTx {
    int wants = 0;
    int created = 0;
    int destroyed = 0;
    int activeCalls = 0;
    int pulls = 0;
    std::size_t frames = 0;
    std::size_t maxAsk = 0;
    std::size_t lastAsk = 0;
    PullMode mode = PullMode::Tone;
    float value = 0.25f;
    bool failCreate = false;
};

FakeTx g_a;
FakeTx g_b;

FakeTx& self(void* h) { return *static_cast<FakeTx*>(h); }

void* createA(uint32_t) {
    if (g_a.failCreate) { return nullptr; }
    ++g_a.created;
    return &g_a;
}
void* createB(uint32_t) {
    if (g_b.failCreate) { return nullptr; }
    ++g_b.created;
    return &g_b;
}
void fakeProcess(void*, const float*, std::size_t) {}
std::int32_t fakePollText(void*, char*, std::size_t) { return 0; }
void fakeDestroy(void* h) { ++self(h).destroyed; }

std::int32_t fakeActive(void* h) {
    ++self(h).activeCalls;
    return self(h).wants;
}

std::int32_t fakePull(void* h, float* mono, std::size_t frames) {
    FakeTx& f = self(h);
    ++f.pulls;
    f.lastAsk = frames;
    f.maxAsk = std::max(f.maxAsk, frames);
    if (f.mode == PullMode::Dead) { return -1; }
    if (f.mode == PullMode::Nothing) { return 0; }
    std::size_t n = frames;
    if (f.mode == PullMode::Overstate) { n = frames + 500; }  // returns MORE than it was asked
    // Writes only what it was asked for (an overstating plugin lies about the count, it
    // does not scribble): the host must not trust the count.
    for (std::size_t i = 0; i < frames; ++i) { mono[i] = f.value; }
    f.frames += frames;
    return static_cast<std::int32_t>(n);
}

CascadeDecoderApi makeDecoder(void* (*create)(uint32_t)) {
    CascadeDecoderApi d{};
    d.structSize = static_cast<std::uint32_t>(sizeof(CascadeDecoderApi));
    d.requiredRateHz = 0;
    d.create = create;
    d.process = &fakeProcess;
    d.poll_text = &fakePollText;
    d.destroy = &fakeDestroy;
    return d;
}

CascadeTxAudioApi makeTx() {
    CascadeTxAudioApi t{};
    t.structSize = static_cast<std::uint32_t>(sizeof(CascadeTxAudioApi));
    t.sampleRateHz = 48000u;
    t.channels = 1u;
    t.flags = 0u;
    t.pull = &fakePull;
    t.active = &fakeActive;
    return t;
}

LoadedPlugin makePlugin(const char* name, const char* path, const CascadeDecoderApi* dec,
                        const CascadeTxAudioApi* tx) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.path = path;
    p.version = "1.0.0";
    p.capabilities = CASCADE_CAP_DECODER | CASCADE_CAP_TX_AUDIO;
    p.decoder = dec;
    p.txAudio = tx;
    return p;
}

struct Rig {
    CascadeDecoderApi decA = makeDecoder(&createA);
    CascadeDecoderApi decB = makeDecoder(&createB);
    CascadeTxAudioApi tx = makeTx();
    std::vector<LoadedPlugin> plugins;
    PluginRunner runner;
    std::shared_ptr<TxAudioLink> link = std::make_shared<TxAudioLink>();

    // A is "a.dll" and B is "b.dll"; load order is A then B.
    explicit Rig(bool withB = false) {
        g_a = FakeTx{};
        g_b = FakeTx{};
        plugins.push_back(makePlugin("Plugin A", "C:/p/a.dll", &decA, &tx));
        if (withB) { plugins.push_back(makePlugin("Plugin B", "C:/p/b.dll", &decB, &tx)); }
        runner.setTxLink(link);
        runner.rebuild(plugins, 48000.0, 2400000.0, 100.0e6);
    }
    void poll(int n = 1) {
        for (int i = 0; i < n; ++i) { runner.pollTxAudio(); }
    }
};

void testRunnerGating() {
    std::printf("  the runner: pull() only while granted AND keyed AND the plugin wants it\n");
    // R1. WANTS THE KEY, KEYED, BUT NOT GRANTED: polled (so it has ASKED), never pulled from.
    {
        Rig r;
        g_a.wants = 1;
        r.runner.setTxKeyed(true);
        r.poll(20);
        CHECK(g_a.activeCalls == 20);
        CHECK(g_a.pulls == 0);
        CHECK(r.link->backlog() == 0u);
        CHECK(!r.link->wantsKey(TxAudioLink::nowMs()));  // an ungranted plugin is not a request
        const PluginRunner::TxPluginState s = r.runner.txState("a.dll");
        CHECK(s.declared && s.asked && !s.holder);
        CHECK(r.runner.txAskers().size() == 1u);
    }
    // R2. GRANTED AND KEYED, BUT IT DOES NOT WANT THE KEY: asked, never pulled.
    {
        Rig r;
        r.runner.setTxGranted({"a.dll"});
        g_a.wants = 0;
        r.runner.setTxKeyed(true);
        r.poll(20);
        CHECK(g_a.activeCalls == 20);
        CHECK(g_a.pulls == 0);
        CHECK(!r.runner.txState("a.dll").asked);  // it has never wanted it: no key is offered
        CHECK(r.runner.txAskers().empty());
    }
    // R3. GRANTED AND WANTING, BUT THE HOST HAS NOT KEYED: it is a request, and no audio is taken.
    {
        Rig r;
        r.runner.setTxGranted({"a.dll"});
        g_a.wants = 1;
        r.poll(20);
        CHECK(g_a.pulls == 0);
        CHECK(r.link->backlog() == 0u);
        CHECK(r.link->wantsKey(TxAudioLink::nowMs()));
        CHECK(r.runner.txState("a.dll").holder);
        CHECK(r.runner.txHolderName() == "Plugin A");
    }
    // R4. ALL THREE: pulls fill the ring to the lead and no further, in bounded asks.
    {
        Rig r;
        r.runner.setTxGranted({"a.dll"});
        g_a.wants = 1;
        r.poll(2);
        r.runner.setTxKeyed(true);
        r.poll(50);
        CHECK(g_a.pulls >= 1);
        CHECK(r.link->backlog() == TxAudioLink::kLeadFrames);  // the lead, exactly: not 1 s
        CHECK(g_a.maxAsk <= TxAudioLink::kMaxPullFrames);
        const int before = g_a.pulls;
        r.poll(50);
        CHECK(g_a.pulls == before);  // a full lead asks for nothing more
        // The TX thread reads a block: the next poll asks for exactly that shortfall.
        std::vector<float> sink(480);
        CHECK(r.link->read(sink.data(), sink.size()) == 480u);
        r.poll();
        CHECK(g_a.lastAsk == 480u);
        CHECK(r.link->backlog() == TxAudioLink::kLeadFrames);
        CHECK(sink[0] == 0.25f);  // and it is the plugin's audio that came through
        // NOT KEYED ANY MORE: setTxKeyed(false) has returned, so no more is pulled.
        r.runner.setTxKeyed(false);
        const int frozen = g_a.pulls;
        r.link->read(sink.data(), sink.size());
        r.poll(20);
        CHECK(g_a.pulls == frozen);
    }
    // R5. THE GRANT TAKEN BACK WHILE KEYED stops the pulling at the next block, and the holder.
    {
        Rig r;
        r.runner.setTxGranted({"a.dll"});
        g_a.wants = 1;
        r.poll();
        r.runner.setTxKeyed(true);
        r.poll(5);
        const int before = g_a.pulls;
        CHECK(before >= 1);
        r.runner.setTxGranted({});
        // AT ONCE, with no audio block in between: the GUI reads the holder every frame.
        CHECK(r.runner.txHolderKey().empty());
        CHECK(!r.link->wantsKey(TxAudioLink::nowMs()));
        r.poll(20);
        CHECK(g_a.pulls == before);
        CHECK(r.runner.txHolderKey().empty());
        CHECK(!r.link->wantsKey(TxAudioLink::nowMs()));
    }
    // R6. pull() returning 0 is a gap, not an end: still the holder, still wanted, no error.
    {
        Rig r;
        r.runner.setTxGranted({"a.dll"});
        g_a.wants = 1;
        g_a.mode = PullMode::Nothing;
        r.poll();
        r.runner.setTxKeyed(true);
        r.poll(10);
        CHECK(g_a.pulls >= 1);
        CHECK(r.link->backlog() == 0u);
        CHECK(r.link->wantsKey(TxAudioLink::nowMs()));
        CHECK(r.runner.txState("a.dll").holder && !r.runner.txState("a.dll").stopped);
    }
    // R7. A PLUGIN THAT OVERSTATES WHAT IT WROTE cannot overfill the ring.
    {
        Rig r;
        r.runner.setTxGranted({"a.dll"});
        g_a.wants = 1;
        g_a.mode = PullMode::Overstate;
        r.poll();
        r.runner.setTxKeyed(true);
        r.poll(20);
        CHECK(r.link->backlog() == TxAudioLink::kLeadFrames);  // clamped to what was asked for
    }
}

void testRunnerStopsForGood() {
    std::printf("  the runner: a negative pull() stops that plugin for good\n");
    Rig r;
    r.runner.setTxGranted({"a.dll"});
    g_a.wants = 1;
    r.poll();
    r.runner.setTxKeyed(true);
    r.poll(3);
    g_a.mode = PullMode::Dead;
    // Drain the lead so the next poll must ask.
    std::vector<float> sink(8000);
    r.link->read(sink.data(), sink.size());
    r.poll();
    CHECK(r.runner.txState("a.dll").stopped);
    CHECK(r.runner.txHolderKey().empty());
    CHECK(!r.link->wantsKey(TxAudioLink::nowMs()));
    const int pulls = g_a.pulls;
    const int actives = g_a.activeCalls;
    r.poll(20);
    CHECK(g_a.pulls == pulls);        // never pulled again
    CHECK(g_a.activeCalls == actives); // and never even asked again
    // Even with a fresh keyed request and the grant: stopped is for good.
    r.runner.setTxKeyed(false);
    r.runner.setTxKeyed(true);
    r.poll(20);
    CHECK(g_a.pulls == pulls);
}

void testRunnerOneHolder() {
    std::printf("  the runner: exactly one plugin holds the transmitter, first in load order, sticky\n");
    Rig r(true);
    r.runner.setTxGranted({"a.dll", "b.dll"});
    g_a.wants = 1;
    g_b.wants = 1;
    r.poll(3);
    CHECK(r.runner.txHolderKey() == "a.dll");   // A is first in load order
    CHECK(r.runner.txState("b.dll").waiting);   // B is told so
    CHECK(!r.runner.txState("a.dll").waiting);
    r.runner.setTxKeyed(true);
    r.poll(30);
    CHECK(g_a.pulls >= 1);
    CHECK(g_b.pulls == 0);                      // a second plugin gets NOTHING
    CHECK(g_b.activeCalls >= 30);               // but is still asked, so it can wait its turn

    // A lets go while keyed, B wants it: the holder is STICKY until the key is open.
    g_a.wants = 0;
    const int aBefore = g_a.pulls;
    r.poll(20);
    CHECK(r.runner.txHolderKey() == "a.dll");
    CHECK(g_b.pulls == 0);
    CHECK(g_a.pulls == aBefore);  // A is not pulled (it does not want to be)
    // The key opens (the GUI saw A's drain end): B gets its turn.
    r.runner.setTxKeyed(false);
    r.link->clear();  // what the transmitter does at key-down: A's queued audio is not B's
    r.poll(3);
    CHECK(r.runner.txHolderKey() == "b.dll");
    r.runner.setTxKeyed(true);
    r.poll(20);
    CHECK(g_b.pulls >= 1);
    CHECK(g_a.pulls == aBefore);

    // Neither granted: nobody holds it.
    r.runner.setTxKeyed(false);
    r.runner.setTxGranted({});
    r.poll(3);
    CHECK(r.runner.txHolderKey().empty());
    CHECK(r.runner.txFirstGrantedName().empty());
    r.runner.setTxGranted({"b.dll"});
    CHECK(r.runner.txFirstGrantedName() == "Plugin B");
}

void testRunnerFreshnessAndDrain() {
    std::printf("  the runner: the plugin's active() is fresh or it is not a request; the tail plays out\n");
    Rig r;
    r.runner.setTxGranted({"a.dll"});
    g_a.wants = 1;
    r.poll(2);
    r.runner.setTxKeyed(true);
    r.poll(5);
    CHECK(r.link->wantsKey(TxAudioLink::nowMs()));
    // The DSP thread stops polling (the receiver was stopped): after the fresh bound the plugin's
    // "yes" is no longer a request, however loud it was.
    sleepMs(static_cast<int>(TxAudioLink::kActiveFreshMs) + 150);
    CHECK(!r.link->wantsKey(TxAudioLink::nowMs()));
    r.poll();
    CHECK(r.link->wantsKey(TxAudioLink::nowMs()));  // polled again: wanted again

    // The plugin lets go with the lead queued: the key stays to play it out...
    g_a.wants = 0;
    r.poll();
    CHECK(r.link->backlog() > 0u);
    CHECK(r.link->wantsKey(TxAudioLink::nowMs()));
    const int pulls = g_a.pulls;
    r.poll(10);
    CHECK(g_a.pulls == pulls);  // ...without asking it for any more audio...
    std::vector<float> sink(8000);
    CHECK(r.link->read(sink.data(), sink.size()) > 0u);
    CHECK(!r.link->wantsKey(TxAudioLink::nowMs()));  // ...and goes the moment it is out.
}

void testRunnerRebuild() {
    std::printf("  the runner: asking survives a rebuild; a plugin with no decoder has no claim\n");
    {
        Rig r;
        r.runner.setTxGranted({"a.dll"});
        g_a.wants = 1;
        r.poll(3);
        CHECK(r.runner.txState("a.dll").asked);
        r.runner.rebuild(r.plugins, 48000.0, 2400000.0, 100.0e6);  // a rate change, a rescan
        CHECK(r.runner.txState("a.dll").asked);   // it asked once; the key stays on its plate
        CHECK(r.runner.txAskers().size() == 1u);
        r.runner.clear();
        CHECK(!r.runner.txState("a.dll").declared);
        CHECK(r.runner.txHolderKey().empty());
        CHECK(!r.link->wantsKey(TxAudioLink::nowMs()));
    }
    {
        // Its decoder failed to start: there is no handle to ride on, so no transmit claim.
        g_a = FakeTx{};
        g_a.failCreate = true;
        Rig r;  // resets g_a, so set the failure again and rebuild
        g_a.failCreate = true;
        r.runner.rebuild(r.plugins, 48000.0, 2400000.0, 100.0e6);
        CHECK(!r.runner.txState("a.dll").declared);
        r.runner.setTxGranted({"a.dll"});
        g_a.wants = 1;
        r.poll(5);
        CHECK(g_a.pulls == 0 && g_a.activeCalls == 0);
    }
    {
        // stopPlugin takes the holder with it, and the GUI is told nobody wants the key.
        Rig r;
        r.runner.setTxGranted({"a.dll"});
        g_a.wants = 1;
        r.poll(3);
        CHECK(r.runner.txHolderKey() == "a.dll");
        CHECK(r.link->wantsKey(TxAudioLink::nowMs()));
        r.runner.stopPlugin("a.dll");
        CHECK(r.runner.txHolderKey().empty());
        CHECK(!r.link->wantsKey(TxAudioLink::nowMs()));
        CHECK(!r.runner.txState("a.dll").declared);
    }
}

// ===========================================================================
// 5. THE TRANSMITTER
// ===========================================================================

// A radio that keeps every sample, at 480 kS/s (an integer ten times the audio rate, so the
// interpolator is exact enough to demodulate), and can be told to fault or to refuse.
class CaptureSink : public cascade::source::IqSink {
public:
    bool start() override {
        if (refuseStart) {
            error_ = "this test radio was told to refuse";
            return false;
        }
        ++starts;
        running_ = true;
        return true;
    }
    void stop() override {
        if (running_) { ++stops; }
        running_ = false;
    }
    void finish() override { stop(); }
    bool running() const override { return running_; }
    double sampleRateHz() const override { return 480000.0; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return 145.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 70.0e6;
        hi = 6.0e9;
        return true;
    }
    bool sampleRateRangeHz(double& lo, double& hi) const override {
        lo = 2.083e6;
        hi = 61.44e6;
        return true;
    }
    bool gainRangeDb(double& lo, double& hi) const override {
        lo = -89.75;
        hi = 0.0;
        return true;
    }
    double gainDb() const override { return -89.75; }
    bool setGainDb(double) override { return true; }
    std::size_t write(const std::complex<float>* s, std::size_t n) override {
        if (!running_ || faulted_) { return 0; }
        std::lock_guard<std::mutex> lk(m_);
        all_.insert(all_.end(), s, s + n);
        if (faultAfter > 0 && static_cast<long long>(all_.size()) >= faultAfter) {
            faulted_ = true;
            error_ = "this test radio was told to fault";
        }
        return n;
    }
    bool faulted() const override { return faulted_; }
    const char* name() const override { return "capture radio"; }
    const char* lastError() const override { return error_.c_str(); }

    std::vector<std::complex<float>> samples() const {
        std::lock_guard<std::mutex> lk(m_);
        return all_;
    }
    void forget() {
        std::lock_guard<std::mutex> lk(m_);
        all_.clear();
    }

    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    bool refuseStart = false;
    long long faultAfter = 0;

private:
    mutable std::mutex m_;
    std::vector<std::complex<float>> all_;
    std::atomic<bool> running_{false};
    std::atomic<bool> faulted_{false};
    std::string error_;
};

// Stands in for the DSP thread: writes a steady tone into the ring at REAL TIME while `on` is
// set, the way the runner does while it is keyed. Paced from the wall clock and not from a
// sleep: Windows sleeps in 15 ms steps, and a feeder that wrote 240 frames after each
// sleep(5) supplied a third of the audio and measured as a third of the deviation.
class Feeder {
public:
    Feeder(const std::shared_ptr<TxAudioLink>& link, double toneHz, float amp)
        : link_(link), toneHz_(toneHz), amp_(amp), thread_([this] { run(); }) {}
    ~Feeder() {
        stop_.store(true);
        thread_.join();
    }
    std::atomic<bool> on{false};
    std::atomic<std::size_t> offered{0};

private:
    void run() {
        std::vector<float> blk(240);
        double phase = 0.0;
        const double step = 2.0 * kPi * toneHz_ / 48000.0;
        const auto t0 = std::chrono::steady_clock::now();
        double produced = 0.0;  // frames the wall clock says should exist by now
        bool wasOn = false;
        while (!stop_.load()) {
            const double owed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() *
                48000.0;
            if (on.load()) {
                if (!wasOn) {
                    produced = owed;  // no backfill of the time it was off
                    wasOn = true;
                }
                while (owed - produced >= 1.0) {
                    const std::size_t n = std::min<std::size_t>(
                        blk.size(), static_cast<std::size_t>(owed - produced));
                    for (std::size_t i = 0; i < n; ++i) {
                        blk[i] = amp_ * static_cast<float>(std::sin(phase));
                        phase += step;
                    }
                    link_->write(blk.data(), n);
                    offered.fetch_add(n);
                    produced += static_cast<double>(n);
                }
            } else {
                wasOn = false;
            }
            sleepMs(1);
        }
    }
    std::shared_ptr<TxAudioLink> link_;
    double toneHz_;
    float amp_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// Ticks like the frame loop until `fn` is true or the bound passes.
template <typename Fn>
bool waitTicking(Transmitter& tx, Fn fn, int boundMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(boundMs);
    while (std::chrono::steady_clock::now() < deadline) {
        tx.tick();
        if (fn()) { return true; }
        sleepMs(2);
    }
    tx.tick();
    return fn();
}

void tickFor(Transmitter& tx, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        tx.tick();
        sleepMs(2);
    }
    tx.tick();
}

// The amplitude, in Hz of deviation, of `toneHz` in the instantaneous frequency of an FM
// signal sampled at fs - the thing a receiver's discriminator would hear. The 80th percentile
// of the amplitude over 50 ms stretches, so stretches in which a loaded machine starved the
// transmit thread of its audio do not drag the figure down: it measures the modulator, not how
// busy the desk was (other builders share this machine).
double fmDeviationAt(const std::vector<std::complex<float>>& x, std::size_t from, std::size_t to,
                     double fs, double toneHz) {
    const std::size_t chunk = static_cast<std::size_t>(fs * 0.05);
    std::vector<double> amps;
    for (std::size_t base = from; base + chunk <= to && base + chunk <= x.size(); base += chunk) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t k = base + 1; k < base + chunk; ++k) {
            const std::complex<double> a(x[k].real(), x[k].imag());
            const std::complex<double> b(x[k - 1].real(), x[k - 1].imag());
            const double f = std::arg(a * std::conj(b)) * fs / (2.0 * kPi);
            const double t = 2.0 * kPi * toneHz * static_cast<double>(k) / fs;
            re += f * std::cos(t);
            im += f * std::sin(t);
        }
        amps.push_back(2.0 * std::sqrt(re * re + im * im) / static_cast<double>(chunk - 1));
    }
    if (amps.empty()) { return 0.0; }
    std::sort(amps.begin(), amps.end());
    return amps[amps.size() * 4 / 5];
}

double magAt(const std::vector<std::complex<float>>& x, std::size_t from, std::size_t to,
             double freqHz, double fs) {
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t k = from; k < to && k < x.size(); ++k) {
        const double ang = -2.0 * kPi * freqHz * static_cast<double>(k) / fs;
        acc += std::complex<double>(x[k].real(), x[k].imag()) *
               std::complex<double>(std::cos(ang), std::sin(ang));
    }
    return std::abs(acc) / static_cast<double>(to - from);
}

struct Capture {
    std::vector<std::complex<float>> iq;
    TxMode effective = TxMode::CW;
    bool pre = true;
    TxInput source = TxInput::Tone;
    bool keyed = false;
    std::uint64_t frames = 0;
};

// Keys the transmitter with the PLUGIN's key for `runMs`, a 3 kHz tone of amplitude 0.2 in the
// ring, and reports what went to the radio and how the modulator was set while it did.
Capture runPlugin(TxMode pageMode, bool dataFlat, int runMs = 500) {
    Transmitter tx;
    auto link = std::make_shared<TxAudioLink>();
    tx.setPluginLink(link);
    auto sink = std::make_unique<CaptureSink>();
    CaptureSink* raw = sink.get();
    tx.setSink(std::move(sink));
    tx.setMode(pageMode);
    tx.setDataFlat(dataFlat);
    Feeder feeder(link, 3000.0, 0.2f);
    feeder.on = true;
    tx.setPluginKey(true);
    Capture c;
    c.keyed = waitTicking(tx, [&] { return tx.pluginKeyed(); }, 2000);
    tickFor(tx, runMs);
    c.effective = tx.effectiveMode();
    c.pre = tx.preemphasis();
    c.source = tx.activeInput();
    c.frames = tx.pluginFramesSent();
    tx.setPluginKey(false);
    waitTicking(tx, [&] { return !tx.transmitting(); }, 2000);
    feeder.on = false;
    c.iq = raw->samples();
    return c;
}

void testTransmitterPluginAudio() {
    std::printf("  the transmitter: plugin audio through the modulator, flat by default\n");
    constexpr double kFs = 480000.0;

    // DATA (flat) ticked (the default), page mode USB: the plugin still goes out as NFM,
    // pre-emphasis OFF. A 0.2-amplitude 3 kHz tone is 0.2 * 2500 = 500 Hz of deviation.
    const Capture flat = runPlugin(TxMode::USB, true);
    CHECK(flat.keyed);
    CHECK(flat.source == TxInput::Plugin);
    CHECK(flat.effective == TxMode::NFM);
    CHECK(!flat.pre);
    CHECK(flat.frames > 4800u);
    CHECK(flat.iq.size() > 100000u);
    std::printf("    frames of plugin audio sent %llu\n", static_cast<unsigned long long>(flat.frames));
    const double flatDev = fmDeviationAt(flat.iq, 120000, flat.iq.size(), kFs, 3000.0);
    std::printf("    DATA flat, page USB: NFM, %.0f Hz deviation for 0.2 at 3 kHz (flat: 500)\n", flatDev);
    CHECK_NEAR(flatDev, 500.0, 40.0);

    // DATA (flat) UNTICKED with page mode NFM: the microphone's own pre-emphasis applies, so a
    // 3 kHz tone is lifted by about 2.9x against the flat one.
    const Capture emph = runPlugin(TxMode::NFM, false);
    CHECK(emph.keyed);
    CHECK(emph.effective == TxMode::NFM);
    CHECK(emph.pre);
    const double emphDev = fmDeviationAt(emph.iq, 120000, emph.iq.size(), kFs, 3000.0);
    std::printf("    DATA unticked, page NFM: %.0f Hz deviation (pre-emphasised)\n", emphDev);
    CHECK(emphDev > flatDev * 2.5);
    CHECK(emphDev < flatDev * 3.4);

    // DATA (flat) UNTICKED with page mode USB: the PAGE'S mode, upper sideband - the tone is on
    // the positive side of the carrier and not on the negative.
    const Capture usb = runPlugin(TxMode::USB, false);
    CHECK(usb.keyed);
    CHECK(usb.effective == TxMode::USB);
    const double up = magAt(usb.iq, 120000, usb.iq.size(), 3000.0, kFs);
    const double down = magAt(usb.iq, 120000, usb.iq.size(), -3000.0, kFs);
    std::printf("    DATA unticked, page USB: +3 kHz %.4f, -3 kHz %.5f\n", up, down);
    CHECK(up > 0.03);
    CHECK(down < up * 0.05);
}

void testTransmitterStallAndStop() {
    std::printf("  the transmitter: a stall sends silence and keeps the key only while asked\n");
    Transmitter tx;
    auto link = std::make_shared<TxAudioLink>();
    tx.setPluginLink(link);
    auto sink = std::make_unique<CaptureSink>();
    CaptureSink* raw = sink.get();
    tx.setSink(std::move(sink));
    tx.setMode(TxMode::NFM);

    // Asked, with NOTHING in the ring (pull() returning 0): keyed, and a carrier with no
    // modulation - the frequency does not move.
    tx.setPluginKey(true);
    CHECK(waitTicking(tx, [&] { return tx.pluginKeyed(); }, 2000));
    tickFor(tx, 300);
    CHECK(tx.pluginKeyed());
    CHECK(tx.pluginUnderruns() > 5u);
    const std::vector<std::complex<float>> quiet = raw->samples();
    CHECK(quiet.size() > 50000u);
    CHECK(fmDeviationAt(quiet, 100000, quiet.size(), 480000.0, 3000.0) < 5.0);
    // ...and every sample sent in the stall is the unmodulated carrier, inside the unit circle.
    float worst = 0.0f;
    for (const auto& s : quiet) { worst = std::max(worst, std::abs(s)); }
    CHECK(worst <= 1.0001f);

    // No longer asked: unkeyed, the radio told to stop, nothing more sent.
    tx.setPluginKey(false);
    CHECK(waitTicking(tx, [&] { return !tx.transmitting(); }, 2000));
    CHECK(raw->stops.load() >= 1);
    CHECK(!tx.pluginFault());  // letting go is not a fault
    const std::size_t after = raw->samples().size();
    tickFor(tx, 100);
    CHECK(raw->samples().size() == after);
}

void testTransmitterCap() {
    std::printf("  the transmitter: the cap is the latch's own number, and it ends the plugin's key\n");
    Transmitter tx;
    auto link = std::make_shared<TxAudioLink>();
    tx.setPluginLink(link);
    auto sink = std::make_unique<CaptureSink>();
    CaptureSink* raw = sink.get();
    tx.setSink(std::move(sink));
    // THE LATCH'S OWN KNOB: if the plugin's cap were a second constant this would not move it.
    tx.setLatchTimeoutForTest(std::chrono::milliseconds(400));
    Feeder feeder(link, 1000.0, 0.3f);
    feeder.on = true;

    tx.setPluginKey(true);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(waitTicking(tx, [&] { return tx.pluginKeyed(); }, 2000));
    // The plugin keeps asking (the frame loop calls setPluginKey(true) every frame).
    bool cut = false;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(3000)) {
        tx.setPluginKey(true);
        tx.tick();
        if (!tx.transmitting()) {
            cut = true;
            break;
        }
        sleepMs(2);
    }
    const double heldMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("    key held %.0f ms against a 400 ms cap\n", heldMs);
    CHECK(cut);
    CHECK(heldMs >= 350.0);
    CHECK(heldMs < 1500.0);
    CHECK(tx.pluginFault());
    CHECK(tx.pluginFaultReason().find("held the key") != std::string::npos);
    CHECK(tx.lastAutoUnkeyReason().find("held the key") != std::string::npos);
    CHECK(!tx.pluginKeyRequested());

    // THE PLUGIN IS NOT LET STRAIGHT BACK IN: it keeps asking, and the fault latch refuses.
    const int starts = raw->starts.load();
    for (int i = 0; i < 100; ++i) {
        tx.setPluginKey(true);
        tx.tick();
        sleepMs(2);
    }
    CHECK(!tx.transmitting());
    CHECK(raw->starts.load() == starts);
    CHECK(!tx.pluginKeyRequested());

    // ...until the operator arms again, which clears the latch.
    tx.clearPluginFault();
    CHECK(!tx.pluginFault());
    tx.setPluginKey(true);
    CHECK(waitTicking(tx, [&] { return tx.pluginKeyed(); }, 2000));
    CHECK(raw->starts.load() == starts + 1);
    tx.setPluginKey(false);
    waitTicking(tx, [&] { return !tx.transmitting(); }, 2000);
    feeder.on = false;
}

void testTransmitterFaults() {
    std::printf("  the transmitter: a fault, the frozen window and a refusal cut the plugin's key\n");
    // A RADIO FAULT MID-TRANSMISSION.
    {
        Transmitter tx;
        auto link = std::make_shared<TxAudioLink>();
        tx.setPluginLink(link);
        auto sink = std::make_unique<CaptureSink>();
        sink->faultAfter = 60000;
        tx.setSink(std::move(sink));
        Feeder feeder(link, 1000.0, 0.3f);
        feeder.on = true;
        CHECK(tx.faultCount() == 0u);
        tx.setPluginKey(true);
        CHECK(waitTicking(tx, [&] { return tx.pluginKeyed(); }, 2000));
        for (int i = 0; i < 300 && tx.faultCount() == 0u; ++i) {
            tx.setPluginKey(true);
            tx.tick();
            sleepMs(2);
        }
        CHECK(tx.faultCount() == 1u);
        CHECK(!tx.transmitting());
        CHECK(!tx.pluginKeyed());
        CHECK(tx.pluginFault());                 // latched out: a fault is not retried
        CHECK(!tx.pluginKeyRequested());
        CHECK(tx.pluginFaultReason().find("faulted") != std::string::npos);
        feeder.on = false;
    }
    // THE FROZEN WINDOW: nothing ticks for longer than the dead-man's handle.
    {
        Transmitter tx;
        auto link = std::make_shared<TxAudioLink>();
        tx.setPluginLink(link);
        auto sink = std::make_unique<CaptureSink>();
        CaptureSink* raw = sink.get();
        tx.setSink(std::move(sink));
        Feeder feeder(link, 1000.0, 0.3f);
        feeder.on = true;
        tx.setPluginKey(true);
        CHECK(waitTicking(tx, [&] { return tx.pluginKeyed(); }, 2000));
        sleepMs(static_cast<int>(Transmitter::kKeyAliveWait.count()) + 400);
        // The thread has let go by itself; the frames come back and the plugin is still asking.
        const int starts = raw->starts.load();
        for (int i = 0; i < 60; ++i) {
            tx.setPluginKey(true);
            tx.tick();
            sleepMs(2);
        }
        CHECK(!tx.transmitting());
        CHECK(tx.faultCount() == 1u);
        CHECK(tx.pluginFault());
        CHECK(raw->starts.load() == starts);     // NOT re-keyed on thaw
        CHECK(tx.pluginFaultReason().find("stopped responding") != std::string::npos);
        feeder.on = false;
    }
    // THE RADIO WOULD NOT KEY.
    {
        Transmitter tx;
        auto sink = std::make_unique<CaptureSink>();
        sink->refuseStart = true;
        tx.setSink(std::move(sink));
        tx.setPluginKey(true);
        tx.tick();
        CHECK(!tx.transmitting());
        CHECK(tx.faultCount() == 1u);
        CHECK(tx.pluginFault());
        CHECK(!tx.pluginKeyRequested());
        for (int i = 0; i < 20; ++i) {
            tx.setPluginKey(true);
            tx.tick();
        }
        CHECK(tx.faultCount() == 1u);  // no retry loop on the radio
    }
    // stop() withdraws the request but is not a way to clear a fault.
    {
        Transmitter tx;
        tx.setSink(std::make_unique<CaptureSink>());
        tx.setPluginKey(true);
        CHECK(tx.pluginKeyRequested());
        tx.stop();
        CHECK(!tx.pluginKeyRequested());
    }
}

void testTransmitterHandPrecedence() {
    std::printf("  the transmitter: a hand has precedence; plugin audio only when its key is alone\n");
    Transmitter tx;
    auto link = std::make_shared<TxAudioLink>();
    tx.setPluginLink(link);
    auto sink = std::make_unique<CaptureSink>();
    CaptureSink* raw = sink.get();
    tx.setSink(std::move(sink));
    tx.setInput(TxInput::Tone);
    tx.setMode(TxMode::NFM);
    tx.setToneHz(1000.0);
    Feeder feeder(link, 3000.0, 0.2f);
    feeder.on = true;

    tx.setPluginKey(true);
    CHECK(waitTicking(tx, [&] { return tx.pluginKeyed(); }, 2000));
    CHECK(tx.activeInput() == TxInput::Plugin);
    CHECK(!tx.preemphasis());

    // The operator presses PTT: the plugin's key stops being the only one. The audio is the
    // tone's (the page's input) and the pre-emphasis is back; the plugin's ring is not read.
    tx.setPttHeld(true);
    tickFor(tx, 100);
    CHECK(tx.transmitting());
    CHECK(!tx.pluginKeyed());
    CHECK(tx.activeInput() == TxInput::Tone);
    CHECK(tx.preemphasis());
    CHECK(tx.effectiveMode() == TxMode::NFM);
    const std::uint64_t read1 = tx.pluginFramesSent();
    tickFor(tx, 150);
    CHECK(tx.pluginFramesSent() == read1);   // not a frame of the plugin's went out under the hand
    const std::size_t backlog = link->backlog();
    CHECK(backlog > 2000u);                   // it is simply sitting in the ring, unread

    // PTT released: the plugin (still asking) is the only key again.
    tx.setPttHeld(false);
    CHECK(waitTicking(tx, [&] { return tx.pluginKeyed(); }, 1000));
    CHECK(tx.activeInput() == TxInput::Plugin);
    tx.setPluginKey(false);
    waitTicking(tx, [&] { return !tx.transmitting(); }, 2000);
    feeder.on = false;
    (void)raw;
}

void testSanitiser() {
    std::printf("  the sanitiser: NaN and infinity are silence, the rest is held to +/-1\n");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> a = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.0001f, -1.0001f, 50.0f, -50.0f,
                            nan, inf, -inf, 3.0e38f, -3.0e38f, 1.0e-30f};
    const std::vector<float> want = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, -1.0f,
                                     0.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0e-30f};
    // A NaN and the two infinities are not audio: silence. A finite sample, however large, is
    // audio that is too loud: held to +/-1 on its own side.
    cascade::core::sanitiseTxAudio(a.data(), a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != want[i]) { std::printf("    [%zu] became %g, wanted %g\n", i, a[i], want[i]); }
        CHECK(a[i] == want[i]);
    }
    // Inside the range is left exactly alone (no rounding, no scaling).
    std::vector<float> ok(480);
    for (std::size_t i = 0; i < ok.size(); ++i) {
        ok[i] = 0.9f * std::sin(0.01f * static_cast<float>(i));
    }
    const std::vector<float> copy = ok;
    cascade::core::sanitiseTxAudio(ok.data(), ok.size());
    CHECK(ok == copy);
    cascade::core::sanitiseTxAudio(nullptr, 10);  // no fault
    float one = 7.0f;
    cascade::core::sanitiseTxAudio(&one, 0);      // no frames: untouched
    CHECK(one == 7.0f);
}

void testTransmitterHostileAudio() {
    std::printf("  the transmitter: NaN, infinity and out-of-range samples never reach the modulator\n");
    // END TO END. The modulators guard their own input as well (dsp/modulator.cpp), so this
    // passes with or without the transmitter's sanitiser - the sanitiser's own proof is the
    // function test above. What this pins is the property the contract promises: whatever a
    // plugin returns, what reaches the radio is finite and inside the unit circle.
    Transmitter tx;
    auto link = std::make_shared<TxAudioLink>();
    tx.setPluginLink(link);
    auto sink = std::make_unique<CaptureSink>();
    CaptureSink* raw = sink.get();
    tx.setSink(std::move(sink));
    tx.setMode(TxMode::USB);
    tx.setDataFlat(false);  // upper sideband: the arm where an amplitude of 50 shows

    std::atomic<bool> feeding{true};
    std::thread bad([&] {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        std::vector<float> blk(240);
        std::size_t k = 0;
        while (feeding.load()) {
            for (float& s : blk) {
                switch (k++ % 6) {
                    case 0: s = 50.0f; break;
                    case 1: s = nan; break;
                    case 2: s = -inf; break;
                    case 3: s = -50.0f; break;
                    case 4: s = inf; break;
                    default: s = 0.5f; break;
                }
            }
            link->write(blk.data(), blk.size());
            sleepMs(5);
        }
    });
    tx.setPluginKey(true);
    CHECK(waitTicking(tx, [&] { return tx.pluginKeyed(); }, 2000));
    tickFor(tx, 400);
    tx.setPluginKey(false);
    waitTicking(tx, [&] { return !tx.transmitting(); }, 2000);
    feeding = false;
    bad.join();

    const std::vector<std::complex<float>> iq = raw->samples();
    CHECK(iq.size() > 100000u);
    bool finite = true;
    float worst = 0.0f;
    for (const auto& s : iq) {
        if (!std::isfinite(s.real()) || !std::isfinite(s.imag())) { finite = false; }
        worst = std::max(worst, std::abs(s));
    }
    std::printf("    %zu samples, finite %d, largest magnitude %.4f\n", iq.size(), finite ? 1 : 0, worst);
    CHECK(finite);
    CHECK(worst <= 1.0001f);
}

void testTransmitterInputsAndRing() {
    std::printf("  the transmitter: a plugin is never a chosen input; the ring is emptied at key-down\n");
    // PLUGIN IS NOT SOMETHING A HAND CAN SELECT.
    {
        CHECK(cascade::core::txInputFromIndex(static_cast<int>(TxInput::Plugin)) == TxInput::Tone);
        CHECK(cascade::core::txInputFromIndex(2) == TxInput::Tone);
        CHECK(cascade::core::txInputFromIndex(1) == TxInput::Tone);
        CHECK(cascade::core::txInputFromIndex(0) == TxInput::Microphone);
        TxInput back = TxInput::Microphone;
        CHECK(!cascade::core::txInputFromName("PLUGIN", back));
        CHECK(std::string(cascade::core::txInputName(TxInput::Plugin)) == "PLUGIN");
        static_assert(cascade::core::kTxHandInputCount == 2, "MIC and TONE are the hand's inputs");
        Transmitter tx;
        tx.setInput(TxInput::Plugin);
        CHECK(tx.input() == TxInput::Tone);
        CHECK(tx.activeInput() == TxInput::Tone);  // and no plugin key is down
    }
    // STALE AUDIO IS NOT SENT: queued before the key went down, it is gone at key-down.
    {
        Transmitter tx;
        auto link = std::make_shared<TxAudioLink>();
        tx.setPluginLink(link);
        auto sink = std::make_unique<CaptureSink>();
        CaptureSink* raw = sink.get();
        tx.setSink(std::move(sink));
        tx.setMode(TxMode::NFM);
        std::vector<float> old(4800);
        for (std::size_t i = 0; i < old.size(); ++i) {
            old[i] = 0.4f * static_cast<float>(std::sin(2.0 * kPi * 2000.0 * static_cast<double>(i) / 48000.0));
        }
        CHECK(link->write(old.data(), old.size()) == old.size());
        tx.setPluginKey(true);
        CHECK(waitTicking(tx, [&] { return tx.pluginKeyed(); }, 2000));
        tickFor(tx, 150);
        const std::vector<std::complex<float>> iq = raw->samples();
        CHECK(iq.size() > 20000u);
        // Nothing was fed after the key went down, so the carrier is unmodulated: had the old
        // 2 kHz audio been sent there would be a 2 kHz deviation of 0.4 * 2500 = 1000 Hz.
        CHECK(fmDeviationAt(iq, 20000, iq.size(), 480000.0, 2000.0) < 10.0);
        CHECK(tx.pluginFramesSent() == 0u);
        tx.setPluginKey(false);
        waitTicking(tx, [&] { return !tx.transmitting(); }, 2000);
        // ...and what is queued when the key goes up is not sent at the next one either.
        link->write(old.data(), old.size());
        tx.setPluginKey(true);
        CHECK(waitTicking(tx, [&] { return tx.pluginKeyed(); }, 2000));
        CHECK(tx.pluginFramesSent() == 0u);
        tx.setPluginKey(false);
        waitTicking(tx, [&] { return !tx.transmitting(); }, 2000);
    }
}

}  // namespace

int main() {
    cascade::core::DiagLog::instance().resetForTest();
    testAbiSurface();
    testAbiSurface();
    testApiFlags();
    testDecision();
    testArming();
    testRingNeverBlocks();
    testLinkWantsKey();
    testRunnerGating();
    testRunnerStopsForGood();
    testRunnerOneHolder();
    testRunnerFreshnessAndDrain();
    testRunnerRebuild();
    testTransmitterPluginAudio();
    testTransmitterStallAndStop();
    testTransmitterCap();
    testTransmitterFaults();
    testTransmitterHandPrecedence();
    testSanitiser();
    testTransmitterHostileAudio();
    testTransmitterInputsAndRing();
    return testSummary("test_tx_audio");
}
