// Tests for source/soapy_log_bridge.{hpp,cpp} - the SoapySDR logger brought
// into our own log.
//
// Everything here exercises the PURE half: the level map, the prefix, the
// scrub and the per-second limit, driven by a clock the test owns. The
// registration with SoapySDR is one line and is not called here, so this
// passes identically on a machine with a radio, without one, and on CI - and
// it says nothing about whether a real driver's lines arrive; that half is
// only proven on a bench with a driver that logs.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_log_bridge.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "test_check.hpp"

using cascade::source::classifySoapyLogLevel;
using cascade::source::SoapyLogAction;
using cascade::source::SoapyLogBridge;
using cascade::source::SoapyLogEmit;
using cascade::source::soapyDebugWanted;

namespace {

// SoapySDRLogLevel values, spelled out so this file does not need the SoapySDR
// header - the point is that the bridge's pure half needs nothing of it.
constexpr int kFatal = 1, kCritical = 2, kError = 3, kWarning = 4, kNotice = 5, kInfo = 6,
              kDebug = 7, kTrace = 8, kSsi = 9;

std::string joined(const std::vector<SoapyLogEmit>& v) {
    std::string out;
    for (const SoapyLogEmit& e : v) { out += std::string(e.level) + "|" + e.text + "\n"; }
    return out;
}

}  // namespace

