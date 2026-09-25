// THE SOUND CARD SOURCE INSIDE THE REAL AppWindow, after 0.99.37.
//
// feature/soundcard-source was written against 0.99.35; 0.99.36/37 then added
// the Source combo's row keys (fix/rsp-fallback), the radio-not-open lamp,
// the dead-radio re-pick, the converter in front of the radio
// (feature/converter) and installSource (fix/stop-recordings). Each of those
// knows about radios and files; this proves each one also knows about the
// sound card row, through the application's own members:
//
//   B  the combo's row keys have a sound card row, so a native re-scan
//      neither drops the sound card selection nor moves another selection
//      onto the wrong row;
//   C  the lamp does not light while a sound card is still opening;
//   D  picking the row of a sound card that has died opens it again, as
//      picking a dead radio's row does;
//   E  the converter is kept per CARD, applied when the card is installed,
//      never applied to an I/Q card (whose typed centre IS the translation),
//      and only ever a down-converter in front of a real-mode card - and a
//      tune on a real-mode card through one is not translated twice.
//
// THE SOUND CARDS ARE FAKES (AppWindow::testHooks_.soundCardBackend): nothing
// lists or opens this desk's audio inputs. Native radios are a scripted list
// (testHooks_.nativeScan) and none is ever opened. Hermetic like
// test_converter_app_paths: no config file, per-user directories pointed at a
// scratch folder, telemetry at a black hole, and the receiver is never
// started (AppWindow::run is not called), so the test thread may read the
// installed card itself.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/freq_converter.hpp"
#include "gui/app_window.hpp"
#include "gui/source_fallback.hpp"
#include "source/soundcard_source.hpp"
#include "test_check.hpp"

using cascade::core::ConverterMode;
using cascade::core::ConverterSetting;
using cascade::source::SoundCardBackend;
using cascade::source::SoundCardDevice;
using cascade::source::SoundCardFormat;
using cascade::source::SoundCardSettings;

namespace {

ConverterSetting up(double lo) { return {ConverterMode::Up, lo, false}; }
ConverterSetting down(double lo) { return {ConverterMode::Down, lo, false}; }

// --- The fake cards ------------------------------------------------------------

const char* const kApi = "Windows WASAPI";
const char* const kCardA = "Line In (Fake Audio A)";
const char* const kCardB = "Line In (Fake Audio B)";

struct CardWorld {
    std::mutex m;
    std::vector<SoundCardDevice> devices;
    std::atomic<int> opens{0};         // successful opens, every backend
    std::atomic<bool> dead{false};     // every open card: quiet, and not alive
    std::atomic<bool> holdOpen{false}; // open() waits while set (an open in flight)
    std::atomic<bool> refuse{false};   // open() refuses
};
CardWorld g_cards;

class FakeCard final : public SoundCardBackend {
public:
    ~FakeCard() override { close(); }

    std::vector<SoundCardDevice> listDevices() override {
        std::lock_guard<std::mutex> lk(g_cards.m);
        return g_cards.devices;
    }

