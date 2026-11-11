// Test for the SDK template's heartbeat decoder. It calls the exported entry point and the decoder table exactly as the
// host does, with generated audio that carries noise, a frequency offset and a sample-rate error.
//
// SPDX-License-Identifier: MIT
#include "plugin_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" const CascadePluginDesc* cascade_plugin_query(uint32_t hostAbiVersion);

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL  %s\n", what);
    }
}

// Tone + noise, so the audio is not silence. The decoder counts samples, so none of this changes its answer: that is
// what the checks below hold it to.
class Audio {
public:
    Audio() : rng_(20260111u), noise_(0.0f, 0.05f) {}
    void fill(std::vector<float>& v, size_t n) {
        v.resize(n);
        for (size_t i = 0; i < n; ++i) {
            v[i] = static_cast<float>(0.3 * std::sin(phase_)) + noise_(rng_);
            phase_ += 2.0 * 3.14159265358979323846 * 1000.3 / 8004.0;   // 1000.3 Hz in a stream that is really 8004 Hz
        }
    }
private:
    std::mt19937 rng_;
    std::normal_distribution<float> noise_;
    double phase_ = 0.0;
};

const CascadeDecoderApi* decoderTable(const CascadePluginDesc* d) {
    for (uint32_t i = 0; d != nullptr && i < d->capabilityCount; ++i) {
        if (d->capabilityTables[i].capability == CASCADE_CAP_DECODER) {
            return static_cast<const CascadeDecoderApi*>(d->capabilityTables[i].table);
        }
    }
    return nullptr;
}

// Everything the decoder has to say right now, collected through a buffer of `cap` bytes (cap > 0).
std::string drain(const CascadeDecoderApi* api, void* h, size_t cap) {
    std::string out;
    std::vector<char> buf(cap);
    for (int guard = 0; guard < 100000; ++guard) {
        const int32_t n = api->poll_text(h, buf.data(), buf.size());
        if (n <= 0) {
            break;
        }
        check(static_cast<size_t>(n) <= cap, "poll_text never writes more than it was given room for");
        out.append(buf.data(), static_cast<size_t>(n));
    }
    return out;
}

size_t count(const std::string& s, const std::string& what) {
    size_t n = 0;
    for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + what.size())) { ++n; }
    return n;
}

}  // namespace

