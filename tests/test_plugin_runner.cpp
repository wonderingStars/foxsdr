// PluginRunner: the class that actually drives decoder plugins with audio.
//
// No DLL is loaded here. LoadedPlugin carries plain pointers to API tables, so
// a fake decoder is a static table plus a counter - which lets every case below
// assert on what the runner DID to the decoder (how many samples, how many
// instances, in what order it destroyed them) rather than on side effects
// observed through a real plugin.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/plugin_runner.hpp"
#include "test_check.hpp"

namespace {

using cascade::core::DecodedLine;
using cascade::core::DecoderIdleReason;
using cascade::core::DecoderStatus;
using cascade::core::LoadedPlugin;
using cascade::core::PluginRunner;

// --- A fake decoder whose behaviour each test can steer -------------------

struct FakeState {
    int created = 0;
    int destroyed = 0;
    uint32_t lastRate = 0;
    std::size_t samples = 0;
    std::string queued;    // handed out by poll_text, a chunk at a time
    std::size_t chunk = 0; // 0 = all at once
    bool failCreate = false;
    bool failPoll = false; // poll_text returns < 0
};

FakeState g_fake;

void* fakeCreate(uint32_t rateHz) {
    if (g_fake.failCreate) { return nullptr; }
    ++g_fake.created;
    g_fake.lastRate = rateHz;
    return &g_fake;  // any non-null handle
}

void fakeProcess(void*, const float*, size_t count) { g_fake.samples += count; }

int32_t fakePoll(void*, char* buf, size_t cap) {
    if (g_fake.failPoll) { return -1; }
    if (g_fake.queued.empty()) { return 0; }
    std::size_t n = g_fake.queued.size();
    if (g_fake.chunk != 0 && g_fake.chunk < n) { n = g_fake.chunk; }
    if (n > cap) { n = cap; }
    std::memcpy(buf, g_fake.queued.data(), n);
    g_fake.queued.erase(0, n);
    return static_cast<int32_t>(n);
}

void fakeDestroy(void*) { ++g_fake.destroyed; }

CascadeDecoderApi makeApi(uint32_t rateHz) {
    CascadeDecoderApi a{};
    a.structSize = static_cast<uint32_t>(sizeof(CascadeDecoderApi));
    a.requiredRateHz = rateHz;
    a.create = &fakeCreate;
    a.process = &fakeProcess;
    a.poll_text = &fakePoll;
    a.destroy = &fakeDestroy;
    return a;
}

LoadedPlugin makePlugin(const char* name, const CascadeDecoderApi* api) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.version = "1.0.0";
    p.decoder = api;
    return p;
}

void resetFake() { g_fake = FakeState{}; }

}  // namespace

