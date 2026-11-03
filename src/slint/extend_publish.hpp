// extend_publish.hpp - THE EXTEND BANK'S PUBLISHER and the PLUGIN STORE PAGE'S (docs/engine-stage4.md, the Slint port, phase 3 bank 4): the UI family's models
// (ui/extend_bank.slint: WebModel, CatModel, and the StoreModel the page of kind 9 shows) filled from the host's pure logic (extend_logic.hpp: readExtend over the C API
// table) and set only when a model differs from the one last set, and every callback of the ExtendEvents global turned into the Effect the logic names for it and
// EXECUTED here: the commands through the plugin kit's one submit, the three drafts (Web, Cat, Store) this object keeps and hands back to every read, the asks kept
// against their tickets (a SERVER_CONFIG's answer is applyServerAnswer's), the store's pictures decoded on a worker and published as images.
//
// THE PATTERN IS tools_publish.hpp's / system_publish.hpp's: the constructor takes the window and the kit, wire() registers the callbacks once, publish(state) runs
// once a tick and onAnswer(result) for each answer main.cpp polls.
//
// THE SEAM WITH THE KIT'S STORE PAGE (kit_publish.hpp): the KIT owns the page (its PageTab, its tab, its tear-off, the DECODE bank's row); this object computes the
// whole ::StoreModel and hands it over (KitPublisher::setStoreModel), asks whether the page is up (storeView) so the store's lists are read only while someone can
// see them, and hands over the DECODE row's chip (setStoreRail). The Web access and CAT bodies are properties of the window (web-model, cat-model) and are set here.
//
// THE PASSWORD RULE. The two password boxes live in the Web draft; they reach the engine inside a SERVER_CONFIG's text and nowhere else. They are never logged and
// never in a line this object prints, and scrubEffect overwrites the effect's copies as soon as the command is submitted (the draft's boxes are wiped by the effect
// itself when a password is set).
//
// A BOUNDED RUN NEVER SENDS: a command an effect carries is logged by its op and kept in the process (the constructor's `bounded` flag) - the catalogue read the
// store asks for when it opens, the pictures it asks for, the install and update keys, the server settings. What the engine does on its own is not this object's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <slint.h>

#include "extend_logic.hpp"
#include "kit_publish.hpp"
#include "main.h"
#include "plugin_kit.hpp"

namespace foxsdr::slintextend {

class ExtendPublisher {
public:
    ExtendPublisher(MainWindow& ui, foxsdr::slintkit::PluginKit& kit, foxsdr::slintkit::KitPublisher& pages, const FoxEngineApi* api, FoxSession* session, bool bounded);
    ~ExtendPublisher();
    ExtendPublisher(const ExtendPublisher&) = delete;
    ExtendPublisher& operator=(const ExtendPublisher&) = delete;

    // Registers every ExtendEvents callback (once, after the window is created).
    void wire();
    // Reads the models and sets the ones that changed (once a tick, BEFORE the kit's publish so the store page is built from this tick's model).
    void publish(const FoxReceiverState& state);
    // An answer main.cpp polled: the ones this object waits for (a SERVER_CONFIG's) are settled, the rest are not its business.
    void onAnswer(const FoxCommandResult& r);

    // The chip a row of bank 3 (EXTEND) shows: row 0 Web access, row 1 CAT; `valid` is false before the first read.
    struct RowFace {
        bool valid = false;
        std::string chip;
        ::ChipTone tone = ::ChipTone::Neutral;
    };
    RowFace face(int row) const;

