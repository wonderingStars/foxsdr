// THE FAILURE COUNTS, DRIVEN THROUGH THE FAILURES THAT REALLY HAPPEN
// (core/health_events.hpp, 0.99.64).
//
// test_health_events.cpp holds the vocabulary, the ledger and the documents. This
// file holds each event to the place it is counted: it makes the real thing fail -
// the real sound output with a device that is not there, the real drivers opened
// against radios that are not plugged in, the SDRplay driver against the fake API,
// the update check and download and the plugin catalogue and installer against a
// transport the test stands in for (PluginRepo::setTransportForTest), the plugin
// host loading a file that is not a module and a module built for another ABI, a
// recording started in a folder that cannot exist, the Store check and install
// through the environment seams that already exist - and reads what the ledger
// counted. None of them calls the counter.
//
// WHAT THIS CANNOT SEE, stated so a green run is not read as more: a radio that is
// THERE and refuses to open is not driven - the one dongle on the machine this was
// written on is never opened (another program may be using it), so what is driven is
// each driver's own sentence for a serial that is not there, which every driver
// returns through the same lastError(). And no network is touched: the transport
// seam is the only way a download or a catalogue "arrives".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/health_events.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_repo.hpp"
#include "core/recorder.hpp"
#include "core/store_update.hpp"
#include "core/telemetry.hpp"
#include "core/updater.hpp"
#include "gui/record_start.hpp"
#include "sdrplay_fake_api.hpp"
#include "sink/audio_out.hpp"
#include "source/aor_source.hpp"
#include "source/airspy_source.hpp"
#include "source/airspyhf_source.hpp"
#include "source/hackrf_source.hpp"
#include "source/mirisdr_source.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/rx888_source.hpp"
#include "source/sdrplay_source.hpp"
#include "source/soapy_source.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace health = cascade::core::health;
using cascade::core::PluginCatalogEntry;
using cascade::core::PluginPlatform;
using cascade::core::PluginRepo;
using health::Counts;

namespace {

const char* g_abiFixture = nullptr;       // argv[1]: a plugin that declares another ABI
const char* g_declineFixture = nullptr;   // argv[2]: one whose query declines this host

unsigned long pidNow() {
#if defined(_WIN32)
    return ::GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

fs::path scratch(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::temp_directory_path();
    const fs::path d = base / (std::string("cascade-healthpaths-") + tag + "-" + std::to_string(pidNow()));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value);
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) { ::setenv(name, value, 1); } else { ::unsetenv(name); }
#endif
}

// A fresh, armed, in-memory ledger for one case: the process-wide one every call
// site counts into, as it is in the application.
std::shared_ptr<health::HealthLedger> fresh() {
    auto g = health::globalLedger();
    g->reset();
    g->arm("", cascade::core::newInstallId(), false);
    return g;
}

std::string show(const Counts& c) { return health::encode(c); }

#define EXPECT_COUNTS(ledger, ...)                                                       \
    do {                                                                                 \
        const Counts want_ = __VA_ARGS__;                                                \
        const Counts got_ = (ledger)->counts();                                          \
        if (got_ != want_) {                                                             \
            std::printf("FAIL %s:%d counted \"%s\", wanted \"%s\"\n", __FILE__, __LINE__, \
                        show(got_).c_str(), show(want_).c_str());                        \
        }                                                                                \
        CHECK(got_ == want_);                                                            \
    } while (0)

