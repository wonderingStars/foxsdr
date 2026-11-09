// system_publish.cpp - the SYSTEM bank's publisher (system_publish.hpp): the host's models (system_logic.hpp) as the window's generated structs, the
// window's gestures as the Effects system_logic.hpp names for them. SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "system_publish.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>

#include "cjk_register.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "core/feature_request.hpp"
#include "core/i18n.hpp"
#include "core/problem_report.hpp"
#include "core/version.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#pragma comment(lib, "shell32.lib")
#endif

namespace foxsdr::slintsystem {

namespace cc = cascade::core;

namespace {

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

::ChipTone tone(Tone t) {
    switch (t) {
        case Tone::Good: return ::ChipTone::Good;
        case Tone::Warn: return ::ChipTone::Warn;
        case Tone::Err: return ::ChipTone::Err;
        default: return ::ChipTone::Neutral;
    }
}

std::vector<::SelectItem> items(const std::vector<SelectItem>& v) {
    std::vector<::SelectItem> out;
    out.reserve(v.size());
    for (const SelectItem& s : v) {
        ::SelectItem o{};
        o.label = ss(s.label);
        o.id = ss(s.id);
        o.enabled = s.enabled;
        o.note = ss(s.note);
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<slint::SharedString> strings(const std::vector<std::string>& v) {
    std::vector<slint::SharedString> out;
    out.reserve(v.size());
    for (const std::string& s : v) { out.push_back(ss(s)); }
    return out;
}

// A host figure inside its range as the 0..1 the sliders take, and back.
float toNorm(float value, float lo, float hi) {
    if (!(hi > lo)) { return 0.f; }
    return std::clamp((value - lo) / (hi - lo), 0.f, 1.f);
}
float fromNorm(float norm, float lo, float hi) { return lo + std::clamp(norm, 0.f, 1.f) * (hi - lo); }

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) { ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); }
    return w;
}
#endif

void say(const char* line) { std::fprintf(stderr, "foxsdr-slint: system: %s\n", line); }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// A list model kept between publishes and updated row by row.
template <class T>
struct Synced {
    std::shared_ptr<slint::VectorModel<T>> model = std::make_shared<slint::VectorModel<T>>();
    std::shared_ptr<slint::Model<T>> set(const std::vector<T>& rows) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i < model->row_count()) {
                if (!(*model->row_data(i) == rows[i])) { model->set_row_data(i, rows[i]); }
            } else {
                model->push_back(rows[i]);
            }
        }
        while (model->row_count() > rows.size()) { model->erase(model->row_count() - 1u); }
        return model;
    }
};

struct SystemPublisher::Lists {
    Synced<::SelectItem> scales, themes, freqDisplays, figures, trailStyles, bandSizes, bandColours, regions, languages, countries, gpsPorts, bauds, problemKinds;
    Synced<slint::SharedString> serialPorts, usagePayload, featureEvidence, bugEvidence;
    Synced<::KeyGroup> keyGroups;
    std::vector<Synced<::KeyRow>> keyRows;
};

struct SystemPublisher::Last {
    std::optional<::DisplayModel> display;
    std::optional<::LanguageModel> language;
    std::optional<::UpdatesModel> updates;
    std::optional<::SerialPortsModel> serial;
    std::optional<::KeyBindingsModel> keys;
    std::optional<::DiagnosticsModel> diagnostics;
    std::optional<::UsageModel> usage;
    std::optional<::TesterModel> tester;
    std::optional<::ReportPageModel> report;
};

SystemPublisher::SystemPublisher(MainWindow& ui, foxsdr::slintkit::PluginKit& kit, foxsdr::slinthost::HostDuties& duties, foxsdr::slinthost::LiveWorld& world,
                                 const FoxEngineApi* api, FoxSession* session, bool bounded, Hooks hooks)
    : ui_(ui), kit_(kit), duties_(duties), world_(world), api_(api), session_(session), bounded_(bounded), hooks_(std::move(hooks)), table_(api, session),
      lists_(std::make_unique<Lists>()), lastSet_(std::make_unique<Last>()) {
    // Read once: the language the machine itself speaks (the Automatic row's name) and where the diagnostics go.
    opts_.system_locale = cascade::i18n::systemLocale();
    // The ImGui window's own rule for the pages: a feature request opens on a clean page; the bug page starts with neither kind chosen (a default would file every hurried
    // report as whichever came first) and the attach box unticked until the kind says otherwise.
    opts_.feature.shown = false;
    opts_.bug.shown = false;
}

SystemPublisher::~SystemPublisher() = default;

