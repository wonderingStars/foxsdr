// signal_publish.hpp - THE SIGNAL BANK'S PUBLISHER (docs/engine-stage4.md, the Slint port, phase 3 bank 1): the UI family's models (ui/signal_bank.slint:
// SourceModel, RadioExtrasModel, AudioFiltersModel, SinksModel, TransmitModel, RecorderModel, RadioSetupModel, ImportDialogModel) filled once a tick from
// the host's pure logic (signal_logic.hpp: readSignal over the C API table) and set only when a model's content signature changed (signatures.hpp), and
// every callback of the SignalEvents global turned into the exact command the ImGui drawer sends (signal_logic.hpp's op* builders, submitted through the
// plugin kit's one submit) or into a local change of what this object owns - the drafts (the I/Q path, the converter LO text), the Radio-setup page and
// the import dialog (their open flags, the probe, the driver install, the lookup of a path with no root), the clocks of the disk-wait lines.
//
// THE PATTERN IS kit_publish.hpp's: the constructor takes the window and the kit, wire() registers the callbacks once, publish(state) runs after the
// kit's tick. Lists are kept as persistent VectorModels updated IN PLACE (a replaced list model recreates every row behind it and closes an open
// dropdown); a struct is set only when its signature moved.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <slint.h>

#include "main.h"
#include "plugin_kit.hpp"
#include "signal_logic.hpp"
#include "signatures.hpp"

namespace foxsdr::slintsignal {

class SignalPublisher {
public:
    SignalPublisher(MainWindow& ui, foxsdr::slintkit::PluginKit& kit, const FoxEngineApi* api, FoxSession* session);
    ~SignalPublisher();
    SignalPublisher(const SignalPublisher&) = delete;
    SignalPublisher& operator=(const SignalPublisher&) = delete;

    // Registers every SignalEvents callback and the two bias-tee callbacks of the window (once, after the window is created).
    void wire();
    // Reads the models and sets the ones whose content changed (after kit.tick()).
    void publish(const FoxReceiverState& state);

    // The chip a SIGNAL PATH row of bank 0 shows, from the host's models (rows 1 Source, 3 Audio filters, 4 Sinks, 5 Transmit, 6 Recorder, 7 Frequency
    // correction); `valid` is false for the other rows and before the first publish.
    struct RowFace {
        bool valid = false;
        std::string chip;
        ::ChipTone tone = ::ChipTone::Neutral;
    };
    RowFace face(int row) const;

    // A bounded run's evidence: the Source chip, the device count, the sink device, the recorder label, the commands sent.
    std::string summaryLine() const;
    // The model the last publish read (for the bounded run and the tests).
    const SignalModels& models() const { return last_; }
    std::size_t commandsSent() const { return sent_; }

    // A BOUNDED RUN'S FILE SEAMS (docs 6.63; the classic's FOXSDR_FILE_PICK): FOXSDR_FILE_PICK=<path> is the file the I/Q row's file dialog "chooses" (set and empty = cancelled),
    // FOXSDR_FILE_CLIPBOARD=<path> the file Ctrl+V finds on the clipboard (set and empty = none). A native dialog cannot be pressed from a test, and the desk's clipboard is
    // never the test's. Read only when the run is bounded, so a release can never be steered by them; call once, after construction and before wire().
    void setBounded(bool bounded);