// ---------------------------------------------------------------------------
// Nothing counts on the hot paths.
// ---------------------------------------------------------------------------
std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void testNothingOnTheHotPathsCounts() {
    const fs::path root(CASCADE_SOURCE_DIR);
    // The DSP library, the pipeline and the sink's callback never mention it.
    std::size_t files = 0;
    for (const auto& e : fs::recursive_directory_iterator(root / "src" / "dsp")) {
        if (!e.is_regular_file()) { continue; }
        ++files;
        const std::string text = readFile(e.path());
        if (text.find("health_events") != std::string::npos || text.find("health::") != std::string::npos) {
            std::printf("FAIL %s counts failures\n", e.path().string().c_str());
        }
        CHECK(text.find("health_events") == std::string::npos);
        CHECK(text.find("health::") == std::string::npos);
    }
    CHECK(files > 10);
    const std::string pipeline = readFile(root / "src" / "core" / "pipeline.cpp");
    CHECK(pipeline.find("health::") == std::string::npos);
    CHECK(pipeline.find("health_events") == std::string::npos);
    // audio_out.cpp counts ONLY in open(), never in the callback: pullBlock's body has no call.
    const std::string audio = readFile(root / "src" / "sink" / "audio_out.cpp");
    const std::size_t from = audio.find("std::size_t AudioOut::pullBlock(");
    const std::size_t to = audio.find("std::size_t AudioOut::ringFrames()");
    CHECK(from != std::string::npos && to != std::string::npos && to > from);
    CHECK(audio.substr(from, to - from).find("health::") == std::string::npos);
    CHECK(audio.substr(from, to - from).find("health_events") == std::string::npos);
    // ...and the one note in it sits after the lock is released (open()), not in openLocked().
    const std::size_t note = audio.find("noteSoundFail");
    const std::size_t locked = audio.find("bool AudioOut::openLocked(");
    CHECK(note != std::string::npos && locked != std::string::npos && note < locked);
}

// ---------------------------------------------------------------------------
// SOUND: the real output, refused.
// ---------------------------------------------------------------------------
void testASoundOutputThatCannotBeOpenedIsCounted() {
    auto g = fresh();
    cascade::sink::AudioOut out;

    // No such device: PortAudio is never asked, the host API is not reached.
    CHECK(!out.open(99999, 48000.0, 1));
    EXPECT_COUNTS(g, (Counts{{"sound_fail.none.nodevice", 1}}));

    // The request itself unusable: a rate of zero.
    CHECK(!out.open(-1, 0.0, 1));
    EXPECT_COUNTS(g, (Counts{{"sound_fail.none.nodevice", 1}, {"sound_fail.none.format", 1}}));

    // A repeated refusal - the audio watchdog retries once a second - is one count a session.
    for (int i = 0; i < 5; ++i) { CHECK(!out.open(99999, 48000.0, 1)); }
    EXPECT_COUNTS(g, (Counts{{"sound_fail.none.nodevice", 1}, {"sound_fail.none.format", 1}}));

    // A REFUSAL FROM THE HOST API ITSELF, when this machine has an output to refuse
    // with: a rate of 1 Hz is turned away by the real PortAudio, and the count names
    // the host API (never the device) and a reason read off the error CODE.
    const std::vector<cascade::sink::AudioDevice> devices = out.listOutputDevices();
    if (devices.empty()) {
        std::printf("SKIP the host API's own refusal: no output device on this machine\n");
        ++g_checksSkipped;
    } else {
        g->reset();
        g->arm("", cascade::core::newInstallId(), false);
        const bool opened = out.open(-1, 1.0, 1);
        if (opened) {
            out.close();
            std::printf("SKIP the host API's own refusal: this host API took a 1 Hz stream\n");
            ++g_checksSkipped;
        } else {
            const Counts c = g->counts();
            CHECK(c.size() == 1);
            const std::string token = c.empty() ? std::string() : c.begin()->first;
            std::printf("health: the host API refused a 1 Hz stream as %s\n", token.c_str());
            CHECK(token.rfind("sound_fail.", 0) == 0);
            CHECK(token.rfind("sound_fail.none.", 0) != 0);   // a host API WAS reached
            // never a device name: it is all vocabulary
            CHECK(health::validToken(token));
        }
    }
    g->reset();
}

