// TWO RUNNING COPIES ON ONE PROFILE (0.99.115, brief 83, review 4 item D1): a switch turned OFF in any running copy silences every running copy, and no copy writes a data switch back ON or an
// identifier back that the person did not just choose in that copy.
//
// Held with real EngineServices on a scratch settings file (the keeper driven as foxsdr-engine's wait loop drives it), the OTHER copy being what it is to this one: a click in it is a write to the
// file (a merge of the consent fields, made here by hand under the process's one I/O mutex), and the lock it holds is a ConfigLock the test takes. Two statements per case from the brief:
//
//   a  A copy WITHOUT the settings lock keeps both data families off for its whole run: no identifier, no ledgers, no queue folder, it deletes none of the holder's files; the interface's half
//      it serves says Diagnostics and the update check are off
//   b  No copy writes the consent fields from memory in an ordinary save: a second copy leaves them to the disk, and the holder lays the disk's over its own
//   c  The copy that HOLDS the lock follows the file: a Usage switch the disk says is OFF is turned OFF here at once (and stays OFF through later saves); an ON is adopted from empty; before
//      any send the disk is asked (core::consent::sendVetoed); the Diagnostics and update-check switches follow the disk
//   d  A click in the copy without the lock still works for the person: it is written into the file at once, as a merge, for the holder to adopt
//   e  A window that still carries the old value in its half does not put a switch back; and a click of the holder's own is not reverted by the disk it has not written yet
//
// Also here: the writer's overlay as a function (core::consent::overlay) and the merge (core::consent::mergeKeys).
//
// Break-it: each gate named in report 83's table, removed one at a time; the case that holds it goes RED.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/config_halves.hpp"
#include "core/config_lock.hpp"
#include "core/consent_file.hpp"
#include "core/crash_upload.hpp"
#include "core/telemetry.hpp"
#include "engine/engine_service.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;
namespace cmd = cascade::core::cmd;
namespace consent = cascade::core::consent;
using json = nlohmann::json;
using cascade::core::AppConfig;
using cascade::core::ConfigStore;
using cascade::engine::EngineService;
using cascade::engine::ServiceLock;
using cascade::engine::ServiceOptions;

namespace {

const char* const kIdA = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
const char* const kIdB = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";

template <class F>
bool tickUntil(EngineService& svc, int ms, F cond) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        svc.keep();
        if (cond()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return cond();
}
void tick(EngineService& svc, int ms) { tickUntil(svc, ms, [] { return false; }); }

json readConfig(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) { return json(); }
    const json j = json::parse(emt::readFile(p), nullptr, false);
    return j.is_object() ? j : json();
}
bool enabledOnDisk(const fs::path& p) { const json j = readConfig(p); return j.is_object() && j.value("telemetryEnabled", false); }
std::string idOnDisk(const fs::path& p) { const json j = readConfig(p); return j.is_object() ? j.value("telemetryInstallId", std::string()) : std::string(); }
std::string pendingOnDisk(const fs::path& p) { const json j = readConfig(p); return j.is_object() ? j.value("telemetryPending", std::string()) : std::string(); }
bool diagOnDisk(const fs::path& p) { const json j = readConfig(p); return j.is_object() && j.value("diagnosticsEnabled", true); }
bool updateOnDisk(const fs::path& p) { const json j = readConfig(p); return j.is_object() && j.value("updateCheckEnabled", true); }
double squelchOnDisk(const fs::path& p) { const json j = readConfig(p); return j.is_object() ? j.value("squelchDb", 0.0) : 0.0; }

// WHAT ANOTHER RUNNING COPY DOES WHEN ITS PERSON CLICKS: a merge of the consent fields into the file (under the process's one I/O mutex only so that this test's own write cannot meet the
// engine's writer in one process: two programs have no such mutex, which is the limit core/consent_file.hpp names).
void otherCopyClicks(const fs::path& cfg, const std::function<void(AppConfig&)>& change) {
    std::lock_guard<std::mutex> lk(consent::ioMutex());
    AppConfig c;
    std::string err;
    CHECK(ConfigStore::load(cfg.string(), c, err));
    change(c);
    CHECK(ConfigStore::save(cfg.string(), c, err));
}
void otherCopyUsageOff(const fs::path& cfg) {
    otherCopyClicks(cfg, [](AppConfig& c) { c.telemetryEnabled = false; c.telemetryInstallId.clear(); c.telemetryPending.clear(); c.telemetryFirstRun.clear(); c.telemetryFirstVersion.clear(); c.telemetryLaunches = 0; });
}