    // A bounded run's self-capture (FOXSDR_SLINT_SETUP=1, FOXSDR_SLINT_IMPORT=1): raises the Radio-setup page / the import dialog. Never set in a release.
    void raiseSetupForCapture();
    void raiseImportForCapture();
    // The import dialog opened for an Airband preset (docs 6.61), and what a path typed in it does: the host hands it to the TOOLS bank's preset import.
    void openImportForPreset(const std::string& preset);
    void setPresetImport(std::function<void(const std::string& path, const std::string& preset)> fn) { presetImport_ = std::move(fn); }
    // A FILE DROPPED ON THE WINDOW (slint-gaps.md item 8; the classic's submitPendingDrop): a recording is opened as the I/Q file (the Source row moves to the I/Q file, the file is opened as Open would
    // open it); a frequency list goes to the import, as the Import key's box would send it. The caller has already told which it is (gaps_logic classifyDrop).
    void openDroppedRecording(const std::string& path);
    void importDroppedList(const std::string& path);
    // RESTART SDRPLAY SERVICE (slint-gaps.md item 9; the classic's app_window_sdrplay.cpp). The key asks Windows for elevation (a UAC prompt) and restarts the service on a worker; a TEST replaces
    // the elevated launcher with a stand-in so nothing is ever asked of Windows (the real call is only reached by pressing the key on a machine that shows it).
    void setSdrPlayLauncherForTest(std::function<void()> standIn);
    // The last restart's phase word for the drive scripts ("idle", "running", "done", "failed", "cancelled", "timedout", "notstarted") and how many times the key reached the launcher.
    std::string sdrPlayRestartState() const;
    int sdrPlayRestartCalls() const;
    // TESTS ONLY: makes the key appear as if the driver had trouble reaching the service (the machine here has no SDRplay service to show it for).
    void forceSdrPlayKeyForTest(const std::string& serviceName);

private:
    // A list model kept between publishes and updated row by row.
    template <class T>
    struct Synced {
        std::shared_ptr<slint::VectorModel<T>> model = std::make_shared<slint::VectorModel<T>>();
        std::shared_ptr<slint::Model<T>> set(const std::vector<T>& rows);
    };

    // ---- commands ----
    void send(const Command& c);
    void sendAll(const std::vector<Command>& cs);
    std::string fact(const std::string& key);

    // ---- the page and dialog logic ----
    void startProbe();
    void pollSetup();
    void startInstall(const std::string& target);
    void openLink(const std::string& url);
    void pollImport();
    std::function<void(const std::string&, const std::string&)> presetImport_;
    void importPath(const std::string& typed);
    void importBrowse();
    // The I/Q file row (docs 6.63): Open (the box cleaned; an empty box is the dialog), Browse (the dialog; a choice is opened) and Ctrl+V with a copied file.
    void iqOpen();
    void iqBrowse();
    bool iqPaste();
    void updateClocks();
    double nowS() const;
    void pollSdrPlay();
    void sdrPlayRestartPressed();
    struct SdrState;
    std::unique_ptr<SdrState> sdr_;

    // ---- the models ----
    ::SourceModel mapSource(const SourceModel& m);
    ::RadioExtrasModel mapRadio(const RadioExtrasModel& m);
    ::AudioFiltersModel mapFilters(const AudioFiltersModel& m);
    ::SinksModel mapSinks(const SinksModel& m);
    ::TransmitModel mapTransmit(const TransmitModel& m);
    ::RecorderModel mapRecorder(const RecorderModel& m);
    ::RadioSetupModel mapSetup(const RadioSetupModel& m);
    ::ImportDialogModel mapImport(const ImportDialogModel& m);

    MainWindow& ui_;
    foxsdr::slintkit::PluginKit& kit_;
    const FoxEngineApi* api_;
    FoxSession* session_;
    foxsdr::slintkit::LiveTable table_;
    foxsdr::slinthost::Signatures sigs_;

    ReadOptions opts_;
    SignalModels last_;
    bool haveModels_ = false;
    FoxReceiverState state_{};
    std::size_t sent_ = 0;
    std::int64_t lastReadMs_ = 0;
    bool forceRead_ = true;
    bool txPageOpen_ = false;
    bool bounded_ = false;
    std::optional<std::string> filePickSeam_, fileClipboardSeam_;   // the bounded run's seams (setBounded)
    bool plutoDraftSet_ = false, rtltcpDraftSet_ = false;
    std::string plutoDraft_, rtltcpDraft_;

    // the clocks of the disk-wait lines (seconds on nowS(), < 0 none out)
    double iqOpenSince_ = -1.0, iqStartSince_ = -1.0, audioStartSince_ = -1.0, importSince_ = -1.0;

    // the Radio-setup page
    struct SetupState;
    std::unique_ptr<SetupState> setup_;
    // the import dialog
    struct ImportState;
    std::unique_ptr<ImportState> import_;

    // persistent list models, one per list (the generated structs hold them by pointer)
    struct Models;
    std::unique_ptr<Models> lists_;
};

}  // namespace foxsdr::slintsignal