// ---------------------------------------------------------------------------
// RADIOS: the real drivers' sentences for a radio that is not there.
// ---------------------------------------------------------------------------
template <typename Source>
void expectNativeDriverRefusal(const char* kind) {
    Source src;
    const bool opened = src.open("serial=HEALTHTESTNOSUCHRADIO");
    CHECK(!opened);
    const std::string said = src.lastError();
    CHECK(!said.empty());
    const health::RadioReason why = health::classifyRadioOpen(kind, said);
    // A serial that is not there: the radio is not bound (the transport sees no device
    // of that kind) or not present (it sees others, but not this serial) - never "other".
    if (why != health::RadioReason::Bind && why != health::RadioReason::Absent) {
        std::printf("FAIL %s said \"%s\" -> %s\n", kind, said.c_str(), health::radioReasonWord(why));
    }
    CHECK(why == health::RadioReason::Bind || why == health::RadioReason::Absent);
    // And what is counted is vocabulary only, whatever the sentence quoted back.
    auto g = fresh();
    health::noteRadioFail(kind, said);
    CHECK(g->counts().size() == 1);
    const std::string token = g->counts().begin()->first;
    CHECK(token.rfind(std::string("radio_fail.") + kind + ".", 0) == 0);
    CHECK(token.find("HEALTHTEST") == std::string::npos);
    g->reset();
}

void testEveryNativeDriversRefusalIsClassified() {
    expectNativeDriverRefusal<cascade::source::RtlSdrSource>("rtlsdr");
    expectNativeDriverRefusal<cascade::source::HackRfSource>("hackrf");
    expectNativeDriverRefusal<cascade::source::AirspySource>("airspy");
    expectNativeDriverRefusal<cascade::source::AirspyHfSource>("airspyhf");
    expectNativeDriverRefusal<cascade::source::MiriSdrSource>("mirisdr");
    expectNativeDriverRefusal<cascade::source::Rx888Source>("rx888");
}

void testTheAorDriversRefusalIsClassified() {
    cascade::source::AorSource src;
    CHECK(!src.open("serial=HEALTHTESTNOSUCHRADIO"));
    const std::string said = src.lastError();
    CHECK(!said.empty());
    const health::RadioReason why = health::classifyRadioOpen("aor", said);
    if (why != health::RadioReason::Absent && why != health::RadioReason::Bind) {
        std::printf("FAIL aor said \"%s\" -> %s\n", said.c_str(), health::radioReasonWord(why));
    }
    CHECK(why == health::RadioReason::Absent || why == health::RadioReason::Bind);
}

void testASoapyDriverThatIsNotThereIsClassified() {
    cascade::source::SoapySource src;
    CHECK(!src.open("driver=healthtestnosuchdriver"));
    const std::string said = src.lastError();
    CHECK(!said.empty());
    const health::RadioReason why = health::classifyRadioOpen("soapy", said);
    std::printf("health: SoapySDR said \"%s\" -> %s\n", said.c_str(), health::radioReasonWord(why));
    // The vendor stack's own words: not found, or its own error. Never "other" for soapy.
    CHECK(why == health::RadioReason::Absent || why == health::RadioReason::Vendor ||
          why == health::RadioReason::Driver);
}

void testTheSdrPlayDriversThreeRefusals() {
    namespace abi = cascade::source::sdrplay_abi;
    // 1. NO API INSTALLED: the table resolved nothing.
    {
        abi::Api absent;
        cascade::source::SdrPlaySource src;
        src.setApiForTest(&absent);
        CHECK(!src.open(""));
        CHECK(health::classifyRadioOpen("sdrplay", src.lastError()) == health::RadioReason::Driver);
    }
    // 2. THE SERVICE DID NOT ANSWER: Open() fails.
    {
        fakesdrplay::FakeSdrPlayApi fake;
        fake.openResult = abi::Fail;
        cascade::source::SdrPlaySource src;
        src.setApiForTest(&fake.table);
        CHECK(!src.open(""));
        const std::string said = src.lastError();
        if (health::classifyRadioOpen("sdrplay", said) != health::RadioReason::Driver) {
            std::printf("FAIL sdrplay said \"%s\"\n", said.c_str());
        }
        CHECK(health::classifyRadioOpen("sdrplay", said) == health::RadioReason::Driver);
    }
    // 3. THE SERVICE IS UP AND NO RSP IS PLUGGED IN.
    {
        fakesdrplay::FakeSdrPlayApi fake;
        cascade::source::SdrPlaySource src;
        src.setApiForTest(&fake.table);
        CHECK(!src.open(""));
        const std::string said = src.lastError();
        if (health::classifyRadioOpen("sdrplay", said) != health::RadioReason::Absent) {
            std::printf("FAIL sdrplay said \"%s\"\n", said.c_str());
        }
        CHECK(health::classifyRadioOpen("sdrplay", said) == health::RadioReason::Absent);
    }
}