    bool open(const SoundCardDevice&, int channels, double, bool, PushFn push, void* user,
              std::string& error) override {
        const auto t0 = std::chrono::steady_clock::now();
        while (g_cards.holdOpen.load() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (g_cards.refuse.load()) {
            error = "fake refused";
            return false;
        }
        close();
        push_ = push;
        user_ = user;
        channels_ = channels;
        open_ = true;
        feeding_ = true;
        feeder_ = std::thread([this] { feed(); });
        ++g_cards.opens;
        return true;
    }

    void close() override {
        feeding_ = false;
        if (feeder_.joinable()) { feeder_.join(); }
        open_ = false;
    }

    bool alive() override { return open_.load() && !g_cards.dead.load(); }

private:
    // 10 ms of silence every 10 ms, until the card "dies" or closes.
    void feed() {
        std::vector<float> block(static_cast<std::size_t>(480 * channels_), 0.0f);
        while (feeding_.load()) {
            if (!g_cards.dead.load()) { push_(user_, block.data(), 480); }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    PushFn push_ = nullptr;
    void* user_ = nullptr;
    int channels_ = 1;
    std::atomic<bool> open_{false};
    std::atomic<bool> feeding_{false};
    std::thread feeder_;
};

std::shared_ptr<SoundCardBackend> makeCard() { return std::make_shared<FakeCard>(); }

SoundCardDevice device(int index, const char* name) {
    SoundCardDevice d;
    d.index = index;
    d.name = name;
    d.hostApi = kApi;
    d.maxInputChannels = 2;
    d.defaultRateHz = 48000.0;
    d.isDefault = index == 0;
    d.rates = {{48000.0, false}, {96000.0, false}};
    return d;
}

void resetCards() {
    std::lock_guard<std::mutex> lk(g_cards.m);
    g_cards.devices = {device(0, kCardA), device(1, kCardB)};
    g_cards.opens = 0;
    g_cards.dead = false;
    g_cards.holdOpen = false;
    g_cards.refuse = false;
}

SoundCardSettings card(const char* name, SoundCardFormat f, double rateHz, double iqCentreHz = 0.0) {
    SoundCardSettings s;
    s.device = name;
    s.hostApi = kApi;
    s.cardRateHz = rateHz;
    s.format = f;
    s.iqCentreHz = iqCentreHz;
    s.pickedFromList = true;
    return s;
}

// The converter key a card is kept under (the patch page's device key for
// the same card - core::converterRadioKey IS the patch key).
std::string cardKey(const char* name) {
    return cascade::core::converterRadioKey("soundcard", cascade::source::soundCardDeviceArgs(name, kApi));
}

// --- The native radios (listed, never opened) -------------------------------------

std::mutex g_nativeMutex;
std::vector<cascade::source::NativeDeviceInfo> g_native;

std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    std::lock_guard<std::mutex> lk(g_nativeMutex);
    return g_native;
}

void setNative(std::vector<cascade::source::NativeDeviceInfo> v) {
    std::lock_guard<std::mutex> lk(g_nativeMutex);
    g_native = std::move(v);
}

const cascade::source::NativeDeviceInfo kRtlA{"rtlsdr", "RTL-SDR A", "serial=00000001"};
const cascade::source::NativeDeviceInfo kRtlB{"rtlsdr", "RTL-SDR B", "serial=00000002"};
const cascade::source::NativeDeviceInfo kPluto{"pluto", "ADALM-Pluto", "uri=ip:192.168.2.1"};

// --- Isolation ---------------------------------------------------------------------

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::filesystem::path g_scratch;

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() /
                ("foxsdr_soundcard_app_paths_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

}  // namespace

namespace cascade::gui {

// The friend AppWindow names for its tests (see AppWindow::testHooks_).
struct AppWindowTestAccess {
    static void installHooks() {
        AppWindow::testHooks_.soundCardBackend = &makeCard;
        AppWindow::testHooks_.nativeScan = &fakeScan;
    }

    static constexpr int kSoundCardRow = AppWindow::kSoundCardRow;
    static constexpr int kNativeRowBase = AppWindow::kNativeRowBase;

    // Collects scans and opens as the frame loop does. False on a timeout.
    static bool settle(AppWindow& a) {
        const auto t0 = std::chrono::steady_clock::now();
        while (a.soundCardOpenPending_ || a.soundCardScanPending_) {
            a.pollSoundCard();
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
    static bool listCards(AppWindow& a) {
        a.scanSoundCards();
        return settle(a);
    }
    // The Source section's Open with these settings on the Sound card row.
    static void startOpen(AppWindow& a, const SoundCardSettings& s) {
        a.sourceSel_ = AppWindow::kSoundCardRow;
        a.soundCard_ = s;
        a.launchSoundCardOpen(false, s);
    }
    static bool open(AppWindow& a, const SoundCardSettings& s) {
        startOpen(a, s);
        return settle(a);
    }
    static bool pending(AppWindow& a) { return a.soundCardOpenPending_; }
    static void restore(AppWindow& a, const cascade::core::AppConfig& cfg) { a.applyConfig(cfg); }
    static cascade::core::AppConfig saved(AppWindow& a) { return a.currentConfig(); }
    static const std::string& kind(AppWindow& a) { return a.sourceKind_; }

    // The Source combo.
    static int sel(AppWindow& a) { return a.sourceSel_; }
    static void setSel(AppWindow& a, int row) { a.sourceSel_ = row; }
    static void setSoapy(AppWindow& a, std::vector<cascade::source::SoapyDeviceInfo> v) {
        a.soapyDevices_ = std::move(v);
    }
    static int soapyRowBase(AppWindow& a) { return a.soapyRowBase(); }
    static std::vector<SourceRowKey> rowKeys(AppWindow& a) { return a.sourceRowKeys(); }
    static void scanNative(AppWindow& a) { a.scanNative(); }
    static void pick(AppWindow& a, int row) { a.selectSource(row); }

    // The lamp.
    static bool lamp(AppWindow& a) { return a.radioNotOpenLit(); }

    // The installed card, read by the test thread (the receiver is stopped).
    static bool cardDead(AppWindow& a) {
        auto* dev = dynamic_cast<cascade::source::DeviceSource*>(&a.pipeline_.rawSource());
        return dev != nullptr && dev->deviceDead();
    }
    // What the receiver's source thread does, done by the test thread: the
    // card is started (a stopped source reads nothing) and then read.
    static void startCard(AppWindow& a) { (void)a.pipeline_.rawSource().start(); }
    static void readOnce(AppWindow& a) {
        std::complex<float> buf[256];
        (void)a.pipeline_.rawSource().read(buf, 256);
    }

    // The converter.
    static void setConverter(AppWindow& a, const std::string& key, const ConverterSetting& s) {
        a.converters_[key] = s;
    }
    static ConverterSetting stored(AppWindow& a, const std::string& key) {
        const auto it = a.converters_.find(key);
        return it == a.converters_.end() ? ConverterSetting{} : it->second;
    }
    static void changeConverter(AppWindow& a, const ConverterSetting& s) { a.changeConverter(s); }
    static ConverterSetting live(AppWindow& a) { return a.pipeline_.converter(); }
    static std::string keyNow(AppWindow& a) { return a.converterRadioKeyNow(); }
    static std::vector<ConverterMode> offered(AppWindow& a) { return a.converterModesOffered(); }
    static std::string unusableNote(AppWindow& a) { return a.converterUnusableNote(); }
    static double airCentre(AppWindow& a) { return a.pipeline_.activeSource().centerFrequencyHz(); }
    static double radioNow(AppWindow& a) { return a.pipeline_.rawSource().centerFrequencyHz(); }
    static double counter(AppWindow& a) { return a.currentAbsoluteHz(); }
    static void setVfo(AppWindow& a, double hz) { a.pipeline_.setVfoOffsetHz(hz); }
    static void setBandwidth(AppWindow& a, double hz) { a.vfoBandwidthHz_ = hz; }
    static std::string tuneNote(AppWindow& a) { return a.tuneMismatchNote_; }
    // A tune by the counter, a bookmark or a preset (retuneSourceHz).
    static void retune(AppWindow& a, double centreHz) { a.retuneSourceHz(centreHz, false); }
    // The I/Q centre box, as drawSoundCardControls applies it to a running
    // I/Q card.
    static void typeIqCentre(AppWindow& a, double hz) {
        a.soundCard_.iqCentreHz = hz;
        a.soundCardLive_.iqCentreHz = hz;
        a.applyRetuneNow(hz, false);
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

// --- B: the combo's rows -------------------------------------------------------------

void testRowKeysHaveTheSoundCardRow() {
    std::printf("  the row keys are index-aligned with the combo, sound card row included\n");
    resetCards();
    setNative({kRtlA, kPluto});
    cascade::gui::AppWindow app;
    Access::scanNative(app);
    Access::setSoapy(app, {{"B200", "driver=uhd,serial=31"}});
    const std::vector<cascade::gui::SourceRowKey> keys = Access::rowKeys(app);
    // One key per combo row: generator, file, sound card, two natives, one Soapy.
    CHECK(static_cast<int>(keys.size()) == Access::soapyRowBase(app) + 1);
    CHECK(keys.size() == 6);
    const std::size_t sc = static_cast<std::size_t>(Access::kSoundCardRow);
    CHECK(keys.size() > sc && keys[sc].kind == "soundcard");
    const std::size_t nb = static_cast<std::size_t>(Access::kNativeRowBase);
    CHECK(keys.size() > nb + 2 && keys[nb].kind == "rtlsdr" && keys[nb].args == kRtlA.args);
    CHECK(keys.size() > nb + 2 && keys[nb + 1].kind == "pluto");
    CHECK(keys.size() > nb + 2 && keys[nb + 2].kind == "soapy");
}

void testSoundCardRowSurvivesARescan() {
    std::printf("  a native re-scan keeps the Sound card row selected\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    Access::scanNative(app);
    Access::setSel(app, Access::kSoundCardRow);
    // The combo opening re-scans; with no radio plugged in the sound card row
    // used to fall off the end of the keys and read -1 (its controls gone).
    Access::scanNative(app);
    CHECK(Access::sel(app) == Access::kSoundCardRow);
    // A radio plugged in meanwhile appears AFTER it; the row does not move.
    setNative({kRtlA});
    Access::scanNative(app);
    CHECK(Access::sel(app) == Access::kSoundCardRow);
    setNative({});
    Access::scanNative(app);
    CHECK(Access::sel(app) == Access::kSoundCardRow);
}

void testOtherSelectionsFollowTheirRow() {
    std::printf("  a re-scan moves a Soapy or Pluto selection with its row, never onto another\n");
    resetCards();
    setNative({kRtlA});
    cascade::gui::AppWindow app;
    Access::scanNative(app);
    Access::setSoapy(app, {{"B200", "driver=uhd,serial=31"}});
    // The last Soapy row, selected (a Soapy radio chosen and not yet open).
    Access::setSel(app, Access::soapyRowBase(app));
    Access::scanNative(app);
    CHECK(Access::sel(app) == Access::soapyRowBase(app));
    setNative({kRtlB, kRtlA});  // a dongle plugged in ahead of it
    Access::scanNative(app);
    CHECK(Access::sel(app) == Access::soapyRowBase(app));
    CHECK(Access::sel(app) == Access::kNativeRowBase + 2);

    // The Pluto row selected (it selects and does not open); a radio listed
    // ahead of it goes away. The selection follows the Pluto.
    setNative({kRtlA, kPluto});
    Access::scanNative(app);
    Access::setSel(app, Access::kNativeRowBase + 1);
    setNative({kPluto});
    Access::scanNative(app);
    CHECK(Access::sel(app) == Access::kNativeRowBase);
    // A radio's row selected with nothing open, and the radio unplugged: the
    // selection is gone (-1), never moved onto the sound card row or the
    // Pluto that slid into its place.
    setNative({kRtlA, kPluto});
    Access::scanNative(app);
    Access::setSel(app, Access::kNativeRowBase);
    setNative({kPluto});
    Access::scanNative(app);
    CHECK(Access::sel(app) == -1);
}

// --- C: the lamp -----------------------------------------------------------------------

void testLampQuietWhileTheCardOpens() {
    std::printf("  the not-open lamp stays dark while a sound card is still opening\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    CHECK(Access::listCards(app));

    // The startup restore of a saved card, its open still in flight.
    g_cards.holdOpen = true;
    cascade::core::AppConfig cfg;
    cfg.sourceKind = "soundcard";
    cfg.soundCard.device = kCardA;
    cfg.soundCard.hostApi = kApi;
    cfg.soundCard.rateHz = 48000.0;
    Access::restore(app, cfg);
    CHECK(Access::pending(app));
    CHECK(!Access::lamp(app));
    // It does not open: the lamp lights now - the saved card is not running.
    g_cards.refuse = true;
    g_cards.holdOpen = false;
    CHECK(Access::settle(app));
    CHECK(Access::kind(app) == "siggen");
    CHECK(Access::lamp(app));
    // Opened by hand: dark again.
    g_cards.refuse = false;
    CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
    CHECK(Access::kind(app) == "soundcard");
    CHECK(!Access::lamp(app));

    // A re-Open of the running card with new settings releases it first
    // (the generator stands in, the card remembered): dark while it reopens.
    g_cards.holdOpen = true;
    Access::startOpen(app, card(kCardA, SoundCardFormat::RealMono, 96000.0));
    CHECK(Access::pending(app));
    CHECK(Access::kind(app) == "siggen");
    CHECK(!Access::lamp(app));
    g_cards.holdOpen = false;
    CHECK(Access::settle(app));
    CHECK(Access::kind(app) == "soundcard");
    CHECK(!Access::lamp(app));
}

// --- D: picking a dead card's row ------------------------------------------------------------

void testPickingADeadCardReopensIt() {
    std::printf("  picking the Sound card row of a card that has died opens it again\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    CHECK(Access::listCards(app));
    CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
    CHECK(Access::kind(app) == "soundcard");
    const int opens = g_cards.opens.load();

    // Picking the row of a card that is running is a no-op, as for a radio.
    Access::pick(app, Access::kSoundCardRow);
    CHECK(Access::settle(app));
    CHECK(g_cards.opens.load() == opens);

    // Unplugged: the stream stops and the card says it is dead.
    Access::startCard(app);
    Access::readOnce(app);
    CHECK(!Access::cardDead(app));
    g_cards.dead = true;
    const auto t0 = std::chrono::steady_clock::now();
    while (!Access::cardDead(app) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6)) {
        Access::readOnce(app);
    }
    CHECK(Access::cardDead(app));
    // Plugged back in, and the row picked again - what the screen says to do.
    g_cards.dead = false;
    Access::pick(app, Access::kSoundCardRow);
    CHECK(Access::settle(app));
    CHECK(g_cards.opens.load() == opens + 1);
    CHECK(Access::kind(app) == "soundcard");
    CHECK(Access::sel(app) == Access::kSoundCardRow);
    CHECK(!Access::cardDead(app));
}

// --- E: the converter in front of a sound card ----------------------------------------------

void testConverterKeptPerCard() {
    std::printf("  a converter is kept per card and applied when the card is installed\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    CHECK(Access::listCards(app));
    CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
    CHECK(Access::keyNow(app) == cardKey(kCardA));
    Access::changeConverter(app, down(10.0e6));
    CHECK(Access::stored(app, cardKey(kCardA)) == down(10.0e6));
    CHECK(Access::live(app) == down(10.0e6));

    // Card B has no converter of its own; A's does not follow it there.
    CHECK(Access::open(app, card(kCardB, SoundCardFormat::RealMono, 48000.0)));
    CHECK(Access::keyNow(app) == cardKey(kCardB));
    CHECK(!cascade::core::converterActive(Access::live(app)));
    // ...and back on A, A's is applied again.
    CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
    CHECK(Access::live(app) == down(10.0e6));
    // The air centre is the card's fs/4 through the down-converter.
    CHECK(Access::radioNow(app) == 12000.0);
    CHECK(Access::airCentre(app) == 10.012e6);
}

void testConverterAppliedOnRestore() {
    std::printf("  the startup restore of a card applies that card's converter\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    cascade::core::AppConfig cfg;
    cfg.sourceKind = "soundcard";
    cfg.soundCard.device = kCardA;
    cfg.soundCard.hostApi = kApi;
    cfg.soundCard.rateHz = 48000.0;
    cfg.converters[cardKey(kCardA)] = down(7.0e6);
    cfg.converters[cardKey(kCardB)] = down(3.0e6);
    Access::restore(app, cfg);
    CHECK(Access::settle(app));
    CHECK(Access::kind(app) == "soundcard");
    CHECK(Access::live(app) == down(7.0e6));
    CHECK(Access::airCentre(app) == 7.012e6);
}

void testIqCardHasNoConverter() {
    std::printf("  an I/Q card's typed centre is the translation: no converter is applied or offered\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    CHECK(Access::listCards(app));
    Access::setConverter(app, cardKey(kCardA), up(125.0e6));
    CHECK(Access::open(app, card(kCardA, SoundCardFormat::IqStereo, 96000.0, 7.1e6)));
    CHECK(Access::kind(app) == "soundcard");
    CHECK(!cascade::core::converterActive(Access::live(app)));
    CHECK(Access::offered(app).empty());
    CHECK(Access::airCentre(app) == 7.1e6);
    CHECK(Access::radioNow(app) == 7.1e6);

    // Even asked directly, nothing is put between the card and the air -
    // neither kind: an up-converter is refused in front of any card, so the
    // DOWN-converter is what proves the I/Q rule itself (the first break-it
    // pass removed that rule and an up-only check stayed green).
    Access::changeConverter(app, up(125.0e6));
    CHECK(!cascade::core::converterActive(Access::live(app)));
    CHECK(Access::radioNow(app) == 7.1e6);
    CHECK(Access::airCentre(app) == 7.1e6);
    Access::changeConverter(app, down(7.0e6));
    CHECK(!cascade::core::converterActive(Access::live(app)));
    CHECK(Access::radioNow(app) == 7.1e6);
    CHECK(Access::airCentre(app) == 7.1e6);
    CHECK(Access::unusableNote(app).empty());

    // A new centre typed: the card, the section and the config all say the
    // SAME figure, so the next launch opens where this one was.
    Access::typeIqCentre(app, 7.2e6);
    CHECK(Access::radioNow(app) == 7.2e6);
    CHECK(Access::airCentre(app) == 7.2e6);
    const cascade::core::AppConfig cfg = Access::saved(app);
    CHECK(cfg.soundCard.centreHz == 7.2e6);
    CHECK(cfg.centerHz == 7.2e6);

    // A down-converter already stored for the card when it opens in I/Q
    // mode is not applied either.
    Access::setConverter(app, cardKey(kCardB), down(7.0e6));
    CHECK(Access::open(app, card(kCardB, SoundCardFormat::IqStereo, 96000.0, 7.1e6)));
    CHECK(!cascade::core::converterActive(Access::live(app)));
    CHECK(Access::airCentre(app) == 7.1e6);
    CHECK(Access::radioNow(app) == 7.1e6);
}

void testRealCardTakesADownConverterOnly() {
    std::printf("  a real-mode card offers Off and Down only; a stored Up reads as Off, with a note\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    CHECK(Access::listCards(app));
    CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
    const std::vector<ConverterMode> offered = Access::offered(app);
    CHECK(offered.size() == 2);
    CHECK(std::find(offered.begin(), offered.end(), ConverterMode::Off) != offered.end());
    CHECK(std::find(offered.begin(), offered.end(), ConverterMode::Down) != offered.end());
    CHECK(std::find(offered.begin(), offered.end(), ConverterMode::Up) == offered.end());
    CHECK(Access::unusableNote(app).empty());

    // An Up asked for anyway is stored (its LO kept) but not applied.
    Access::changeConverter(app, up(125.0e6));
    CHECK(!cascade::core::converterActive(Access::live(app)));
    CHECK(Access::radioNow(app) == 12000.0);
    CHECK(!Access::unusableNote(app).empty());

    // A stored Up (another program's config, an old session) on reopen: Off.
    Access::setConverter(app, cardKey(kCardB), up(125.0e6));
    CHECK(Access::open(app, card(kCardB, SoundCardFormat::RealMono, 48000.0)));
    CHECK(!cascade::core::converterActive(Access::live(app)));
    CHECK(Access::airCentre(app) == 12000.0);
    CHECK(!Access::unusableNote(app).empty());
    // A down-converter does apply.
    Access::changeConverter(app, down(10.0e6));
    CHECK(Access::live(app) == down(10.0e6));
    CHECK(Access::unusableNote(app).empty());
}

void testRealCardTunesThroughTheConverterOnce() {
    std::printf("  a tune on a real-mode card through a down-converter moves the VFO, translated once\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    CHECK(Access::listCards(app));
    CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
    Access::setBandwidth(app, 3000.0);
    Access::setVfo(app, 0.0);
    // Switching the converter on relabels a card (it has no tuner to move):
    // no "out of reach" sentence, the card still at fs/4.
    Access::changeConverter(app, down(10.0e6));
    CHECK(Access::tuneNote(app).empty());
    CHECK(Access::radioNow(app) == 12000.0);
    CHECK(Access::airCentre(app) == 10.012e6);
    // 10.015 MHz on the air is 15 kHz at the card: inside, reached by the VFO.
    Access::retune(app, 10.015e6);
    CHECK(Access::tuneNote(app).empty());
    CHECK(Access::radioNow(app) == 12000.0);
    CHECK(Access::airCentre(app) == 10.012e6);
    CHECK_NEAR(Access::counter(app), 10.015e6, 0.5);
    // 9.9 MHz on the air is below what the card receives through it.
    Access::retune(app, 9.9e6);
    CHECK(!Access::tuneNote(app).empty());
    CHECK(Access::radioNow(app) == 12000.0);
    CHECK_NEAR(Access::counter(app), 10.015e6, 0.5);
}

}  // namespace

int main() {
    std::printf("test_soundcard_app_paths\n");
    isolate();
    Access::installHooks();

    testRowKeysHaveTheSoundCardRow();
    testSoundCardRowSurvivesARescan();
    testOtherSelectionsFollowTheirRow();
    testLampQuietWhileTheCardOpens();
    testPickingADeadCardReopensIt();
    testConverterKeptPerCard();
    testConverterAppliedOnRestore();
    testIqCardHasNoConverter();
    testRealCardTakesADownConverterOnly();
    testRealCardTunesThroughTheConverterOnce();

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_soundcard_app_paths");
}
