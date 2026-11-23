// THE AUDIO WATCHDOG'S RETRY SCHEDULE AND ITS NOTE (2026-11-23, review of the ringdrop branch, section 5; core/audio_probe_backoff.hpp).
//
//   1. the rule by itself: 1, 2, 4, then a CEILING OF 5 s (not 30), and back to 1 s the moment the stream is alive or a re-open succeeded;
//   2. the engine uses it: the probe waits on the rule's interval, a dead stream steps it, a re-open that SUCCEEDED (applyAudioOpenResult)
//      puts it back to a second (it used to be reset only inside the gated probe, so the next death waited up to 30 s), and a stream found
//      alive puts it back too;
//   3. the watchdog's no-wait re-open does not write "audio device busy - still opening" on every attempt (the row used to alternate between
//      that and "could not be reopened"); a user's own pick still does.
//
// Sections 2 and 3 read the engine's source (code, not comments): a real dying device cannot be staged in a unit test, and the loop that
// would stage one is what these two lines of code are. UNVERIFIED against a real device that dies.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "core/audio_probe_backoff.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

std::string readLf(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

// Drops // comments (a call that is only commented out must not satisfy a scan for the call).
std::string stripLineComments(const std::string& s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') { ++i; }
        } else {
            out += s[i++];
        }
    }
    return out;
}

// The text of one member function: from its signature to the first "\n}\n" after it.
std::string bodyOf(const std::string& src, const char* signature) {
    const std::size_t at = src.find(signature);
    if (at == std::string::npos) { return {}; }
    const std::size_t end = src.find("\n}\n", at);
    return end == std::string::npos ? std::string() : src.substr(at, end - at);
}

}  // namespace

int main() {
    using cascade::core::AudioProbeBackoff;

    // --- 1. THE RULE ---------------------------------------------------------------------------------------------------------------------
    {
        AudioProbeBackoff b;
        CHECK(b.intervalS == 1.0);
        b.onProbeDead();
        CHECK(b.intervalS == 2.0);
        b.onProbeDead();
        CHECK(b.intervalS == 4.0);
        b.onProbeDead();
        CHECK(b.intervalS == 5.0);   // the ceiling is five seconds
        b.onProbeDead();
        CHECK(b.intervalS == 5.0);   // and it holds
        for (int i = 0; i < 20; ++i) { b.onProbeDead(); }
        CHECK(b.intervalS <= 5.0);
        b.onAlive();
        CHECK(b.intervalS == 1.0);   // a success (or a live stream) puts it back at once
        b.onProbeDead();
        CHECK(b.intervalS == 2.0);   // and the schedule starts over
    }

    // --- 2 and 3. THE ENGINE USES IT -------------------------------------------------------------------------------------------------------
    const std::string eng = stripLineComments(readLf(fs::path(CASCADE_SOURCE_DIR) / "src" / "engine" / "engine.cpp"));
    {
        const std::string poll = bodyOf(eng, "void Engine::pollAudioHealth()");
        CHECK(!poll.empty());
        CHECK(poll.find("now - rx().lastAudioProbeSec_ < rx().audioProbeBackoff_.intervalS") != std::string::npos);
        CHECK(poll.find("rx().audioProbeBackoff_.onProbeDead()") != std::string::npos);
        CHECK(poll.find("rx().audioProbeBackoff_.onAlive()") != std::string::npos);   // a stream found alive

        // THE RESET ON SUCCESS: in applyAudioOpenResult, after the failure return, so only a re-open that worked resets it.
        const std::string apply = bodyOf(eng, "void Engine::applyAudioOpenResult()");
        CHECK(!apply.empty());
        const std::size_t failed = apply.find("if (!r.ok)");
        const std::size_t reset = apply.find("rx().audioProbeBackoff_.onAlive()");
        CHECK(failed != std::string::npos && reset != std::string::npos && reset > failed);
        CHECK(apply.find("audioProbeBackoff_.onProbeDead()") == std::string::npos);

        // THE NOTE: the busy line is written for a wait (a user's pick) or a non-recovery request, never for the watchdog's no-wait one.
        const std::string req = bodyOf(eng, "bool Engine::requestAudioOpen(");
        CHECK(!req.empty());
        CHECK(req.find("if (wait || !recovery)") != std::string::npos);
        const std::size_t guard = req.find("if (wait || !recovery)");
        const std::size_t note = req.find("rx().audioHealthNote_ = tr(kAudioBusyNote)");
        CHECK(guard != std::string::npos && note != std::string::npos && guard < note);
    }

    return testSummary("test_audio_probe_backoff");
}