// ---------------------------------------------------------------------------
// UPDATES, through the transport seam.
// ---------------------------------------------------------------------------
struct Served {
    std::string body;
    bool fail = false;
    std::string error = "the server returned HTTP 500";
    std::atomic<int> hits{0};
};

void serve(const std::shared_ptr<Served>& s) {
    PluginRepo::setTransportForTest([s](const std::string&, std::uint64_t,
                                        const std::function<bool(const void*, std::size_t)>& sink,
                                        std::string& error) {
        ++s->hits;
        if (s->fail) {
            error = s->error;
            return false;
        }
        return sink(s->body.data(), s->body.size());
    });
}

std::string sha256Of(const std::string& bytes) {
    std::string hex, err;
    CHECK(PluginRepo::sha256Hex(bytes.data(), bytes.size(), hex, err));
    return hex;
}

void testUpdateChecksThatFailAreCounted() {
    auto g = fresh();
    auto s = std::make_shared<Served>();
    serve(s);
    cascade::core::UpdateInfo info;
    std::string err;

    // The service cannot be reached.
    s->fail = true;
    CHECK(!cascade::core::checkForUpdate("https://foxsdr.com/api/update", "0.99.64", "", info, err));
    EXPECT_COUNTS(g, (Counts{{"upd_check", 1}}));
    // It answers with something that is not a manifest.
    s->fail = false;
    s->body = "<html>not a manifest</html>";
    CHECK(!cascade::core::checkForUpdate("https://foxsdr.com/api/update", "0.99.64", "", info, err));
    EXPECT_COUNTS(g, (Counts{{"upd_check", 2}}));
    // A manifest this build refuses (a download from somewhere else) is a check that failed too.
    s->body = "{\"version\":\"9.9.9\",\"url\":\"https://elsewhere.example/x.exe\",\"sha256\":\"" +
              std::string(64, 'a') + "\"}";
    CHECK(!cascade::core::checkForUpdate("https://foxsdr.com/api/update", "0.99.64", "", info, err));
    EXPECT_COUNTS(g, (Counts{{"upd_check", 3}}));

    // A good manifest counts nothing - and neither does an old build being told nothing is newer.
    s->body = "{\"version\":\"9.9.9\",\"url\":\"https://foxsdr.com/download/foxsdr-setup-9.9.9.exe\","
              "\"sha256\":\"" + std::string(64, 'a') + "\"}";
    CHECK(cascade::core::checkForUpdate("https://foxsdr.com/api/update", "0.99.64", "", info, err));
    CHECK(info.newer);
    EXPECT_COUNTS(g, (Counts{{"upd_check", 3}}));
    PluginRepo::setTransportForTest(nullptr);
    g->reset();
}