double SystemPublisher::nowS() const { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::uint64_t SystemPublisher::nowEpoch() const { return world_.nowEpoch(); }

std::string SystemPublisher::fact(const std::string& key) {
    std::string out;
    if (!table_.fact(key, out)) { out.clear(); }
    return out;
}

void SystemPublisher::send(const Command& c) {
    forceRead_ = true;
    if (!c.valid) { return; }
    if (kit_.submit(c.q) != 0u) { ++sent_; }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the shell: a link, a folder. A bounded run opens nothing.

void SystemPublisher::openUrl(const std::string& url) {
    if (url.rfind("https://", 0) != 0) { return; }
    if (bounded_) {
        cc::diagLogf("system: bounded run - not opening %s", url.c_str());
        return;
    }
#if defined(_WIN32)
    ::ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

// The log file is shown by opening the folder it is in (the ImGui window's shellOpen does the same for the reports folder); the reports folder is made first.
void SystemPublisher::openFolder(const std::string& path, bool isFile) {
    if (path.empty()) { return; }
    const std::filesystem::path p{std::u8string(path.begin(), path.end())};
    const std::filesystem::path dir = isFile ? p.parent_path() : p;
    if (bounded_) {
        cc::diagLogf("system: bounded run - not opening a folder");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
#if defined(_WIN32)
    ::ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the report pages

void SystemPublisher::openReport(int kind, const std::string& text) {
    if (kind != 0 && kind != 1) { return; }
    opts_.feature.shown = kind == 0;
    opts_.bug.shown = kind == 1;
    if (!text.empty()) { inputsOf(kind).text = text; }
    bundleValid_ = false;
    forceRead_ = true;
}

void SystemPublisher::raiseReportForCapture(int kind) {
    openReport(kind, kind == 1 ? std::string("Translation (capture): ") : std::string());
    if (kind == 1) {
        opts_.bug.problem_kind = "bug";
        opts_.bug.attach = true;
        opts_.bug.preview_open = true;
    }
}

SenderView SystemPublisher::senderViewOf(bool feature) const {
    SenderView v;
    cc::FeatureRequestState s;
    if (feature) {
        const cc::FeatureRequestSender& f = world_.featureSender();
        s = f.state();
        v.failure = f.failureMessage();
        v.blocked_until = f.blockedUntil();
    } else {
        const cc::ProblemReportSendFlow& f = world_.problemSender();
        s = f.state();
        v.failure = f.failureMessage();
        v.blocked_until = f.blockedUntil();
        v.diagnostics_dropped = f.diagnosticsDropped();
    }
    switch (s) {
        case cc::FeatureRequestState::Sending: v.state = SendState::Sending; break;
        case cc::FeatureRequestState::Sent: v.state = SendState::Sent; break;
        case cc::FeatureRequestState::Failed: v.state = SendState::Failed; break;
        case cc::FeatureRequestState::CoolingDown: v.state = SendState::CoolingDown; break;
        default: v.state = SendState::Idle; break;
    }
    return v;
}

// The log line once per send (never the words, never the contact) and the one-frame clear of the text on the edge into Sent, as app_window_report.cpp does.
void SystemPublisher::pollReports() {
    {
        const cc::FeatureRequestState s = world_.featureSender().state();
        if (featureLast_ == cc::FeatureRequestState::Sending && s != cc::FeatureRequestState::Sending) {
            if (s == cc::FeatureRequestState::Sent) {
                cc::diagLogf("feature request: sent, %zu characters, HTTP %d", featureChars_, world_.featureSender().lastStatus());
            } else {
                cc::diagLogf("feature request: failed, %zu characters, HTTP %d - %s", featureChars_, world_.featureSender().lastStatus(),
                             world_.featureSender().failureMessage().c_str());
            }
        }
        if (cc::featureRequestClearsTextNow(featureLast_, s)) { opts_.feature.text.clear(); forceRead_ = true; }
        featureLast_ = s;
    }
    {
        const cc::FeatureRequestState s = world_.problemSender().state();
        if (problemLast_ == cc::FeatureRequestState::Sending && s != cc::FeatureRequestState::Sending) {
            if (s == cc::FeatureRequestState::Sent) {
                cc::diagLogf("problem report (%s): sent, %zu characters, HTTP %d", problemSentKind_.c_str(), problemChars_, world_.problemSender().lastStatus());
            } else {
                cc::diagLogf("problem report (%s): failed, %zu characters, HTTP %d - %s", problemSentKind_.c_str(), problemChars_,
                             world_.problemSender().lastStatus(), world_.problemSender().failureMessage().c_str());
            }
        }
        if (cc::featureRequestClearsTextNow(problemLast_, s)) { opts_.bug.text.clear(); forceRead_ = true; }
        problemLast_ = s;
    }
}

// What the person has just asked of the usage box shows until the fact says it (the command is answered by the engine's pass).
void SystemPublisher::pollTelemetry() {
    if (!opts_.telemetry_pending) { return; }
    const bool enabled = fact("fact.telemetry.enabled") == "1";
    if (enabled == opts_.telemetry_pending_on) { opts_.telemetry_pending = false; forceRead_ = true; }
}

// The diagnostics a bug report attaches: the bundle the ImGui window's "Copy diagnostics" is made from, with what a window-less host can say (the engine's context
// block, the log ring, the counters). The session-history sections, the SDRplay service line and the ring's figures are the ImGui window's own reads and are left out.
void SystemPublisher::refreshBundle() {
    if (!opts_.bug.shown || !opts_.bug.attach) { return; }
    const std::uint64_t lines = cc::DiagLog::instance().linesWritten();
    const std::uint64_t epoch = nowEpoch();
    if (!cc::problemReportDiagCacheStale(bundleValid_, bundleLines_, lines, bundleEpoch_, epoch)) { return; }
    cc::DiagBundleInput in;
    in.contextBlock = cc::diagContextBlock();
    in.logLines = cc::DiagLog::instance().ringSnapshot();
    in.logLinesTotal = lines;
    in.logPath = cc::DiagLog::instance().filePath();
    in.crashDir = opts_.reports_path;
    in.lastRunUnclean = lastRunUnclean_;
    in.launches = std::strtoull(fact("fact.diag.launches").c_str(), nullptr, 10);
    in.crashes = std::strtoull(fact("fact.diag.crashes").c_str(), nullptr, 10);
    cc::ringCounters(in.ringDroppedSamples, in.sourceSamplesRead);
    bundle_ = cc::prepareDiagnosticsForReport(cc::buildDiagnosticsBundle(in));
    bundleLines_ = lines;
    bundleEpoch_ = epoch;
    bundleValid_ = true;
}

void SystemPublisher::sendReport(const Effect& e) {
    if (!e.report.send) { return; }
    const ReportSend& r = e.report;
    // A BOUNDED RUN NEVER SENDS: the size is logged, never the words.
    if (bounded_) {
        cc::diagLogf("%s: bounded run - not sent, %zu characters", r.kind == 0 ? "feature request" : "problem report", r.chars);
        std::fprintf(stderr, "foxsdr-slint: system: %s not sent (bounded run), %zu characters\n", r.kind == 0 ? "feature request" : "problem report", r.chars);
        return;
    }
    if (r.kind == 0) {
        featureChars_ = r.chars;
        world_.featureSender().send(r.endpoint, r.feature, nowEpoch());
    } else {
        problemChars_ = r.chars;
        problemSentKind_ = r.problem.kind;
        world_.problemSender().send(r.endpoint, r.problem, nowEpoch());
    }
    forceRead_ = true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// executing an Effect

void SystemPublisher::applyWindow(const Effect& e) {
    if ((e.apply & kApplyScale) != 0u) {
        // The Slint window follows the operating system's own scale factor today (Window::scale_factor); the choice is saved for the ImGui window and for the
        // step that lets this one take it. Nothing is applied here.
        std::fprintf(stderr, "foxsdr-slint: system: interface size '%s' saved; this window follows the OS scale and takes it at the next start\n", e.value.c_str());
    }
    if ((e.apply & kApplySkin) != 0u) {
        // The six ImGui looks are one Workbench skin in this window: the key is saved (and the counter, readings and tuner style that come with it), nothing else.
        std::fprintf(stderr, "foxsdr-slint: system: look '%s' saved; this window has the one Workbench skin\n", e.value.c_str());
    }
    if ((e.apply & kApplyDbRange) != 0u) {
        if (hooks_.rangeManual) { hooks_.rangeManual(); }   // the range itself is the SET_DISPLAY_RANGE command the effect carries
    }
    if ((e.apply & kApplyLanguage) != 0u) {
        // As main.cpp does at start: the catalogue in force, then the compiled-in bundle for the window's own words. English is "en" (the default language).
        const std::string inForce = cascade::i18n::setLanguage(e.value);
        const bool ok = slint::select_bundled_translation(inForce);
        std::fprintf(stderr, "foxsdr-slint: language %s (%s)\n", inForce.c_str(), ok ? (inForce == "en" ? "english" : "bundled") : "not bundled");
        const foxsdr::slintfonts::Registered faces = foxsdr::slintfonts::registerForLanguage(ui_.window(), inForce);   // docs 6.62: the system's CJK face for the new language, or Inter again
        ui_.global<Theme>().set_cjk_family(slint::SharedString(faces.family));
        if (!faces.labels.empty()) { std::fprintf(stderr, "foxsdr-slint: CJK faces %s, font-family \"%s\"\n", faces.labels.c_str(), faces.family.c_str()); }
    }
    if ((e.apply & kApplyDiagnostics) != 0u) { duties_.diagnosticsChanged(); }
    if ((e.apply & kApplyUpdateCheck) != 0u) {
        duties_.updateSettingChanged();
        if (hooks_.updateCardChanged) { hooks_.updateCardChanged(); }
    }
    if ((e.apply & kApplyUpdateShowAgain) != 0u) {
        duties_.updateShowAgain();
        if (hooks_.updateCardChanged) { hooks_.updateCardChanged(); }
    }
}

void SystemPublisher::applyLocal(const Effect& e) {
    switch (e.local) {
        case Local::None: break;
        case Local::CaptureStart: opts_.capturing_id = e.text; break;
        case Local::CaptureCancel: opts_.capturing_id.clear(); break;
        case Local::CountryFilter: opts_.country_filter = e.text; break;
        case Local::TesterDraft:
            opts_.tester_draft = e.flag ? std::string() : e.text;
            opts_.tester_error.clear();
            break;
        case Local::ToggleWhat: opts_.what_open = e.flag; break;
        case Local::ToggleTesterSent: opts_.tester_sent_open = e.flag; break;
        case Local::OpenLog: openFolder(e.text, true); break;
        case Local::OpenReports: openFolder(e.text, false); break;
        case Local::OpenUrl: openUrl(e.text); break;
        case Local::OpenReport: openReport(e.number, e.text); break;
        case Local::CloseReport:
            inputsOf(e.number).shown = false;
            break;
        case Local::ReportText: inputsOf(e.number).text = e.text; break;
        case Local::ReportContact: inputsOf(e.number).contact = e.text; break;
        case Local::ReportKind:
            opts_.bug.problem_kind = e.text;
            opts_.bug.attach = e.flag;
            break;
        case Local::ReportAttach: inputsOf(e.number).attach = e.flag; break;
        case Local::ReportPreview: inputsOf(e.number).preview_open = e.flag; break;
        case Local::ReportProbe: opts_.bug.attach_probe = e.flag; break;
        case Local::TesterResolve:
            // The confirm-by-name question of the link flow is not in this window (the duties leave the link poll out): nothing is stored, the draft stays.
            say("a 40-hex tester link needs the confirm-by-name flow, which this window does not have yet; nothing was stored");
            break;
        case Local::TesterError: opts_.tester_error = e.text; break;
        case Local::OpenAbout: ui_.set_about_open(true); break;
        case Local::OpenTeamMessages:
            say("the team message cards are the duties' and have no home in this bank yet; nothing opened");
            break;
    }
}

void SystemPublisher::execute(const Effect& e, const char* what) {
    forceRead_ = true;
    if (e.empty()) { return; }
    if (e.save) {
        ++saves_;
        (void)duties_.saveInterfaceHalf(e.half);
    }
    if (e.apply != kApplyNone) { applyWindow(e); }
    for (const Command& c : e.commands) { send(c); }
    if (e.local != Local::None) { applyLocal(e); }
    if (e.report.send) { sendReport(e); }
    (void)what;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the models

::DisplayModel SystemPublisher::mapDisplay(const DisplayModel& m) {
    ::DisplayModel o{};
    o.scales = lists_->scales.set(items(m.scales));
    o.scale_index = m.scale_index;
    o.themes = lists_->themes.set(items(m.themes));
    o.theme_index = m.theme_index;
    // The sliders take 0..1 of their track; the host's figures are dB and pixels (the callbacks convert back with the same ranges).
    o.db_min = toNorm(m.db_min, m.db_min_lo, m.db_min_hi);
    o.db_min_text = ss(m.db_min_text);
    o.db_max = toNorm(m.db_max, m.db_max_lo, m.db_max_hi);
    o.db_max_text = ss(m.db_max_text);
    o.freq_displays = lists_->freqDisplays.set(items(m.freq_displays));
    o.freq_display_index = m.freq_display_index;
    o.figures = lists_->figures.set(items(m.figures));
    o.figures_index = m.figures_index;
    o.tuner_switches = m.tuner_switches;
    o.enlarge_readings = m.enlarge_readings;
    o.icon_px = toNorm(m.icon_px, m.icon_min, m.icon_max);
    o.icon_text = ss(m.icon_text);
    o.trail_px = toNorm(m.trail_px, m.trail_min, m.trail_max);
    o.trail_text = ss(m.trail_text);
    o.trail_styles = lists_->trailStyles.set(items(m.trail_styles));
    o.trail_style_index = m.trail_style_index;
    o.band_plan = m.band_plan;
    o.band_sizes = lists_->bandSizes.set(items(m.band_sizes));
    o.band_size_index = m.band_size_index;
    o.band_colours = lists_->bandColours.set(items(m.band_colours));
    o.band_colour_index = m.band_colour_index;
    o.regions = lists_->regions.set(items(m.regions));
    o.region_index = m.region_index;
    o.plan_status = ss(m.plan_status);
    o.plan_status_tone = tone(m.plan_status_tone);
    o.region_preview = ss(m.region_preview);
    o.scale_tooltip = ss(m.scale_tooltip);
    o.switches_tooltip = ss(m.switches_tooltip);
    o.icon_tooltip = ss(m.icon_tooltip);
    o.trail_tooltip = ss(m.trail_tooltip);
    return o;
}

::LanguageModel SystemPublisher::mapLanguage(const LanguageModel& m) {
    ::LanguageModel o{};
    o.languages = lists_->languages.set(items(m.languages));
    o.language_index = m.language_index;
    o.machine_hint = ss(m.machine_hint);
    o.suggest_label = ss(m.suggest_label);
    o.countries = lists_->countries.set(items(m.countries));
    o.country_index = m.country_index;
    o.country_filter = ss(m.country_filter);
    o.country_hint = ss(m.country_hint);
    o.plan_line = ss(m.plan_line);
    o.plan_changed_hint = ss(m.plan_changed_hint);
    o.translation_label = ss(m.translation_label);
    o.language_text = ss(m.language_text);
    o.country_text = ss(m.country_text);
    return o;
}

::UpdatesModel SystemPublisher::mapUpdates(const UpdatesModel& m) {
    ::UpdatesModel o{};
    o.check_at_startup = m.check_at_startup;
    o.channel_index = m.channel_index;
    o.channel_help = ss(m.channel_help);
    o.privacy = ss(m.privacy);
    o.version_line = ss(m.version_line);
    o.team_message = ss(m.team_message);
    o.status_line = ss(m.status_line);
    o.status_tone = tone(m.status_tone);
    o.check_label = ss(m.check_label);
    o.stable_label = ss(m.stable_label);
    o.beta_label = ss(m.beta_label);
    o.packaged = m.packaged;
    o.show_again = m.show_again;
    o.show_again_label = ss(m.show_again_label);
    return o;
}

::SerialPortsModel SystemPublisher::mapSerial(const SerialPortsModel& m) {
    ::SerialPortsModel o{};
    o.intro = ss(m.intro);
    o.empty_line = ss(m.empty_line);
    o.ports = lists_->serialPorts.set(strings(m.ports));
    ::GpsRow g{};
    g.ports = lists_->gpsPorts.set(items(m.gps.ports));
    g.port_index = m.gps.port_index;
    g.bauds = lists_->bauds.set(items(m.gps.bauds));
    g.baud_index = m.gps.baud_index;
    g.listening = m.gps.listening;
    g.key_label = ss(m.gps.key_label);
    g.status = ss(m.gps.status);
    g.status_tone = tone(m.gps.status_tone);
    g.satellites = ss(m.gps.satellites);
    g.key_enabled = m.gps.key_enabled;
    g.refusal = ss(m.gps.refusal);
    o.gps = std::move(g);
    o.gps_note = ss(m.gps_note);
    o.refresh_label = ss(m.refresh_label);
    o.column_label = ss(m.column_label);
    o.gps_heading = ss(m.gps_heading);
    o.gps_hint = ss(m.gps_hint);
    return o;
}

::KeyBindingsModel SystemPublisher::mapKeys(const KeyBindingsModel& m) {
    ::KeyBindingsModel o{};
    o.help = ss(m.help);
    o.reset_label = ss(m.reset_label);
    if (lists_->keyRows.size() < m.groups.size()) { lists_->keyRows.resize(m.groups.size()); }
    std::vector<::KeyGroup> groups;
    for (std::size_t g = 0; g < m.groups.size(); ++g) {
        std::vector<::KeyRow> rows;
        for (const KeyRow& r : m.groups[g].rows) {
            ::KeyRow x{};
            x.id = ss(r.id);
            x.action = ss(r.action);
            x.chord = ss(r.chord);
            x.capturing = r.capturing;
            x.clash = ss(r.clash);
            rows.push_back(std::move(x));
        }
        ::KeyGroup kg{};
        kg.heading = ss(m.groups[g].heading);
        kg.rows = lists_->keyRows[g].set(rows);
        groups.push_back(std::move(kg));
    }
    o.groups = lists_->keyGroups.set(groups);
    o.footer = ss(m.footer);
    o.changed = m.changed;
    o.prompt = ss(m.prompt);
    return o;
}

::DiagnosticsModel SystemPublisher::mapDiagnostics(const DiagnosticsModel& m) {
    ::DiagnosticsModel o{};
    o.explanation = ss(m.explanation);
    o.record = m.record;
    o.what_open = m.what_open;
    o.what_text = ss(m.what_text);
    o.minidump = m.minidump;
    o.log_path = ss(m.log_path);
    o.reports_path = ss(m.reports_path);
    o.offer = ss(m.offer);
    o.record_label = ss(m.record_label);
    o.minidump_label = ss(m.minidump_label);
    o.what_label = ss(m.what_label);
    o.sent_text = ss(m.sent_text);
    o.dump_note = ss(m.dump_note);
    // The pointer ledger, Copy diagnostics' status and the SDRplay diagnostic are window-bound in the ImGui window (the input ledger, the clipboard, a child
    // process) and have no host model: left empty, which hides the SDRplay block.
    return o;
}

::UsageModel SystemPublisher::mapUsage(const UsageModel& m) {
    ::UsageModel o{};
    o.disclosure = ss(m.disclosure);
    o.send = m.send;
    o.install_id_line = ss(m.install_id_line);
    o.what_label = ss(m.what_label);
    o.privacy_label = ss(m.privacy_label);
    o.payload = lists_->usagePayload.set(strings(m.payload));
    o.what_open = m.what_open;
    o.what_text = ss(m.what_text);
    o.state_line = ss(m.state_line);
    o.send_label = ss(m.send_label);
    return o;
}

::TesterModel SystemPublisher::mapTester(const TesterModel& m) {
    ::TesterModel o{};
    o.shown = m.shown;
    o.explanation = ss(m.explanation);
    o.linked_line = ss(m.linked_line);
    o.code_line = ss(m.code_line);
    o.invalid_line = ss(m.invalid_line);
    o.code_draft = ss(m.code_draft);
    o.use_label = ss(m.use_label);
    o.sent_open = m.sent_open;
    o.sent_preview = ss(m.sent_preview);
    o.field_shown = m.field_shown;
    o.error_line = ss(m.error_line);
    o.sent_text = ss(m.sent_text);
    o.field_label = ss(m.field_label);
    o.unlink_label = ss(m.unlink_label);
    o.remove_label = ss(m.remove_label);
    o.show_label = ss(m.show_label);
    o.hide_label = ss(m.hide_label);
    return o;
}

::ReportPageModel SystemPublisher::mapReport(const ReportPageModel& m) {
    ::ReportPageModel o{};
    o.shown = m.shown;
    o.kind = m.kind;
    o.title = ss(m.title);
    o.text = ss(m.text);
    o.attach = m.attach;
    o.attach_note = ss(m.attach_note);
    o.evidence = (m.kind == 0 ? lists_->featureEvidence : lists_->bugEvidence).set(strings(m.evidence));
    o.preview_open = m.preview_open;
    o.preview_text = ss(m.preview_text);
    o.send_enabled = m.send_enabled;
    o.refusal = ss(m.refusal);
    o.no_email_note = ss(m.no_email_note);
    o.sent_line = ss(m.sent_line);
    o.sent_tone = tone(m.sent_tone);
    o.prompt = ss(m.prompt);
    o.counter = ss(m.counter);
    o.contact = ss(m.contact);
    o.contact_label = ss(m.contact_label);
    o.kind_question = ss(m.kind_question);
    o.problem_kinds = lists_->problemKinds.set(items(m.problem_kinds));
    o.problem_kind_index = m.problem_kind_index;
    o.recognised_line = ss(m.recognised_line);
    o.probe_shown = m.probe_shown;
    o.probe_on = m.probe_on;
    o.probe_label = ss(m.probe_label);
    o.attach_label = ss(m.attach_label);
    o.preview_label = ss(m.preview_label);
    o.send_label = ss(m.send_label);
    o.local_only_line = ss(m.local_only_line);
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------------------
ReadOptions SystemPublisher::buildOptions() {
    // the monitor's scale as a whole percent (the Auto row's "% here")
    opts_.monitor_percent = std::clamp(static_cast<int>(std::lround(static_cast<double>(ui_.window().scale_factor()) * 100.0)), 50, 400);
    if (opts_.language_reason.empty()) {
        // The languages this machine has no face for, and why (docs 6.62; cached, so the font files are read once): the list greys them and says so beside the name.
        for (const cascade::i18n::Language& l : cascade::i18n::languages()) {
            const std::string reason = foxsdr::slintfonts::languageUnavailableReason(l.code);
            if (!reason.empty()) { opts_.language_reason.emplace_back(l.code, reason); }
        }
    }
    opts_.log_path = cc::DiagLog::instance().filePath();
    opts_.reports_path = duties_.about().diagnosticsDir.empty() ? std::string() : cc::diagCrashDir();
    opts_.gps_ports_waiting = serialAsked_ && state_.listSeq == serialAskedSeq_ && nowS() - serialAskedAt_ < 2.0;

    const foxsdr::slinthost::UpdateStatus u = duties_.updateStatus();
    UpdatesFacts f;
    f.packaged = u.packaged;
    f.started = u.started;
    f.pending = u.pending;
    f.newer = u.newer;
    f.critical = u.critical;
    f.dismissed = u.dismissed;
    f.new_version = u.newVersion;
    f.error = u.error;
    f.build_version = cascade::versionString();
    f.unseen_messages = u.unseenMessages;
    opts_.updates = f;

    const std::uint64_t now = nowEpoch();
    for (ReportInputs* r : {&opts_.feature, &opts_.bug}) {
        r->now_epoch = now;
        r->version = cascade::versionString();
        r->reports_dir = opts_.reports_path;
    }
    opts_.feature.sender = senderViewOf(true);
    opts_.bug.sender = senderViewOf(false);
    opts_.bug.diagnostics = bundle_;
    return opts_;
}

void SystemPublisher::publish(const FoxReceiverState& state) {
    state_ = state;
    const std::int64_t nowMs = static_cast<std::int64_t>(nowS() * 1000.0);
    pollReports();
    pollTelemetry();
    // The lists and facts are read at 10 Hz (and at once after a gesture): the ImGui window reads them every frame, this one does not need a thirtieth of a second.
    if (!forceRead_ && haveModels_ && nowMs - lastReadMs_ < 100) { return; }
    forceRead_ = false;
    lastReadMs_ = nowMs;
    refreshBundle();
    const ReadOptions options = buildOptions();
    SystemModels m = readSystem(table_, duties_.interfaceHalf(), state, options);
    last_ = m;
    haveModels_ = true;

    const auto setIf = [](auto& slot, auto&& value, auto&& setter) {
        if (!slot.has_value() || !(*slot == value)) {
            slot = value;
            setter(value);
        }
    };
    setIf(lastSet_->display, mapDisplay(m.display), [&](const ::DisplayModel& v) { ui_.set_display_model(v); });
    setIf(lastSet_->language, mapLanguage(m.language), [&](const ::LanguageModel& v) { ui_.set_language_model(v); });
    setIf(lastSet_->updates, mapUpdates(m.updates), [&](const ::UpdatesModel& v) { ui_.set_updates_model(v); });
    setIf(lastSet_->serial, mapSerial(m.serial), [&](const ::SerialPortsModel& v) { ui_.set_serial_model(v); });
    setIf(lastSet_->keys, mapKeys(m.keys), [&](const ::KeyBindingsModel& v) { ui_.set_keys_model(v); });
    setIf(lastSet_->diagnostics, mapDiagnostics(m.diagnostics), [&](const ::DiagnosticsModel& v) { ui_.set_diag_model(v); });
    setIf(lastSet_->usage, mapUsage(m.usage), [&](const ::UsageModel& v) { ui_.set_usage_model(v); });
    setIf(lastSet_->tester, mapTester(m.tester), [&](const ::TesterModel& v) { ui_.set_tester_model(v); });
    // ONE sheet serves both pages: the bug page when it is open, else the feature page (whose `shown` says whether any sheet is up).
    const ReportPageModel& page = opts_.bug.shown ? m.bug : m.feature;
    setIf(lastSet_->report, mapReport(page), [&](const ::ReportPageModel& v) { ui_.set_report_page(v); });
}

SystemPublisher::RowFace SystemPublisher::face(int row) const {
    RowFace f;
    if (!haveModels_ || row < 0 || row >= kSystemRowCount || row == kRowFrequency) { return f; }
    const RowChip& c = last_.chips[static_cast<std::size_t>(row)];
    f.chip = c.word;
    f.tone = tone(c.tone);
    f.valid = !c.word.empty();
    return f;
}

void SystemPublisher::sectionOpened(int logicalRow) {
    if (logicalRow == kRowSerial && !serialAsked_) {
        send(opSerialRefresh());
        serialAsked_ = true;
        serialAskedSeq_ = state_.listSeq;
        serialAskedAt_ = nowS();
    }
}

std::string SystemPublisher::summaryLine() const {
    if (!haveModels_) { return "system: no models read"; }
    const auto pick = [](const std::vector<SelectItem>& v, int i) { return i >= 0 && i < static_cast<int>(v.size()) ? v[static_cast<std::size_t>(i)].label : std::string("?"); };
    std::string chips;
    for (int r = 0; r < kSystemRowCount; ++r) {
        if (r == kRowFrequency) { continue; }
        chips += (chips.empty() ? "" : ", ") + std::to_string(r) + "=" + last_.chips[static_cast<std::size_t>(r)].word;
    }
    return "system: display scale=\"" + pick(last_.display.scales, last_.display.scale_index) + "\" theme=\"" + pick(last_.display.themes, last_.display.theme_index) +
           "\" range=" + last_.display.db_min_text + ".." + last_.display.db_max_text + " dB; language=\"" + last_.language.language_text + "\" in force=" +
           cascade::i18n::current() + " country=\"" + last_.language.country_text + "\" countries=" + std::to_string(last_.language.countries.size()) +
           "; updates=\"" + last_.updates.status_line + "\" check=" + (last_.updates.check_at_startup ? "on" : "off") + "; serial ports=" +
           std::to_string(last_.serial.ports.size()) + "; keys " + std::to_string(last_.keys.action_count) + " actions, " + std::to_string(last_.keys.clash_count) +
           " clashes, " + std::to_string(last_.keys.changed_count) + " changed; diagnostics=" + (last_.diagnostics.record ? "on" : "off") + " dump=" +
           (last_.diagnostics.minidump ? "on" : "off") + "; usage=" + (last_.usage.send ? "on" : "off") + "; tester=" + (last_.tester.shown ? "shown" : "hidden") +
           "; chips " + chips + "; commands sent=" + std::to_string(sent_) + " saves=" + std::to_string(saves_);
}

void SystemPublisher::languageChangeForCapture(const std::string& code) {
    // The dropdown's row for `code`: index 0 is Automatic, then the catalogues in the order of cascade::i18n::languages().
    const std::vector<cascade::i18n::Language>& langs = cascade::i18n::languages();
    for (std::size_t i = 0; i < langs.size(); ++i) {
        if (langs[i].code == code) {
            std::fprintf(stderr, "foxsdr-slint: system: capture - the Language dropdown picks %s\n", code.c_str());
            execute(applyLanguageSelected(duties_.interfaceHalf(), static_cast<int>(i) + 1), "language-capture");
            return;
        }
    }
    std::fprintf(stderr, "foxsdr-slint: system: capture - no language %s\n", code.c_str());
}

// ---------------------------------------------------------------------------------------------------------------------------------
void SystemPublisher::wire() {
    auto&& ev = ui_.global<::SystemEvents>();
    const auto cur = [this]() -> const cascade::core::AppConfig& { return duties_.interfaceHalf(); };
    const auto page = [this] { return opts_.bug.shown ? 1 : 0; };

    // ---- Display ----
    ev.on_display_scale([this, cur](int i) { execute(applyDisplayScale(cur(), i), "display-scale"); });
    ev.on_display_theme([this, cur](int i) { execute(applyDisplayTheme(cur(), i), "display-theme"); });
    ev.on_display_db_min([this, cur](float v) {
        const DisplayModel& d = last_.display;
        execute(applyDisplayDbMin(cur(), state_, std::round(fromNorm(v, d.db_min_lo, d.db_min_hi))), "display-db-min");
    });
    ev.on_display_db_max([this, cur](float v) {
        const DisplayModel& d = last_.display;
        execute(applyDisplayDbMax(cur(), state_, std::round(fromNorm(v, d.db_max_lo, d.db_max_hi))), "display-db-max");
    });
    ev.on_display_freq_display([this, cur](int i) { execute(applyDisplayFreqDisplay(cur(), i), "display-freq-display"); });
    ev.on_display_figures([this, cur](int i) { execute(applyDisplayFigures(cur(), i), "display-figures"); });
    ev.on_display_tuner_switches([this, cur](bool on) { execute(applyDisplayTunerSwitches(cur(), on), "display-tuner-switches"); });
    ev.on_display_enlarge([this, cur](bool on) { execute(applyDisplayEnlarge(cur(), on), "display-enlarge"); });
    ev.on_display_icon_px([this, cur](float v) {
        const DisplayModel& d = last_.display;
        execute(applyDisplayIconPx(cur(), std::round(fromNorm(v, d.icon_min, d.icon_max))), "display-icon-px");
    });
    ev.on_display_trail_px([this, cur](float v) {
        const DisplayModel& d = last_.display;
        execute(applyDisplayTrailPx(cur(), std::round(fromNorm(v, d.trail_min, d.trail_max))), "display-trail-px");
    });
    ev.on_display_trail_style([this, cur](int i) { execute(applyDisplayTrailStyle(cur(), i), "display-trail-style"); });
    ev.on_display_band_plan([this, cur](bool on) { execute(applyDisplayBandPlan(cur(), on), "display-band-plan"); });
    ev.on_display_band_size([this, cur](int i) { execute(applyDisplayBandSize(cur(), i), "display-band-size"); });
    ev.on_display_band_colour([this, cur](int i) { execute(applyDisplayBandColour(cur(), i), "display-band-colour"); });
    ev.on_display_region([this](int i) { execute(applyDisplayRegion(last_.display, i), "display-region"); });

    // ---- Language & country ----
    ev.on_language_selected([this, cur](int i) { execute(applyLanguageSelected(cur(), i), "language-selected"); });
    ev.on_language_suggest([this, cur] { execute(applyLanguageSuggest(cur()), "language-suggest"); });
    ev.on_language_translation([this] { execute(applyLanguageTranslationReport(opts_.bug.text), "language-translation"); });
    ev.on_country_filter([this](slint::SharedString t) { execute(applyCountryFilter(std::string(t)), "country-filter"); });
    // The index is a position in the list AS LAST PUBLISHED: the filter that list was built with, not the one typed since (the field says "" a moment before a pick).
    ev.on_country_selected([this, cur](int i) { execute(applyCountrySelected(table_, cur(), last_.language.country_filter, i), "country-selected"); });

    // ---- Updates ----
    ev.on_updates_check([this, cur](bool on) { execute(applyUpdatesCheck(cur(), on), "updates-check"); });
    ev.on_updates_channel([this, cur](int i) { execute(applyUpdatesChannel(cur(), i, last_.updates.packaged), "updates-channel"); });
    ev.on_updates_team_message([this] { execute(applyUpdatesTeamMessage(), "updates-team-message"); });
    ev.on_updates_show_again([this] { execute(applyUpdatesShowAgain(), "updates-show-again"); });

    // ---- Serial ports ----
    ev.on_serial_refresh([this] {
        serialAsked_ = true;
        serialAskedSeq_ = state_.listSeq;
        serialAskedAt_ = nowS();
        execute(applySerialRefresh(), "serial-refresh");
    });
    ev.on_gps_port([this, cur](int i) { execute(applyGpsPort(cur(), last_.serial.gps, i), "gps-port"); });
    ev.on_gps_baud([this, cur](int i) { execute(applyGpsBaud(cur(), last_.serial.gps, i), "gps-baud"); });
    ev.on_gps_toggle([this, cur] { execute(applyGpsToggle(cur(), last_.serial.gps), "gps-toggle"); });

    // ---- Key bindings ----
    ev.on_keys_reset([this, cur] { execute(applyKeysReset(cur()), "keys-reset"); });
    ev.on_keys_capture([this](slint::SharedString id) { execute(applyKeysCapture(opts_.capturing_id, std::string(id)), "keys-capture"); });
    ev.on_keys_captured([this, cur](slint::SharedString id, slint::SharedString chord) { execute(applyKeysCaptured(cur(), std::string(id), std::string(chord)), "keys-captured"); });
    ev.on_keys_cancel([this] { execute(applyKeysCancel(), "keys-cancel"); });
    ev.on_keys_clear([this, cur](slint::SharedString id) { execute(applyKeysClear(cur(), std::string(id)), "keys-clear"); });

    // ---- Diagnostics ----
    ev.on_diag_record([this, cur](bool on) { execute(applyDiagRecord(cur(), on), "diag-record"); });
    ev.on_diag_minidump([this, cur](bool on) { execute(applyDiagMinidump(cur(), on), "diag-minidump"); });
    ev.on_diag_what([this](bool on) { execute(applyDiagWhat(on), "diag-what"); });
    ev.on_diag_open_log([this] { execute(applyDiagOpenLog(opts_.log_path), "diag-open-log"); });
    ev.on_diag_open_reports([this] { execute(applyDiagOpenReports(opts_.reports_path), "diag-open-reports"); });
    // The UI's three extras have no host effect: the pointer ledger, Copy diagnostics and the SDRplay diagnostic are window-bound in the ImGui window (the input
    // ledger, the clipboard, a child process). A line says so; nothing else happens.
    ev.on_diag_ledger([](bool) { say("the pointer ledger is the ImGui window's (its input ledger); nothing to show here"); });
    ev.on_diag_copy([] { say("Copy diagnostics needs the clipboard and the full bundle, which this window does not have yet; nothing copied"); });
    ev.on_diag_sdrplay([] { say("the SDRplay diagnostic is a child process of the ImGui window; not started"); });

    // ---- Usage reporting ----
    ev.on_usage_send([this](bool on) {
        opts_.telemetry_pending = true;
        opts_.telemetry_pending_on = on;
        execute(applyUsageSend(on), "usage-send");
    });
    ev.on_usage_what([this](bool on) { execute(applyUsageWhat(on), "usage-what"); });
    ev.on_usage_privacy([this] { execute(applyUsagePrivacy(), "usage-privacy"); });

    // ---- Beta tester ----
    ev.on_tester_unlink([this, cur] { execute(applyTesterUnlink(cur()), "tester-unlink"); });
    ev.on_tester_remove_code([this, cur] { execute(applyTesterRemoveCode(cur()), "tester-remove-code"); });
    ev.on_tester_code_committed([this](slint::SharedString t) { execute(applyTesterCodeCommitted(std::string(t)), "tester-code-committed"); });
    ev.on_tester_use([this, cur] { execute(applyTesterUse(cur(), opts_.tester_draft), "tester-use"); });
    ev.on_tester_show_sent([this](bool on) { execute(applyTesterShowSent(on), "tester-show-sent"); });

    // ---- the two report pages ----
    ev.on_report_open([this](int kind) { execute(applyReportOpen(kind), "report-open"); });
    ev.on_report_text([this, page](slint::SharedString t) { execute(applyReportText(page(), std::string(t)), "report-text"); });
    ev.on_report_contact([this, page](slint::SharedString t) { execute(applyReportContact(page(), std::string(t)), "report-contact"); });
    ev.on_report_kind([this](int i) { execute(applyReportKind(i), "report-kind"); });
    ev.on_report_attach([this, page](bool on) { execute(applyReportAttach(page(), on), "report-attach"); });
    ev.on_report_probe([this](bool on) { execute(applyReportProbe(on), "report-probe"); });
    ev.on_report_preview([this, page](bool on) { execute(applyReportPreview(page(), on), "report-preview"); });
    ev.on_report_send([this, page] {
        const int kind = page();
        execute(applyReportSend(kind, inputsOf(kind)), "report-send");
    });
    ev.on_report_closed([this, page] { execute(applyReportClosed(page()), "report-closed"); });

    // ---- About ----
    ev.on_about_open([this] { execute(applyAboutOpen(), "about-open"); });
}

}  // namespace foxsdr::slintsystem
