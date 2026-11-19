// Tests for core/radio_events.hpp: what a driver says about a radio after it opened (a refused setting, a radio that went away, a stalled stream, a vendor
// fault). Everything runs against a stand-in server and a temporary queue folder; no radio, no network, no window. Every message is also handed to the Worker's own
// validator (node, where it is found).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/health_events.hpp"
#include "core/radio_events.hpp"
#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "test_check.hpp"

using namespace cascade::core::radioreport;
using nlohmann::json;
namespace fs = std::filesystem;

#ifndef CASCADE_SOURCE_ROOT
#error "CASCADE_SOURCE_ROOT is defined by tests/CMakeLists.txt"
#endif
#ifndef CASCADE_NODE_PATH
#define CASCADE_NODE_PATH ""
#endif

namespace {

constexpr std::int64_t kT0 = 1794830400;
const char* const kInstallId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";

struct Server {
    std::mutex mu;
    std::vector<std::string> bodies;
    Transport transport() {
        return [this](const std::string&, const std::string& body) {
            std::lock_guard<std::mutex> lk(mu);
            bodies.push_back(body);
            return PostResult{true, 204, 0, ""};
        };
    }
    std::vector<json> all() {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<json> out;
        for (const std::string& b : bodies) { out.push_back(json::parse(b)); }
        return out;
    }
    void clear() { std::lock_guard<std::mutex> lk(mu); bodies.clear(); }
};

struct Rig {
    fs::path dir;
    std::atomic<std::int64_t> clock{kT0};
    Server server;
    explicit Rig(const char* tag) {
        dir = fs::temp_directory_path() / (std::string("rre-") + tag + "-" + randomHex(8));
        fs::create_directories(dir);
    }
    ~Rig() { global().shutdown(); std::error_code ec; fs::remove_all(dir, ec); }
    void start(bool usage = true, bool live = true) {
        Reporter::Options o;
        o.dir = (dir / "radio-reports").string();
        o.endpoint = "http://127.0.0.1:9/";
        o.nowEpoch = [this]() { return clock.load(); };
        o.transport = server.transport();
        o.liveSend = live;
        if (usage) { o.identity.installId = kInstallId; }
        o.identity.version = "0.99.109";
        o.identity.os = "Windows 10.0.22631";
        o.identity.arch = "x64";
        o.identity.launches = 3;
        global().configure(o);
    }
    std::vector<json> flush() {
        server.clear();
        global().runBatchNow();
        return server.all();
    }
};

std::vector<json> ofKind(const std::vector<json>& all, const char* kind) {
    std::vector<json> out;
    for (const json& m : all) { if (m.value("kind", std::string()) == kind) { out.push_back(m); } }
    return out;
}

// cmd.exe (std::system on Windows) strips ONE outer pair of quotes from a line that starts with a quote; sh reads that pair as a single word and finds no
// such program. So the outer pair is Windows's alone.
static std::string systemLine(const std::string& inner) {
#if defined(_WIN32)
    return "\"" + inner + "\"";
#else
    return inner;
#endif
}

int validateWithWorker(const std::vector<json>& msgs) {
    // FOXSDR_DUMP_APP_MESSAGES=<file>: every message handed to the Worker's validator is also appended to that file, one JSON message a line. The website's
    // repository keeps the result as testdata/app-messages.jsonl and holds its own validators to it (must-fix lane E: what the application sends is what the site takes).
    if (const char* dump = std::getenv("FOXSDR_DUMP_APP_MESSAGES"); dump != nullptr && *dump != '\0') {
        std::ofstream df(dump, std::ios::binary | std::ios::app);
        for (const json& m : msgs) { df << m.dump(-1, ' ', false, json::error_handler_t::replace) << "\n"; }
    }
    const std::string node = CASCADE_NODE_PATH;
    if (node.empty() || msgs.empty()) { return -1; }
    const fs::path d = fs::temp_directory_path() / ("rre-validate-" + randomHex(8));
    fs::create_directories(d);
    const fs::path in = d / "in.jsonl";
    const fs::path out = d / "out.txt";
    {
        std::ofstream f(in, std::ios::binary);
        for (const json& m : msgs) { f << m.dump(-1, ' ', false, json::error_handler_t::replace) << "\n"; }
    }
    const std::string script = std::string(CASCADE_SOURCE_ROOT) + "/tests/radio_reports_validate.mjs";
    const std::string cmd = systemLine("\"" + node + "\" \"" + script + "\" \"" + in.string() + "\" > \"" + out.string() + "\"");
    const int rc = std::system(cmd.c_str());
    std::ifstream r(out);
    std::string line;
    int refused = 0;
    std::size_t n = 0;
    while (std::getline(r, line)) {
        ++n;
        if (line.rfind("OK ", 0) != 0) { ++refused; std::printf("WORKER VERDICT: %s\n", line.c_str()); }
    }
    std::error_code ec;
    fs::remove_all(d, ec);
    if (rc != 0 || n != msgs.size()) { std::printf("validator run failed: rc=%d lines=%zu messages=%zu\n", rc, n, msgs.size()); return 1000; }
    return refused;
}

const RadioIdentity kHf = identify("airspyhf", "serial=HF0001A2");
const RadioIdentity kRtl = identify("rtlsdr", "serial=00000001");
int g_source1 = 0;
int g_source2 = 0;
int g_source3 = 0;

}  // namespace