void testUpdateDownloadsThatFailAreCounted() {
    auto g = fresh();
    auto s = std::make_shared<Served>();
    serve(s);
    const std::string version = "0.0.1-healthtest." + std::to_string(pidNow());
    const std::string payload = "pretend this is an installer";
    cascade::core::UpdateInfo info;
    info.newer = true;
    info.version = version;
    info.url = "https://foxsdr.com/download/foxsdr-setup-" + version + ".exe";
    info.sha256 = sha256Of(payload);
    std::string path, err;

    // THE BYTES ARRIVE AND ARE NOT THE PUBLISHED ONES: verification fails, and the
    // file is not left behind as an installer.
    s->body = payload + " (tampered)";
    CHECK(!cascade::core::downloadUpdate(info, path, err));
    CHECK(err.find("sha256 mismatch") == 0);
    CHECK(path.empty());
    EXPECT_COUNTS(g, (Counts{{"upd_verify", 1}}));

    // THE DOWNLOAD DOES NOT COMPLETE.
    s->fail = true;
    CHECK(!cascade::core::downloadUpdate(info, path, err));
    EXPECT_COUNTS(g, (Counts{{"upd_verify", 1}, {"upd_dl", 1}}));

    // A CANCEL IS NEITHER: a quit, or the user withdrawing.
    s->fail = false;
    s->body = payload;
    std::atomic<bool> cancel{true};
    CHECK(!cascade::core::downloadUpdate(info, path, err, nullptr, &cancel));
    CHECK(err == "cancelled");
    s->fail = true;
    s->error = "cancelled";
    cancel = false;
    CHECK(!cascade::core::downloadUpdate(info, path, err, nullptr, &cancel));
    // ...and a refusal before any transfer (a URL that is not foxsdr.com's) is not a download that failed.
    cascade::core::UpdateInfo hostile = info;
    hostile.url = "https://elsewhere.example/x.exe";
    CHECK(!cascade::core::downloadUpdate(hostile, path, err));
    EXPECT_COUNTS(g, (Counts{{"upd_verify", 1}, {"upd_dl", 1}}));

    // A download that matches counts nothing.
    s->fail = false;
    s->error.clear();
    s->body = payload;
    CHECK(cascade::core::downloadUpdate(info, path, err));
    CHECK(!path.empty());
    EXPECT_COUNTS(g, (Counts{{"upd_verify", 1}, {"upd_dl", 1}}));
    std::error_code ec;
    fs::remove(path, ec);
    PluginRepo::setTransportForTest(nullptr);
    g->reset();
}

void testTheStoresCheckAndInstallThroughTheEnvironmentSeams() {
    auto g = fresh();
    // THE CHECK THAT DOES NOT COMPLETE.
    setEnv("FOXSDR_FAKE_STORE_UPDATE", "error");
    CHECK(!cascade::core::checkStoreForUpdates(nullptr).ok);
    EXPECT_COUNTS(g, (Counts{{"upd_check", 1}}));
    // An answer, whatever it says, is not a failure.
    setEnv("FOXSDR_FAKE_STORE_UPDATE", "none");
    CHECK(cascade::core::checkStoreForUpdates(nullptr).ok);
    setEnv("FOXSDR_FAKE_STORE_UPDATE", "available");
    CHECK(cascade::core::checkStoreForUpdates(nullptr).ok);
    EXPECT_COUNTS(g, (Counts{{"upd_check", 1}}));

    // THE INSTALL REQUEST THAT FAILS - not one the user declined, and not one that succeeded.
    setEnv("FOXSDR_FAKE_STORE_INSTALL", "cancelled");
    CHECK(cascade::core::requestStoreUpdateInstall(nullptr).result ==
          cascade::core::StoreInstallResult::Cancelled);
    setEnv("FOXSDR_FAKE_STORE_INSTALL", "installed");
    CHECK(cascade::core::requestStoreUpdateInstall(nullptr).result ==
          cascade::core::StoreInstallResult::Installed);
    EXPECT_COUNTS(g, (Counts{{"upd_check", 1}}));
    setEnv("FOXSDR_FAKE_STORE_INSTALL", "failed");
    CHECK(cascade::core::requestStoreUpdateInstall(nullptr).result ==
          cascade::core::StoreInstallResult::Failed);
    EXPECT_COUNTS(g, (Counts{{"upd_check", 1}, {"upd_run", 1}}));
    setEnv("FOXSDR_FAKE_STORE_UPDATE", nullptr);
    setEnv("FOXSDR_FAKE_STORE_INSTALL", nullptr);
    g->reset();
}

