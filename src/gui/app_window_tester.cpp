// app_window_tester.cpp - THE BETA TESTER'S TWO FLOWS, AS THE WINDOW'S: the usage record a tester opts into by pasting a code, and the link from the tester portal
// (SYSTEM > Beta tester, the "Link this FoxSDR?" prompt, the one-shot link-request file the process boot writes). AppWindow members, kept out of app_window.cpp because
// they are one subject (and because that file is at the object-file limit).
// Restored in stub campaign B15 (TESTER-USAGE 0.99.44, TESTER-LINK 0.99.45 / 0.99.52), from master's text of 0.99.90 - the functions are unchanged to 0.99.95. The record
// itself is core/tester_usage.* and the network calls core/tester_link.*, both in tree and tested alone; this file is the window half they never had here.
//
// WHERE THE RECORDER LIVES, AND WHY NOT THE ENGINE (the owner's decision O8, the stage-7 survey section 8, docs/engine-stage4.md 6.22 B15): a headless engine must never be
// able to send a tester report, so every part that holds a code, queues a report or sends one is HERE - in host glue, which the foxsdr-engine binary does not build. What the
// recorder notes comes from two places: the window's own events (a control pressed, a page opened: the noteFeature calls beside each of them, as in master) and facts the table
// already publishes (testerNoteFacts: a take in progress, the scanner running, RF going out, the kind of radio in use). Two marks only the listeners' own threads can make -
// "a browser was served", "a CAT client connected" - come through installTesterUsageHooks, which hands the window's closures to the engine's servers by the one door
// (EngineShell::setServerUsageHooks). Nothing in src/engine or src/net names the recorder, and tests/test_tester_host_only.cpp holds that, and holds that a foxsdr-engine
// carrying a code in its settings sends nothing.
//
// THE PROCESS BOOT IS NOT THIS FILE'S (engine/process_boot.cpp, STEP 5): the foxsdr:// activation, the instance claim and the link-request file's WRITE are the host
// process's, once, before the window exists; this file only polls that file (on a worker: gui/link_request_poll.hpp) and puts the question to the tester.
//
// THE TOKEN IS A CREDENTIAL: it is never a log argument, a diagnostics line or a census name in full (the masked form only); tests/test_tester_usage.cpp scans this file for it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/patch_devices.hpp"
#include "core/telemetry.hpp"
#include "core/version.hpp"
#include "gui/bench_rail.hpp"
#include "gui/text_fit.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

namespace {

// THE ONE RED (app_window.cpp's kErrorRed, which this file cannot see): the theme's, read when it is used.
struct ErrorRed {
    operator ImVec4() const { return cascade::gui::theme::bad(); }
};
constexpr ErrorRed kErrorRed{};

// FOR THE UI CENSUS ONLY (a no-op unless FOXSDR_UI_CENSUS names a file): the rectangle of the item just drawn, so a test aims its press.
void censusRect(const char* name) {
    if (!cascade::gui::census::enabled()) { return; }
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    cascade::gui::census::rect(name, a.x, a.y, b.x, b.y);
}

// ...and the frame a line was LAST drawn on: a census item is "ever seen", which cannot say that a line stopped; a rectangle keeps its newest value, so the frame number does.
void censusFrame(const char* name) {
    if (!cascade::gui::census::enabled()) { return; }
    cascade::gui::census::rect(name, static_cast<float>(ImGui::GetFrameCount()), 0.0f, 0.0f, 0.0f);
}

// A tester's sign-up name is free text on the site (sanitized on arrival -
// core::sanitizeTesterName - but never REQUIRED to be non-empty there), and
// every place this application shows one formats it into a name-shaped slot
// ("Linked to %s", "Link this FoxSDR to beta tester %s?"). An empty name
// substituted in there reads as a sentence missing a word ("Linked to ."); a
// placeholder that reads like a name slot itself does not.
std::string testerNameOrPlaceholder(const std::string& name) {
    return name.empty() ? tr("(no name set)") : name;
}
}  // namespace