struct Profile {
    fs::path root, cfg;
    explicit Profile(const char* tag, bool usageOn = true, bool diagOn = true, bool updateOn = true) {
        // (one base for the whole run: the previous profile's own TEMP is gone by now, and temp_directory_path() would throw on it)
        static const fs::path base = fs::temp_directory_path();
        root = base / (std::string("foxsdr-") + tag + "-" + std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(root);
        const fs::path home = root / "home";
        fs::create_directories(home / "tmp");
        emt::isolate(home);
        cfg = home / "k" / "config.json";
        fs::create_directories(cfg.parent_path());
        AppConfig seed;
        seed.volume = 0.0f;
        seed.telemetryEnabled = usageOn;
        seed.telemetryInstallId = usageOn ? kIdA : "";
        seed.telemetryFirstRun = usageOn ? "2026-01-02" : "";
        seed.telemetryFirstVersion = usageOn ? "0.99.80" : "";
        seed.telemetryLaunches = usageOn ? 3 : 0;
        seed.telemetryCleanExit = true;
        seed.diagnosticsEnabled = diagOn;
        seed.updateCheckEnabled = updateOn;
        std::ofstream out(cfg, std::ios::binary);
        out << ConfigStore::serialize(seed);
    }
    ~Profile() {
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(root, ec); }
    }
};

