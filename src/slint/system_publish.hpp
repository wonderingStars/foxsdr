// system_publish.hpp - THE SYSTEM BANK'S PUBLISHER (docs/engine-stage4.md, the Slint port, phase 3 bank 2): the UI family's models (ui/system_bank.slint:
// DisplayModel, LanguageModel, UpdatesModel, SerialPortsModel, KeyBindingsModel, DiagnosticsModel, UsageModel, TesterModel, ReportPageModel) filled from
// the host's pure logic (system_logic.hpp: readSystem over the C API table and the interface half the duties hold) and set only when a model's content
// changed, and every callback of the SystemEvents global turned into the Effect the logic names for it and EXECUTED here: the save through the duties
// (saveInterfaceHalf), the window-side application (language, the duties' three switches), the commands through the plugin kit's one submit, the local state
// this object owns (the country filter, the key capture, the disclosures, the report pages, the tester draft) and the two report senders.
//
// THE PATTERN IS signal_publish.hpp's: the constructor takes the window, the kit and the duties; wire() registers the callbacks once; publish(state) runs
// after the kit's tick. Lists are persistent VectorModels updated IN PLACE (a replaced list model recreates every row behind it and closes an open dropdown),
// and a struct is set only when it differs from the one last set: the generated structs carry operator==, and every list in them is one of the persistent
// models, so equal pointers plus equal scalars means nothing a repeater shows has changed.
//
// A BOUNDED RUN NEVER SENDS: a report is logged by its size and not handed to a sender, a link or a folder is logged and not opened (the constructor's
// `bounded` flag).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <slint.h>

#include "host_duties.hpp"
#include "main.h"
#include "plugin_kit.hpp"
#include "system_logic.hpp"

namespace foxsdr::slintsystem {

class SystemPublisher {
public:
    // the counter's digit grouping is the host's own record (workspace_prefs, `dg=`); the Display body shows it and the host applies a pick (SystemEvents.display-digit-grouping is wired by the host)
    int diagCopies() const { return diagCopies_; }                 // the drive scripts' read-back of Copy diagnostics
    const std::string& bundleStatus() const { return bundleStatus_; }
    void setDigitGroupingSource(std::function<int()> f) { digitGrouping_ = std::move(f); }
    void setHighContrastSource(std::function<bool()> f) { highContrast_ = std::move(f); }
    struct Hooks {
        std::function<void()> updateCardChanged;   // the duties changed the update card's state (a re-tick, "Show it again"): the host applies it
        std::function<void()> rangeManual;         // a dB slider was dragged: the range is Manual from then on (the host's own flag and the strip's key follow)
    };
    SystemPublisher(MainWindow& ui, foxsdr::slintkit::PluginKit& kit, foxsdr::slinthost::HostDuties& duties, foxsdr::slinthost::LiveWorld& world,
                    const FoxEngineApi* api, FoxSession* session, bool bounded, Hooks hooks);
    ~SystemPublisher();
    SystemPublisher(const SystemPublisher&) = delete;
    SystemPublisher& operator=(const SystemPublisher&) = delete;

    // Registers every SystemEvents callback (once, after the window is created).
    void wire();
    // Reads the models and sets the ones that changed (after kit.tick()).
    void publish(const FoxReceiverState& state);

    // The chip a SYSTEM row of bank 4 shows, by LOGICAL row (system_logic.hpp's Row: 0 Display, 1 Language, 3 Updates, 4 Serial ports, 5 Key bindings,
    // 6 Diagnostics, 7 Usage reporting, 8 Beta tester); `valid` is false for row 2 (the SIGNAL family's), row 9 (About) and before the first publish.
    struct RowFace {
        bool valid = false;
        std::string chip;
        ::ChipTone tone = ::ChipTone::Neutral;
    };
    RowFace face(int logicalRow) const;
    // The Beta tester row exists only while the tester feature is on (TesterModel::shown).
    bool testerShown() const { return haveModels_ && last_.tester.shown; }

    // A section was opened by the operator (or by a capture): the Serial ports section asks the engine for a scan the first time, as the ImGui drawer does.
    void sectionOpened(int logicalRow);

    // A bounded run's evidence: the Display scale and theme, the language, the updates status, the port count, the bindings footer counts, the switches.
    std::string summaryLine() const;
    const SystemModels& models() const { return last_; }
    std::size_t commandsSent() const { return sent_; }
    std::size_t savesMade() const { return saves_; }

    // The interface size as the Display dropdown sets it (0 = Auto, 1..6 the steps): saved, and seen by the window's look on the next tick (Ctrl+0 is index 0).
    void setInterfaceSize(int index);
    // The operating system's own scale (the window's look knows it apart from the one it dispatched): the Auto row's "% here" quotes this, not the window's.
    void setOsScale(float scale) { osScale_ = scale; }