// ---------------------------------------------------------------------------
// PLUGINS: the catalogue and the installer, through the same seam.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
const std::string kExt = ".dll";
#else
const std::string kExt = ".so";
#endif

PluginCatalogEntry entryFor(const std::string& stem, const std::string& sha) {
    PluginCatalogEntry e;
    e.id = "healthprobe";
    e.name = "Health Probe";
    e.version = "1.0.0";
    e.abiVersion = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
    e.compatible = true;
    PluginPlatform p;
    p.os = PluginRepo::hostOs();
    p.arch = PluginRepo::hostArch();
    p.file = stem + kExt;
    p.url = "https://example.invalid/" + stem + kExt;
    p.sha256 = sha;
    e.platforms.push_back(p);
    return e;
}

void testACatalogueThatCannotBeHadIsCounted() {
    auto g = fresh();
    auto s = std::make_shared<Served>();
    serve(s);
    PluginRepo repo;
    std::string err;

    s->fail = true;
    CHECK(!repo.fetchIndex("https://example.invalid/index.json", err));
    EXPECT_COUNTS(g, (Counts{{"plug_cat", 1}}));
    s->fail = false;
    s->body = "this is not a catalogue";
    CHECK(!repo.fetchIndex("https://example.invalid/index.json", err));
    EXPECT_COUNTS(g, (Counts{{"plug_cat", 2}}));
    CHECK(!repo.fetchIndex("http://example.invalid/index.json", err));   // refused before any socket
    EXPECT_COUNTS(g, (Counts{{"plug_cat", 3}}));

    // A cancel (a quit while it was fetching) is not a catalogue that failed.
    PluginRepo::setTransportForTest([&repo](const std::string&, std::uint64_t,
                                            const std::function<bool(const void*, std::size_t)>&,
                                            std::string& error) {
        repo.cancel();
        error = "cancelled";
        return false;
    });
    CHECK(!repo.fetchIndex("https://example.invalid/index.json", err));
    EXPECT_COUNTS(g, (Counts{{"plug_cat", 3}}));

    // A catalogue counts nothing.
    serve(s);
    s->body = "{\"schemaVersion\":1,\"plugins\":[]}";
    CHECK(repo.fetchIndex("https://example.invalid/index.json", err));
    EXPECT_COUNTS(g, (Counts{{"plug_cat", 3}}));
    PluginRepo::setTransportForTest(nullptr);
    g->reset();
}