// One running copy: the service, started the way a window's does (best-effort lock: a copy that cannot take it carries on).
struct Copy {
    std::unique_ptr<EngineService> svc;
    explicit Copy(const fs::path& cfg) {
        ServiceOptions o;
        o.configPath = cfg.string();
        o.lock = ServiceLock::BestEffort;
        svc = std::make_unique<EngineService>(o);
        std::string sentence;
        CHECK(svc->acquireConfig(sentence));
        const EngineService::Restored restored = svc->restore();
        CHECK(restored.loaded && !restored.damaged);
        std::string why;
        CHECK(svc->start(why));
    }
    EngineService& s() { return *svc; }
    cascade::engine::Engine& e() { return svc->engine(); }
    AppConfig snap() { return svc->engine().configSnapshot(); }
    // an ordinary change, saved after the debounce window: returns once the save has been asked for and has landed
    void changeSquelch(const fs::path& cfg, double db) {
        const std::uint64_t before = svc->savesRequested();
        e().submitCommand(cmd::makeNum(FOXAPI_OP_SET_SQUELCH, db));
        CHECK(tickUntil(*svc, 7000, [&] { return svc->savesRequested() > before; }));
        CHECK(tickUntil(*svc, 3000, [&] { return squelchOnDisk(cfg) == db; }));
    }
    void stop() { svc->stop(); }
    // A fact as a window reads it: through the engine's own API table, on a local session (what the Classic window's readFactText and the new window's LiveWorld::fact do).
    std::string apiFact(const char* key) {
        const FoxEngineApi* api = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
        FoxEngine* handle = e().apiHandle();
        if (api == nullptr || handle == nullptr) { return "no-api"; }
        FoxSession* session = nullptr;
        FoxSessionParams sp{};
        sp.structSize = sizeof(sp);
        sp.flags = FOXAPI_SESSION_LOCAL | FOXAPI_SESSION_FRAME;
        sp.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS;
        sp.clientName = "test_two_copies";
        sp.clientVersion = "0";
        if (api->open_session(handle, &sp, &session) != FOXAPI_OK) { return "no-session"; }
        char buf[64] = {0};
        const int32_t n = api->get_setting(session, key, buf, sizeof(buf));
        api->close_session(session);
        return n >= 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string("not-found");
    }
};

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_two_copies\n");

    // --- THE OVERLAY AND THE MERGE, AS FUNCTIONS ----------------------------------------------------------------------------------------------------------
    {
        std::printf("  O1  overlay: the holder lays the disk's switches, identifier and (conditionally) pending record over its own document; a second copy lays every consent key\n");
        Profile p("two-copies-overlay", /*usageOn=*/false, /*diagOn=*/false, /*updateOn=*/false);   // the disk: everything OFF, no identifier
        AppConfig mine;
        mine.telemetryEnabled = true;
        mine.telemetryInstallId = kIdA;
        mine.telemetryPending = "{\"v\":1}";
        mine.telemetryLaunches = 9;
        mine.diagnosticsEnabled = true;
        mine.updateCheckEnabled = true;
        mine.testerUsagePending = {"queued-report"};
        mine.squelchDb = -61.0;
        const std::string text = ConfigStore::serialize(mine);
        const json holder = json::parse(consent::overlay(text, p.cfg.string(), consent::Role::Holder));
        CHECK(holder["telemetryEnabled"] == false);
        CHECK(holder["telemetryInstallId"] == "");
        CHECK(holder["telemetryPending"] == "");               // OFF on the disk wins: a record built under an identifier the disk has deleted is never written back
        CHECK(holder["diagnosticsEnabled"] == false);
        CHECK(holder["updateCheckEnabled"] == false);
        CHECK(holder["squelchDb"] == -61.0);                   // everything else is the holder's own
        CHECK(holder["telemetryLaunches"] == 9);               // (counters are memory's: they describe this copy's own session)
        CHECK(!holder["testerUsagePending"].empty());          // (the tester's queue is the interface's, laid only for a second copy)
        const json second = json::parse(consent::overlay(text, p.cfg.string(), consent::Role::Second));
        CHECK(second["telemetryEnabled"] == false);
        CHECK(second["telemetryInstallId"] == "");
        CHECK(second["telemetryPending"] == "");
        CHECK(second["telemetryLaunches"] == 0);
        CHECK(second["diagnosticsEnabled"] == false);
        CHECK(second["testerUsagePending"].empty());           // the disk's (empty) tester queue, not this copy's
        CHECK(second["squelchDb"] == -61.0);
        // a disk with Usage ON under the SAME identifier keeps the holder's pending record; under ANOTHER identifier it does not
        otherCopyClicks(p.cfg, [](AppConfig& c) { c.telemetryEnabled = true; c.telemetryInstallId = kIdA; });
        const json keep = json::parse(consent::overlay(text, p.cfg.string(), consent::Role::Holder));
        CHECK(keep["telemetryEnabled"] == true && keep["telemetryInstallId"] == kIdA);
        CHECK(keep["telemetryPending"] == "{\"v\":1}");
        otherCopyClicks(p.cfg, [](AppConfig& c) { c.telemetryInstallId = kIdB; });
        const json other = json::parse(consent::overlay(text, p.cfg.string(), consent::Role::Holder));
        CHECK(other["telemetryInstallId"] == kIdB);
        CHECK(other["telemetryPending"] == "");                // not this copy's record: it was built under another identifier
        // a missing or damaged file leaves the document as it is
        CHECK(consent::overlay(text, (p.cfg.parent_path() / "nothing.json").string(), consent::Role::Holder) == text);

        std::printf("  O2  merge: only the named keys change; the rest of the file is kept; a file that is not there is written whole\n");
        AppConfig src;
        src.telemetryEnabled = false;
        src.telemetryInstallId.clear();
        src.volume = 0.77f;                                    // not a named key
        std::string err;
        CHECK(consent::mergeKeys(p.cfg.string(), ConfigStore::serialize(src), consent::usageKeys(), err));
        const json after = readConfig(p.cfg);
        CHECK(after["telemetryEnabled"] == false && after["telemetryInstallId"] == "");
        CHECK(after["volume"] != 0.77);                        // the file's own volume (0.0) stands
        const fs::path fresh = p.cfg.parent_path() / "fresh" / "config.json";
        CHECK(consent::mergeKeys(fresh.string(), ConfigStore::serialize(src), consent::usageKeys(), err));
        CHECK(fs::is_regular_file(fresh));
    }

    // --- a, b: A COPY WITHOUT THE LOCK -----------------------------------------------------------------------------------------------------------------------
    {
        std::printf("  S1  a copy without the settings lock keeps both families off, deletes none of the holder's files, and never writes the consent fields\n");
        Profile p("two-copies-second");
        const fs::path folder = p.cfg.parent_path();
        // what the holder keeps beside the file: a ledger, and a queue folder with a held failure
        { std::ofstream(folder / "telemetry-stalls") << "x"; }
        { std::ofstream(folder / "telemetry-health") << "x"; }
        fs::create_directories(folder / "radio-reports");
        { std::ofstream(folder / "radio-reports" / "state.json") << "{}"; }
        cascade::core::ConfigLock holder;
        CHECK(holder.acquire(p.cfg.string(), "other-copy") == cascade::core::ConfigLock::State::Held);
        Copy copy(p.cfg);
        CHECK(copy.e().consentSecondCopy());
        const AppConfig s = copy.snap();
        CHECK(!s.telemetryEnabled && s.telemetryInstallId.empty());           // no identifier, no reporting in this copy
        CHECK(!s.diagnosticsEnabled && !s.updateCheckEnabled);                // and the half its window is served says both switches are off
        std::string holderFact, generation, diag, update;
        CHECK(copy.e().consentFact("holder", holderFact) && holderFact == "0");
        CHECK(copy.e().consentFact("diagnostics", diag) && diag == "0");
        CHECK(copy.e().consentFact("updateCheck", update) && update == "0");
        CHECK(copy.apiFact("fact.consent.holder") == "0");                    // and the windows read it through the table
        CHECK(fs::exists(folder / "telemetry-stalls") && fs::exists(folder / "telemetry-health"));   // the holder's ledgers are still there
        CHECK(fs::exists(folder / "radio-reports" / "state.json"));
        // b: a save of an ordinary change leaves the consent fields as the disk has them (the file said ON, id A, Diagnostics ON, updates ON)
        copy.changeSquelch(p.cfg, -55.0);
        CHECK(enabledOnDisk(p.cfg) && idOnDisk(p.cfg) == kIdA);
        CHECK(diagOnDisk(p.cfg) && updateOnDisk(p.cfg));
        // the holder's person switches Usage and Diagnostics OFF; this copy's next ordinary save must not put either back
        otherCopyUsageOff(p.cfg);
        otherCopyClicks(p.cfg, [](AppConfig& c) { c.diagnosticsEnabled = false; c.updateCheckEnabled = false; });
        copy.changeSquelch(p.cfg, -44.0);
        CHECK(!enabledOnDisk(p.cfg));
        CHECK(idOnDisk(p.cfg).empty());
        CHECK(pendingOnDisk(p.cfg).empty());
        CHECK(!diagOnDisk(p.cfg) && !updateOnDisk(p.cfg));
        copy.stop();
        CHECK(!enabledOnDisk(p.cfg) && idOnDisk(p.cfg).empty() && !diagOnDisk(p.cfg));   // the final save at the quit too
    }

    // --- d: A CLICK IN THE COPY WITHOUT THE LOCK ---------------------------------------------------------------------------------------------------------------
    {
        std::printf("  S2  a click in the copy without the lock reaches the file at once (Usage OFF, then ON with a new identifier); the copy itself stays off\n");
        Profile p("two-copies-second-click");
        cascade::core::ConfigLock holder;
        CHECK(holder.acquire(p.cfg.string(), "other-copy") == cascade::core::ConfigLock::State::Held);
        Copy copy(p.cfg);
        CHECK(copy.e().consentSecondCopy());
        copy.e().submitCommand(cmd::makeInt(FOXAPP_OP_TELEMETRY_CONSENT, 0));
        CHECK(tickUntil(copy.s(), 1400, [&] { return !enabledOnDisk(p.cfg); }));
        CHECK(idOnDisk(p.cfg).empty());
        CHECK(!copy.snap().telemetryEnabled);
        copy.e().submitCommand(cmd::makeInt(FOXAPP_OP_TELEMETRY_CONSENT, 1));
        CHECK(tickUntil(copy.s(), 1400, [&] { return enabledOnDisk(p.cfg); }));
        CHECK(!idOnDisk(p.cfg).empty() && idOnDisk(p.cfg) != kIdA);
        CHECK(!copy.snap().telemetryEnabled && copy.snap().telemetryInstallId.empty());   // this copy reports nothing under it
        // the Diagnostics tick in this copy's window: the half it was served says OFF, so a half that says ON is a click, and it is written into the file (here: the file said OFF)
        otherCopyClicks(p.cfg, [](AppConfig& c) { c.diagnosticsEnabled = false; });
        CHECK(!diagOnDisk(p.cfg));
        AppConfig half = copy.e().configSnapshot();
        half.diagnosticsEnabled = true;
        copy.e().submitCommand(cmd::makeText(FOXAPP_OP_INTERFACE_CONFIG, cascade::core::serializeInterfaceHalf(half)));
        CHECK(tickUntil(copy.s(), 1400, [&] { return diagOnDisk(p.cfg); }));
        copy.changeSquelch(p.cfg, -50.0);
        CHECK(diagOnDisk(p.cfg));                          // and an ordinary save of this copy leaves it as the file has it
        copy.stop();
    }

    {
        std::printf("  S3  a click of the copy without the lock that a write of another copy undid is written again (re-checked a few times after the click)\n");
        Profile p("two-copies-second-verify");
        cascade::core::ConfigLock holder;
        CHECK(holder.acquire(p.cfg.string(), "other-copy") == cascade::core::ConfigLock::State::Held);
        Copy copy(p.cfg);
        copy.e().submitCommand(cmd::makeInt(FOXAPP_OP_TELEMETRY_CONSENT, 0));
        CHECK(tickUntil(copy.s(), 1400, [&] { return !enabledOnDisk(p.cfg); }));
        // the holder's save, which had read the file before the click, replaces it: ON and the old identifier are back
        otherCopyClicks(p.cfg, [](AppConfig& c) { c.telemetryEnabled = true; c.telemetryInstallId = kIdA; });
        CHECK(enabledOnDisk(p.cfg));
        CHECK(tickUntil(copy.s(), 5000, [&] { return !enabledOnDisk(p.cfg); }));
        CHECK(idOnDisk(p.cfg).empty());
        copy.stop();
    }

    // --- c: THE HOLDER FOLLOWS THE FILE ---------------------------------------------------------------------------------------------------------------------------
    {
        std::printf("  H1  the holder turns Usage reporting OFF at once when another copy's click says so, and no later save of its own puts it back\n");
        Profile p("two-copies-holder-off");
        Copy copy(p.cfg);
        CHECK(!copy.e().consentSecondCopy());
        CHECK(tickUntil(copy.s(), 5000, [&] { return !pendingOnDisk(p.cfg).empty(); }));    // an ON period's file carries the pending record
        CHECK(copy.snap().telemetryEnabled && copy.snap().telemetryInstallId == kIdA);
        otherCopyUsageOff(p.cfg);
        CHECK(tickUntil(copy.s(), 3000, [&] { return !copy.snap().telemetryEnabled; }));
        CHECK(copy.snap().telemetryInstallId.empty());
        copy.changeSquelch(p.cfg, -33.0);                  // an ordinary save of the holder
        CHECK(!enabledOnDisk(p.cfg));
        CHECK(idOnDisk(p.cfg).empty());
        CHECK(pendingOnDisk(p.cfg).empty());
        // a quit writes the file again: still OFF
        copy.stop();
        CHECK(!enabledOnDisk(p.cfg) && idOnDisk(p.cfg).empty() && pendingOnDisk(p.cfg).empty());
    }
    {
        std::printf("  H2  the holder adopts an ON that another copy's click wrote, from empty, under the identifier on the disk\n");
        Profile p("two-copies-holder-on", /*usageOn=*/false);
        Copy copy(p.cfg);
        CHECK(!copy.snap().telemetryEnabled);
        otherCopyClicks(p.cfg, [](AppConfig& c) { c.telemetryEnabled = true; c.telemetryInstallId = kIdB; c.telemetryLaunches = 1; c.telemetryFirstRun = "2026-11-25"; c.telemetryFirstVersion = "0.99.115"; });
        CHECK(tickUntil(copy.s(), 3000, [&] { return copy.snap().telemetryEnabled; }));
        CHECK(copy.snap().telemetryInstallId == kIdB);
        CHECK(copy.snap().telemetryLaunches == 1);
        copy.changeSquelch(p.cfg, -33.0);
        CHECK(enabledOnDisk(p.cfg) && idOnDisk(p.cfg) == kIdB);
        copy.stop();
    }
    {
        std::printf("  H3  before every send the disk is asked: a report is refused when the disk says its switch is OFF, whatever the holder's memory says (no pass has run)\n");
        Profile p("two-copies-holder-send");
        Copy copy(p.cfg);
        const std::string usageUrl = cascade::core::telemetryEndpoint();
        const std::string crashUrl = cascade::core::crashUploadEndpoint();
        CHECK(!consent::sendVetoed(usageUrl));             // ON on the disk, ON in memory: allowed
        CHECK(!consent::sendVetoed(crashUrl));
        otherCopyUsageOff(p.cfg);
        CHECK(consent::sendVetoed(usageUrl));              // refused at once, before this copy has adopted anything
        CHECK(!consent::sendVetoed(crashUrl));             // the other family is untouched
        otherCopyClicks(p.cfg, [](AppConfig& c) { c.diagnosticsEnabled = false; });
        CHECK(consent::sendVetoed(crashUrl));
        CHECK(!consent::sendVetoed("http://127.0.0.1:4321/not-a-data-destination"));   // anything else is not a data switch's
        copy.stop();
    }
    {
        std::printf("  H4  the Diagnostics and update-check switches follow the disk; a window still carrying the old value does not put them back; a real click does\n");
        Profile p("two-copies-holder-switches");
        Copy copy(p.cfg);
        std::string g0, g1, diag;
        CHECK(copy.e().consentFact("generation", g0));
        CHECK(copy.apiFact("fact.consent.holder") == "1");
        CHECK(copy.apiFact("fact.consent.generation") == g0);
        otherCopyClicks(p.cfg, [](AppConfig& c) { c.diagnosticsEnabled = false; c.updateCheckEnabled = false; });
        CHECK(tickUntil(copy.s(), 3000, [&] { std::string d; return copy.e().consentFact("diagnostics", d) && d == "0"; }));
        CHECK(copy.e().consentFact("generation", g1) && g1 != g0);                 // the windows are told
        CHECK(copy.apiFact("fact.consent.generation") == g1);
        std::string update;
        CHECK(copy.e().consentFact("updateCheck", update) && update == "0");
        // the window has not heard yet: its next half still says ON (the value it started with); the engine keeps OFF, and so does the file
        AppConfig stale = copy.e().configSnapshot();
        stale.diagnosticsEnabled = true;
        stale.updateCheckEnabled = true;
        stale.squelchDb = -70.0;
        copy.e().submitCommand(cmd::makeText(FOXAPP_OP_INTERFACE_CONFIG, cascade::core::serializeInterfaceHalf(stale)));
        tick(copy.s(), 700);
        CHECK(copy.e().consentFact("diagnostics", diag) && diag == "0");
        copy.changeSquelch(p.cfg, -66.0);
        CHECK(!diagOnDisk(p.cfg) && !updateOnDisk(p.cfg));
        // the window follows (sends OFF), then the person clicks ON: that is a click, and the file says ON
        AppConfig follow = stale;
        follow.diagnosticsEnabled = false;
        follow.updateCheckEnabled = false;
        copy.e().submitCommand(cmd::makeText(FOXAPP_OP_INTERFACE_CONFIG, cascade::core::serializeInterfaceHalf(follow)));
        tick(copy.s(), 300);
        follow.diagnosticsEnabled = true;
        copy.e().submitCommand(cmd::makeText(FOXAPP_OP_INTERFACE_CONFIG, cascade::core::serializeInterfaceHalf(follow)));
        CHECK(tickUntil(copy.s(), 1400, [&] { return diagOnDisk(p.cfg); }));
        CHECK(copy.e().consentFact("diagnostics", diag) && diag == "1");
        copy.stop();
    }
    {
        std::printf("  H5  the holder's own click is not reverted by the file it has not written yet (a write in flight is not a disagreement)\n");
        Profile p("two-copies-holder-click");
        Copy copy(p.cfg);
        CHECK(tickUntil(copy.s(), 5000, [&] { return !pendingOnDisk(p.cfg).empty(); }));
        {
            // hold the file's I/O mutex: the merge the click starts on its worker waits behind it, so the disk still says ON while the memory says OFF; and another copy's ordinary save
            // touches the file meanwhile (a new time and size: the holder looks again and finds ON on the disk)
            std::unique_lock<std::mutex> hold(consent::ioMutex());
            copy.e().submitCommand(cmd::makeInt(FOXAPP_OP_TELEMETRY_CONSENT, 0));
            tick(copy.s(), 300);
            {
                AppConfig c;
                std::string err;
                CHECK(ConfigStore::load(p.cfg.string(), c, err));
                c.squelchDb = -12.0;
                CHECK(ConfigStore::save(p.cfg.string(), c, err));
            }
            tick(copy.s(), 1300);                          // the pass has looked at the file at least twice
            CHECK(!copy.snap().telemetryEnabled);          // not put back ON from the disk that has not caught up
            CHECK(enabledOnDisk(p.cfg));
        }
        CHECK(tickUntil(copy.s(), 3000, [&] { return !enabledOnDisk(p.cfg); }));
        tick(copy.s(), 1200);
        CHECK(!copy.snap().telemetryEnabled && !enabledOnDisk(p.cfg) && idOnDisk(p.cfg).empty());
        copy.stop();
    }

    // --- A START-UP MINT IS A CONSENT WRITE --------------------------------------------------------------------------------------------------------------------
    {
        std::printf("  M1  a first start (Usage ON by default, no identifier): the identifier minted at the start reaches the file, and the pending record follows it\n");
        Profile p("two-copies-mint", /*usageOn=*/false);
        otherCopyClicks(p.cfg, [](AppConfig& c) { c.telemetryEnabled = true; c.telemetryInstallId.clear(); });   // on by default, nothing minted yet
        Copy copy(p.cfg);
        CHECK(copy.snap().telemetryEnabled && !copy.snap().telemetryInstallId.empty());
        const std::string minted = copy.snap().telemetryInstallId;
        CHECK(tickUntil(copy.s(), 3000, [&] { return idOnDisk(p.cfg) == minted; }));
        copy.stop();                                       // (the final save carries the record, under the identifier the file now holds)
        CHECK(idOnDisk(p.cfg) == minted && enabledOnDisk(p.cfg));
        CHECK(!pendingOnDisk(p.cfg).empty());
    }

    {
        std::printf("  M2  a run that is refused after the restore (a provision that fails) leaves the file exactly as it was: the minted identifier is written when the run starts, not before\n");
        Profile p("two-copies-refused", /*usageOn=*/false);
        otherCopyClicks(p.cfg, [](AppConfig& c) { c.telemetryEnabled = true; c.telemetryInstallId.clear(); });
        const std::string before = emt::readFile(p.cfg);
        {
            ServiceOptions o;
            o.configPath = p.cfg.string();
            o.lock = ServiceLock::BestEffort;
            EngineService svc(o);
            std::string sentence;
            CHECK(svc.acquireConfig(sentence));
            const EngineService::Restored restored = svc.restore();
            CHECK(restored.loaded);
            CHECK(!svc.engine().configSnapshot().telemetryInstallId.empty());   // an identifier was minted in memory ...
            // passes before the start (a window test's way): the disk has no identifier yet, which is not "OFF on the disk" for a copy that minted one a moment ago
            CHECK(svc.engine().runPassNow(std::chrono::milliseconds(2000)));
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            CHECK(svc.engine().runPassNow(std::chrono::milliseconds(2000)));
            CHECK(svc.engine().configSnapshot().telemetryEnabled && !svc.engine().configSnapshot().telemetryInstallId.empty());
        }
        CHECK(emt::readFile(p.cfg) == before);                                  // ... and nothing was written
    }

    return testSummary("test_two_copies");
}
