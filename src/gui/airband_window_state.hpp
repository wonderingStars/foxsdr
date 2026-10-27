// airband_window_state.hpp - what the AIRBAND section keeps in the window (stub campaign B7, docs/engine-stage4.md 6.22).
//
// The monitor itself - the blocks, the squelch, the scan between them, the heard time, the hand-back from the patch view - is the ENGINE's
// (src/engine/engine_airband.cpp, B6). The window keeps only what a window has: the text fields the user is typing in, the group the list
// below them shows, the notes it has said, the airports a lookup offered, the answers it is waiting for, and a copy of what the engine
// published - the facts (read again when listSeq moves) and the live channels (AIRBAND_CHANNELS, read once a frame, NEVER cached on
// listSeq: a channel's level and open lamp change every pass and the list moves no listSeq at all).
//
// One struct rather than forty members so app_window.hpp (and with it every translation unit that includes it) grows by a handful of
// lines; the members that act on it are in app_window_airband.cpp. The disk job (the file read for an Import into a preset) is behind an
// opaque pointer for the reason app_window_disk_state.hpp gives: its DiskJob instantiation belongs in one file. (The preset's Export CSV was a
// second job here until stub campaign B19: the ENGINE writes it now, from its store, on a worker of its own - FOXAPP_OP_BOOKMARK_EXPORT.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_AIRBAND_WINDOW_STATE_HPP
#define CASCADE_GUI_AIRBAND_WINDOW_STATE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/airband_data.hpp"
#include "core/freq_import.hpp"

namespace cascade::gui {

struct AirbandJobs;  // defined in app_window_airband.cpp

// One entry of AIRBAND_CHANNELS: a channel of the plan (not only the block on the air), as the engine published it this frame.
struct AirbandChannel {
    double freqHz = 0.0;
    double bandwidthHz = 0.0;
    float levelDb = -200.0f;       // the level its squelch judges; -200 until it has reported one
    double pendingHeardS = 0.0;    // heard since the monitor last wrote the row's total to the list: add it to the row's own figure
    bool open = false;             // FOXAPI_ITEM_ACTIVE: its squelch is open now
    bool onAir = false;            // FOXAPI_ITEM_ON_AIR: it is in the block that is tuned
    std::string name;              // the first ticked row on its frequency
};

// fact.airband.*, read when listSeq moves.
struct AirbandFacts {
    std::uint64_t listSeq = 0;     // 0: never read
    bool listening = false;
    bool pending = false;          // a LISTEN is waiting for the patch view to hand the radio back
    std::string preset;            // the preset on the air ("" = every ticked AM or NFM row)
    std::string note;              // what the monitor last said (a stop's reason, a refused LISTEN)
    std::string block;             // "Block 2 of 6: ..." / "All 7 channels at once, ..."; "" when not listening
    int blocks = 0;
    float squelchDb = -55.0f;
    double holdS = 2.0;
};

// The rows the section lists, as indexes into the window's one copy of BOOKMARKS (tuningLists().markers), rebuilt only when that copy, the
// group on show or the preset named in the field changed.
struct AirbandView {
    std::uint32_t copies = ~0u;    // tuningCache_.markerCopies it was built from
    std::string group;             // the group on show it was built for
    std::string preset;            // the preset named in the field it was built for
    std::vector<std::string> groups;       // the groups that hold a row the monitor plays, sorted
    std::vector<std::uint32_t> rows;       // the rows listed: that group's AM and NFM rows, or every ticked one
    std::size_t presetRows = 0;            // every row of the preset named in the field, whatever its mode or tick
};

// One command the window sent and is waiting to hear the answer to.
struct AirbandAsk {
    enum class Kind { TypedRow, AirportRows, ImportRows, RemoveGroup, Listen, ExportPreset };
    Kind kind = Kind::TypedRow;
    std::uint64_t ticket = 0;
    std::string group;             // the preset / the airport's group the ask is about
    std::size_t total = 0;         // an airport's rows, as sent
    std::size_t ticked = 0;        // ...of which ticked
};

// The Import into a preset that is out: what the file held, and the rows still to send (a long list goes in chunks of at most
// FOXAPI_MAX_LONG_TEXT).
struct AirbandImportRun {
    bool active = false;
    std::string group;
    std::size_t found = 0;
    std::size_t playable = 0;
    std::size_t skipped = 0;
    std::size_t shifted = 0;
    std::size_t added = 0;         // rows the engine added so far
    std::size_t chunksOut = 0;     // chunks sent and not yet answered
    std::vector<std::string> queue;  // chunks not yet sent, in order
    std::size_t next = 0;
    bool refused = false;
};

struct AirbandWindowState {
    // --- what the user types and chooses -----------------------------------------------------------------------------------------
    char code[16] = {};
    char addMhz[32] = {};
    char addName[96] = {};
    int addMode = 0;                         // 0 AM, 1 NFM
    char presetName[128] = "Manual";         // what a chosen group puts here
    char importPath[512] = {};
    bool busyFirst = false;
    std::string group;                       // the group on show; "" = every ticked AM or NFM row
    // --- what the section has said ---------------------------------------------------------------------------------------------------
    std::string note;                        // the lookup / add / monitor note under the add row
    std::string noteSeen;                    // fact.airband.note as last taken over
    std::string presetNote;                  // the preset's file keys' note
    bool presetFolderKey = false;            // a file was written: offer the key that opens its folder
    std::vector<const cascade::core::Airport*> choices;                       // a code that meant several airports
    std::vector<std::pair<const cascade::core::Airport*, double>> nearest;   // the airports nearest the receiver, with km
    // --- what the engine published ---------------------------------------------------------------------------------------------------
    AirbandFacts facts;
    AirbandView view;
    std::vector<AirbandChannel> live;        // AIRBAND_CHANNELS this frame; only the first `liveCount` are valid (the rows keep their room)
    std::size_t liveCount = 0;
    int liveFrame = -1;                      // the ImGui frame `live` was read in
    // --- the monitor's two settings, while the hand is on them -------------------------------------------------------------------
    float squelchEdit = -55.0f;
    bool squelchHeld = false;                // the slider shows squelchEdit until the engine's figure catches up
    double holdEdit = 2.0;
    bool holdHeld = false;
    // --- edges and answers -----------------------------------------------------------------------------------------------------------
    bool wasWanting = false;                 // fact.airband.listening or .pending last frame (the view switch is made on the rising edge)
    std::vector<AirbandAsk> asks;
    AirbandImportRun import;
    std::string importAgain;                 // an Import pressed while one was being read: the path, read when the first is back
    cascade::core::ImportInto importAgainInto;   // ...and the preset (and tick) it was pressed for
    bool exportWaiting = false;              // an Export CSV was accepted and its file is being written (the engine says when: fact.bookmarks.export.*)
    std::uint64_t exportSeqSeen = 0;         // fact.bookmarks.export.seq as last taken over
    std::shared_ptr<AirbandJobs> jobs;       // made on first use
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_AIRBAND_WINDOW_STATE_HPP
