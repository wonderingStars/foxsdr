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
    std::atomic<int> attempts{0};      // every open() asked, refused or not
    std::atomic<bool> dead{false};     // every open card: quiet, and not alive
    std::atomic<bool> holdOpen{false}; // open() waits while set (an open in flight)
    std::atomic<bool> refuse{false};   // open() refuses
    std::atomic<double> refuseRate{0.0};  // open() refuses this card rate only
    std::atomic<int> openNow[4]{};     // streams open now, per device index (& 3)
    std::atomic<int> overlaps{0};      // an open while the same device was still open
};
CardWorld g_cards;

class FakeCard final : public SoundCardBackend {
public:
    ~FakeCard() override { close(); }

    std::vector<SoundCardDevice> listDevices() override {
        std::lock_guard<std::mutex> lk(g_cards.m);
        return g_cards.devices;
    }

    bool open(const SoundCardDevice& dv, int channels, double rateHz, bool, PushFn push, void* user,
              std::string& error) override {
        const auto t0 = std::chrono::steady_clock::now();
        while (g_cards.holdOpen.load() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ++g_cards.attempts;
        if (g_cards.refuse.load() || rateHz == g_cards.refuseRate.load()) {
            error = "fake refused";
            return false;
        }
        close();
        idx_ = dv.index;
        if (g_cards.openNow[idx_ & 3].fetch_add(1) != 0) { ++g_cards.overlaps; }
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
        if (open_.exchange(false) && idx_ >= 0) { g_cards.openNow[idx_ & 3].fetch_sub(1); }
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

    int idx_ = -1;
    PushFn push_ = nullptr;
    void* user_ = nullptr;
    int channels_ = 1;
    std::atomic<bool> open_{false};
    std::atomic<bool> feeding_{false};
    std::thread feeder_;
};

std::shared_ptr<SoundCardBackend> makeCard() { return std::make_shared<FakeCard>(); }

SoundCardDevice device(int index, const char* name, const char* api = kApi) {
    SoundCardDevice d;
    d.index = index;
    d.name = name;
    d.hostApi = api;
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
    g_cards.attempts = 0;
    g_cards.dead = false;
    g_cards.holdOpen = false;
    g_cards.refuse = false;
    g_cards.refuseRate = 0.0;
    g_cards.overlaps = 0;
}

// A LINUX MACHINE, as PortAudio's ALSA backend names its inputs: the card
// number N in "(hw:N,M)" is the order ALSA found the cards in at this boot.
const char* const kAlsa = "ALSA";
const char* const kUsbHw1 = "USB Audio CODEC: USB Audio (hw:1,0)";
const char* const kUsbHw2 = "USB Audio CODEC: USB Audio (hw:2,0)";
const char* const kHdaHw0 = "HDA Intel PCH: ALC892 Analog (hw:0,0)";

void setAlsaCards(const char* usbName) {
    std::lock_guard<std::mutex> lk(g_cards.m);
    g_cards.devices = {device(0, kHdaHw0, kAlsa), device(1, usbName, kAlsa)};
}

SoundCardSettings card(const char* name, SoundCardFormat f, double rateHz, double iqCentreHz = 0.0,
                       const char* api = kApi) {
    SoundCardSettings s;
    s.device = name;
    s.hostApi = api;
    s.cardRateHz = rateHz;
    s.format = f;
    s.iqCentreHz = iqCentreHz;
    s.pickedFromList = true;
    return s;
}

// The converter key a card is kept under (the patch page's device key for
// the same card - core::converterRadioKey IS the patch key).
std::string cardKey(const char* name, const char* api = kApi) {
    return cascade::core::converterRadioKey("soundcard", cascade::source::soundCardDeviceArgs(name, api));
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
    // The Source section's controls, edited and not Opened.
    static void setSection(AppWindow& a, const SoundCardSettings& s) { a.soundCard_ = s; }
    static SoundCardSettings section(AppWindow& a) { return a.soundCard_; }
    static SoundCardSettings liveCard(AppWindow& a) { return a.soundCardLive_; }
    static std::string err(AppWindow& a) { return a.sourceError_; }
    static std::string keepKind(AppWindow& a) { return a.restoreKeep_.kind; }
    static std::string keepLabel(AppWindow& a) { return a.restoreKeepLabel_; }
    static double rateNow(AppWindow& a) { return a.pipeline_.activeSource().sampleRateHz(); }
    // The "Receives X to Y." line under the Source section's controls.
    static std::string receives(AppWindow& a) { return a.soundCardReceivesText(); }
    // THE PATCH PAGE takes the receiver's card when the patch starts (one
    // reconcile, as the first frame of a running patch), and hands it back
    // on STOP. The page's first-frame scan is not under test.
    static void startPatch(AppWindow& a) {
        a.patchRunning_ = true;
        a.patchWasOpen_ = true;
        a.patchReconcile();
    }
    static bool lent(AppWindow& a) { return a.patchMainKeep_.valid; }
    static bool stopPatch(AppWindow& a) {
        a.patchRunning_ = false;
        a.patchStopAll(true);
        return settle(a);
    }
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
    // The converter a patch radio with this key is given.
    static ConverterSetting forKey(AppWindow& a, const std::string& key) { return a.converterForKey(key); }
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

// --- F: the third review (4ab1ff0) ----------------------------------------------------------

// The running card goes quiet and says it is dead, as an unplugged one does.
bool killCard(cascade::gui::AppWindow& app) {
    Access::startCard(app);
    g_cards.dead = true;
    const auto t0 = std::chrono::steady_clock::now();
    while (!Access::cardDead(app) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6)) {
        Access::readOnce(app);
    }
    return Access::cardDead(app);
}

// Item 1 (probes P1, P6): THE CONFIG NAMES THE CARD THAT RAN, never the
// Source section's unopened edits - the next launch opens what it saves.
void testConfigNamesTheCardThatRan() {
    std::printf("  the config names the card that ran, never the section's unopened edits\n");
    resetCards();
    setNative({});
    {
        cascade::gui::AppWindow app;
        CHECK(Access::listCards(app));
        CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
        // Card B, I/Q, 96 kHz picked in the section - and Open never pressed.
        Access::setSection(app, card(kCardB, SoundCardFormat::IqStereo, 96000.0, 7.0e6));
        const cascade::core::AppConfig cfg = Access::saved(app);
        std::printf("    running A, section B: cfg names \"%s\" %.0f Hz %s\n", cfg.soundCard.device.c_str(),
                    cfg.soundCard.rateHz, cfg.soundCard.format.c_str());
        CHECK(cfg.sourceKind == "soundcard");
        CHECK(cfg.soundCard.device == kCardA);
        CHECK(cfg.soundCard.rateHz == 48000.0);
        CHECK(cfg.soundCard.format == "real");
        // P6: a same-card re-Open in flight (the card released, the generator
        // standing in): the card as it RAN, not the settings being tried.
        g_cards.holdOpen = true;
        Access::startOpen(app, card(kCardA, SoundCardFormat::RealMono, 96000.0));
        const cascade::core::AppConfig mid = Access::saved(app);
        std::printf("    re-Open in flight: cfg %s, %.0f Hz\n", mid.sourceKind.c_str(), mid.soundCard.rateHz);
        CHECK(mid.sourceKind == "soundcard");
        CHECK(mid.soundCard.device == kCardA);
        CHECK(mid.soundCard.rateHz == 48000.0);
        g_cards.holdOpen = false;
        CHECK(Access::settle(app));
        // Opened: now the new rate is what ran.
        CHECK(Access::saved(app).soundCard.rateHz == 96000.0);
        // The generator chosen deliberately: no card live or remembered, so
        // the section's settings are what the file keeps.
        Access::pick(app, 0);
        Access::setSection(app, card(kCardB, SoundCardFormat::RealMono, 48000.0));
        const cascade::core::AppConfig gen = Access::saved(app);
        CHECK(gen.sourceKind == "siggen");
        CHECK(gen.soundCard.device == kCardB);
    }
    // A startup restore that could not open the saved card keeps naming it,
    // whatever the section is edited to afterwards.
    {
        cascade::gui::AppWindow app;
        CHECK(Access::listCards(app));
        g_cards.refuse = true;
        cascade::core::AppConfig cfg;
        cfg.sourceKind = "soundcard";
        cfg.soundCard.device = kCardA;
        cfg.soundCard.hostApi = kApi;
        cfg.soundCard.rateHz = 48000.0;
        Access::restore(app, cfg);
        CHECK(Access::settle(app));
        CHECK(Access::kind(app) == "siggen");
        Access::setSection(app, card(kCardB, SoundCardFormat::IqStereo, 96000.0, 7.0e6));
        const cascade::core::AppConfig out = Access::saved(app);
        CHECK(out.sourceKind == "soundcard");
        CHECK(out.soundCard.device == kCardA);
        CHECK(out.soundCard.rateHz == 48000.0);
        g_cards.refuse = false;
    }
}

// Item 7 R17: A CARD LENT TO THE PATCH is what the config names - the card
// as it ran, never the section's edits - and neither radio slot is touched.
void testLentCardIsSaved() {
    std::printf("  a card lent to the patch page is saved as it ran; the radio slots keep theirs\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    CHECK(Access::listCards(app));
    CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
    Access::setSection(app, card(kCardB, SoundCardFormat::IqStereo, 96000.0, 7.0e6));
    Access::startPatch(app);
    CHECK(Access::lent(app));
    CHECK(Access::kind(app) == "siggen");
    const cascade::core::AppConfig cfg = Access::saved(app);
    std::printf("    lent: cfg %s \"%s\" %.0f Hz, nativeArgs \"%s\" soapyArgs \"%s\"\n", cfg.sourceKind.c_str(),
                cfg.soundCard.device.c_str(), cfg.soundCard.rateHz, cfg.nativeArgs.c_str(), cfg.soapyArgs.c_str());
    CHECK(cfg.sourceKind == "soundcard");
    CHECK(cfg.soundCard.device == kCardA);
    CHECK(cfg.soundCard.rateHz == 48000.0);
    CHECK(cfg.nativeArgs.empty());
    CHECK(cfg.soapyArgs.empty());
    // STOP: the card comes back as it was lent.
    CHECK(Access::stopPatch(app));
    CHECK(Access::kind(app) == "soundcard");
    CHECK(Access::liveCard(app).device == kCardA);
    CHECK(Access::rateNow(app) == 24000.0);

    // Lent again, and this time the hand-back is refused: the config goes on
    // naming the card as it was lent, not the section's edits.
    Access::setSection(app, card(kCardB, SoundCardFormat::IqStereo, 96000.0, 7.0e6));
    Access::startPatch(app);
    CHECK(Access::lent(app));
    g_cards.refuse = true;
    CHECK(Access::stopPatch(app));
    CHECK(Access::kind(app) == "siggen");
    const cascade::core::AppConfig after = Access::saved(app);
    CHECK(after.sourceKind == "soundcard");
    CHECK(after.soundCard.device == kCardA);
    CHECK(after.soundCard.rateHz == 48000.0);
    g_cards.refuse = false;
}

// Item 3: "Receives X to Y." is the AIR range, through the card's converter.
void testReceivesLineThroughTheConverter() {
    std::printf("  the Receives line is the air range, through the section card's converter\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    CHECK(Access::listCards(app));
    CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
    std::printf("    no converter: \"%s\"\n", Access::receives(app).c_str());
    CHECK(Access::receives(app) == "Receives 0 kHz to 24 kHz.");
    Access::changeConverter(app, down(10.0e6));
    std::printf("    down 10 MHz: \"%s\" (counter %.0f Hz)\n", Access::receives(app).c_str(), Access::counter(app));
    CHECK(Access::receives(app) == "Receives 10.0000 MHz to 10.0240 MHz.");
    // Inverted: air = LO - radio, so the edges swap.
    Access::changeConverter(app, {ConverterMode::Down, 10.0e6, true});
    CHECK(Access::receives(app) == "Receives 9.9760 MHz to 10.0000 MHz.");
    Access::changeConverter(app, down(10.0e6));
    // The section set up for card B (not Opened): B's own converter.
    Access::setConverter(app, cardKey(kCardB), down(3.0e6));
    Access::setSection(app, card(kCardB, SoundCardFormat::RealMono, 48000.0));
    CHECK(Access::receives(app) == "Receives 3.0000 MHz to 3.0240 MHz.");
    // The section set up for I/Q on A: an I/Q card takes no converter; the
    // typed centre is the air.
    Access::setSection(app, card(kCardA, SoundCardFormat::IqStereo, 96000.0, 7.1e6));
    CHECK(Access::receives(app) == "Receives 7.0520 MHz to 7.1480 MHz.");
}

// Item 4: a re-Open whose settings are what was running - a dead card picked
// again, or Open pressed with nothing changed - is tried ONCE, and a refusal
// is said as a plain "did not open".
void testSameSettingsTriedOnce() {
    std::printf("  a re-Open with the running settings is tried once and said plainly\n");
    const std::string expect =
        std::string(kCardA) + " (" + kApi + ") did not open (\"" + kCardA + "\": fake refused).";
    // Open pressed with the running settings.
    {
        resetCards();
        setNative({});
        cascade::gui::AppWindow app;
        CHECK(Access::listCards(app));
        CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
        const int before = g_cards.attempts.load();
        g_cards.refuse = true;
        CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
        std::printf("    same settings: %d attempt(s), \"%s\"\n", g_cards.attempts.load() - before,
                    Access::err(app).c_str());
        CHECK(g_cards.attempts.load() == before + 1);
        CHECK(Access::err(app) == expect);
        CHECK(Access::kind(app) == "siggen");
        CHECK(Access::lamp(app));
        CHECK(Access::saved(app).sourceKind == "soundcard");
        g_cards.refuse = false;
    }
    // The dead card's row picked again.
    {
        resetCards();
        setNative({});
        cascade::gui::AppWindow app;
        CHECK(Access::listCards(app));
        CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
        CHECK(killCard(app));
        g_cards.refuse = true;
        const int before = g_cards.attempts.load();
        Access::pick(app, Access::kSoundCardRow);
        CHECK(Access::settle(app));
        std::printf("    dead re-pick: %d attempt(s), \"%s\"\n", g_cards.attempts.load() - before,
                    Access::err(app).c_str());
        CHECK(g_cards.attempts.load() == before + 1);
        CHECK(Access::err(app) == expect);
        g_cards.refuse = false;
        g_cards.dead = false;
    }
    // New settings still get the old ones to fall back on (two attempts).
    {
        resetCards();
        setNative({});
        cascade::gui::AppWindow app;
        CHECK(Access::listCards(app));
        CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
        g_cards.refuse = true;
        const int before = g_cards.attempts.load();
        CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 96000.0)));
        CHECK(g_cards.attempts.load() == before + 2);
        CHECK(Access::err(app).find("and reopening it as it was failed too") != std::string::npos);
        g_cards.refuse = false;
    }
}

// Item 5: ON LINUX a card's converter, and a patch radio's match to the
// section, survive ALSA numbering the card differently at the next boot.
void testAlsaConverterSurvivesRenumbering() {
    std::printf("  an ALSA card's converter survives the card being renumbered at the next boot\n");
    resetCards();
    setNative({});
    setAlsaCards(kUsbHw1);
    cascade::core::AppConfig saved;
    {
        cascade::gui::AppWindow app;
        CHECK(Access::listCards(app));
        CHECK(Access::open(app, card(kUsbHw1, SoundCardFormat::RealMono, 48000.0, 0.0, kAlsa)));
        Access::changeConverter(app, down(10.0e6));
        CHECK(Access::live(app) == down(10.0e6));
        saved = Access::saved(app);
    }
    // The next boot: the same card is hw:2,0.
    setAlsaCards(kUsbHw2);
    {
        cascade::gui::AppWindow app;
        Access::restore(app, saved);
        CHECK(Access::settle(app));
        CHECK(Access::kind(app) == "soundcard");
        std::printf("    renumbered: live \"%s\", converter %s\n", Access::liveCard(app).device.c_str(),
                    cascade::core::converterActive(Access::live(app)) ? "applied" : "LOST");
        CHECK(Access::liveCard(app).device == kUsbHw2);
        CHECK(Access::live(app) == down(10.0e6));
        CHECK(Access::airCentre(app) == 10.012e6);
        // A patch radio names the card by its full name - this boot's, or
        // the one it was saved under last boot: the same converter.
        CHECK(Access::forKey(app, cardKey(kUsbHw2, kAlsa)) == down(10.0e6));
        CHECK(Access::forKey(app, cardKey(kUsbHw1, kAlsa)) == down(10.0e6));
        // ...and another PCM device on the card is another input.
        CHECK(!cascade::core::converterActive(
            Access::forKey(app, cardKey("USB Audio CODEC: USB Audio (hw:2,1)", kAlsa))));
    }
    // A config written before this rule, keyed by the full name: migrated.
    {
        cascade::core::AppConfig old = saved;
        old.converters.clear();
        old.converters[cardKey(kUsbHw1, kAlsa)] = down(7.0e6);
        cascade::gui::AppWindow app;
        Access::restore(app, old);
        CHECK(Access::settle(app));
        CHECK(Access::kind(app) == "soundcard");
        CHECK(Access::live(app) == down(7.0e6));
        CHECK(Access::stored(app, Access::keyNow(app)) == down(7.0e6));
    }
    resetCards();
}

// Item 6: ON LINUX a dead card is not reopened by its index - ALSA may have
// given that index to another card plugged in since - but refused with the
// restart sentence. (The seam is the host API, so this runs everywhere;
// testPickingADeadCardReopensIt is the Windows half, unchanged.)
void testAlsaDeadCardAsksForARestart() {
    std::printf("  a dead ALSA card is refused with the restart sentence, never reopened by index\n");
    resetCards();
    setNative({});
    setAlsaCards(kUsbHw1);
    cascade::gui::AppWindow app;
    CHECK(Access::listCards(app));
    CHECK(Access::open(app, card(kUsbHw1, SoundCardFormat::RealMono, 48000.0, 0.0, kAlsa)));
    // A HEALTHY ALSA card re-Opened with new settings is simply reopened.
    CHECK(Access::open(app, card(kUsbHw1, SoundCardFormat::RealMono, 96000.0, 0.0, kAlsa)));
    CHECK(Access::kind(app) == "soundcard");
    CHECK(Access::rateNow(app) == 48000.0);
    CHECK(Access::err(app).empty());
    CHECK(Access::open(app, card(kUsbHw1, SoundCardFormat::RealMono, 48000.0, 0.0, kAlsa)));
    CHECK(Access::rateNow(app) == 24000.0);
    CHECK(killCard(app));
    g_cards.dead = false;  // "a card" answers again at that index
    const int before = g_cards.attempts.load();
    Access::pick(app, Access::kSoundCardRow);
    CHECK(Access::settle(app));
    std::printf("    re-pick: %d attempt(s), \"%s\"\n", g_cards.attempts.load() - before, Access::err(app).c_str());
    CHECK(g_cards.attempts.load() == before);
    CHECK(Access::err(app).find("restart FoxSDR") != std::string::npos);
    CHECK(Access::err(app).find(kUsbHw1) != std::string::npos);
    // Open with new settings on the same dead card: refused the same way.
    Access::setSection(app, card(kUsbHw1, SoundCardFormat::RealMono, 96000.0, 0.0, kAlsa));
    CHECK(Access::open(app, Access::section(app)));
    CHECK(g_cards.attempts.load() == before);
    CHECK(Access::err(app).find("restart FoxSDR") != std::string::npos);
    // The config goes on naming the card.
    CHECK(Access::saved(app).sourceKind == "soundcard");
    CHECK(Access::saved(app).soundCard.device == kUsbHw1);
    resetCards();
}

// Item 7 R3 (probe P2): released, and NEITHER the new settings nor the old
// ones open. The section shows what ran; the config names it; the combo says
// so; a later Open with the card back simply opens it.
void testReleasedAndBothRefused() {
    std::printf("  a released card whose new and old settings are both refused\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    CHECK(Access::listCards(app));
    CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
    g_cards.refuse = true;
    Access::startOpen(app, card(kCardA, SoundCardFormat::RealMono, 96000.0));
    CHECK(Access::kind(app) == "siggen");
    CHECK(Access::settle(app));
    const cascade::core::AppConfig cfg = Access::saved(app);
    CHECK(Access::kind(app) == "siggen");
    CHECK(Access::lamp(app));
    CHECK(Access::section(app).cardRateHz == 48000.0);
    CHECK(Access::section(app).device == kCardA);
    CHECK(!Access::keepLabel(app).empty());
    CHECK(Access::err(app).find("and reopening it as it was failed too") != std::string::npos);
    CHECK(cfg.sourceKind == "soundcard");
    CHECK(cfg.soundCard.device == kCardA);
    CHECK(cfg.soundCard.rateHz == 48000.0);
    CHECK(g_cards.overlaps.load() == 0);
    g_cards.refuse = false;
    CHECK(Access::open(app, Access::section(app)));
    CHECK(Access::kind(app) == "soundcard");
    CHECK(!Access::lamp(app));
    CHECK(Access::keepKind(app).empty());
}

// Item 7 R4, R5 (probe P5): the generator picked while a released card
// reopens. Whether the reopen then succeeds or fails, the choice stands: no
// card is installed, and nothing is said about a card nobody is waiting for.
void testGeneratorPickedDuringRelease() {
    std::printf("  the generator picked while a released card reopens: the choice stands\n");
    for (const bool refused : {false, true}) {
        resetCards();
        setNative({});
        cascade::gui::AppWindow app;
        CHECK(Access::listCards(app));
        CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
        g_cards.holdOpen = true;
        g_cards.refuse = refused;
        Access::startOpen(app, card(kCardA, SoundCardFormat::RealMono, 96000.0));
        Access::pick(app, 0);
        g_cards.holdOpen = false;
        CHECK(Access::settle(app));
        const cascade::core::AppConfig cfg = Access::saved(app);
        std::printf("    reopen %s: kind %s, sel %d, err \"%s\", label \"%s\", cfg %s\n",
                    refused ? "refused" : "opened", Access::kind(app).c_str(), Access::sel(app),
                    Access::err(app).c_str(), Access::keepLabel(app).c_str(), cfg.sourceKind.c_str());
        CHECK(Access::kind(app) == "siggen");
        CHECK(Access::sel(app) == 0);
        CHECK(cfg.sourceKind == "siggen");
        CHECK(Access::err(app).empty());
        CHECK(Access::keepLabel(app).empty());
        CHECK(!Access::lamp(app));
        CHECK(g_cards.openNow[0].load() == 0);  // the card that opened late was closed again
        g_cards.refuse = false;
    }
}

// Item 7 R23: another card's Open refused while card A runs - the section
// goes back to A, which is what is running.
void testFailedOtherCardKeepsTheSection() {
    std::printf("  a refused Open of another card puts the section back on the running one\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    CHECK(Access::listCards(app));
    CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
    g_cards.refuse = true;
    CHECK(Access::open(app, card(kCardB, SoundCardFormat::IqStereo, 96000.0, 7.0e6)));
    CHECK(Access::kind(app) == "soundcard");
    CHECK(Access::liveCard(app).device == kCardA);
    CHECK(Access::section(app).device == kCardA);
    CHECK(Access::section(app).cardRateHz == 48000.0);
    CHECK(Access::section(app).format == SoundCardFormat::RealMono);
    CHECK(!Access::err(app).empty());
    g_cards.refuse = false;
}

// Item 7 R16: the converter belongs to the RUNNING card, whatever the
// section has been edited to.
void testConverterKeyFollowsTheRunningCard() {
    std::printf("  the converter key is the running card's, not the section's\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    CHECK(Access::listCards(app));
    CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
    Access::setSection(app, card(kCardB, SoundCardFormat::RealMono, 48000.0));
    CHECK(Access::keyNow(app) == cardKey(kCardA));
    Access::changeConverter(app, down(10.0e6));
    CHECK(Access::stored(app, cardKey(kCardA)) == down(10.0e6));
    CHECK(!cascade::core::converterActive(Access::stored(app, cardKey(kCardB))));
    CHECK(Access::live(app) == down(10.0e6));
}

// Item 7 R18: a dead card picked again reopens AS IT RAN, not as the section
// has been edited meanwhile.
void testDeadRepickIgnoresSectionEdits() {
    std::printf("  a dead card picked again reopens as it ran, not as the section was edited\n");
    resetCards();
    setNative({});
    cascade::gui::AppWindow app;
    CHECK(Access::listCards(app));
    CHECK(Access::open(app, card(kCardA, SoundCardFormat::RealMono, 48000.0)));
    Access::setSection(app, card(kCardB, SoundCardFormat::IqStereo, 96000.0, 7.0e6));
    CHECK(killCard(app));
    g_cards.dead = false;
    Access::pick(app, Access::kSoundCardRow);
    CHECK(Access::settle(app));
    CHECK(Access::kind(app) == "soundcard");
    CHECK(Access::liveCard(app).device == kCardA);
    CHECK(Access::liveCard(app).format == SoundCardFormat::RealMono);
    CHECK(Access::rateNow(app) == 24000.0);
    CHECK(Access::section(app).device == kCardA);
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
    // The third review (4ab1ff0).
    testConfigNamesTheCardThatRan();
    testLentCardIsSaved();
    testReceivesLineThroughTheConverter();
    testSameSettingsTriedOnce();
    testAlsaConverterSurvivesRenumbering();
    testAlsaDeadCardAsksForARestart();
    testReleasedAndBothRefused();
    testGeneratorPickedDuringRelease();
    testFailedOtherCardKeepsTheSection();
    testConverterKeyFollowsTheRunningCard();
    testDeadRepickIgnoresSectionEdits();

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_soundcard_app_paths");
}
