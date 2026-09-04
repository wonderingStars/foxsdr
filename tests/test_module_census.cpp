// Tests for gui/module_census.hpp - the census of what this machine holds, and
// the sentence a surface prints when it is holding none of it.
//
// WHY THIS FILE EXISTS AT ALL. These functions decided what a user was TOLD
// while living in an anonymous namespace inside a 12,000-line translation unit,
// where no test could reach a single one of them. Everything below was
// unreachable a change ago; the move is what makes it assertable.
//
// THE FAULT BEING PINNED, which this product has now shipped and repaired five
// times. A surface asks a live object for its list, finds it empty, and prints
// "nothing is installed" - but PluginUi::rebuild and PluginRunner::rebuild both
// SKIP a module that did not load and one the user stopped before writing
// anything, so an empty list is exactly as empty on a machine with nothing in
// the folder as on one whose module was REFUSED and one whose module the user
// STOPPED. In the last two the product told a user who already owns the module
// to go and buy it, while the Fitted modules window one key away lettered that
// same file REFUSED or STOPPED BY YOU.
//
// WHAT EACH GROUP OF CHECKS CAN ACTUALLY CATCH.
//
//   THE CENSUS. Each of the four counters is asserted on a record built to be
//   in exactly that state, and - the ones that matter - on records built to be
//   in a NEIGHBOURING state, because the whole failure mode is one state being
//   read as another. A refused record's tables are null, so a census that
//   tested the table rather than the capability word would count it in no state
//   at all; a stopped record's tables are NOT null, so a census that forgot the
//   stop set would count it live.
//
//   THE ORDER OF THE ANSWERS. stopped, refused, live, absent, in both notes.
//   The order is the design: the user's own choice is not a fault, and it is
//   the one state where the module they need is already on the disk. A record
//   in two states at once must letter the earlier one.
//
//   THE CASE THAT STARTED ALL THIS: a module present, loaded, and stopped. The
//   note it produces must name the module, must say the user stopped it, and
//   MUST NOT contain the word "install" anywhere - not "no decoder is
//   installed", not an invitation to install one. That check is the whole point
//   of the change and it is asserted mechanically rather than by reading.
//
//   THE REMEDY CLAUSE, pinned with a SENTINEL rather than by eye. The
//   track-source note takes the "where the store is from here" clause as a
//   parameter; passing a string that appears nowhere in the product and
//   asserting it comes back in the absent case and in NO other case proves the
//   rule the header states - only the last branch mentions fitting anything.
//
//   ABSENT IS NOT KNOWN. A file the host refused before it could read the
//   descriptor has no capability word, so nothing can say what it was. The
//   notes must carry that clause rather than claim the folder holds no decoder.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_host.hpp"
#include "gui/module_census.hpp"
#include "test_check.hpp"

using cascade::core::LoadedPlugin;
using cascade::gui::censusModules;
using cascade::gui::decoderAbsenceNote;
using cascade::gui::kDecoderCaps;
using cascade::gui::ModuleCensus;
using cascade::gui::moduleProvides;
using cascade::gui::trackSourceAbsenceNote;