    // A bounded run's self-captures. Never set in a release.
    //   languageChangeForCapture: as if the Language dropdown had picked `code` (the whole path: save, setLanguage, select_bundled_translation, re-publish).
    //   raiseReportForCapture: opens the feature (0) or bug (1) page.
    void languageChangeForCapture(const std::string& code);
    void raiseReportForCapture(int kind);
    // The page of the report callback "report-open" and the palette: opens the page of `kind` (0 feature, 1 bug), starting the draft with `text`.
    void openReport(int kind, const std::string& text = std::string());
    // The program's own knowledge of the last run, for the diagnostics bundle a bug report attaches.
    void setLastRunUnclean(bool unclean) { lastRunUnclean_ = unclean; }

private:
    struct Lists;

    void execute(const Effect& e, const char* what);
    void applyWindow(const Effect& e);
    void applyLocal(const Effect& e);
    void send(const Command& c);
    void sendReport(const Effect& e);
    void changed() { forceRead_ = true; }
    std::string fact(const std::string& key);
    double nowS() const;
    std::uint64_t nowEpoch() const;

    ReadOptions buildOptions();
    void pollReports();
    void pollTelemetry();
    void refreshBundle();
    void copyDiagnostics();           // slint-gaps.md item 9: Copy diagnostics (the classic's copyDiagnosticsBundle): the bundle to the clipboard and to <reports>/diagnostics.txt
    int diagCopies_ = 0;
    std::string bundleStatus_;        // the line under the key ("Copied to the clipboard.")
    SenderView senderViewOf(bool feature) const;
    ReportInputs& inputsOf(int kind) { return kind == 0 ? opts_.feature : opts_.bug; }

    void openUrl(const std::string& url);
    void openFolder(const std::string& path, bool isFile);

    // ---- the models ----
    ::DisplayModel mapDisplay(const DisplayModel& m);
    std::function<bool()> highContrast_;
    std::function<int()> digitGrouping_;   // slint-gaps.md item 2: the saved grouping (0 | 1), which the host keeps in the workspace entry of the window half
    ::LanguageModel mapLanguage(const LanguageModel& m);
    ::UpdatesModel mapUpdates(const UpdatesModel& m);
    ::SerialPortsModel mapSerial(const SerialPortsModel& m);
    ::KeyBindingsModel mapKeys(const KeyBindingsModel& m);
    ::DiagnosticsModel mapDiagnostics(const DiagnosticsModel& m);
    ::UsageModel mapUsage(const UsageModel& m);
    ::TesterModel mapTester(const TesterModel& m);
    ::ReportPageModel mapReport(const ReportPageModel& m);

    MainWindow& ui_;
    foxsdr::slintkit::PluginKit& kit_;
    foxsdr::slinthost::HostDuties& duties_;
    foxsdr::slinthost::LiveWorld& world_;
    const FoxEngineApi* api_;
    FoxSession* session_;
    bool bounded_;
    Hooks hooks_;
    foxsdr::slintkit::LiveTable table_;

    ReadOptions opts_;
    SystemModels last_;
    bool haveModels_ = false;
    float osScale_ = 0.0f;   // 0 = not told yet: the window's own scale stands in
    FoxReceiverState state_{};
    std::size_t sent_ = 0;
    std::size_t saves_ = 0;
    std::int64_t lastReadMs_ = 0;
    bool forceRead_ = true;
    bool lastRunUnclean_ = false;

    // the serial scan: asked once when the section first opens, "waiting" for two seconds or until the lists move (serialPortsWaiting)
    bool serialAsked_ = false;
    std::uint64_t serialAskedSeq_ = 0;
    double serialAskedAt_ = 0.0;

    // the report senders' last states (the log line and the clear of the text fire once, on the edge out of Sending)
    cascade::core::FeatureRequestState featureLast_ = cascade::core::FeatureRequestState::Idle;
    cascade::core::FeatureRequestState problemLast_ = cascade::core::FeatureRequestState::Idle;
    std::size_t featureChars_ = 0, problemChars_ = 0;
    std::string problemSentKind_;

    // the diagnostics bundle a bug report attaches (built when the page shows with the box ticked, rebuilt at most once a second)
    std::string bundle_;
    std::uint64_t bundleLines_ = 0;
    std::uint64_t bundleEpoch_ = 0;
    bool bundleValid_ = false;

    std::unique_ptr<Lists> lists_;
    // the structs last set (equality is the guard)
    struct Last;
    std::unique_ptr<Last> lastSet_;
};

}  // namespace foxsdr::slintsystem