int main() {
    // --- the entry point and the descriptor ---------------------------------------------------------------------
    check(cascade_plugin_query(CASCADE_PLUGIN_ABI_VERSION + 1u) == nullptr, "a host that speaks another ABI gets NULL");
    check(cascade_plugin_query(0u) == nullptr, "ABI 0 gets NULL");
    const CascadePluginDesc* d = cascade_plugin_query(CASCADE_PLUGIN_ABI_VERSION);
    check(d != nullptr, "the host's own ABI gets a descriptor");
    if (d == nullptr) {
        std::printf("%d checks, %d failed\n", g_checks, g_failed);
        return 1;
    }
    check(d->structSize == sizeof(CascadePluginDesc), "descriptor structSize is sizeof as compiled");
    check(d->abiVersion == CASCADE_PLUGIN_ABI_VERSION, "descriptor abiVersion is this header's");
    check(d->name != nullptr && d->name[0] != '\0', "name is set");
    check(d->version != nullptr && std::strcmp(d->version, "1.0.0") == 0, "version is 1.0.0 (CMakeLists.txt PROJECT_VERSION)");
    check(d->licence != nullptr && std::strcmp(d->licence, "MIT") == 0, "licence is declared");
    check(d->capabilities == CASCADE_CAP_DECODER, "only the decoder capability is declared");
    check(d->capabilityCount == 1u && d->capabilityTables != nullptr, "one capability entry");
    check(d->capabilityTables[0].capability == CASCADE_CAP_DECODER, "the entry is the decoder");
    check(d->capabilityTables[0].tableSize == sizeof(CascadeDecoderApi), "tableSize is sizeof the table as compiled");
    const CascadeDecoderApi* api = decoderTable(d);
    check(api != nullptr, "decoder table found");
    if (api == nullptr) {
        std::printf("%d checks, %d failed\n", g_checks, g_failed);
        return 1;
    }
    check(api->structSize == sizeof(CascadeDecoderApi), "decoder structSize");
    check(api->create != nullptr && api->process != nullptr && api->poll_text != nullptr && api->destroy != nullptr,
          "every function pointer is non-NULL");
    check(api->requiredRateHz == 0u ||
              (api->requiredRateHz >= CASCADE_AUDIO_RATE_MIN_HZ && api->requiredRateHz <= CASCADE_AUDIO_RATE_MAX_HZ),
          "requiredRateHz is 0 or inside the ABI's range");
    check(api->requiredRateHz == 8000u, "requires 8000 Hz audio");

    // --- behaviour, with generated audio -------------------------------------------------------------------------
    Audio audio;
    std::vector<float> block;

    {   // 3.5 s fed in awkward block sizes -> exactly three lines, in order, none for the half second
        void* h = api->create(8000u);
        check(h != nullptr, "create succeeds");
        const size_t sizes[] = {1, 7, 160, 4099, 333, 8000, 1};
        size_t fed = 0, k = 0;
        std::string got;
        const size_t total = 28000;   // 3.5 s
        while (fed < total) {
            size_t n = sizes[k++ % (sizeof(sizes) / sizeof(sizes[0]))];
            if (n > total - fed) { n = total - fed; }
            audio.fill(block, n);
            check(block.size() == n, "the block holds the count passed");
            api->process(h, block.data(), n);
            fed += n;
            got += drain(api, h, 64);
        }
        check(got == "heartbeat 1\nheartbeat 2\nheartbeat 3\n", "3.5 s of audio gives heartbeat 1, 2, 3 and nothing for the half second");
        audio.fill(block, 4000);
        api->process(h, block.data(), 4000);
        check(drain(api, h, 64) == "heartbeat 4\n", "the next half second completes heartbeat 4");
        api->destroy(h);
    }
    {   // a sample-rate error does not matter: one line per `rate` samples (8004 samples/s really arriving)
        void* h = api->create(8000u);
        std::string got;
        for (int s = 0; s < 10; ++s) {
            audio.fill(block, 8004);
            api->process(h, block.data(), block.size());
            got += drain(api, h, 64);
        }
        check(count(got, "heartbeat ") == 10, "10 s of a 8004 Hz stream gives 10 lines (80040 samples / 8000)");
        check(got.find("heartbeat 10\n") != std::string::npos, "the tenth line is heartbeat 10");
        api->destroy(h);
    }
    {   // a tiny poll buffer loses nothing
        void* h = api->create(8000u);
        audio.fill(block, 16000);
        api->process(h, block.data(), 16000);
        std::string got = drain(api, h, 5);   // one line is waiting; the second second was dropped (not collected in time)
        check(got == "heartbeat 1\n", "polled 5 bytes at a time, the line comes out whole");
        audio.fill(block, 8000);
        api->process(h, block.data(), 8000);
        check(drain(api, h, 1) == "heartbeat 3\n", "polled one byte at a time, the line comes out whole");
        api->destroy(h);
    }
    {   // back-pressure: nobody polls for 5 s, only the first line is held, and the gap shows in the number
        void* h = api->create(8000u);
        for (int s = 0; s < 5; ++s) {
            audio.fill(block, 8000);
            api->process(h, block.data(), 8000);
        }
        check(drain(api, h, 64) == "heartbeat 1\n", "5 s unpolled: one line held, the other four dropped");
        audio.fill(block, 8000);
        api->process(h, block.data(), 8000);
        check(drain(api, h, 64) == "heartbeat 6\n", "after draining, the next line is heartbeat 6 (the gap is visible)");
        api->destroy(h);
    }
    {   // a long run: an hour of audio, polled as it goes
        void* h = api->create(8000u);
        size_t lines = 0;
        const size_t iterations = 28800000u / 4099u;
        for (size_t i = 0; i < iterations; ++i) {
            audio.fill(block, 4099);
            api->process(h, block.data(), 4099);
            lines += count(drain(api, h, 64), "heartbeat ");
        }
        check(lines == (iterations * 4099u) / 8000u, "an hour of audio, polled as it goes, gives one line per 8000 samples");
        api->destroy(h);
    }
    {   // other rates: create() is given the rate and the line follows it
        void* h = api->create(48000u);
        audio.fill(block, 47999);
        api->process(h, block.data(), 47999);
        check(drain(api, h, 64).empty(), "at 48000 Hz, 47999 samples are not yet a second");
        audio.fill(block, 1);
        api->process(h, block.data(), 1);
        check(drain(api, h, 64) == "heartbeat 1\n", "the 48000th sample completes heartbeat 1");
        api->destroy(h);
        void* z = api->create(0u);   // 0 = the host's rate unknown: falls back to the plugin's own 8000
        audio.fill(block, 8000);
        api->process(z, block.data(), 8000);
        check(drain(api, z, 64) == "heartbeat 1\n", "create(0) falls back to 8000 Hz");
        api->destroy(z);
    }
    {   // two instances do not share state
        void* a = api->create(8000u);
        void* b = api->create(8000u);
        audio.fill(block, 8000);
        api->process(a, block.data(), 8000);
        check(drain(api, b, 64).empty(), "an instance that was fed nothing says nothing");
        check(drain(api, a, 64) == "heartbeat 1\n", "the one that was fed says it");
        api->destroy(a);
        api->destroy(b);
    }
    {   // the edges: nothing to do, bad arguments
        void* h = api->create(8000u);
        api->process(h, nullptr, 0);
        api->process(h, block.data(), 0);
        check(drain(api, h, 64).empty(), "zero samples change nothing");
        check(api->poll_text(h, nullptr, 64) == 0, "poll_text with a NULL buffer answers 0");
        char c = 0;
        check(api->poll_text(h, &c, 0) == 0, "poll_text with zero capacity answers 0");
        api->process(nullptr, block.data(), 10);
        check(api->poll_text(nullptr, &c, 1) == 0, "poll_text on a NULL handle answers 0");
        api->process(h, nullptr, 5);   // NULL samples with a count: ignored, not read
        check(drain(api, h, 64).empty(), "NULL samples with a count are ignored");
        api->destroy(h);
        api->destroy(nullptr);
        check(true, "destroy(NULL) does not crash");
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