namespace {

// Tables the census only ever tests for null. Zero-initialised: nothing here
// calls through them.
const CascadeDecoderApi kAudioApi{};
const CascadeIqDecoderApi kIqApi{};
const CascadeImageDecoderApi kImageApi{};
const CascadeTrackSourceApi kTrackApi{};
const CascadeBasemapApi kBasemapApi{};
const CascadePanelApi kPanelApi{};

// A module the host LOADED. Tables are attached by the caller.
LoadedPlugin loadedRecord(const char* path, const char* name, std::uint32_t caps) {
    LoadedPlugin p;
    p.path = path;
    p.name = name;
    p.capabilities = caps;
    p.loaded = true;
    return p;
}

// A module the host FOUND, read the descriptor of, and REFUSED - the duplicate
// resolver's outcome. Every borrowed table is null and the capability word it
// read survives, which is exactly why the census must read the word here.
LoadedPlugin refusedRecord(const char* path, const char* name, std::uint32_t caps) {
    LoadedPlugin p;
    p.path = path;
    p.name = name;
    p.capabilities = caps;
    p.loaded = false;
    p.error = "a newer copy of this plugin was kept instead";
    return p;
}

// A file refused before a descriptor was ever accepted: no name, no capability
// word, so NOTHING can say what it was.
LoadedPlugin unreadRecord(const char* path) {
    LoadedPlugin p;
    p.path = path;
    p.loaded = false;
    p.error = "expected ABI 3, plugin reports 2";
    return p;
}

// The stop set as the application supplies it: a predicate over module FILE
// names, never display names.
cascade::gui::ModuleStoppedFn stopSet(std::vector<std::string> keys) {
    return [keys](const std::string& file) {
        for (const std::string& k : keys) {
            if (k == file) { return true; }
        }
        return false;
    };
}

cascade::gui::ModuleStoppedFn nothingStopped() {
    return [](const std::string&) { return false; };
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

// Case-insensitive, so "Install" and "installed" are caught as surely as
// "install". The claim being pinned is about the WORD, not one spelling of it.
bool mentionsInstalling(const std::string& s) {
    std::string lower;
    lower.reserve(s.size());
    for (char c : s) {
        lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    return lower.find("install") != std::string::npos;
}

// A remedy string that appears nowhere in the product, so its presence in a
// note can only have come from the absent branch.
const char* const kRemedySentinel = "REMEDY-SENTINEL-9f3c";

}  // namespace

int main() {
    // --- moduleProvides: the table, and only for a record that loaded -------
    {
        LoadedPlugin p = loadedRecord("C:/p/adsb-1.0.0.dll", "ADS-B", CASCADE_CAP_TRACK_SOURCE);
        p.trackSource = &kTrackApi;
        CHECK(moduleProvides(p, CASCADE_CAP_TRACK_SOURCE));
        CHECK(!moduleProvides(p, kDecoderCaps));
        CHECK(!moduleProvides(p, CASCADE_CAP_BASEMAP));
        // A mask covering several bits answers for ANY of them.
        CHECK(moduleProvides(p, CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_BASEMAP));

        // THE WORD IS NOT THE TABLE. A record that did not load has every
        // borrowed pointer nulled, so this must be false however the
        // capability word reads - censusModules reads the word for those.
        LoadedPlugin r = refusedRecord("C:/p/adsb-1.0.0.dll", "ADS-B", CASCADE_CAP_TRACK_SOURCE);
        CHECK(!moduleProvides(r, CASCADE_CAP_TRACK_SOURCE));
    }
    // Each of the three decoder tables satisfies the decoder mask on its own.
    {
        LoadedPlugin a = loadedRecord("C:/p/a.dll", "A", CASCADE_CAP_DECODER);
        a.decoder = &kAudioApi;
        LoadedPlugin b = loadedRecord("C:/p/b.dll", "B", CASCADE_CAP_IQ_DECODER);
        b.iqDecoder = &kIqApi;
        LoadedPlugin c = loadedRecord("C:/p/c.dll", "C", CASCADE_CAP_IMAGE_DECODER);
        c.imageDecoder = &kImageApi;
        CHECK(moduleProvides(a, kDecoderCaps));
        CHECK(moduleProvides(b, kDecoderCaps));
        CHECK(moduleProvides(c, kDecoderCaps));
        // And a module that is emphatically not a decoder satisfies none of it.
        LoadedPlugin m = loadedRecord("C:/p/map.dll", "Basemap", CASCADE_CAP_BASEMAP);
        m.basemap = &kBasemapApi;
        CHECK(!moduleProvides(m, kDecoderCaps));
    }

    // --- the census, state by state ----------------------------------------

    // THE EMPTY CASE. No records at all: every counter zero, both names empty.
    {
        const ModuleCensus c = censusModules({}, kDecoderCaps, nothingStopped());
        CHECK(c.live == 0);
        CHECK(c.stopped == 0);
        CHECK(c.refused == 0);
        CHECK(c.unread == 0);
        CHECK(c.stoppedName.empty());
        CHECK(c.refusedName.empty());
    }

    // LIVE: loaded, carrying the table, not in the stop set.
    {
        LoadedPlugin p = loadedRecord("C:/p/adsb-1.0.0.dll", "ADS-B", CASCADE_CAP_DECODER);
        p.decoder = &kAudioApi;
        const ModuleCensus c = censusModules({p}, kDecoderCaps, nothingStopped());
        CHECK(c.live == 1);
        CHECK(c.stopped == 0);
        CHECK(c.refused == 0);
        CHECK(c.unread == 0);
    }

    // STOPPED: the same record, in the stop set. THE CASE THAT STARTED ALL
    // THIS. Keyed on the FILE NAME, never the display name.
    {
        LoadedPlugin p = loadedRecord("C:/p/adsb-1.0.0.dll", "ADS-B", CASCADE_CAP_DECODER);
        p.decoder = &kAudioApi;
        const ModuleCensus c =
            censusModules({p}, kDecoderCaps, stopSet({"adsb-1.0.0.dll"}));
        CHECK(c.stopped == 1);
        CHECK(c.live == 0);
        CHECK(c.stoppedName == "ADS-B");
        // The display name must NOT be what the stop set is consulted with: a
        // stop set keyed on "ADS-B" must stop nothing.
        const ModuleCensus byName = censusModules({p}, kDecoderCaps, stopSet({"ADS-B"}));
        CHECK(byName.stopped == 0);
        CHECK(byName.live == 1);
    }

    // REFUSED: not loaded, tables null, capability word intact.
    {
        const LoadedPlugin p =
            refusedRecord("C:/p/adsb-1.0.0.dll", "ADS-B", CASCADE_CAP_DECODER);
        const ModuleCensus c = censusModules({p}, kDecoderCaps, nothingStopped());
        CHECK(c.refused == 1);
        CHECK(c.live == 0);
        CHECK(c.unread == 0);
        CHECK(c.refusedName == "ADS-B");
    }

    // UNREAD: not loaded and no capability word. Not refused - nothing knows
    // it was a decoder - and emphatically not absent.
    {
        const ModuleCensus c =
            censusModules({unreadRecord("C:/p/broken.dll")}, kDecoderCaps, nothingStopped());
        CHECK(c.unread == 1);
        CHECK(c.refused == 0);
        CHECK(c.live == 0);
        CHECK(c.stopped == 0);
    }

    // A REFUSED MODULE OF ANOTHER KIND IS NEITHER refused NOR unread HERE. Its
    // word says basemap, so this census knows it was not a decoder.
    {
        const ModuleCensus c =
            censusModules({refusedRecord("C:/p/map-2.dll", "Basemap", CASCADE_CAP_BASEMAP)},
                          kDecoderCaps, nothingStopped());
        CHECK(c.refused == 0);
        CHECK(c.unread == 0);
        CHECK(c.live == 0);
    }

    // A LOADED MODULE OF ANOTHER KIND IS NOT LIVE HERE either, stopped or not.
    {
        LoadedPlugin m = loadedRecord("C:/p/map-2.dll", "Basemap", CASCADE_CAP_BASEMAP);
        m.basemap = &kBasemapApi;
        const ModuleCensus c = censusModules({m}, kDecoderCaps, stopSet({"map-2.dll"}));
        CHECK(c.live == 0);
        CHECK(c.stopped == 0);
    }

    // NO NAME IN THE DESCRIPTOR: the file name stands in, because a note that
    // quotes an empty string names nothing.
    {
        LoadedPlugin p = loadedRecord("C:/p/nameless-1.0.0.dll", "", CASCADE_CAP_DECODER);
        p.decoder = &kAudioApi;
        const ModuleCensus c =
            censusModules({p}, kDecoderCaps, stopSet({"nameless-1.0.0.dll"}));
        CHECK(c.stoppedName == "nameless-1.0.0.dll");
    }

    // FIRST IN SCAN ORDER wins the one name each note has room for, and the
    // counters still see both.
    {
        LoadedPlugin a = loadedRecord("C:/p/first-1.0.0.dll", "First", CASCADE_CAP_DECODER);
        a.decoder = &kAudioApi;
        LoadedPlugin b = loadedRecord("C:/p/second-1.0.0.dll", "Second", CASCADE_CAP_IQ_DECODER);
        b.iqDecoder = &kIqApi;
        const ModuleCensus c = censusModules(
            {a, b}, kDecoderCaps, stopSet({"first-1.0.0.dll", "second-1.0.0.dll"}));
        CHECK(c.stopped == 2);
        CHECK(c.stoppedName == "First");
    }

    // A NULL PREDICATE CANNOT SAY, so nothing is counted live off a stop set
    // nobody supplied.
    {
        LoadedPlugin p = loadedRecord("C:/p/adsb-1.0.0.dll", "ADS-B", CASCADE_CAP_DECODER);
        p.decoder = &kAudioApi;
        const ModuleCensus c = censusModules({p}, kDecoderCaps, cascade::gui::ModuleStoppedFn());
        CHECK(c.live == 0);
        CHECK(c.stopped == 0);
    }

    // ONE MACHINE, ALL FOUR STATES AT ONCE - the mixture a real plugin folder
    // produces after an upgrade the user has half tidied up.
    {
        LoadedPlugin live = loadedRecord("C:/p/live-1.dll", "Live", CASCADE_CAP_DECODER);
        live.decoder = &kAudioApi;
        LoadedPlugin stopped = loadedRecord("C:/p/stopped-1.dll", "Stopped", CASCADE_CAP_DECODER);
        stopped.decoder = &kAudioApi;
        const ModuleCensus c = censusModules(
            {live, stopped, refusedRecord("C:/p/old-1.dll", "Old", CASCADE_CAP_IQ_DECODER),
             unreadRecord("C:/p/junk.dll")},
            kDecoderCaps, stopSet({"stopped-1.dll"}));
        CHECK(c.live == 1);
        CHECK(c.stopped == 1);
        CHECK(c.refused == 1);
        CHECK(c.unread == 1);
        CHECK(c.stoppedName == "Stopped");
        CHECK(c.refusedName == "Old");
    }

    // --- the decoder note, state by state ----------------------------------

    // STOPPED. THE SENTENCE THIS WHOLE CHANGE EXISTS TO PRODUCE. It must name
    // the module, say the user stopped it, letter it in the Fitted modules
    // window's own word - and contain the word "install" NOWHERE, in any
    // spelling: not "no decoder is installed", not "install one".
    {
        ModuleCensus c;
        c.stopped = 1;
        c.stoppedName = "ADS-B";
        const std::string s = decoderAbsenceNote(c);
        CHECK(contains(s, "\"ADS-B\""));
        CHECK(contains(s, "you stopped it"));
        CHECK(contains(s, "STOPPED BY YOU"));
        CHECK(!mentionsInstalling(s));
        CHECK(!contains(s, "Plugin store"));
        // And it may not claim the module decodes any particular thing: the
        // ABI's capability word says a module decodes, never what.
        CHECK(contains(s, "carries a decoder"));
    }

    // STOPPED, MORE THAN ONE: counted rather than named, and the same rule.
    {
        ModuleCensus c;
        c.stopped = 3;
        c.stoppedName = "ADS-B";
        const std::string s = decoderAbsenceNote(c);
        CHECK(contains(s, "3 fitted decoders are stopped"));
        CHECK(!mentionsInstalling(s));
    }

    // STOPPED WITH NO NAME AT ALL still counts rather than quoting an empty
    // string.
    {
        ModuleCensus c;
        c.stopped = 1;
        const std::string s = decoderAbsenceNote(c);
        CHECK(contains(s, "1 fitted decoder is stopped"));
        CHECK(!contains(s, "\"\""));
    }

    // REFUSED: the host's word, a pointer at the verbatim reason, and nothing
    // to fetch.
    {
        ModuleCensus c;
        c.refused = 1;
        c.refusedName = "POCSAG";
        const std::string s = decoderAbsenceNote(c);
        CHECK(contains(s, "\"POCSAG\""));
        CHECK(contains(s, "the host refused it"));
        CHECK(contains(s, "REFUSED"));
        CHECK(contains(s, "verbatim"));
        CHECK(!mentionsInstalling(s));
        CHECK(!contains(s, "Plugin store"));
    }
    {
        ModuleCensus c;
        c.refused = 2;
        const std::string s = decoderAbsenceNote(c);
        CHECK(contains(s, "2 fitted decoders were refused"));
        CHECK(!contains(s, "\"\""));
        CHECK(!mentionsInstalling(s));
    }

    // LIVE: a decoder is fitted, loaded and not stopped, and the runner's list
    // does not have it. Nothing is wrong with the module and nothing needs
    // fetching, so this branch must not send anyone to the store either.
    {
        ModuleCensus c;
        c.live = 1;
        const std::string s = decoderAbsenceNote(c);
        CHECK(contains(s, "a fitted decoder is not in the receiver's decoder list"));
        CHECK(!mentionsInstalling(s));
        CHECK(!contains(s, "Plugin store"));
    }

    // ABSENT: the ONLY branch that may send the user anywhere.
    {
        const ModuleCensus c;
        const std::string s = decoderAbsenceNote(c);
        CHECK(contains(s, "No fitted module carries a decoder"));
        CHECK(contains(s, "Plugin store"));
    }

    // ABSENT, WITH A FILE NOBODY COULD READ. Absent is not known: the note must
    // say so rather than claim the folder holds no decoder.
    {
        ModuleCensus c;
        c.unread = 1;
        const std::string one = decoderAbsenceNote(c);
        CHECK(contains(one, "1 file in the plugin folder was refused"));
        CHECK(contains(one, "read what it was"));
        c.unread = 4;
        const std::string many = decoderAbsenceNote(c);
        CHECK(contains(many, "4 files in the plugin folder were refused"));
        CHECK(contains(many, "read what they were"));
    }

    // THE ORDER OF THE ANSWERS. A machine in several states at once letters
    // the earliest: stopped, then refused, then live, then absent.
    {
        ModuleCensus c;
        c.stopped = 1;
        c.stoppedName = "Stopped";
        c.refused = 1;
        c.refusedName = "Refused";
        c.live = 1;
        c.unread = 1;
        CHECK(contains(decoderAbsenceNote(c), "you stopped it"));
        c.stopped = 0;
        c.stoppedName.clear();
        CHECK(contains(decoderAbsenceNote(c), "the host refused it"));
        c.refused = 0;
        c.refusedName.clear();
        CHECK(contains(decoderAbsenceNote(c), "not in the receiver's decoder list"));
        c.live = 0;
        CHECK(contains(decoderAbsenceNote(c), "No fitted module carries a decoder"));
    }

    // --- the track-source note, in the same four states --------------------
    //
    // The remedy SENTINEL is the pin: it must come back in the absent case and
    // in no other, which is the rule the header states about both notes.
    {
        ModuleCensus c;
        c.stopped = 1;
        c.stoppedName = "ADS-B";
        const std::string s = trackSourceAbsenceNote(c, "aircraft positions", kRemedySentinel);
        CHECK(contains(s, "Nothing is publishing aircraft positions"));
        CHECK(contains(s, "\"ADS-B\" is a track source and you stopped it"));
        CHECK(!contains(s, kRemedySentinel));
        CHECK(!mentionsInstalling(s));
    }
    {
        ModuleCensus c;
        c.refused = 1;
        c.refusedName = "Sat tracker";
        const std::string s =
            trackSourceAbsenceNote(c, "satellite positions", kRemedySentinel);
        CHECK(contains(s, "\"Sat tracker\" is a track source and the host refused it"));
        CHECK(contains(s, "verbatim"));
        CHECK(!contains(s, kRemedySentinel));
        CHECK(!mentionsInstalling(s));
    }
    {
        ModuleCensus c;
        c.live = 1;
        const std::string s = trackSourceAbsenceNote(c, "aircraft positions", kRemedySentinel);
        CHECK(contains(s, "a fitted track source did not start"));
        CHECK(!contains(s, kRemedySentinel));
        CHECK(!mentionsInstalling(s));
    }
    {
        const ModuleCensus c;
        const std::string s = trackSourceAbsenceNote(c, "aircraft positions", kRemedySentinel);
        CHECK(contains(s, "No fitted module publishes tracks of any kind"));
        CHECK(contains(s, "there are no aircraft positions here"));
        CHECK(contains(s, kRemedySentinel));
    }
    {
        ModuleCensus c;
        c.unread = 2;
        const std::string s = trackSourceAbsenceNote(c, "aircraft positions", kRemedySentinel);
        CHECK(contains(s, "2 files in the plugin folder were refused"));
        CHECK(contains(s, kRemedySentinel));
    }
    // NO MODULE MAY BE SAID TO PUBLISH A PARTICULAR SUBJECT. The ABI gives a
    // module one capability bit for tracks and puts the kind on each TRACK, so
    // a stopped tracker asked about SATELLITE positions must be called a track
    // source and nothing more - never "the module that publishes satellites".
    {
        ModuleCensus c;
        c.stopped = 1;
        c.stoppedName = "ADS-B";
        const std::string s =
            trackSourceAbsenceNote(c, "satellite positions", kRemedySentinel);
        CHECK(contains(s, "\"ADS-B\" is a track source"));
        CHECK(!contains(s, "\"ADS-B\" publishes satellite"));
        CHECK(!contains(s, "ADS-B\" is a satellite"));
    }

    // --- the whole path, from records to sentence --------------------------
    //
    // THE FIFTH INSTANCE, END TO END. One decoder on the disk, loaded, and
    // stopped by the user: the sentence the Decoders section prints must not
    // tell them nothing is installed.
    {
        LoadedPlugin p = loadedRecord("C:/p/adsb-1.2.0.dll", "ADS-B", CASCADE_CAP_DECODER);
        p.decoder = &kAudioApi;
        const std::string s = decoderAbsenceNote(
            censusModules({p}, kDecoderCaps, stopSet({"adsb-1.2.0.dll"})));
        CHECK(contains(s, "\"ADS-B\""));
        CHECK(contains(s, "you stopped it"));
        CHECK(!mentionsInstalling(s));
    }
    // And the same module REFUSED, from the records the host would hand over.
    {
        const std::string s = decoderAbsenceNote(
            censusModules({refusedRecord("C:/p/adsb-1.2.0.dll", "ADS-B", CASCADE_CAP_DECODER)},
                          kDecoderCaps, nothingStopped()));
        CHECK(contains(s, "\"ADS-B\""));
        CHECK(contains(s, "the host refused it"));
        CHECK(!mentionsInstalling(s));
    }
    // And a machine that really does hold nothing, which is the ONE case the
    // old sentence was right about.
    {
        const std::string s =
            decoderAbsenceNote(censusModules({}, kDecoderCaps, nothingStopped()));
        CHECK(contains(s, "No fitted module carries a decoder"));
        CHECK(contains(s, "Plugin store"));
    }
    // A basemap-only machine holds no decoder either - and the note must say
    // that rather than "no decoder is running", which would put an idle
    // decoder on a machine that has none.
    {
        LoadedPlugin m = loadedRecord("C:/p/map-1.dll", "Basemap", CASCADE_CAP_BASEMAP);
        m.basemap = &kBasemapApi;
        LoadedPlugin panel = loadedRecord("C:/p/panel-1.dll", "Panel", CASCADE_CAP_PANEL);
        panel.panel = &kPanelApi;
        const ModuleCensus c = censusModules({m, panel}, kDecoderCaps, nothingStopped());
        CHECK(c.live == 0);
        CHECK(c.stopped == 0);
        CHECK(c.refused == 0);
        CHECK(c.unread == 0);
        CHECK(contains(decoderAbsenceNote(c), "No fitted module carries a decoder"));
    }

    return testSummary("test_module_census");
}