// --- "Beta tester": a TESTER's own opt-in, tied to their own entry on the -----
// foxsdr.com beta portal - see core/tester_usage.hpp for the design and
// PRIVACY.md for the payload field by field.
//
// INDEPENDENT OF THE SWITCH ABOVE. Anonymous usage reporting counts an
// install and never identifies anyone; this counts a TESTER, by the
// credential their own portal link carries, because the owner cannot
// otherwise tell which of the features a tester said they would cover they
// actually ran. PASTING A CODE IS THE OPT-IN - with none, nothing below this
// point collects or sends anything, ever (testerUsage_->armed() is exactly
// that condition, checked at every hook rather than here).
void AppWindow::drawTesterUsageSection() {
    // VERIFICATION ONLY, the same house rule as FOXSDR_OPEN_SERIAL_PORTS and
    // FOXSDR_OPEN_KEY_BINDINGS above: this row's open/closed state is ImGui's
    // own in-memory storage, not AppConfig, so nothing a bounded self-capture
    // supplies would put it on screen otherwise. ImGuiCond_Once, so a real
    // session that closes the row again is not fought every frame.
    if (std::getenv("FOXSDR_OPEN_BETA_TESTER") != nullptr) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    }
    // THE PREVIEW'S OWN SEAM, same rule: its open/closed state is a plain
    // bool with no config field to seed instead, so a bounded self-capture
    // needs a way to see it without scripting a click. Once, on the first
    // frame that sees the variable - a real session toggling the button
    // afterwards is not fought.
    if (!testerPreviewOpenedByEnv_ &&
        std::getenv("FOXSDR_OPEN_BETA_TESTER_PREVIEW") != nullptr) {
        testerPreviewOpenedByEnv_ = true;
        testerShowPreview_ = true;
    }
    const bool active = !testerToken_.empty() || !testerAppToken_.empty();
    const char* chip = active ? (testerTokenInvalid_ ? tr("INVALID") : tr("ON")) : tr("OFF");
    const ImU32 lamp = testerTokenInvalid_
                           ? ImGui::ColorConvertFloat4ToU32(cascade::gui::theme::bad())
                           : cascade::gui::theme::kPhosphor;
    cascade::gui::census::note("tester:chip:", chip);
    censusFrame("tester:section-frame");
    if (!railSection(trId("Beta tester"), false, chip, lamp, active)) { return; }
    uiNotePanel("beta tester");
    cascade::gui::census::note("tester:open");

    ImGui::TextWrapped(
        "%s",
        tr("Paste the code from your tester link (or the whole link) to report, "
           "per session, which features, plugins and radios you actually used - "
           "linked to your own entry on the tester list so it can be marked as "
           "tested. Pasting a code IS the opt-in; with none, nothing here is "
           "collected or sent."));
    ImGui::Spacing();

    if (!testerAppToken_.empty()) {
        // A CONFIRMED LINK (core/tester_link.hpp) - the paste box below is
        // for the older portal-token flow and for a fresh --link-tester
        // paste, neither of which applies once a link has been confirmed by
        // name.
        ImGui::TextDisabled(tr("Linked to %s"),
                           testerNameOrPlaceholder(testerAppTokenName_).c_str());
        cascade::gui::census::note("tester:linked:", testerNameOrPlaceholder(testerAppTokenName_));
        censusFrame("tester:linked-frame");
        const bool unlink = ImGui::SmallButton(trId("Unlink"));
        censusRect("tester:unlink");
        if (unlink) { clearTesterAppToken(); }
    } else if (!testerToken_.empty()) {
        const std::string codeShown = cascade::core::maskTesterToken(testerToken_);
        ImGui::TextDisabled(tr("Code: %s"), codeShown.c_str());
        // THE SAME STRING IN THE CENSUS AS ON THE SCREEN, and masked: a name that carried the whole credential would be a second place it could be read.
        cascade::gui::census::note("tester:code:", codeShown);
        censusFrame("tester:code-frame");
        const bool removeCode = ImGui::SmallButton(trId("Remove code"));
        censusRect("tester:remove");
        if (removeCode) {
            setTesterToken(std::string(), /*startFreshSession=*/false);
            std::memset(testerCodeBuf_, 0, sizeof(testerCodeBuf_));
            testerCodeError_.clear();
        }
        if (testerTokenInvalid_) {
            cascade::gui::census::note("tester:invalid");
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
            ImGui::TextWrapped(
                "%s",
                tr("This code is no longer valid, so sending has stopped. The code "
                   "above is kept so you can fix it - paste a current one below."));
            ImGui::PopStyleColor();
        }
    }
    if (testerAppToken_.empty() && (testerToken_.empty() || testerTokenInvalid_)) {
        // MASKED WHILE TYPING, like the web server's own password field: this
        // is the same credential the Code line above shows only four-and-four,
        // and a field showing it in full while it is entered would be the one
        // place on this screen that still could.
        ImGui::InputText(cascade::gui::labelAboveIfNeeded(trId("Tester code")), testerCodeBuf_,
                         sizeof(testerCodeBuf_), ImGuiInputTextFlags_Password);
        censusRect("tester:field");
        censusFrame("tester:field-frame");
        const bool useCode = ImGui::Button(trId("Use this code"));
        censusRect("tester:use");
        if (useCode) {
            // AN APP TOKEN (40 hex, from the portal's "Link FoxSDR" button,
            // typed here via --link-tester) is never stored directly - it
            // goes through confirm-by-name first, exactly like the protocol
            // activation path (PORTAL-LINK-VERDICT.md finding 1). A PORTAL
            // token (32 hex, or the whole manage link) keeps the existing
            // immediate opt-in this box has always had.
            const std::string extractedApp = cascade::core::extractAppToken(testerCodeBuf_);
            const std::string extracted = cascade::core::extractTesterToken(testerCodeBuf_);
            if (!extractedApp.empty()) {
                testerCodeError_.clear();
                testerLinkResolvingToken_ = extractedApp;
                testerLinkNameSender_.send(cascade::core::betaApiBaseUrl(), extractedApp);
                std::memset(testerCodeBuf_, 0, sizeof(testerCodeBuf_));
            } else if (!extracted.empty()) {
                testerCodeError_.clear();
                setTesterToken(extracted, /*startFreshSession=*/testerToken_.empty());
                std::memset(testerCodeBuf_, 0, sizeof(testerCodeBuf_));
            } else {
                testerCodeError_ = tr("that does not look like a tester code or link");
            }
        }
        if (!testerCodeError_.empty()) {
            cascade::gui::census::note("tester:error:", testerCodeError_);
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
            ImGui::TextWrapped("%s", testerCodeError_.c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::Spacing();
    ImGui::TextWrapped(
        "%s",
        tr("What is sent: the app version, platform and architecture, when a "
           "session started and how long it ran, which of the features above "
           "were used, which plugins ran and for how long, and which radio "
           "kinds were opened. Never a frequency, never anything decoded, "
           "never your location, and no IP address is recorded."));

    const bool previewKey = ImGui::SmallButton(testerShowPreview_ ? trId("Hide what is sent")
                                                                  : trId("Show what is sent"));
    censusRect("tester:preview-key");
    if (previewKey) { testerShowPreview_ = !testerShowPreview_; }
    if (testerShowPreview_) {
        uiNotePanel("beta tester preview");
        cascade::gui::census::note("tester:preview");
        ImGui::Indent();
        if (activeTesterToken().empty()) {
            ImGui::TextDisabled("%s", tr("Nothing - no code is set."));
            cascade::gui::census::note("tester:preview-nothing");
        } else {
            const double now = wallTimeS();
            const double sessionSeconds =
                now > testerSessionStart_ ? now - testerSessionStart_ : 0.0;
            // MASKED, like every other on-screen appearance of the token -
            // this is what "show what is sent" means, not a second place the
            // full credential can be read off the screen.
            const std::string json =
                cascade::core::maskedPreviewJson(buildTesterUsageReport(sessionSeconds));
            cascade::gui::census::note("tester:json:", json);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(json.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            if (!testerUsageQueue_.empty()) {
                ImGui::TextDisabled(tr("%d report(s) queued to retry"),
                                    static_cast<int>(testerUsageQueue_.size()));
                cascade::gui::census::note("tester:queued:", std::to_string(testerUsageQueue_.size()));
            }
        }
        ImGui::Unindent();
    }
}

// --- Beta tester usage --------------------------------------------------------

// THE TWO LISTENERS' MARKS (the window's closures on the engine's servers, by the one door). Called once, from the constructor, before the settings file can start
// a listener. The closures hold the recorder itself, not this window: a server's last request can come after the window's members are gone.
void AppWindow::installTesterUsageHooks() {
    const std::shared_ptr<cascade::core::TesterUsageRecorder> recorder = testerUsage_;
    const bool installed = engine_.setServerUsageHooks([recorder]() { recorder->noteFeature("browser"); },
                                                       [recorder]() { recorder->noteFeature("cat"); });
    if (!installed) {
        // Said once, in the log, without a word of what any of it would have recorded.
        cascade::core::diagWarnf("tester usage: a listener was already running, so browser and CAT use is not recorded this session");
    }
}

// THE MARKS THE TABLE SAYS (once a frame, after the state is read). Master noted each of these where the engine did the work - the recorder's start, the scanner's start,
// the transmitter's key, a device's open - and the engine has no recorder here, so the window reads what the engine already publishes: the state's flags, and the DEVICES
// list's entry that is the source in use. A no-op while no code is set, like every other mark.
//
//   rec-iq, rec-audio   a take is in progress (FOXAPI_RX_RECORDING_*)
//   scanner             the scanner is running (FOXAPI_RX_SCANNER_ACTIVE) - from the panel, the browser or a script alike
//   transmit            RF is going out now (FOXAPI_RX_TX_KEYED): "actually keyed", as master's comment says, whoever's key it is (the hand's, a plugin's, the browser's)
//   siggen              the built-in generator is the source in use and the receiver is running
//   <kind>, native-drivers   a radio is open: its kind (the part of the DEVICES id before the first colon - "rtlsdr", "soapy", "hackrf"...), and "native-drivers" when it
//                       is one of this product's own drivers (not SoapySDR, not the sound card); the IQ file is not a radio and earns nothing
void AppWindow::testerNoteFacts() {
    // Disarmed, the radio kind is forgotten, so the first armed frame reads it afresh (a code pasted mid-session counts the radio in use).
    if (!testerUsage_->armed()) {
        testerFactsSeq_ = ~std::uint64_t{0};
        return;
    }
    const std::uint32_t flags = uiState_.flags;
    if ((flags & FOXAPI_RX_RECORDING_IQ) != 0u) { testerUsage_->noteFeature("rec-iq"); }
    if ((flags & FOXAPI_RX_RECORDING_AUDIO) != 0u) { testerUsage_->noteFeature("rec-audio"); }
    if ((flags & FOXAPI_RX_SCANNER_ACTIVE) != 0u) { testerUsage_->noteFeature("scanner"); }
    if ((flags & FOXAPI_RX_TX_KEYED) != 0u) { testerUsage_->noteFeature("transmit"); }

    // THE SOURCE IN USE changes with the list (a fresh pass of DEVICES), so it is read when listSeq moves and kept; the flags are read every frame above.
    if (uiSession_ != nullptr && testerFactsSeq_ != uiState_.listSeq) {
        testerFactsSeq_ = uiState_.listSeq;
        testerSourceKind_.clear();
        const int32_t rows = std::max<int32_t>(0, engineApi_->read_list(uiSession_, FOXAPI_LIST_DEVICES, 0, nullptr));
        for (int32_t i = 0; i < rows; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            if (engineApi_->read_list(uiSession_, FOXAPI_LIST_DEVICES, static_cast<uint32_t>(i), &it) <= i) { break; }
            if ((it.flags & FOXAPI_ITEM_ACTIVE) == 0u) { continue; }
            // The kind of a row is the part of its id before the first ':' (the id is cut at 159 bytes; the kind is in the first ones). The generator's is "siggen" with no
            // colon, the IQ file's "row:iqfile" and the sound card's "row:soundcard" (API 0.3 8.10).
            const char* colon = std::strchr(it.detail, ':');
            std::string kind(it.detail, colon != nullptr ? static_cast<std::size_t>(colon - it.detail) : std::strlen(it.detail));
            if (kind == "row") { kind = colon != nullptr && std::strcmp(colon + 1, "soundcard") == 0 ? "soundcard" : "file"; }
            testerSourceKind_ = std::move(kind);
            break;
        }
    }
    if (testerSourceKind_ == "siggen") {
        if ((flags & FOXAPI_RX_RUNNING) != 0u) { testerUsage_->noteFeature("siggen"); }
    } else if (!testerSourceKind_.empty() && testerSourceKind_ != "file" && (flags & FOXAPI_RX_DEVICE_OPEN) != 0u) {
        testerNoteRadio(testerSourceKind_);
    }
    // THE PATCH'S RADIOS count the same way (master noted a patch radio where the engine opened it): each Radio node of the running graph whose radio is open, by the
    // driver half of its device key. A recording and the generator are not radios.
    if ((flags & FOXAPI_RX_PATCH_RUNNING) != 0u) {
        for (const cascade::core::patch::Node& node : patchDraft_.nodes()) {
            if (node.kind != cascade::core::patch::NodeKind::Radio) { continue; }
            const PatchListCache::Radio* radio = patchRadio(node.id);
            if (radio == nullptr || !radio->open) { continue; }
            const std::string driver = cascade::core::patch::deviceDriver(node.device);
            if (driver.empty() || driver == "iqfile" || driver == "file" || driver == "siggen") { continue; }
            testerNoteRadio(driver);
        }
    }
}

// One radio kind in use: the kind itself, and "native-drivers" when it is one of this product's own drivers (not SoapySDR, not the sound card).
void AppWindow::testerNoteRadio(const std::string& kind) {
    testerUsage_->noteRadio(kind);
    if (kind != "soapy" && kind != "soundcard") { testerUsage_->noteFeature("native-drivers"); }
}

// The module files being fed now: the receiver's decoders (DECODERS, joined to the module by name as the rail's cache joins them) and, while the patch runs, the
// decoder nodes it is running (PATCH_PLAN: a decoder entry's name is its module's file name). Master read the runner and the patch's graph for the same two sets.
void AppWindow::testerFedPlugins(std::vector<std::string>& files, bool& patchDecoder) {
    files.clear();
    patchDecoder = false;
    if (uiSession_ == nullptr) { return; }
    const auto count = [this](uint32_t list) { return std::max<int32_t>(0, engineApi_->read_list(uiSession_, list, 0, nullptr)); };
    const int32_t decoders = count(FOXAPI_LIST_DECODERS);
    std::vector<std::string> fedNames;
    for (int32_t i = 0; i < decoders; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (engineApi_->read_list(uiSession_, FOXAPI_LIST_DECODERS, static_cast<uint32_t>(i), &it) <= i) { break; }
        if ((it.flags & FOXAPI_ITEM_FED) != 0u) { fedNames.emplace_back(it.name); }
    }
    if (!fedNames.empty()) {
        const int32_t plugins = count(FOXAPI_LIST_PLUGINS);
        for (int32_t i = 0; i < plugins; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            if (engineApi_->read_list(uiSession_, FOXAPI_LIST_PLUGINS, static_cast<uint32_t>(i), &it) <= i) { break; }
            if ((it.flags & FOXAPI_ITEM_ACTIVE) == 0u) { continue; }
            if (std::find(fedNames.begin(), fedNames.end(), std::string(it.name)) == fedNames.end()) { continue; }
            if (std::find(files.begin(), files.end(), std::string(it.detail)) == files.end()) { files.emplace_back(it.detail); }
        }
    }
    if ((uiState_.flags & FOXAPI_RX_PATCH_RUNNING) != 0u) {
        const int32_t plan = count(FOXAPI_LIST_PATCH_PLAN);
        for (int32_t i = 0; i < plan; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            if (engineApi_->read_list(uiSession_, FOXAPI_LIST_PATCH_PLAN, static_cast<uint32_t>(i), &it) <= i) { break; }
            if (std::strncmp(it.detail, "decoder", 7) != 0 || it.name[0] == '\0') { continue; }
            patchDecoder = true;
            if (std::find(files.begin(), files.end(), std::string(it.name)) == files.end()) { files.emplace_back(it.name); }
        }
    }
}

void AppWindow::testerUsageAccrue() {
    // Same shape as telemetryAccrueMode: seconds banked since the last call
    // are credited to whatever is running NOW, which is the same
    // approximation that function's own comment accepts - crediting a whole
    // span to its end state rather than tracking every plugin's exact
    // start/stop is what keeps this a once-a-save accrual instead of a
    // per-frame one.
    const std::uint64_t secs = testerUsageAccrual_.advance(wallTimeS());
    if (secs == 0 || !testerUsage_->armed()) { return; }
    const double secsD = static_cast<double>(secs);
    std::vector<std::string> files;
    bool patchDecoder = false;
    testerFedPlugins(files, patchDecoder);
    std::string text;
    for (const std::string& file : files) {
        // THE CATALOGUE ID, from the install record that names the file (fact.plugin.<file>.record: the key, "<id>@<site label>" for a plugin from a site), and the
        // version the descriptor declared. SIDELOADED PLUGINS SHARE ONE BUCKET WITH NO VERSION: a file no install record names must not be reported
        // alongside a version number either - a version means something only paired with a real catalogue id (see kSideloadedPluginId's own comment).
        readFactText(("fact.plugin." + file + ".record").c_str(), text);
        cascade::gui::PluginRecordFact record;
        cascade::gui::parsePluginRecord(text, record);
        std::string id = record.key.substr(0, record.key.find('@'));
        std::string version;
        if (id.empty()) {
            id = cascade::core::kSideloadedPluginId;
        } else {
            readFactText(("fact.plugin." + file + ".version").c_str(), version);
        }
        testerUsage_->accruePlugin(id, version, secsD);
    }
    // THE PATCH'S OWN DECODER NODES count as "plugins" too, as master noted for each running one.
    if (patchDecoder) { testerUsage_->noteFeature("plugins"); }
}

cascade::core::TesterUsageReport AppWindow::buildTesterUsageReport(double sessionSeconds) const {
    cascade::core::TesterUsageReport r;
    // THE APP TOKEN WINS when held - the site accepts either during the
    // migration transition (PORTAL-LINK-VERDICT.md §7), but once a link has
    // been confirmed by name it is the identity that should be credited.
    r.token = activeTesterToken();
    r.version = cascade::versionString();
    r.platform = cascade::core::testerUsagePlatform();
    r.arch = cascade::core::archDescription();
    r.session.start = cascade::core::rfc3339Utc(testerSessionStartWall_);
    const long long mins =
        std::llround(sessionSeconds > 0.0 ? sessionSeconds / 60.0 : 0.0);
    r.session.minutes =
        static_cast<std::uint64_t>(std::clamp<long long>(mins, 0, 1440));
    r.session.features = testerUsage_->features();
    r.session.plugins = testerUsage_->plugins();
    r.session.radios = testerUsage_->radios();
    // THE SITE'S OWN BOUNDS, applied before this ever reaches toJson() -
    // see finalizeTesterUsageReport's own comment for what each one is and
    // why the app enforces them itself rather than trusting the site to
    // reject cleanly.
    return cascade::core::finalizeTesterUsageReport(std::move(r), std::time(nullptr));
}

void AppWindow::testerUsageJournal(cascade::core::AppConfig& cfg) {
    cfg.testerToken = testerToken_;
    cfg.testerTokenInvalid = testerTokenInvalid_;
    cfg.testerAppToken = testerAppToken_;
    cfg.testerAppTokenName = testerAppTokenName_;
    cfg.testerUsagePending = testerUsageQueue_.items();
    cfg.testerUsageCurrent.clear();
    if (activeTesterToken().empty()) {
        // Opted out: nothing here is collected, so there is nothing to write
        // beyond the token fields above (which record the removal itself).
        return;
    }
    testerUsageAccrue();
    const double now = wallTimeS();
    const double sessionSeconds = now > testerSessionStart_ ? now - testerSessionStart_ : 0.0;
    cfg.testerUsageCurrent = buildTesterUsageReport(sessionSeconds).toJson();
}

std::string AppWindow::activeTesterToken() const {
    return !testerAppToken_.empty() ? testerAppToken_ : testerToken_;
}

void AppWindow::setTesterToken(const std::string& token, bool startFreshSession) {
    testerToken_ = token;
    testerTokenInvalid_ = false;
    testerUsageQueueTriedThisSession_ = false;
    // A portal-token paste, replace or removal is a full identity reset - any
    // APP TOKEN linked earlier is superseded or removed with it
    // (PORTAL-LINK-VERDICT.md finding 7: "Remove code" must clear the app
    // token too, and there is no reason a fresh portal-token paste should
    // leave a stale link-confirmed identity behind it either).
    testerAppToken_.clear();
    testerAppTokenName_.clear();
    // NEVER CARRY A REPORT QUEUED UNDER A DIFFERENT CODE. Pasting a new code,
    // replacing an invalid one, or removing the code altogether (token == "")
    // must not leave an old session's report waiting to be sent under an
    // identity it was not recorded for - it would land on the wrong tester's
    // entry, or on nobody's once there is no entry at all.
    testerUsageQueue_.dropOthers(testerToken_);
    if (startFreshSession) {
        // A fresh opt-in starts a fresh session record - nothing from before
        // the code existed is this tester's to report.
        testerUsage_->reset();
        testerSessionStart_ = wallTimeS();
        testerSessionStartWall_ = std::time(nullptr);
        testerUsageAccrual_.reset(testerSessionStart_);
    }
    testerUsage_->setArmed(!testerToken_.empty());
}

void AppWindow::setTesterAppToken(const std::string& token, const std::string& name) {
    testerAppToken_ = token;
    testerAppTokenName_ = name;
    // The portal-token identity is superseded, not kept alongside the new
    // one - PORTAL-LINK-VERDICT.md finding 7's rule applied in the other
    // direction.
    testerToken_.clear();
    testerTokenInvalid_ = false;
    testerUsageQueueTriedThisSession_ = false;
    testerUsageQueue_.dropOthers(token);
    testerUsage_->reset();
    testerSessionStart_ = wallTimeS();
    testerSessionStartWall_ = std::time(nullptr);
    testerUsageAccrual_.reset(testerSessionStart_);
    testerUsage_->setArmed(true);
}

void AppWindow::clearTesterAppToken() {
    testerAppToken_.clear();
    testerAppTokenName_.clear();
    testerUsageQueueTriedThisSession_ = false;
    // Whatever was queued under the app token is this identity's alone -
    // dropOthers against whatever remains active (ordinarily nothing, since
    // the portal token is cleared the moment an app token is confirmed) drops
    // it rather than leaving it to be sent under nobody's credential.
    testerUsageQueue_.dropOthers(activeTesterToken());
    testerUsage_->setArmed(!activeTesterToken().empty());
}

bool AppWindow::testerSectionVisible() const {
    // A HELD LEGACY PORTAL TOKEN COUNTS TOO. testerToken_ keeps reporting
    // under activeTesterToken() (buildTesterUsageReport, testerUsageJournal)
    // whether or not the migration exchange ever succeeds - a NetworkError
    // leaves testerMigrationSender_ no longer busy() and testerAppToken_
    // still empty, and the section used to vanish right there even though
    // the tester's own code was still active and still sending. Checking
    // testerAppToken_ alone was the bug; activeTesterToken() covers both
    // identities the same way buildTesterUsageReport already does.
    return !activeTesterToken().empty() || testerMigrationSender_.busy() || testerLinkReveal_;
}

void AppWindow::testerUsageStartup(const cascade::core::AppConfig& cfg) {
    testerToken_ = cfg.testerToken;
    testerTokenInvalid_ = cfg.testerTokenInvalid;
    testerAppToken_ = cfg.testerAppToken;
    testerAppTokenName_ = cfg.testerAppTokenName;
    testerMigrationTried_ = false;
    testerUsage_->reset();
    testerUsage_->setArmed(!activeTesterToken().empty());
    testerSessionStart_ = wallTimeS();
    testerSessionStartWall_ = std::time(nullptr);
    testerUsageAccrual_.reset(testerSessionStart_);
    testerUsageQueueTriedThisSession_ = false;

    // FOLD THE PREVIOUS SESSION'S REPORT INTO THE RETRY QUEUE. testerUsageCurrent
    // is the previous run's "session so far" snapshot, rewritten on every save
    // exactly like telemetryPending - by the time this runs the process has
    // restarted, so whatever is there is that session's FINISHED report.
    std::vector<std::string> items = cfg.testerUsagePending;
    if (!cfg.testerUsageCurrent.empty()) { items.push_back(cfg.testerUsageCurrent); }
    testerUsageQueue_.setItems(items);
    // AND DROPPED IF IT DOES NOT MATCH THE ACTIVE IDENTITY - config.cpp
    // already filters testerUsagePending by that same rule on load, but
    // testerUsageCurrent is folded in fresh here, so it is re-applied to the
    // combined queue rather than trusted to have already held for both
    // halves.
    testerUsageQueue_.dropOthers(activeTesterToken());
    // The actual send is driven from testerUsagePoll() (called once a frame),
    // never blocking start-up - the same reason telemetry's own send happens
    // off the constructor. The migration exchange (if a portal token needs
    // one) is likewise started from testerLinkPoll(), not here.
}

void AppWindow::testerUsagePoll() {
    if (const std::optional<cascade::core::TesterUsageOutcome> outcome =
            testerUsageSender_.takeOutcome()) {
        using cascade::core::TesterUsageOutcome;
        if (*outcome == TesterUsageOutcome::Rejected) {
            cascade::core::diagWarnf(
                "tester usage: report rejected (malformed or oversized) - dropped");
        } else if (*outcome == TesterUsageOutcome::Invalid) {
            cascade::core::diagWarnf("tester usage: code no longer valid - sending stopped");
        }
        // THE ACTUAL DECISION IS core::applyTesterUsageOutcome'S, not this
        // function's - see its own comment for why it is a free function
        // rather than logic inlined here.
        const cascade::core::TesterUsageOutcomeEffect effect =
            cascade::core::applyTesterUsageOutcome(*outcome, testerUsageQueue_);
        if (effect.nowInvalid) {
            if (!testerAppToken_.empty()) {
                // A revoked APP TOKEN is cleared outright, unlike a revoked
                // portal token - there is nothing to fix in place, only a
                // fresh portal link produces a new one
                // (PORTAL-LINK-VERDICT.md §8).
                clearTesterAppToken();
            } else {
                testerTokenInvalid_ = true;
            }
        }
        if (effect.stop) { testerUsageQueueTriedThisSession_ = true; }
        // Sent/Rejected fall through with neither flag set, which is what
        // lets the loop below try the NEXT queued report immediately rather
        // than waiting for the next launch.
    }
    if (testerUsageSender_.busy() || testerUsageQueueTriedThisSession_) { return; }
    const std::string token = activeTesterToken();
    if (token.empty() || testerTokenInvalid_ || testerUsageQueue_.empty()) { return; }
    testerUsageSender_.send(cascade::core::testerUsageEndpoint(), testerUsageQueue_.front());
}

AppWindow::TesterLinkPending AppWindow::computeLinkPending(
    const std::string& token, const std::string& name, bool appTokenHeld, bool portalTokenHeld,
    const std::optional<std::string>& migrationResolvedAppToken) {
    TesterLinkPending pending;
    pending.token = token;
    pending.name = name;
    const bool replacingApp = appTokenHeld;
    const bool replacingLegacy = !replacingApp && portalTokenHeld;
    pending.replacing = replacingApp || replacingLegacy;
    pending.replacingLegacyPortalToken = replacingLegacy;
    pending.keepsLegacyQueue = replacingLegacy && migrationResolvedAppToken.has_value() &&
                               *migrationResolvedAppToken == pending.token;
    return pending;
}

void AppWindow::testerLinkPoll() {
    // --- migration exchange result --------------------------------------
    if (const std::optional<cascade::core::BetaMigrationResult> result =
            testerMigrationSender_.takeResult()) {
        using cascade::core::BetaMigrationOutcome;
        if (result->outcome == BetaMigrationOutcome::Ok) {
            // REWRITE, NEVER DROP: the reports queued under the portal token
            // are this same tester's session data, not a different tester's
            // (PORTAL-LINK-VERDICT.md finding 6b - see
            // TesterUsageQueue::rewriteToken's own comment).
            testerUsageQueue_.rewriteToken(result->appToken);
            // Cached for a LATER manual link confirmation to compare against
            // (TesterLinkPending::keepsLegacyQueue) - kept even though
            // testerAppToken_/testerToken_ below already act on it.
            testerMigrationResolvedAppToken_ = result->appToken;
            testerAppToken_ = result->appToken;
            testerAppTokenName_ = result->name;
            testerToken_.clear();
            testerTokenInvalid_ = false;
            testerUsage_->setArmed(true);
        } else if (result->outcome == BetaMigrationOutcome::Invalid) {
            // 404/401: the portal token is dead. Clear it and stop - retrying
            // forever would show "linking..." to someone no longer a tester
            // (PORTAL-LINK-VERDICT.md finding 6c).
            testerToken_.clear();
            testerTokenInvalid_ = false;
            testerUsageQueue_.dropOthers(activeTesterToken());
            testerUsage_->setArmed(!activeTesterToken().empty());
        }
        // NetworkError: testerToken_ is untouched - testerMigrationTried_
        // stays true for the rest of THIS session (a black-holed endpoint
        // must not become a per-frame retry loop), and the exchange is tried
        // again at the NEXT launch via testerUsageStartup resetting the flag.
    }
    if (!testerMigrationTried_ && !testerMigrationSender_.busy() && testerAppToken_.empty() &&
        !testerToken_.empty() && cascade::core::validTesterToken(testerToken_)) {
        testerMigrationTried_ = true;
        testerMigrationSender_.send(cascade::core::betaApiBaseUrl(), testerToken_);
    }

    // --- confirm-by-name result ------------------------------------------
    if (const std::optional<cascade::core::BetaLinkResolved> resolved =
            testerLinkNameSender_.takeResult()) {
        using cascade::core::BetaLinkOutcome;
        if (resolved->outcome == BetaLinkOutcome::Ok) {
            testerLinkPending_ = computeLinkPending(
                testerLinkResolvingToken_, resolved->name, !testerAppToken_.empty(),
                !testerToken_.empty(), testerMigrationResolvedAppToken_);
            testerLinkError_.clear();
        } else if (resolved->outcome == BetaLinkOutcome::Invalid) {
            testerLinkError_ = tr("that link is not valid");
        } else {
            testerLinkError_ = tr("could not check that link - try again");
        }
        testerLinkResolvingToken_.clear();
    }

    // --- the one-shot link-request file, ~1 Hz -----------------------------
    // Hermetic runs (empty configPath_) never touch the disk, same rule as
    // maybeSaveConfig.
    if (configPath_.empty()) { return; }

    // THE FILESYSTEM IS ASKED ON A WORKER AND NO FRAME WAITS FOR IT. This was
    // a synchronous exists() under the comment "a stat() is cheap", in the
    // directory config.json lives in; the first second %APPDATA% could not
    // answer froze the window (field report "hang ntdll.dll @
    // __std_fs_get_stats", 0.99.59). gui/link_request_poll.hpp has the whole
    // argument. Collected FIRST and every frame, so an answer is acted on the
    // frame it lands rather than up to a second later; the question itself is
    // still asked at ~1 Hz, below.
    std::string token;
    if (linkRequestPoll_.poll(token) && !token.empty()) {
        // The worker has already claimed AND deleted the file - if a lookup
        // or a confirm/replace prompt is already occupying this flow, this
        // token must be held rather than dropped, or a link clicked while
        // one was already in progress would simply be lost. LATEST WINS: a
        // second arrival before the first is even acted on overwrites
        // whatever was queued, since only the most recent click is the one
        // the tester actually meant.
        testerLinkQueuedToken_ = token;
    }
    // THE ONLY RECORD A REPORT WILL CARRY of a configuration directory that
    // went away for a while: once when a check has been out for 5 s, once when
    // it comes back. Nothing here names the directory or the token.
    double noticeS = 0.0;
    switch (linkRequestPoll_.takeNotice(noticeS)) {
        case cascade::gui::LinkRequestPoll::Notice::Stuck:
            cascade::core::diagWarnf(
                "tester link: the configuration directory has not answered a status check "
                "for %.0f s - the check is waiting on a worker thread, the window is not",
                noticeS);
            break;
        case cascade::gui::LinkRequestPoll::Notice::Recovered:
            cascade::core::diagLogf(
                "tester link: the configuration directory answered after %.0f s", noticeS);
            break;
        case cascade::gui::LinkRequestPoll::Notice::None:
            break;
    }
    const double now = wallTimeS();
    if (now - testerLinkPollLast_ >= 1.0) {
        testerLinkPollLast_ = now;
        // A no-op while the previous check is still out: a directory that
        // never answers costs one parked thread, not one a second.
        linkRequestPoll_.request(std::filesystem::path(configPath_).parent_path().string());
    }
    if (!testerLinkQueuedToken_.empty() && !testerLinkNameSender_.busy() && !testerLinkPending_) {
        testerLinkResolvingToken_ = testerLinkQueuedToken_;
        testerLinkNameSender_.send(cascade::core::betaApiBaseUrl(), testerLinkQueuedToken_);
        testerLinkQueuedToken_.clear();
    }
}

void AppWindow::drawTesterLinkPrompt() {
    if (!testerLinkPending_ && testerLinkError_.empty()) { return; }
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::Begin(trId("Link this FoxSDR?"), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        uiNotePanel("beta tester link prompt");
        cascade::gui::census::note("tester:link-prompt");
        if (testerLinkPending_) {
            const std::string shownName = testerNameOrPlaceholder(testerLinkPending_->name);
            cascade::gui::census::note("tester:link-prompt:", shownName);
            cascade::gui::census::note(testerLinkPending_->replacingLegacyPortalToken ? "tester:link-prompt-kind:legacy"
                                       : testerLinkPending_->replacing                ? "tester:link-prompt-kind:replace"
                                                                                      : "tester:link-prompt-kind:fresh");
            if (testerLinkPending_->replacingLegacyPortalToken) {
                // A legacy PORTAL token has no confirmed name of its own to
                // offer as "linked to OLDNAME" - it was never resolved by
                // name (that is exactly what the app-token flow adds).
                ImGui::TextWrapped(
                    tr("This FoxSDR has a tester code set. Replace it with a confirmed link "
                       "to %s?"),
                    shownName.c_str());
                ImGui::Spacing();
                if (testerLinkPending_->keepsLegacyQueue) {
                    ImGui::TextWrapped(
                        "%s",
                        tr("Reports already queued under the old code will be kept, linked "
                           "to the new one."));
                } else {
                    ImGui::TextWrapped(
                        "%s", tr("Reports already queued under the old code will be discarded."));
                }
            } else if (testerLinkPending_->replacing) {
                ImGui::TextWrapped(tr("This FoxSDR is linked to %s. Replace with %s?"),
                                   testerNameOrPlaceholder(testerAppTokenName_).c_str(),
                                   shownName.c_str());
            } else {
                ImGui::TextWrapped(tr("Link this FoxSDR to beta tester %s?"), shownName.c_str());
            }
            ImGui::Spacing();
            const bool link = ImGui::Button(trId("Link"));
            censusRect("tester:link:yes");
            if (link) {
                if (testerLinkPending_->replacingLegacyPortalToken &&
                    testerLinkPending_->keepsLegacyQueue) {
                    // SAME TESTER, proved by this session's own migration
                    // exchange: rewrite the queue onto the new identity
                    // BEFORE setTesterAppToken's dropOthers(token) runs, so
                    // every item already carries the token it is about to be
                    // kept under (PORTAL-LINK-VERDICT.md finding 6b's rule,
                    // applied to this second path onto the same identity).
                    testerUsageQueue_.rewriteToken(testerLinkPending_->token);
                }
                setTesterAppToken(testerLinkPending_->token, testerLinkPending_->name);
                testerLinkPending_.reset();
            }
            ImGui::SameLine();
            const bool notNow = ImGui::Button(trId("Not now"));
            censusRect("tester:link:no");
            if (notNow) { testerLinkPending_.reset(); }
        } else if (!testerLinkError_.empty()) {
            cascade::gui::census::note("tester:link-error:", testerLinkError_);
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorRed);
            ImGui::TextWrapped("%s", testerLinkError_.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            const bool dismiss = ImGui::Button(trId("Not now"));
            censusRect("tester:link:dismiss");
            if (dismiss) { testerLinkError_.clear(); }
        }
    }
    ImGui::End();
    if (!open) {
        testerLinkPending_.reset();
        testerLinkError_.clear();
    }
}

}  // namespace cascade::gui