void testAPluginInstallThatFailsIsCountedByClass() {
    auto g = fresh();
    auto s = std::make_shared<Served>();
    serve(s);
    const fs::path dir = scratch("install");
    PluginRepo repo;
    std::string path, err;
    const std::string payload = "pretend this is a plugin module";
    const std::string rightHash = sha256Of(payload);
    const std::string wrongHash(64, 'a');

    // THE HASH: the bytes arrive, do not match, and nothing is left in the plugins folder.
    s->body = payload;
    CHECK(!repo.install(entryFor("healthprobe", wrongHash), (dir / "plugins").string(), path, err));
    CHECK(err.find("integrity check") != std::string::npos);
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}}));
    CHECK(!fs::exists(dir / "plugins" / ("healthprobe" + kExt)));
    std::size_t debris = 0;
    for (const auto& e : fs::directory_iterator(dir / "plugins")) { (void)e; ++debris; }
    CHECK(debris == 0);

    // THE NETWORK.
    s->fail = true;
    CHECK(!repo.install(entryFor("healthprobe", rightHash), (dir / "plugins").string(), path, err));
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}, {"plug_inst.net", 1}}));

    // THE DISK: the plugins folder cannot be made (its parent is a file).
    s->fail = false;
    {
        std::ofstream(dir / "afile") << "x";
    }
    CHECK(!repo.install(entryFor("healthprobe", rightHash), (dir / "afile" / "plugins").string(), path, err));
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}, {"plug_inst.net", 1}, {"plug_inst.write", 1}}));
    // ...and the temporary file cannot be created (a directory is where it would go).
    fs::create_directories(dir / "plugins2");
    // (not empty: install() clears a leftover temporary first, and removes an empty directory too)
    const fs::path blocker = dir / "plugins2" / ("healthprobe" + kExt + "." + std::to_string(pidNow()) + ".part");
    fs::create_directories(blocker);
    {
        std::ofstream(blocker / "keep") << "x";
    }
    CHECK(!repo.install(entryFor("healthprobe", rightHash), (dir / "plugins2").string(), path, err));
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}, {"plug_inst.net", 1}, {"plug_inst.write", 2}}));

    // REFUSED BEFORE ANY DOWNLOAD: built for another ABI, no build for this machine, an unsafe file name.
    PluginCatalogEntry wrongAbi = entryFor("healthprobe", rightHash);
    wrongAbi.abiVersion += 1;
    CHECK(!repo.install(wrongAbi, (dir / "plugins").string(), path, err));
    PluginCatalogEntry unsafe = entryFor("..", rightHash);
    CHECK(!repo.install(unsafe, (dir / "plugins").string(), path, err));
    const int hitsBefore = s->hits.load();
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}, {"plug_inst.net", 1}, {"plug_inst.write", 2},
                             {"plug_inst.other", 2}}));
    CHECK(s->hits.load() == hitsBefore);   // no transfer was attempted for those

    // A CANCEL is not an install that failed.
    PluginRepo::setTransportForTest([](const std::string&, std::uint64_t,
                                       const std::function<bool(const void*, std::size_t)>&,
                                       std::string& error) {
        error = "cancelled";
        return false;
    });
    CHECK(!repo.install(entryFor("healthprobe", rightHash), (dir / "plugins").string(), path, err));
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}, {"plug_inst.net", 1}, {"plug_inst.write", 2},
                             {"plug_inst.other", 2}}));

    // AN INSTALL THAT WORKS counts nothing.
    serve(s);
    s->body = payload;
    CHECK(repo.install(entryFor("healthprobe", rightHash), (dir / "plugins").string(), path, err));
    CHECK(fs::exists(path));
    EXPECT_COUNTS(g, (Counts{{"plug_inst.hash", 1}, {"plug_inst.net", 1}, {"plug_inst.write", 2},
                             {"plug_inst.other", 2}}));
    PluginRepo::setTransportForTest(nullptr);
    g->reset();
}