int main() {
    // --- Rate matching --------------------------------------------------
    {
        resetFake();
        const CascadeDecoderApi any = makeApi(0u);  // "any rate"
        std::vector<LoadedPlugin> ps{makePlugin("Any", &any)};
        PluginRunner r;
        r.rebuild(ps, 48000.0);
        CHECK(r.activeCount() == 1u);
        CHECK(g_fake.created == 1);
        // A rate-0 decoder is told the rate it will actually receive, not 0 -
        // otherwise it cannot size a filter.
        CHECK(g_fake.lastRate == 48000u);
    }
    {
        resetFake();
        const CascadeDecoderApi exact = makeApi(48000u);
        std::vector<LoadedPlugin> ps{makePlugin("Exact", &exact)};
        PluginRunner r;
        r.rebuild(ps, 48000.0);
        CHECK(r.activeCount() == 1u);
        CHECK(g_fake.lastRate == 48000u);
    }
    {
        // The case that must NOT silently do nothing: a fixed-rate decoder the
        // pipeline cannot currently feed. It is not created, and the reason is
        // reported in words a user can act on.
        resetFake();
        const CascadeDecoderApi wrong = makeApi(8000u);
        std::vector<LoadedPlugin> ps{makePlugin("Fixed8k", &wrong)};
        PluginRunner r;
        r.rebuild(ps, 48000.0);
        CHECK(r.activeCount() == 0u);
        CHECK(g_fake.created == 0);
        const std::vector<DecoderStatus> st = r.status();
        CHECK(st.size() == 1u);
        CHECK(st[0].reason == DecoderIdleReason::RateMismatch);
        CHECK(st[0].wantRateHz == 8000.0);
        CHECK(st[0].detail.find("8000") != std::string::npos);
        CHECK(st[0].detail.find("48000") != std::string::npos);
    }

    // --- Samples actually reach the decoder ------------------------------
    {
        resetFake();
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("Any", &any)};
        PluginRunner r;
        r.rebuild(ps, 48000.0);
        std::vector<float> block(512, 0.25f);
        r.processAudio(block.data(), block.size());
        r.processAudio(block.data(), block.size());
        CHECK(g_fake.samples == 1024u);
        // Degenerate inputs are no-ops, not crashes.
        r.processAudio(nullptr, 10);
        r.processAudio(block.data(), 0);
        CHECK(g_fake.samples == 1024u);
    }

    // --- Line assembly ----------------------------------------------------
    {
        resetFake();
        g_fake.queued = "alpha\nbeta\n";
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("Tag", &any)};
        PluginRunner r;
        r.rebuild(ps, 48000.0);
        std::vector<float> block(64, 0.0f);
        r.processAudio(block.data(), block.size());
        const std::vector<DecodedLine> out = r.drainText();
        CHECK(out.size() == 2u);
        CHECK(out[0].text == "alpha");
        CHECK(out[1].text == "beta");
        // Tagged, so two decoders running together are distinguishable.
        CHECK(out[0].plugin == "Tag");
        // Draining twice does not repeat.
        CHECK(r.drainText().empty());
    }
    {
        // A line SPLIT ACROSS POLLS must arrive once and whole. The ABI
        // explicitly permits a partial line at the end of a poll, so this is
        // ordinary behaviour rather than an edge case, and emitting the
        // fragment would show the user a line that then changed.
        resetFake();
        g_fake.queued = "hello world\n";
        g_fake.chunk = 3;  // 3 bytes per poll
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("Split", &any)};
        PluginRunner r;
        r.rebuild(ps, 48000.0);
        std::vector<float> block(64, 0.0f);
        r.processAudio(block.data(), block.size());
        const std::vector<DecodedLine> out = r.drainText();
        CHECK(out.size() == 1u);
        CHECK(out[0].text == "hello world");
    }
    {
        // Text with no trailing newline stays buffered rather than being
        // emitted early...
        resetFake();
        g_fake.queued = "partial";
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("Partial", &any)};
        PluginRunner r;
        r.rebuild(ps, 48000.0);
        std::vector<float> block(64, 0.0f);
        r.processAudio(block.data(), block.size());
        CHECK(r.drainText().empty());
        // ...and completes when the newline arrives.
        g_fake.queued = " line\n";
        r.processAudio(block.data(), block.size());
        const std::vector<DecodedLine> out = r.drainText();
        CHECK(out.size() == 1u);
        CHECK(out[0].text == "partial line");
    }

    // --- Failure paths ----------------------------------------------------
    {
        resetFake();
        g_fake.failCreate = true;
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("Broken", &any)};
        PluginRunner r;
        r.rebuild(ps, 48000.0);
        CHECK(r.activeCount() == 0u);
        const std::vector<DecoderStatus> st = r.status();
        CHECK(st.size() == 1u);
        CHECK(st[0].reason == DecoderIdleReason::CreateFailed);
    }
    {
        // A decoder that declares no audio table is listed with a reason, not
        // dropped: "my plugin is installed and nothing happens" is the
        // question this answers.
        resetFake();
        LoadedPlugin iqOnly;
        iqOnly.loaded = true;
        iqOnly.name = "IqThing";
        iqOnly.decoder = nullptr;
        std::vector<LoadedPlugin> ps{iqOnly};
        PluginRunner r;
        r.rebuild(ps, 48000.0);
        CHECK(r.activeCount() == 0u);
        const std::vector<DecoderStatus> st = r.status();
        CHECK(st.size() == 1u);
        CHECK(st[0].reason == DecoderIdleReason::NoAudioTable);
    }

    // --- Lifetime: every instance is destroyed exactly once ---------------
    {
        // The rule that matters most. A handle outliving its module is a crash
        // inside someone else's DLL, so rebuild() must destroy the old set
        // before creating a new one, and clear() must destroy the rest.
        resetFake();
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("A", &any), makePlugin("B", &any)};
        {
            PluginRunner r;
            r.rebuild(ps, 48000.0);
            CHECK(g_fake.created == 2);
            CHECK(g_fake.destroyed == 0);
            r.rebuild(ps, 48000.0);  // a rescan
            CHECK(g_fake.destroyed == 2);
            CHECK(g_fake.created == 4);
            r.clear();
            CHECK(g_fake.destroyed == 4);
            CHECK(r.activeCount() == 0u);
            r.clear();  // idempotent
            CHECK(g_fake.destroyed == 4);
        }
        // ...and the destructor does not double-destroy what clear() took.
        CHECK(g_fake.created == g_fake.destroyed);
    }
    {
        // Destruction via the destructor alone, with instances still live.
        resetFake();
        const CascadeDecoderApi any = makeApi(0u);
        std::vector<LoadedPlugin> ps{makePlugin("A", &any)};
        {
            PluginRunner r;
            r.rebuild(ps, 48000.0);
            CHECK(g_fake.created == 1);
        }
        CHECK(g_fake.destroyed == 1);
    }

    // --- Feeding with nothing loaded is safe ------------------------------
    {
        resetFake();
        PluginRunner r;
        std::vector<float> block(64, 0.0f);
        r.processAudio(block.data(), block.size());
        CHECK(r.drainText().empty());
        CHECK(r.activeCount() == 0u);
    }

    return testSummary("test_plugin_runner");
}
