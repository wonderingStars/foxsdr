/*
 * PORTAUDIO'S ALSA BACKEND MUST NOT DIVIDE BY A PERIOD COUNT OF ZERO.
 *
 * THE REPORT (0.99.81, Linux x64 Kali, kernel 7.1.5; one report, 2026-10-16):
 * SIGFPE with si_code FPE_INTDIV (integer divide by zero), fully symbolised:
 *
 *   PaAlsaStreamComponent_DetermineFramesPerBuffer (+216)
 *   <- PaAlsaStream_Configure <- OpenStream <- Pa_OpenStream
 *   <- cascade::sink::AudioOut::openLocked <- AudioOut::open
 *   <- core::patch::makeDeviceDest <- AppWindow::patchPublishSets
 *
 * WHAT IT IS. AudioOut opens with paFramesPerBufferUnspecified (0), so in that
 * function the user-buffer alignment (whose divisions are already guarded by
 * PaAlsa_AlignForward's `align ?` test) is skipped, and the ONE unguarded
 * division left on the live path is
 *
 *     framesPerHostBuffer = (bufferSize / numPeriods);
 *
 * numPeriods is clamped just above it to PA_MIN(maxPeriods, ...), so it is zero
 * exactly when the device reports a maximum of zero periods. Upstream's only
 * defence is `assert( maxPeriods > 1 )`, and a release build is -DNDEBUG. That
 * a device reports zero is INFERRED from the shape of the fault (a divisor that
 * is zero by construction there, FPE_INTDIV, +216 bytes in, i.e. just after the
 * two get_periods_* calls); the report does not carry the device's parameters.
 * third_party/portaudio/FOXSDR-PATCHES.md (alsa-zero-periods) has the account.
 *
 * WHAT THIS TEST CAN AND CANNOT PROVE. ALSA exists only on Linux, so the
 * function cannot be run here. What is testable everywhere is the TEXT of the
 * guard: that `PA_UNLESS( maxPeriods > 0, ...)` sits between the call that
 * reads maxPeriods and the first division by numPeriods, and that nothing
 * divides by numPeriods before it. Like test_glfw_wayland_terminate this holds
 * the patch in place against a re-vendor of plain upstream. The behaviour itself
 * (zero periods -> the function returns paInternalError instead of faulting) was
 * shown by compiling the extracted function against stubbed ALSA calls, once,
 * and is recorded in the report that added this; only a real ALSA device that
 * reports zero periods verifies it end to end.
 *
 * THE BREAK-IT CHECK: deleting the PA_UNLESS line (plain upstream) turns the
 * ordering block red.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The body of one function with every C comment removed (block and line), so
// that the patch's own fence - which names these very expressions in words -
// can never satisfy or break a check. Empty when the function is not found,
// which every caller turns into a failure.
std::string liveBody(const std::string& text, const std::string& signature) {
    const std::size_t sig = text.find(signature);
    if (sig == std::string::npos) { return {}; }
    const std::size_t open = text.find('{', sig);
    if (open == std::string::npos) { return {}; }
    std::string out;
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        if (text.compare(i, 2, "/*") == 0) {
            const std::size_t end = text.find("*/", i + 2);
            if (end == std::string::npos) { return {}; }
            i = end + 1;
            continue;
        }
        if (text.compare(i, 2, "//") == 0) {
            const std::size_t eol = text.find('\n', i);
            if (eol == std::string::npos) { return {}; }
            i = eol - 1;
            continue;
        }
        if (text[i] == '\r') { continue; }
        if (text[i] == '{') { ++depth; }
        if (text[i] == '}' && --depth == 0) { return out + "}"; }
        out += text[i];
    }
    return {};
}

std::vector<std::size_t> allOf(const std::string& hay, const std::string& needle) {
    std::vector<std::size_t> at;
    for (std::size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) {
        at.push_back(p);
    }
    return at;
}

}  // namespace

int main() {
    const fs::path file = fs::path(CASCADE_SOURCE_DIR) / "third_party" / "portaudio" / "src" /
                          "hostapi" / "alsa" / "pa_linux_alsa.c";
    const std::string text = readFile(file);
    CHECK(!text.empty());

    const std::string fn = liveBody(
        text, "static PaError PaAlsaStreamComponent_DetermineFramesPerBuffer(");
    CHECK(!fn.empty());

    // Where maxPeriods is read, the guard, the clamp, and the division.
    const std::size_t readAt = fn.find("alsa_snd_pcm_hw_params_get_periods_max(");
    const std::size_t guardAt = fn.find("PA_UNLESS( maxPeriods > 0,");
    const std::size_t clampAt = fn.find("numPeriods = PA_MIN(maxPeriods,");
    CHECK(readAt != std::string::npos);
    CHECK(guardAt != std::string::npos);
    CHECK(clampAt != std::string::npos);

    // THE PROPERTY: read -> guard -> clamp -> every division by numPeriods.
    CHECK(readAt != std::string::npos && guardAt != std::string::npos && readAt < guardAt);
    CHECK(guardAt != std::string::npos && clampAt != std::string::npos && guardAt < clampAt);

    // The guard refuses the stream with an ordinary PortAudio error that
    // AudioOut's refusal path words ("... refused the stream: ...").
    CHECK(fn.find("PA_UNLESS( maxPeriods > 0, paInternalError )") != std::string::npos);

    // Every division by numPeriods in the function - the live one and the
    // obsolete-calc one the build does not compile - comes after the guard, and
    // there is at least one (a function that stopped dividing would make this
    // test vacuous, and should be looked at, not waved through).
    const std::vector<std::size_t> divs = allOf(fn, "/ numPeriods");
    CHECK(!divs.empty());
    for (std::size_t d : divs) { CHECK(guardAt != std::string::npos && d > guardAt); }

    // The patch is recorded where the next person to touch PortAudio will look.
    CHECK(text.find("FOXSDR PATCH (alsa-zero-periods)") != std::string::npos);
    const std::string notes =
        readFile(fs::path(CASCADE_SOURCE_DIR) / "third_party" / "portaudio" / "FOXSDR-PATCHES.md");
    CHECK(notes.find("alsa-zero-periods") != std::string::npos);

    return testSummary("test_portaudio_alsa_periods");
}
