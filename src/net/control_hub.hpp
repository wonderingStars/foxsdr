// net/control_hub.hpp - THE ONE PLACE other programs' requests meet the engine (screen lane S4b, G3-31 the WebSocket / SSE API and G3-32 the MCP server).
//
// WHAT A CLIENT MAY DO, AND NOTHING ELSE. The hub offers exactly five things, the same five the MCP server's allow-list has:
//   READ   status        the tuned frequency, centre, mode, bandwidth, sample rate, signal level and the source's name
//   READ   bookmarks     the saved bookmarks (name, frequency, mode, group)
//   READ   decodes       the most recent decoded lines of the plugins that are not private
//   TUNE   tune          the selected radio's frequency, 1 kHz to 7.2 GHz
//   TUNE   set_mode      the selected radio's demodulator mode, one of the receiver's eight names
// There is NO path from here to transmit or key a radio, to write or read a file, to change a setting, to start or stop anything, to run a program, or to open a network connection: the hub
// has no such operation and its interface has no way to add one by accident. A request is validated here (a finite number inside the range, a mode name from the table) and QUEUED; the engine's
// own pass applies it through its ordinary tuning commands, so every check the window's own tuning meets applies as well. At most ten tune or mode requests a second are accepted (a token
// bucket); the queue holds only the latest of each kind, so a flood cannot grow anything.
//
// THREADS. publish*() and take*() are the engine's pass; every other member is called from client threads. One mutex, held for copies only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_CONTROL_HUB_HPP
#define CASCADE_NET_CONTROL_HUB_HPP

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cascade::net {

struct HubStatus {
    bool running = false;
    double tunedHz = 0.0, centreHz = 0.0, sampleRateHz = 0.0, bandwidthHz = 0.0, signalDb = -200.0;
    std::string mode, source;
};

struct HubBookmark {
    std::string name, mode, group;
    double freqHz = 0.0;
};

struct HubDecode {
    std::uint64_t seq = 0;
    double unixSeconds = 0.0, freqHz = 0.0;
    std::string decoder, text;
};

struct HubResult {
    bool ok = true;
    std::string error;   // a plain sentence when !ok
};

inline constexpr double kHubMinTuneHz = 1000.0;
inline constexpr double kHubMaxTuneHz = 7.2e9;
inline constexpr std::size_t kHubMaxDecodes = 200;       // kept and asked for at most
inline constexpr std::size_t kHubMaxBookmarks = 500;
inline constexpr std::size_t kHubTextBytes = 512;        // a decoded line is cut to this
inline constexpr std::size_t kHubNameBytes = 120;

class ControlHub {
public:
    using Clock = std::chrono::steady_clock;
    ControlHub() = default;

    // ---- the engine's pass ----
    void publishStatus(const HubStatus& s);
    void publishBookmarks(std::vector<HubBookmark> b);                // cut to kHubMaxBookmarks, every text cleaned and cut
    void addDecode(double unixSeconds, double freqHz, const std::string& decoder, const std::string& text);   // never called for a private plugin
    std::optional<double> takeTune();
    std::optional<std::string> takeMode();

    // ---- client threads ----
    HubStatus status() const;
    std::vector<HubBookmark> bookmarks() const;
    std::vector<HubDecode> recentDecodes(std::size_t maxCount) const;    // oldest first, at most kHubMaxDecodes
    std::vector<HubDecode> decodesSince(std::uint64_t& cursor, std::size_t maxCount) const;   // the lines newer than `cursor` (which is moved to the newest returned)
    std::uint64_t newestSeq() const;
    HubResult requestTune(double hz);
    HubResult requestMode(const std::string& modeName);

    // for the tests: a clock for the rate limiter
    void setClock(Clock::time_point (*now)()) { now_ = now; }

    struct Counts {
        std::uint64_t tunesAccepted = 0, modesAccepted = 0, refused = 0, rateLimited = 0;
    };
    Counts counts() const;

    // cleans a decoded text: valid UTF-8 only, control characters but newline and tab replaced, at most `maxBytes`
    static std::string cleanText(const std::string& in, std::size_t maxBytes);
    // the receiver's mode name in canonical case ("usb" -> "USB"), or "" when it is not one of the eight
    static std::string canonicalMode(const std::string& name);

private:
    bool takeToken();    // m_ held

    mutable std::mutex m_;
    HubStatus status_;
    std::vector<HubBookmark> bookmarks_;
    std::deque<HubDecode> decodes_;
    std::uint64_t nextSeq_ = 1;
    std::optional<double> tune_;
    std::optional<std::string> mode_;
    double tokens_ = 20.0;
    Clock::time_point last_{};
    bool started_ = false;
    Clock::time_point (*now_)() = nullptr;
    Counts counts_;
};

}  // namespace cascade::net

#endif  // CASCADE_NET_CONTROL_HUB_HPP
