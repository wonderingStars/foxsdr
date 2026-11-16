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

int validateWithWorker(const std::vector<json>& msgs) {
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
    const std::string cmd = "\"\"" + node + "\" \"" + script + "\" \"" + in.string() + "\" > \"" + out.string() + "\"\"";
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
    CHECK(std::string(controlRefusedWord(Control::Frequency)).empty());     // the contract has no `refused` word for it
    CHECK(std::string(controlRefusedWord(Control::Other)).empty());
    CHECK(std::string(controlRefusedWord(Control::Antenna)) == "antenna");
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
            CHECK(!msgs[1]["body"].contains("refused"));                      // the frequency: no contract word, said in the reason only
            CHECK(std::string(msgs[1]["body"]["reason"]).rfind("refused: frequency", 0) == 0);
            CHECK(msgs[2]["body"]["refused"] == "ppm");
            CHECK(std::string(msgs[2]["body"]["reason"]).rfind("out of range: ppm", 0) == 0);   // told apart from a fault of the radio
            CHECK(msgs[3]["body"]["why"] == "auto");
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
        // no radio of that driver open: a service state is information, not a radio's fault (and no phantom radio is made up)
        noteServiceFault("rtlsdr", "servicestate", "the service is not running", false);
        CHECK(ofKind(t.flush(), "radio_recovery").size() == 0);
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