    // A bounded run's self-capture (FOXSDR_SLINT_STORE_PAGE=<catalogue id>): once the catalogue lists the id, the store page opens on that plugin - the page only; the
    // pictures it would ask for are not (a bounded run asks nothing of the network).
    void openStorePageForCapture(const std::string& id);
    // A bounded run's self-capture of a CATALOGUE with no network (FOXSDR_SLINT_STORE_LOCAL=<path to an index.json>): the one case where a bounded run asks the store for anything.
    // The path must be a local file (no "://"), which the engine reads from the disk and keeps its own copy of: the catalogue address is set to it and the catalogue read once, when
    // the page first shows. Anything else is refused with a line.
    void useLocalCatalogueForCapture(const std::string& path);
    // A bounded run's self-capture of the screenshots' worker (FOXSDR_SLINT_EXTEND_PROBE=<file>;<file>...): each file is run through the decode as a catalogue picture and the
    // strip's mapping is read back at each index - the result is one line in the summary.
    void probePictures(const std::vector<std::string>& paths);
    // A bounded run's evidence: the web and CAT status words, the catalogue line, the card count and the open page.
    std::string summaryLine() const;
    const ExtendModels& models() const { return last_; }
    std::size_t commandsSent() const { return sent_; }

private:
    struct Lists;
    struct Pictures;
    struct Pending;
    struct Last;

    // ---- commands and effects ----
    std::uint64_t send(const Command& c);
    void exec(Effect e, const char* what);
    void applyLocal(const Effect& e);
    double nowS() const;
    void say(const std::string& line) const;

    // ---- the reads ----
    ReadOptions buildOptions(const FoxReceiverState& state, bool readStore);
    bool storeReadDue(const FoxReceiverState& state, std::int64_t nowMs) const;
    void pollPictures();
    void wantPictures(const StorePluginPage& page);
    void runPictureProbe();

    // ---- the models ----
    ::WebModel mapWeb(const WebModel& m);
    ::CatModel mapCat(const CatModel& m);
    ::StoreModel mapStore(const StoreModel& m);
    std::shared_ptr<slint::Model<slint::Image>> mapScreenshots(const StorePluginPage& p, std::string& note);

    MainWindow& ui_;
    foxsdr::slintkit::PluginKit& kit_;
    foxsdr::slintkit::KitPublisher& pages_;
    const FoxEngineApi* api_;
    FoxSession* session_;
    bool bounded_;
    foxsdr::slintkit::LiveTable table_;

    ReadOptions opts_;                         // the drafts (web, cat, store) and the asks pending
    ExtendModels last_;                        // the last read; the store half only when it was read
    StoreFirstOpenState firstOpen_;
    ConsentWatch consents_;
    bool haveModels_ = false;
    bool haveStore_ = false;                   // last_.store was read (the page has been up)
    bool forceRead_ = true;
    bool storeForce_ = true;
    std::int64_t lastReadMs_ = 0, lastStoreReadMs_ = 0;
    std::uint64_t storeSeq_ = ~std::uint64_t{0};
    bool pageWasUp_ = false, shownWas_ = false;
    bool openedPending_ = false;                // the page was opened and has not been read yet: the first read asks the catalogue once (storeShouldCheckOnOpen)
    int captureWaits_ = 0;
    std::string localCatalogue_;               // FOXSDR_SLINT_STORE_LOCAL
    int localStep_ = 0, localWait_ = 0;        // 0 waiting for the page to show, 1 the address set, 2 the read asked
    std::vector<std::string> probePaths_;
    std::string probeLine_;
    bool probeDone_ = false;
    std::uint64_t picturesAskedSeq_ = ~std::uint64_t{0};
    std::uint64_t listSeq_ = 0;
    std::int64_t nowUnix_ = 0;
    int pictureIndex_ = 0;                     // the screenshot the strip shows (the page keeps it; store-picture moves it)
    std::string pictureIndexPage_;             // the plugin page it belongs to
    std::string captureId_;                    // FOXSDR_SLINT_STORE_PAGE
    std::size_t sent_ = 0, withheld_ = 0;

    std::unique_ptr<Lists> lists_;
    std::unique_ptr<Pictures> pictures_;
    std::unique_ptr<Pending> pending_;
    std::unique_ptr<Last> lastSet_;
};

}  // namespace foxsdr::slintextend
