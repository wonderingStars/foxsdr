// test_hamops_scan.cpp - the lane-wide source scan over every file the hamops lane added under src/: nothing to stdout or stderr (the host captures process
// stderr into its log and uploaded reports), no setting write, no thread (the thread inventory under src/ would otherwise need rows), no wide bind, no
// resolver, no engine/pipeline/plugin-ABI include, and no transmit entry point anywhere. Comments and string literals are blanked first, so a sentence in a
// comment is not a use.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cctype>
#include <string>
#include <vector>

#include "hamops_util.hpp"
#include "test_check.hpp"

namespace {

// true when `name(` appears as a call whose name is not the tail of a longer identifier (snprintf( is not printf()
bool hasCall(const std::string& src, const std::string& name) {
    std::size_t pos = 0;
    const std::string pat = name + "(";
    while ((pos = src.find(pat, pos)) != std::string::npos) {
        const bool tail = pos > 0 && (std::isalnum(static_cast<unsigned char>(src[pos - 1])) || src[pos - 1] == '_');
        if (!tail) { return true; }
        pos += pat.size();
    }
    return false;
}

const std::vector<std::string> kFiles = {
    "src/core/qso_log.hpp",          "src/core/cabrillo.hpp",        "src/core/contest_macros.hpp",    "src/core/adif_qso.hpp",       "src/core/contest_score.hpp",
    "src/core/band_map.hpp",         "src/core/omnirig_client.hpp",  "src/core/radioinfo_udp.hpp",     "src/core/antenna_genius.hpp", "src/core/xplane_udp.hpp",
    "src/core/transcript_store.hpp", "src/net/loopback_sock.hpp",    "src/net/adif_broadcast.hpp",     "src/net/xplane_listener.hpp", "src/net/radioinfo_listener.hpp",
    "src/net/antenna_genius_tcp.hpp", "src/input_hid/hid_descriptor.hpp", "src/input_hid/hid_map.hpp"};

void scan() {
    for (const std::string& f : kFiles) {
        const std::string raw = hamops_test::srcFile(f);
        CHECK(!raw.empty());
        const std::string src = hamops_test::stripCommentsAndStrings(raw);
        // host sinks
        for (const char* bad : {"stderr", "stdout", "std::cerr", "std::cout", "std::clog", "perror", "OutputDebugString", "settings_set", "setSetting"}) {
            if (src.find(bad) != std::string::npos) { std::printf("%s uses %s\n", f.c_str(), bad); }
            CHECK(src.find(bad) == std::string::npos);
        }
        CHECK(!hasCall(src, "printf") && !hasCall(src, "puts") && !hasCall(src, "fprintf") && !hasCall(src, "putchar"));
        // threads
        for (const char* bad : {"std::thread", "std::async", "std::jthread", "_beginthread", "CreateThread", "pthread_create"}) { CHECK(src.find(bad) == std::string::npos); }
        // network safety
        for (const char* bad : {"INADDR_ANY", "in6addr_any", "getaddrinfo", "gethostbyname", "WSAConnectByName", "URLDownload", "InternetOpen", "WinHttp", "curl_"}) { CHECK(src.find(bad) == std::string::npos); }
        // no entry into the engine, the pipeline, the plugin ABI, the recorder, or the knob lane's headers
        std::size_t pos = 0;
        while ((pos = raw.find("#include", pos)) != std::string::npos) {
            const std::size_t eol = raw.find(static_cast<char>(10), pos);
            const std::string line = raw.substr(pos, eol - pos);
            for (const char* bad : {"engine/", "pipeline", "receiver", "demod", "plugin_abi", "fox_engine", "recorder", "cat_protocol", "input/", "input_event", "windows.h", "Windows.h"}) {
                CHECK(line.find(bad) == std::string::npos);
            }
            pos = eol == std::string::npos ? raw.size() : eol;
        }
        // no transmit entry point: no function or type named for keying or sending a transmission
        for (const char* bad : {"keyTransmitter", "setPtt", "putPtt", "pttOn", "pttOff", "startTransmit", "transmitNow", "sendTransmit", "keyDown", "SetPTT"}) { CHECK(src.find(bad) == std::string::npos); }
    }
    // new network pieces are off by default: the config structs say so in code
    for (const char* f : {"src/net/adif_broadcast.hpp", "src/net/xplane_listener.hpp", "src/net/radioinfo_listener.hpp", "src/net/antenna_genius_tcp.hpp"}) {
        CHECK(hamops_test::srcFile(f).find("bool enabled = false;") != std::string::npos);
    }
}

}  // namespace

int main() {
    scan();
    return testSummary("test_hamops_scan");
}
