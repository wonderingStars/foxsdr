// test_plugin_ui_data_split.cpp - THE VIEWS DRAW PLUGIN DATA WITHOUT THE MACHINERY THAT FILLS IT (window port group 14, part d;
// the fence ledger of docs/engine-stage4.md).
//
// core/plugin_ui.hpp holds two different things: the DATA the plugin windows draw (HostTrack, HostPath and the visibility rule
// they are drawn by, HostPanel, HostInstrument, MutePreset, PluginWindows) and the MACHINERY that fills it (PluginUi, the host
// table a plugin calls, the bridges, the mute policy). Six gui headers included it for the data. The fence cannot keep
// plugin_ui.hpp out of the interface target while they do, so the data went to core/plugin_ui_data.hpp - a pure move - and the
// headers the interface draws from take that one.
//
// THE COMPILE PROOF IS THIS FILE'S INCLUDES. It includes the five of those headers that do not reach the engine and then
// refuses to compile if core/plugin_ui.hpp came in with any of them (its include guard is defined). The sixth, app_window.hpp,
// is the shell and still reaches plugin_ui.hpp through engine/engine.hpp, so test_command_path_guard holds the text of its
// include instead. Everything below then uses the data through the data header alone, so a name that stayed behind in
// plugin_ui.hpp is a compile error here too.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_ui_data.hpp"
#include "gui/decoder_reads.hpp"
#include "gui/instrument_face.hpp"
#include "gui/map_view.hpp"
#include "gui/scope_view.hpp"
#include "gui/track_detail_view.hpp"

#include "test_check.hpp"

#include <cstdio>
#include <string>
#include <vector>

#ifdef CASCADE_CORE_PLUGIN_UI_HPP
#error "a view header pulled in core/plugin_ui.hpp - the interface may see core/plugin_ui_data.hpp only (group 14 part d)"
#endif

namespace core = cascade::core;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_plugin_ui_data_split\n");

    // The compile proof, restated at run time so a report shows it: the guard macro of the machinery header is not defined.
#ifdef CASCADE_CORE_PLUGIN_UI_HPP
    CHECK(false);
#else
    CHECK(true);
#endif
    std::printf("  the five view headers and the data header compile without core/plugin_ui.hpp\n");

    // A pure move: the structs keep every default they had.
    {
        const core::HostTrack t;
        CHECK(t.plugin.empty());
        CHECK(t.t.latDeg == 0.0);
        const core::HostPath p;
        CHECK(p.id.empty() && p.plugin.empty() && p.kind == 0u && p.flags == 0u && p.points.empty());
        const core::HostPanel pn;
        CHECK(pn.plugin.empty() && pn.title.empty() && pn.headings.empty() && pn.rows.empty());
        const core::HostInstrument in;
        CHECK(in.plugin.empty() && in.title.empty() && in.kind == 0u && !in.have && in.headings.empty() && in.rows.empty());
        CHECK(in.state.seq == 0u && in.state.flags == 0u);
        const core::MutePreset mp;
        CHECK(mp.frequencyHz == 0.0 && mp.bandwidthHz == 0.0 && !mp.deviceCentre && mp.index == 0u && mp.label.empty());
        const core::PluginWindows w;
        CHECK(w.count() == 0u && !w.shown("anything"));
    }

    // The visibility rule the views draw by is the data header's: a fresh aircraft is shown at full strength, a stale one is
    // not, the count agrees with the rule, and a trail takes its owner's verdict.
    {
        CHECK(core::trackPresentation(1000u, CASCADE_TRACK_AIRCRAFT).visible);
        CHECK(core::trackPresentation(1000u, CASCADE_TRACK_AIRCRAFT).alpha == 1.0f);
        CHECK(!core::trackPresentation(120000u, CASCADE_TRACK_AIRCRAFT).visible);
        CHECK(core::kTrackMinAlpha > 0.0f && core::kTrackMinAlpha < 1.0f);

        std::vector<core::HostTrack> tracks(3);
        tracks[0].plugin = "A";
        std::snprintf(tracks[0].t.id, CASCADE_TRACK_ID_CHARS, "one");
        tracks[0].t.kind = CASCADE_TRACK_AIRCRAFT;
        tracks[0].t.ageMs = 1000u;
        tracks[1].plugin = "A";
        std::snprintf(tracks[1].t.id, CASCADE_TRACK_ID_CHARS, "two");
        tracks[1].t.kind = CASCADE_TRACK_AIRCRAFT;
        tracks[1].t.ageMs = 90000u;  // past the 60 s an aircraft is kept
        tracks[2].plugin = "A";
        std::snprintf(tracks[2].t.id, CASCADE_TRACK_ID_CHARS, "three");
        tracks[2].t.kind = CASCADE_TRACK_AIRCRAFT;
        tracks[2].t.ageMs = 59000u;
        CHECK(core::visibleTrackCount(tracks) == 2u);

        core::HostPath live;
        live.id = "one";
        live.plugin = "A";
        core::HostPath dead;
        dead.id = "two";
        dead.plugin = "A";
        core::HostPath orphan;
        orphan.id = "nobody";
        orphan.plugin = "A";
        CHECK(core::pathPresentation(live, tracks).visible);
        CHECK(!core::pathPresentation(dead, tracks).visible);
        CHECK(core::pathPresentation(orphan, tracks).visible);  // a path with no owning track keeps today's behaviour
        std::printf("  the visibility rule (trackPresentation, visibleTrackCount, pathPresentation) answers from the data header\n");
    }

    // The window record of which plugin windows are open.
    {
        core::PluginWindows w;
        w.show("a");
        w.toggle("b");
        CHECK(w.shown("a") && w.shown("b") && w.count() == 2u);
        w.hide("a");
        w.toggle("b");
        CHECK(w.count() == 0u);
    }
    return testSummary("test_plugin_ui_data_split");
}