int main() {
    // --- The level map -------------------------------------------------------
    {
        CHECK(classifySoapyLogLevel(kFatal, false) == SoapyLogAction::Warn);
        CHECK(classifySoapyLogLevel(kCritical, false) == SoapyLogAction::Warn);
        CHECK(classifySoapyLogLevel(kError, false) == SoapyLogAction::Warn);
        CHECK(classifySoapyLogLevel(kWarning, false) == SoapyLogAction::Warn);
        CHECK(classifySoapyLogLevel(kNotice, false) == SoapyLogAction::Info);
        CHECK(classifySoapyLogLevel(kInfo, false) == SoapyLogAction::Info);
        CHECK(classifySoapyLogLevel(kSsi, false) == SoapyLogAction::Info);
        // Debug and trace are dropped by default...
        CHECK(classifySoapyLogLevel(kDebug, false) == SoapyLogAction::Drop);
        CHECK(classifySoapyLogLevel(kTrace, false) == SoapyLogAction::Drop);
        // ...and kept, as info, when FOXSDR_SOAPY_DEBUG asks for them.
        CHECK(classifySoapyLogLevel(kDebug, true) == SoapyLogAction::Info);
        CHECK(classifySoapyLogLevel(kTrace, true) == SoapyLogAction::Info);
        // Debug on never promotes a warning to info or demotes it.
        CHECK(classifySoapyLogLevel(kWarning, true) == SoapyLogAction::Warn);
        // An unknown level: above INFO's number is treated as chatter, below
        // it as something worth a warning.
        CHECK(classifySoapyLogLevel(42, false) == SoapyLogAction::Drop);
        CHECK(classifySoapyLogLevel(0, false) == SoapyLogAction::Warn);
    }

    // --- The environment switch ---------------------------------------------
    {
        CHECK(!soapyDebugWanted(nullptr));
        CHECK(!soapyDebugWanted(""));
        CHECK(!soapyDebugWanted("0"));
        CHECK(!soapyDebugWanted("false"));
        CHECK(soapyDebugWanted("1"));
        CHECK(soapyDebugWanted("true"));
        CHECK(soapyDebugWanted("TRUE"));
        CHECK(soapyDebugWanted("yes"));
        CHECK(soapyDebugWanted("on"));
    }

    // --- One line in, one line out, prefixed and at the mapped level ---------
    {
        SoapyLogBridge b(false);
        std::vector<SoapyLogEmit> out = b.feed(kWarning, "rtlsdr_read_async: dev_lost\n", 0);
        CHECK(out.size() == 1);
        CHECK(!out.empty() && std::string(out[0].level) == "warn");
        CHECK(!out.empty() && out[0].text == "soapy: rtlsdr_read_async: dev_lost");

        out = b.feed(kInfo, "Using device 0: Generic RTL2832U OEM", 1);
        CHECK(out.size() == 1);
        CHECK(!out.empty() && std::string(out[0].level) == "info");
        CHECK(!out.empty() && out[0].text == "soapy: Using device 0: Generic RTL2832U OEM");

        // Dropped means nothing at all, not an empty line.
        out = b.feed(kDebug, "setGain(0, TUNER, 20)", 2);
        CHECK(out.empty());
        // An empty message is not a line either.
        out = b.feed(kInfo, "", 3);
        CHECK(out.empty());
        out = b.feed(kInfo, nullptr, 4);
        CHECK(out.empty());
    }

    // --- The scrub is applied on the way through ----------------------------
    {
        SoapyLogBridge b(true);
        std::vector<SoapyLogEmit> out =
            b.feed(kDebug, "Setting center freq: 101100000", 0);
        CHECK(out.size() == 1);
        CHECK(!out.empty() && out[0].text == "soapy: Setting center freq: #########");
        out = b.feed(kInfo, "Manufacturer: Realtek, Product Name: RTL2838UHIDIR, Serial: 00000001",
                     1);
        CHECK(out.size() == 1);
        CHECK(!out.empty() && out[0].text.find("00000001") == std::string::npos);
        CHECK(!out.empty() && out[0].text.find("Serial: <stripped>") != std::string::npos);
        CHECK(!out.empty() && out[0].text.find("RTL2838UHIDIR") != std::string::npos);
    }

    // --- The limit: 20 a second, then a count, once, when the next second
    //     begins ---------------------------------------------------------------
    {
        SoapyLogBridge b(false);
        int emitted = 0;
        for (int i = 0; i < 25; ++i) {
            const std::vector<SoapyLogEmit> out =
                b.feed(kWarning, ("read failed " + std::to_string(i)).c_str(), 100 + i);
            emitted += static_cast<int>(out.size());
            // Within the second nothing but the admitted lines come out: no
            // report yet, because the count is not final.
            for (const SoapyLogEmit& e : out) {
                CHECK(e.text.find("suppressed") == std::string::npos);
            }
        }
        CHECK(emitted == 20);

        // The first line of the next second carries the report AHEAD of
        // itself: "5 more lines suppressed", then the line.
        const std::vector<SoapyLogEmit> next = b.feed(kWarning, "read failed 25", 1000);
        CHECK(next.size() == 2);
        if (next.size() == 2) {
            CHECK(std::string(next[0].level) == "info");
            CHECK(next[0].text == "soapy: 5 more lines suppressed");
            CHECK(next[1].text == "soapy: read failed 25");
        }
        // ...and only once: the second line of that second has nothing to
        // report.
        const std::vector<SoapyLogEmit> after = b.feed(kWarning, "read failed 26", 1001);
        CHECK(after.size() == 1);
        CHECK(!after.empty() && after[0].text == "soapy: read failed 26");
        std::printf("%s", joined(next).c_str());
    }

    // --- Dropped debug lines do not spend the budget -------------------------
    {
        SoapyLogBridge b(false);
        for (int i = 0; i < 100; ++i) { (void)b.feed(kTrace, "tick", 0); }
        const std::vector<SoapyLogEmit> out = b.feed(kError, "something real", 1);
        CHECK(out.size() == 1);
        CHECK(!out.empty() && out[0].text == "soapy: something real");
    }

    // --- A quiet second in between still reports the count when the next line
    //     eventually arrives ------------------------------------------------------
    {
        SoapyLogBridge b(false, 2);
        (void)b.feed(kInfo, "a", 0);
        (void)b.feed(kInfo, "b", 0);
        CHECK(b.feed(kInfo, "c", 0).empty());
        CHECK(b.feed(kInfo, "d", 0).empty());
        const std::vector<SoapyLogEmit> later = b.feed(kInfo, "e", 30000);
        CHECK(later.size() == 2);
        CHECK(later.size() == 2 && later[0].text == "soapy: 2 more lines suppressed");
    }

    // Not called here, so the record says so: installSoapyLogBridge() is the
    // one function that touches SoapySDR, and it has not been.
    CHECK(!cascade::source::soapyLogBridgeInstalled());

    return testSummary("test_soapy_log_bridge");
}