void testPluginsRefusedAtLoadAreCounted() {
    auto g = fresh();
    const fs::path dir = scratch("load");
    // A FILE THAT IS NOT A MODULE AT ALL.
    {
        std::ofstream(dir / ("notamodule" + kExt)) << "this is text, not a module";
    }
    // A REAL MODULE THAT DECLARES ANOTHER PLUGIN ABI.
    CHECK(g_abiFixture != nullptr && g_declineFixture != nullptr);
    const auto scanRefusals = [&](const fs::path& folder) {
        cascade::core::PluginHost host;
        host.scan(folder.string());
        std::size_t refused = 0;
        for (const cascade::core::LoadedPlugin& p : host.plugins()) {
            if (!p.loaded) { ++refused; }
        }
        return refused;
    };
    if (g_abiFixture != nullptr) {
        std::error_code ec;
        fs::copy_file(g_abiFixture, dir / ("abimismatch" + kExt), fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
    CHECK(scanRefusals(dir) == 2);
    EXPECT_COUNTS(g, (Counts{{"plug_load.abi", 1}, {"plug_load.load", 1}}));

    // A rescan meets the same refusals again: still one of each this session.
    CHECK(scanRefusals(dir) == 2);
    EXPECT_COUNTS(g, (Counts{{"plug_load.abi", 1}, {"plug_load.load", 1}}));

    // A REAL MODULE THAT DECLINES THIS HOST (its query returns null): what a plugin built for
    // an OLDER host does, and the same word - checked alone, because a session counts each once.
    {
        g->reset();
        g->arm("", cascade::core::newInstallId(), false);
        const fs::path only = scratch("decline");
        std::error_code ec;
        fs::copy_file(g_declineFixture, only / ("declines" + kExt), fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
        CHECK(scanRefusals(only) == 1);
        EXPECT_COUNTS(g, (Counts{{"plug_load.abi", 1}}));
    }

#if defined(_WIN32)
    // A LIBRARY THAT IS NOT A PLUGIN (it exports no plugin entry point) is a stray file, not a failure.
    {
        g->reset();
        g->arm("", cascade::core::newInstallId(), false);
        const fs::path stray = scratch("stray");
        char sys[MAX_PATH] = {0};
        ::GetSystemDirectoryA(sys, MAX_PATH);
        std::error_code ec;
        fs::copy_file(fs::path(sys) / "version.dll", stray / "version.dll", fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
        cascade::core::PluginHost host;
        host.scan(stray.string());
        EXPECT_COUNTS(g, (Counts{}));
    }
#endif
    g->reset();
}

// ---------------------------------------------------------------------------
// RECORDING: the real opener, in a folder that cannot exist.
// ---------------------------------------------------------------------------
bool waitFor(cascade::gui::RecordStart& rs, cascade::gui::RecordStart::Result& out) {
    for (int i = 0; i < 1000; ++i) {
        if (rs.poll(out)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

void testARecordingThatCannotBeStartedIsCounted() {
    auto g = fresh();
    const fs::path dir = scratch("record");
    {
        std::ofstream(dir / "afile") << "x";
    }
    cascade::core::Recorder::OpenRequest req;
    req.directory = (dir / "afile" / "sub").string();   // a folder under a FILE: cannot be made
    req.path = (dir / "afile" / "sub" / "take.wav").string();
    cascade::gui::RecordStart rs;
    CHECK(rs.request(&cascade::core::Recorder::openFile, req));
    cascade::gui::RecordStart::Result r;
    CHECK(waitFor(rs, r));
    CHECK(!r.ok && !r.cancelled && !r.error.empty());
    EXPECT_COUNTS(g, (Counts{{"rec_fail", 1}}));

    // A TAKE THE USER WITHDREW while it opened is not a start that failed.
    CHECK(rs.request(
        [](const cascade::core::Recorder::OpenRequest&, cascade::core::Recorder::OpenedFile&,
           std::string& error) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            error = "disk full";
            return false;
        },
        req));
    rs.cancel();
    CHECK(waitFor(rs, r));
    CHECK(r.cancelled);
    EXPECT_COUNTS(g, (Counts{{"rec_fail", 1}}));

    // A start that works counts nothing.
    cascade::core::Recorder::OpenRequest good;
    good.directory = (dir / "good").string();
    good.path = (dir / "good" / "take.wav").string();
    CHECK(rs.request(&cascade::core::Recorder::openFile, good));
    CHECK(waitFor(rs, r));
    CHECK(r.ok);
    EXPECT_COUNTS(g, (Counts{{"rec_fail", 1}}));
    g->reset();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) { g_abiFixture = argv[1]; }
    if (argc > 2) { g_declineFixture = argv[2]; }
    testNothingOnTheHotPathsCounts();
    testASoundOutputThatCannotBeOpenedIsCounted();
    testEveryNativeDriversRefusalIsClassified();
    testTheAorDriversRefusalIsClassified();
    testASoapyDriverThatIsNotThereIsClassified();
    testTheSdrPlayDriversThreeRefusals();
    testUpdateChecksThatFailAreCounted();
    testUpdateDownloadsThatFailAreCounted();
    testTheStoresCheckAndInstallThroughTheEnvironmentSeams();
    testACatalogueThatCannotBeHadIsCounted();
    testAPluginInstallThatFailsIsCountedByClass();
    testPluginsRefusedAtLoadAreCounted();
    testARecordingThatCannotBeStartedIsCounted();
    return testSummary("test_health_paths");
}