int main() {
    std::printf("test_radio_events\n");
    std::vector<json> everything;

    // ---- the word lists ---------------------------------------------------------------------------------------------------------------------------
    CHECK(std::string(controlWord(Control::Frequency)) == "frequency");
    CHECK(std::string(controlWord(Control::AutoGain)) == "gain");
    CHECK(std::string(controlWord(Control::BiasTee)) == "bias");
    CHECK(std::string(controlRefusedWord(Control::Frequency)) == "frequency");   // must-fix lane E, issue 9: the contract now has `frequency` and `other` (was: empty)
    CHECK(std::string(controlRefusedWord(Control::Other)) == "other");
    CHECK(std::string(controlRefusedWord(Control::Antenna)) == "antenna");
    // must-fix lane E, issues 15 and 16: the report's class says a USB transfer and an RTL-SDR tuner fault apart; the health token's class does not change
    namespace hl = cascade::core::health;
    CHECK(std::string(hl::radioReportClassWord("rtlsdr", "the radio stopped answering while it was being opened: a bulk read failed (Windows error 31)")) == "transfer");
    CHECK(std::string(hl::radioReportClassWord("rtlsdr", "WinUsb_ResetPipe failed (Windows error 31)")) == "transfer");
    CHECK(std::string(hl::radioReportClassWord("rtlsdr", "the tuner would not initialise: its PLL did not lock")) == "tuner");
    CHECK(std::string(hl::radioReportClassWord("rtlsdr", "the demodulator's I2C repeater would not open")) == "tuner");
    CHECK(std::string(hl::radioReportClassWord("airspyhf", "the radio stopped answering while restoring the crystal trim: a control transfer failed (Windows error 31)")) == "control");
    CHECK(std::string(hl::radioReportClassWord("rtlsdr", "the radio is in use by another program")) == "busy");   // every other class is the health class
    CHECK(std::string(hl::radioReportClassWord("rtlsdr", "nothing at that address")) == hl::radioReasonWord(hl::classifyRadioOpen("rtlsdr", "nothing at that address")));
    CHECK(hl::classifyRadioOpen("rtlsdr", "a bulk read failed (Windows error 31)") == hl::RadioReason::Control);   // the usage record's token is still `control`
    CHECK(hl::classifyRadioOpen("rtlsdr", "the tuner would not initialise") == hl::RadioReason::Control);
    CHECK(closeCauseFor(false, "the radio was unplugged") == "unplugged");
    CHECK(closeCauseFor(true, "Device removed while reading samples") == "unplugged");
    CHECK(closeCauseFor(true, "a control transfer failed (Windows error 31)") == "fault");
    CHECK(closeCauseFor(false, "a control transfer failed") .empty());
    CHECK(closeCauseFor(true, "the driver threw an exception") == "error");

    // ---- a refused setting ------------------------------------------------------------------------------------------------------------------------
    {
        Rig t("refuse");
        t.start();
        registerSource(&g_source1, kHf);
        noteControlRefused(&g_source1, "airspyhf", Control::Gain, "the radio would not take 40 dB at 145.500 MHz");
        noteControlRefused(&g_source1, "airspyhf", Control::Frequency, "the radio would not tune there");
        noteControlRefused(&g_source1, "airspyhf", Control::Ppm, "a value outside the radio's range", /*userRange=*/true);
        {
            ControlWhyScope why("auto");
            noteControlRefused(&g_source1, "airspyhf", Control::Antenna, "no such antenna");
        }
        const std::vector<json> msgs = ofKind(t.flush(), "radio_settings");
        CHECK(msgs.size() == 4);
        if (msgs.size() == 4) {
            for (const json& m : msgs) {
                CHECK(m["radioKey"] == kHf.key);
                CHECK(m["body"]["serial"] == "HF0001A2");
                CHECK(m["body"]["driver"] == "airspyhf");
            }
            CHECK(msgs[0]["body"]["why"] == "user");                          // no scope on this thread: a person's command
            CHECK(msgs[0]["body"]["refused"] == "gain");
            const std::string reason0 = msgs[0]["body"]["reason"];
            CHECK(reason0.rfind("refused: gain", 0) == 0);
            CHECK(reason0.find("145") == std::string::npos);                  // a frequency in the driver's sentence never leaves
            CHECK(msgs[1]["body"]["refused"] == "frequency");                  // the frequency has a contract word now (issue 9; it was absent)
            CHECK(msgs[1]["body"]["refusedClass"] == "radio" && msgs[0]["body"]["refusedClass"] == "radio");   // issue 10: the radio refused it
            CHECK(msgs[2]["body"]["refusedClass"] == "range");                    // ...and a value typed out of range is told apart by a field, not only a prefix
            CHECK(std::string(msgs[1]["body"]["reason"]).rfind("refused: frequency", 0) == 0);
            CHECK(msgs[2]["body"]["refused"] == "ppm");
            CHECK(std::string(msgs[2]["body"]["reason"]).rfind("out of range: ppm", 0) == 0);   // told apart from a fault of the radio
            CHECK(msgs[3]["body"]["why"] == "auto");
        }
        for (const json& m : msgs) { everything.push_back(m); }
        forgetRadio(kHf.key);
    }

    // ---- A3: a rate the radio took, changed live, is a radio_settings event with the landed rate and no refusal ------------------------------------------
    {
        Rig t("rateapplied");
        t.start();
        registerSource(&g_source1, kHf);
        noteRateApplied(&g_source1, "airspyhf", 384000.0);
        {
            ControlWhyScope why("auto");
            noteRateApplied(&g_source1, "airspyhf", 768000.0);
        }
        noteRateApplied(&g_source1, "airspyhf", 0.0);   // nothing to say
        const std::vector<json> msgs = ofKind(t.flush(), "radio_settings");
        CHECK(msgs.size() == 2);
        if (msgs.size() == 2) {
            CHECK(msgs[0]["body"]["why"] == "user" && msgs[0]["body"]["sampleRateSps"] == 384000);
            CHECK(msgs[1]["body"]["why"] == "auto" && msgs[1]["body"]["sampleRateSps"] == 768000);
            for (const json& m : msgs) { CHECK(!m["body"].contains("refused") && m["radioKey"] == kHf.key); }
        }
        for (const json& m : msgs) { everything.push_back(m); }
        forgetRadio(kHf.key);
    }

    // ---- a radio that went away -------------------------------------------------------------------------------------------------------------------
    {
        Rig t("dead");
        t.start();
        registerSource(&g_source1, kRtl);
        noteDeviceFault(&g_source1, "rtlsdr", "reading samples", "the device was unplugged (serial 00000001)");
        CHECK(takeCloseReported(kRtl.key));      // the engine will not say the same ending twice
        CHECK(!takeCloseReported(kRtl.key));     // and only once
        const std::vector<json> closes = ofKind(t.flush(), "radio_close");
        CHECK(closes.size() == 1);
        if (closes.size() == 1) {
            CHECK(closes[0]["body"]["cause"] == "unplugged");
            CHECK(closes[0]["body"].contains("openSec"));
            const std::string reason = closes[0]["body"]["reason"];
            CHECK(reason.find("00000001") == std::string::npos);   // the serial is its own field, never in the text
            CHECK(closes[0]["body"]["serial"] == "00000001");
        }
        for (const json& m : closes) { everything.push_back(m); }
        // a driver that gives up with no word of the radio being gone: a fault; an exception: an error
        t.start();
        noteDeviceFault(&g_source1, "rtlsdr", "restoring the crystal trim", "a control transfer failed (Windows error 31)");
        // a dead device fails every later transfer too and the driver says so every time: the first cause is the ending, said ONCE for this open
        noteDeviceFault(&g_source1, "rtlsdr", "reading samples", "the device was unplugged");
        CHECK(takeCloseReported(kRtl.key));          // the engine's close of the same ending
        noteDeviceFault(&g_source1, "rtlsdr", "starting the stream", "the driver threw an exception");   // a new ending (the radio was opened again)
        const std::vector<json> more = ofKind(t.flush(), "radio_close");
        CHECK(more.size() == 2);
        if (more.size() == 2) {
            CHECK(more[0]["body"]["cause"] == "fault");
            CHECK(more[1]["body"]["cause"] == "error");
        }
        for (const json& m : more) { everything.push_back(m); }
        forgetRadio(kRtl.key);
    }

    // ---- stream trouble ---------------------------------------------------------------------------------------------------------------------------
    {
        Rig t("stream");
        t.start();
        registerSource(&g_source2, kHf);
        StreamTrouble tr;
        tr.windowSec = 60.2;
        tr.samples = 60ull * 768000ull;
        tr.errors = 3;
        tr.overflows = 5;
        tr.longestGapMs = 1800;
        noteStreamTrouble(&g_source2, "airspyhf", tr);
        noteStreamTrouble(&g_source2, "airspyhf", tr);   // a second window right after: trouble is not coarsened into the routine minute
        const std::vector<json> msgs = ofKind(t.flush(), "radio_stream");
        CHECK(msgs.size() == 2);
        if (!msgs.empty()) {
            CHECK(msgs[0]["body"]["usbErrors"] == 3);
            CHECK(msgs[0]["body"]["stalls"] == 1);
            CHECK(msgs[0]["body"]["drops"] == 5);
            CHECK(msgs[0]["body"]["windowSec"] == 60);
            CHECK(msgs[0]["body"]["samplesPerSec"] == 768000);
        }
        for (const json& m : msgs) { everything.push_back(m); }
        forgetRadio(kHf.key);
    }

    // ---- a vendor fault, a service fault ----------------------------------------------------------------------------------------------------------
    {
        Rig t("vendor");
        t.start();
        registerSource(&g_source1, kRtl);
        noteVendorCallFault(&g_source1, "rtlsdr", "vendorcall", "the vendor library threw while setting the gain", /*absorbed=*/true);
        noteVendorCallFault(&g_source1, "rtlsdr", "pllretry", "the tuner would not lock", /*absorbed=*/false);
        noteServiceFault("rtlsdr", "servicerestart", "the service restarted", /*absorbed=*/true);
        const std::vector<json> msgs = ofKind(t.flush(), "radio_recovery");
        CHECK(msgs.size() == 3);
        if (msgs.size() == 3) {
            CHECK(msgs[0]["body"]["phase"] == "ok");
            CHECK(msgs[0]["body"]["what"] == "vendorcall");
            CHECK(msgs[0]["body"]["reasonClass"] == "vendor");
            CHECK(msgs[1]["body"]["phase"] == "gave-up");
            CHECK(msgs[2]["body"]["what"] == "servicerestart");
            CHECK(msgs[2]["radioKey"] == kRtl.key);   // the one open radio of that driver
        }
        for (const json& m : msgs) { everything.push_back(m); }
        forgetRadio(kRtl.key);
        // no radio of that driver open: no phantom radio is made up, and the state is a session_fault with NO radio key (issue 12; it used to be dropped)
        noteServiceFault("rtlsdr", "servicestate", "the service is not running", false);
        const std::vector<json> flushed = t.flush();
        CHECK(ofKind(flushed, "radio_recovery").empty());   // no phantom radio is made up for the service
        const std::vector<json> sf = ofKind(flushed, "session_fault");
        CHECK(sf.size() == 1);
        if (sf.size() == 1) {
            CHECK(sf[0]["radioKey"] == "");
            CHECK(sf[0]["body"]["service"] == "rtlsdr" && sf[0]["body"]["phase"] == "gave-up");
            CHECK(sf[0]["body"]["reasonClass"] == "driver");
            CHECK(!sf[0]["body"].contains("serial") && !sf[0]["body"].contains("model"));
        }
        for (const json& m : sf) { everything.push_back(m); }
        // a repeat inside ten seconds is folded, not sent twice
        noteServiceFault("rtlsdr", "servicestate", "the service is not running", false);
        CHECK(ofKind(t.flush(), "session_fault").empty());
    }
    // ---- the RTL-SDR's PLL that did not lock: degraded, said as the tuner's, and not urgent -----------------------------------------------------------------
    {
        Rig t("pll");
        t.start();
        registerSource(&g_source1, kRtl);
        noteVendorCallFault(&g_source1, "rtlsdr", "pllunlocked", "the tuner's PLL did not lock at the requested frequency", /*absorbed=*/false);
        const std::vector<json> msgs = ofKind(t.flush(), "radio_recovery");
        CHECK(msgs.size() == 1);
        if (msgs.size() == 1) {
            CHECK(msgs[0]["body"]["phase"] == "degraded");   // issue 11: it was sent as gave-up although the radio keeps streaming
            CHECK(msgs[0]["body"]["what"] == "pllunlocked");
            CHECK(msgs[0]["body"]["reasonClass"] == "tuner");
        }
        for (const json& m : msgs) { everything.push_back(m); }
        forgetRadio(kRtl.key);
    }

    // ---- a radio that is still OPENING: what its driver says is held and said about the right radio once the engine announces it ------------------------
    {
        Rig t("unknown");
        t.start();
        noteControlRefused(&g_source2, "airspyhf", Control::Ppm, "the receiver refused to take back its stored crystal trim", false, "open");
        CHECK(ofKind(t.flush(), "radio_settings").empty());   // not yet: no radio is known to be this one
        registerSource(&g_source2, kHf);                       // the open succeeded: the engine says which radio it was
        const std::vector<json> msgs = ofKind(t.flush(), "radio_settings");
        CHECK(msgs.size() == 1);
        if (msgs.size() == 1) {
            CHECK(msgs[0]["radioKey"] == kHf.key);
            CHECK(msgs[0]["body"]["why"] == "open");
            CHECK(msgs[0]["body"]["refused"] == "ppm");
        }
        for (const json& m : msgs) { everything.push_back(m); }
        // an open that never succeeds says nothing more: the failure of an open is the radio_fail
        noteControlRefused(&g_source3, "airspyhf", Control::Rate, "x");
        // a caller with no source object at all (a service) means the one open radio of that driver, and nobody when none is open
        const std::uint64_t before = unattributedCount();
        noteVendorCallFault(nullptr, "airspyhf", "vendorcall", "a vendor call threw", true);
        CHECK(ofKind(t.flush(), "radio_recovery").size() == 1);
        forgetRadio(kHf.key);
        noteVendorCallFault(nullptr, "airspyhf", "vendorcall", "a vendor call threw", true);
        CHECK(ofKind(t.flush(), "radio_recovery").empty());
        CHECK(unattributedCount() == before + 1);
    }

    // ---- folding: a slider dragged against a refusal is one event and a count, never silence and never a flood ----------------------------------------------
    {
        Rig t("fold");
        t.start();
        registerSource(&g_source1, kRtl);
        for (int i = 0; i < 6; ++i) { noteControlRefused(&g_source1, "rtlsdr", Control::Gain, "the tuner refused that gain"); }
        const std::vector<json> msgs = ofKind(t.flush(), "radio_settings");
        CHECK(msgs.size() == 1);   // the first went; five were folded
        // after the window the next one that goes out says how many were folded
        std::this_thread::sleep_for(std::chrono::milliseconds(5100));
        noteControlRefused(&g_source1, "rtlsdr", Control::Gain, "the tuner refused that gain");
        const std::vector<json> later = ofKind(t.flush(), "radio_settings");
        CHECK(later.size() == 1);
        if (later.size() == 1) { CHECK(later[0]["body"].value("dropped", 0) == 5); }   // the five folded ones are said, as the radio's own dropped count
        for (const json& m : later) { everything.push_back(m); }
        CHECK(lostCount() == 0);
        forgetRadio(kRtl.key);
    }

    // ---- Usage reporting off: a journal event has no anonymous form, so nothing is queued or sent ----------------------------------------------------
    {
        Rig t("usageoff");
        t.start(/*usage=*/false);
        registerSource(&g_source1, kRtl);
        noteControlRefused(&g_source1, "rtlsdr", Control::Gain, "nope");
        noteDeviceFault(&g_source1, "rtlsdr", "reading samples", "the device was unplugged");
        CHECK(t.flush().empty());
        forgetRadio(kRtl.key);
    }

    // ---- the build switch: a build with the live send off queues the events and posts none --------------------------------------------------------------
    {
        Rig t("switch");
        t.start(/*usage=*/true, /*live=*/false);
        registerSource(&g_source1, kRtl);
        noteControlRefused(&g_source1, "rtlsdr", Control::BiasTee, "the radio would not switch its bias tee");
        CHECK(t.flush().empty());
        CHECK(readQueueFiles((t.dir / "radio-reports").string()).size() >= 1);   // queued
        forgetRadio(kRtl.key);
    }

    // ---- NEVER SENT: every helper handed hostile text (a frequency in four forms, a path, the radio's serial, an account, a host, an address) -------------------
    {
        Rig t("never");
        t.start();
        const RadioIdentity hf = identify("airspyhf", "serial=SN9900AA", "Airspy HF+ (serial SN9900AA)");
        registerSource(&g_source1, hf);
        const char* const hostile = "failed at 433.920 MHz / 433920000 Hz / 433,920 kHz for serial SN9900AA in C:\\Users\\alice\\SecretFolder\\zz_secret_capture.iq via 192.168.7.77 (mynas.lan)";   // (a computer name or a callsign in free text cannot be told from a word: no driver is ever handed one)
        noteControlRefused(&g_source1, "airspyhf", Control::Frequency, hostile, true);
        noteControlRefused(&g_source1, "airspyhf", Control::Gain, hostile);
        noteDeviceFault(&g_source1, "airspyhf", hostile, hostile);
        noteVendorCallFault(&g_source1, "airspyhf", "vendorcall", hostile, false);
        noteStreamTrouble(&g_source1, "airspyhf", StreamTrouble{60.0, 1000, 2, 0, 0, 1500});
        const std::vector<json> msgs = t.flush();
        CHECK(msgs.size() >= 5);
        std::string all;
        for (const json& m : msgs) { all += m.dump(); }
        std::string lowered = all;
        for (char& c : lowered) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
        for (const char* needle : {"433.920", "433920000", "433,920", "secretfolder", "zz_secret_capture", "alice", "192.168.7.77", "mynas.lan", "g4zzz", "c:\\users"}) {
            const bool leaked = lowered.find(needle) != std::string::npos;
            if (leaked) { std::printf("LEAK: %s\n", needle); }
            CHECK(!leaked);
        }
        // the serial is allowed in its own field and the radio key, nowhere else
        for (const json& m : msgs) {
            std::string text = m.dump();
            for (const char* pat : {"\"serial\":\"SN9900AA\"", ":SN9900AA\""}) {
                std::size_t at;
                while ((at = text.find(pat)) != std::string::npos) { text.erase(at, std::string(pat).size()); }
            }
            CHECK(text.find("SN9900AA") == std::string::npos);
        }
        for (const json& m : msgs) { everything.push_back(m); }
        forgetRadio(hf.key);
    }

    // ---- the Worker's own validator over every message made above ---------------------------------------------------------------------------------------
    {
        const int refused = validateWithWorker(everything);
        if (refused >= 0) {
            CHECK(refused == 0);
        } else {
            std::printf("SKIP: node not found, the Worker's validator was not run\n");
        }
    }

    return testSummary("test_radio_events");
}
