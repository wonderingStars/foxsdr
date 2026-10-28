// app_window_map.cpp - THE MAP PAGES' TARGET LIST and WHERE A PAGE OPENS INSIDE THE MAIN WINDOW (stub campaign B16, MAP-WINDOWS and WINDOW-PLACEMENT; master
// 0.99.58 - 0.99.78). AppWindow members, kept out of app_window.cpp because they are two small subjects of one kind (and because that file is at the
// object-file limit: drawTrackList's 270 lines moved here from it, which is the room the batch's other edits to drawPluginWindows took).
//
// THE TARGET LIST (drawTrackList), down the left of an ADS-B / AIS / APRS map page. What it counts and what it lists is one rule, so the heading and the
// rows cannot disagree (core/plugin_ui_data.hpp's visibleTrackCount and visibleUnplacedTrackCount, gui/track_metrics.hpp's targetCountText):
//   - A PLACED ROW needs a position and a track the staleness rule still shows (trackPresentation of the whole track - age, kind AND flags, so an
//     aircraft whose position the source carried forward (0.99.78, CASCADE_TRACK_FLAG_ESTIMATED) stays for its 180 s and says "estimated" beside its id);
//   - WITH "On screen only" TICKED (0.99.74) a row is also on the map as last drawn (MapView::isOnScreen), and the heading says "N of M targets" when the
//     tick hid some. The selected or followed target is not exempt: the list shows what the map shows;
//   - A TARGET HEARD WITHOUT A POSITION (0.99.75) is a row AFTER every placed one, dimmed, never drawn and never a place to fly to; the heading counts it
//     as a second number and neither of the two numbers (placed on screen, placed live) counts it. With the tick on it has no row (it is never on screen)
//     and the heading still counts it, and an empty list is "No targets on screen." only when something is live that the tick hid.
// Where each part is, for a bounded run that cannot read pixels, the UI census says: "tracklist:count:<shown>/<total>", "tracklist:unplaced:<n>",
// "tracklist:row:<id>" and "tracklist:heardrow:<id>" (tests/test_unplaced_list_app.cpp).
//
// THE UNPLACED TARGETS COME FROM THE FEED (PluginUi::unplacedTracks, through AppWindow::hostUnplacedTracks and PluginUiPort::unplacedTracks - the one
// door change of this batch, dated and ending at stage 7h; tests/test_engine_shell_guard.cpp holds the budget). The feed's vectors are valid for the frame
// only (the directive applies at the head of the frame), and drawPluginWindows copies this page's share into pageTracks_ / pageUnplaced_ in the same frame
// it hands them here.
//
// PLACEMENT (placeInsideMainWindow): the Decoder output window and a plugin's picture window open INSIDE the main window, where the press that asked for
// them can be seen to have worked, and not past its right edge (report df5aff7da93dc88c: a main window within about 185 px of the screen's right edge put
// the page off the screen with its 19 px margin showing). The arithmetic is gui/page_geometry.hpp's pageOpenInsideStaggered; RESET WINDOW SIZES re-arms the
// FirstUseEver placement through beginPage's generation counter, which is why the size handed back is the size beginPage is given (tests/
// test_decoder_window_app.cpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "core/i18n.hpp"
#include "core/plugin_ui_data.hpp"
#include "gui/map_view.hpp"
#include "gui/page_geometry.hpp"
#include "gui/theme.hpp"
#include "gui/track_detail_view.hpp"
#include "gui/track_metrics.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

void AppWindow::placeInsideMainWindow(int slot, float wantW, float wantH, float& outW, float& outH) {
    // INSIDE THE MAIN WINDOW, the way the plugin store and the request pages
    // open (cascade::gui::pageOpenInside), and not past its right edge the way
    // this used to. The old anchor was the main window's right edge plus 166 px,
    // chosen so the page would be its OWN operating system window, and it
    // looked at no monitor and no main window: with the main window within
    // about 185 px of the right edge of the screen (maximised, snapped, or
    // simply filling a 1080p screen) the page was off the screen, ImGui's
    // clamp left exactly 19 px of it showing - its brass margin - and the
    // person who pressed POCSAG or DMR saw "a partial vertical bar" where the
    // decoded lines should be (report df5aff7da93dc88c, 0.99.71, 125 %
    // scaling, main window 1618 x 947 at 192,82 on a 1920-wide screen). A key
    // that opens nothing visible has told nobody anything. The page is still
    // an ordinary window: dragged out of the main window it becomes its own
    // operating system window, which is how it reaches a second screen.
    //
    // FirstUseEver throughout, so this is a starting position and never fights
    // the user afterwards - and beginPage's RESET WINDOW SIZES re-arms exactly
    // that, so a reset puts the page back HERE, position as well as size. The
    // size is NOT multiplied by the interface size: none of the other pages
    // that open inside (the store, the request pages, the radio setup) are,
    // and the rectangle is held to the main window whatever the scale.
    const ImGuiViewport* mv = ImGui::GetMainViewport();
    float x = 0.0f;
    float y = 0.0f;
    cascade::gui::pageOpenInsideStaggered(mv->Pos.x, mv->Pos.y, mv->Size.x, mv->Size.y, wantW, wantH, slot, x, y, outW, outH);
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(outW, outH), ImGuiCond_FirstUseEver);
}

void AppWindow::drawTrackList(MapPage& page,
                              const std::vector<cascade::core::HostTrack>& tracks,
                              const std::vector<cascade::core::HostTrack>& heard) {
    // `tracks` is this page's plugin's tracks only — the caller filtered them
    // by the tag each HostTrack carries — so every row here belongs to the
    // page it is drawn in, and a click can only ever land on this page's map.
    // THE SAME RULE THE MAP USES, applied here too. A target the map has
    // dropped must not still be occupying a row: the list is what makes the
    // window need a scrollbar, and a decoder that never evicts - the shipped
    // ADS-B one does not - grows it without limit over a session.
    //
    // AND, WITH "On screen only" TICKED (0.99.74), THE SAME RULE AGAIN FOR THE
    // MAP'S EDGES: a target outside the map as last drawn is not a row. `total`
    // is every live target, `shown` is the rows, and they differ only when the
    // switch hid something - which is when the count says "N of M".
    const std::size_t total = cascade::core::visibleTrackCount(tracks);
    const std::size_t shown = listedTrackCount(*page.view, tracks, mapListOnScreenOnly_);
    // AND THE TARGETS HEARD WITHOUT A POSITION (0.99.75), counted on their own and
    // never into `total` or `shown`: they are not on the map. They are rows, after
    // every placed one - unless the "On screen only" switch is ticked, because
    // they have no place on the map and so are never on screen, and the list shows
    // what the map shows. The heading still says how many there are.
    const std::size_t heardCount = cascade::core::visibleUnplacedTrackCount(heard);
    const std::size_t heardRows = mapListOnScreenOnly_ ? 0u : heardCount;
    if (shown + heardRows == 0u) {
        if (total == 0u && heardCount == 0u) {
            ImGui::TextDisabled("%s", tr("No targets."));
            // Wrapped to the list's width, which is narrow: the English is cut
            // there already, and a translation more so.
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", tr("Decoded aircraft, ships and stations appear here."));
            ImGui::PopTextWrapPos();
        } else {
            // THE SWITCH HID EVERYTHING, which is not "nothing has been heard":
            // say what is true, and keep the count of what is out of view so the
            // empty list does not read as a dead receiver. "Decoded aircraft ...
            // appear here" would be false - they are decoded, just elsewhere.
            ImGui::TextDisabled("%s", tr("No targets on screen."));
            ImGui::TextDisabled(
                "%s", cascade::gui::targetCountText(0u, total, heardCount, tr).c_str());
        }
        return;
    }

    // The wording and its six whole strings are targetCountText's, where it is tested.
    ImGui::TextUnformatted(cascade::gui::targetCountText(shown, total, heardCount, tr).c_str());
    // FOLLOW is a toggle rather than a mode buried in a menu, because its
    // effect - the map moving on its own - is confusing if you cannot see at a
    // glance that you asked for it.
    const bool following = !page.view->followedId().empty();
    if (following) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
        ImGui::TextWrapped(tr("following %s"), page.view->followedId().c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton(trId("Stop following"))) { page.view->clearFollow(); }
    }
    ImGui::Separator();

    // --- build the rows ------------------------------------------------------
    // Reduced to numbers FIRST, sorted second, drawn third. The host's track
    // vector is the plugins' output and is rebuilt on every poll, so sorting it
    // in place would be undone by the next frame; and a comparator that read
    // through to the plugin data would have to recompute a great circle per
    // comparison instead of once per target.
    std::vector<TrackRow> rows;
    rows.reserve(shown);
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const cascade::core::HostTrack& ht = tracks[i];
        // A placed row needs a place: its distance and bearing are measured to it.
        // (`tracks` never holds a target heard without a position - those are
        // `heard`, below - so this is the list defending itself, as the count does.)
        if (!cascade::core::trackHasPosition(ht.t)) { continue; }
        if (!cascade::core::trackPresentation(ht.t).visible) { continue; }
        // The map's edges, when asked for. The selected or followed target is
        // NOT exempt: the list shows what the map shows, and a followed target
        // is centred on the map anyway.
        if (mapListOnScreenOnly_ && !page.view->isOnScreen(ht.t.latDeg, ht.t.lonDeg)) {
            continue;
        }
        TrackRow r;
        // The LABEL is the callsign where one has been decoded and the id
        // otherwise. An aircraft's ICAO address is known from its first frame
        // but its callsign only arrives in a separate message type, so a list
        // that showed callsigns alone would leave rows blank for aircraft it
        // is tracking perfectly well.
        r.label = (ht.t.label[0] != '\0') ? ht.t.label : ht.t.id;
        r.id = ht.t.id;
        r.altM = ht.t.altM;
        r.speedMps = ht.t.speedMps;
        r.courseDeg = ht.t.courseDeg;
        // NaN WITHOUT A RECEIVER POSITION, which is what makes the two columns
        // print "no RX" instead of a distance from the Gulf of Guinea. It also
        // sorts them to the bottom, which is the right place for a column that
        // has no values at all.
        if (receiverPositionKnown()) {
            r.distanceKm = greatCircleKm(uiState_.rxLatDeg, uiState_.rxLonDeg, ht.t.latDeg, ht.t.lonDeg);
            r.bearingDeg = initialBearingDeg(uiState_.rxLatDeg, uiState_.rxLonDeg, ht.t.latDeg, ht.t.lonDeg);
        }
        r.ageMs = ht.t.ageMs;
        r.source = i;
        rows.push_back(std::move(r));
    }

    // --- how it is ordered ---------------------------------------------------
    // ABOVE THE TABLE, and spelled out. The eight sort keys used to be eight
    // column headings, and in the width this list gets they were truncated to
    // the point of uselessness. One named key at a time is readable at any
    // width the list can be given, and none of the eight has been dropped.
    drawTrackSortControl();

    // --- the table -----------------------------------------------------------
    // Same idiom as the plugin panel tables above (Borders | RowBg | ScrollY),
    // but NOT Sortable. The sort lives in the control above: ImGui has no
    // public call to set a table's sort arrow, so a menu and clickable headings
    // could not be kept in agreement, and a heading arrow reading "Callsign"
    // over rows ordered by distance is the same unreadable-UI failure in a new
    // form.
    //
    // AND NOT RESIZABLE, WHICH IS DELIBERATE AND COSTS SOMETHING. A resizable
    // ImGui table puts an invisible drag handle over every inner border,
    // TABLE_RESIZE_SEPARATOR_HALF_THICKNESS (4 px) either side of it, and that
    // handle is submitted before the rows are - so it takes the click and the
    // row-spanning Selectable underneath never sees it. The result was an
    // eight-pixel band in every row, right beside the details button, where
    // clicking a row to fly the map to that aircraft did nothing at all.
    // Silent misses on the list's primary gesture are worse than not being able
    // to drag the callsign/id split, especially now the split is computed from
    // what the text actually measures rather than from two guessed weights.
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp;
    // THE HEIGHT IS RESERVED BEFORE THE TABLE IS DRAWN, because a ScrollY table
    // with the default outer_size takes every pixel the pane has left and
    // anything emitted after it lands below the fold. The "no receiver
    // position" note under the table did exactly that: it was drawn, and it
    // took ten wheel notches to see. One line is held back for it, and only
    // when there is a line to hold back.
    const float tableH = tableHeightReservingLines(ImGui::GetContentRegionAvail().y,
                                                   ImGui::GetTextLineHeightWithSpacing(),
                                                   receiverPositionKnown() ? 0 : 1);

    // --- how wide the three columns come out ---------------------------------
    // THE HEADINGS ARE MEASURED, NOT GUESSED. Two weights picked by eye (2.0
    // and 1.6) were what left "Callsign" rendering as "Callsi..." in a 620 px
    // map window - a three-pixel shortfall, and the same unreadable heading the
    // eight-column table was replaced for. cascade::gui::trackListFit takes the
    // real font measurements and answers with the widths and with whether they
    // fit; the arithmetic is pure and tested against those exact pixel figures.
    //
    // TIGHTER CELL PADDING IS PART OF THE FIX. ImGui's default four pixels per
    // side spends twenty-four pixels of a two-hundred-pixel list on empty
    // margins, which is most of what was missing. Two is still a clear gap
    // between text and border.
    const float cellPadX = 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
                        ImVec2(cellPadX, ImGui::GetStyle().CellPadding.y));

    // The width the columns will actually share. The scrollbar and the four
    // borders are ImGui's and are not part of it; this is an estimate and is
    // only ever used to CHOOSE A LABEL, never to set a width, so being a pixel
    // out picks the compact button a pixel early and nothing worse.
    const float availW = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize -
                         4.0f;
    // The widest thing each column must show without truncating. For the
    // callsign that is the heading itself - callsigns are shorter than the word
    // - and for the id it is a six-character ICAO address, which is longer than
    // the heading "ID".
    const float callsignTextW = ImGui::CalcTextSize(tr("Callsign")).x;
    const float idTextW =
        (std::max)(ImGui::CalcTextSize(tr("ID")).x, ImGui::CalcTextSize("000000").x);
    const float framePadX2 = ImGui::GetStyle().FramePadding.x * 2.0f;

    // A NARROWER BUTTON IS WHERE THE LAST PIXELS COME FROM. Below about 600 px
    // of map window the two headings cannot both fit beside a "Details" button;
    // they can beside an "Info" one. The label is the only thing that changes -
    // same button, same row, same window - and it is chosen from the tested
    // fit rather than from a width threshold somebody picked.
    // Measured with the ### id hidden, exactly as the button will draw it.
    const char* detailsLabel = trId("Details");
    cascade::gui::TrackListFit fit = cascade::gui::trackListFit(
        availW, callsignTextW, idTextW,
        ImGui::CalcTextSize(detailsLabel, nullptr, true).x + framePadX2, cellPadX);
    if (!fit.headingsFit) {
        const char* compact = trId("Info");
        const cascade::gui::TrackListFit compactFit = cascade::gui::trackListFit(
            availW, callsignTextW, idTextW,
            ImGui::CalcTextSize(compact, nullptr, true).x + framePadX2, cellPadX);
        if (compactFit.headingsFit) {
            detailsLabel = compact;
            fit = compactFit;
        }
    }

    if (!ImGui::BeginTable("##tracktable", 3, flags, ImVec2(0.0f, tableH))) {
        ImGui::PopStyleVar();
        return;
    }

    // THREE COLUMNS, AND THE THIRD IS A BUTTON. The details column's heading is
    // deliberately blank: "Details" over a column of buttons that all say
    // "Details" is a word printed twice.
    //
    // The two text columns are stretch columns weighted by the widths
    // trackListFit returned. Weights are used rather than the widths
    // themselves because ImGui knows the true available width and this code
    // only estimates it - the RATIO is what the fit decided, and stretch
    // applies it to whatever room there really is. The button column is
    // WidthFixed, so it cannot be squeezed to nothing however narrow the pane
    // gets: a button too small to press is worse than a truncated word.
    ImGui::TableSetupColumn(tr("Callsign"), ImGuiTableColumnFlags_WidthStretch, fit.callsignW);
    ImGui::TableSetupColumn(tr("ID"), ImGuiTableColumnFlags_WidthStretch, fit.idW);
    ImGui::TableSetupColumn("##details", ImGuiTableColumnFlags_WidthFixed |
                                             ImGuiTableColumnFlags_NoResize,
                            fit.detailsW);
    ImGui::TableSetupScrollFreeze(0, 1);  // headings stay put while the list scrolls
    ImGui::TableHeadersRow();

    sortTrackRows(rows, trackSortKey_, trackSortAscending_);

    for (const TrackRow& r : rows) {
        const cascade::core::HostTrack& ht = tracks[r.source];
        const cascade::core::TrackPresentation pres =
            cascade::core::trackPresentation(ht.t);
        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(r.source));
        // Going quiet is visible in the list as well as on the map, and by the
        // same measure: a row that has faded is a target the map is about to
        // drop, which is the warning that makes the disappearance make sense.
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * pres.alpha);

        ImGui::TableNextColumn();
        const bool selected = (page.view->selectedId() == r.id);
        // SPANNING ALL COLUMNS so the whole row is the click target. Clicking a
        // row does exactly what clicking a row has always done - the sort
        // reordered the rows, and r.source is what keeps each one pointing at
        // its own target through that.
        //
        // ALLOWOVERLAP is what keeps the details button from being swallowed by
        // the row underneath it. Without it the Selectable, submitted first and
        // covering the whole row, takes the click and the button never fires -
        // and with the button taking the click but the row not knowing, a press
        // on it would ALSO fly the map somewhere. Exactly one of the two must
        // react to any given press, and this is the flag that arranges it.
        if (ImGui::Selectable(r.label.c_str(), selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap)) {
            // CLICK = GO TO, on THIS page's map — the row and the map it
            // commands live in the same window, which is already open or this
            // code would not be running. One click centres the map on it; the
            // map only ever tightens the zoom, so clicking a flight while
            // already zoomed in does not throw the view back out.
            page.view->setSelected(ht.t.id);
            page.view->goTo(ht.t.latDeg, ht.t.lonDeg);
        }
        // Where this row's label is, for a bounded run that has to see the order of the rows
        // without reading pixels (0.99.75: the placed rows, then the heard-without-a-position
        // ones). The name is only put together when the census is asked for.
        if (cascade::gui::census::enabled()) {
            cascade::gui::census::rect("tracklist:row:" + r.id, ImGui::GetItemRectMin().x,
                                       ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                       ImGui::GetItemRectMax().y);
        }
        // Double click starts following, which is the natural escalation of
        // "take me there" and needs no extra control.
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            page.view->setFollowed(ht.t.id);
        }
        // THE WHOLE BLOCK ON HOVER, the same one the map shows and the same one
        // the details button opens - so the six values the columns used to
        // carry are one hover away, not one click. It replaces a tooltip that
        // showed only the registration and type code and only when a
        // track-info plugin was installed; this one always has something to
        // say, because the position, altitude, speed, course, range, bearing
        // and age come from the track itself. Asking here is also what queues
        // the registry lookup for every listed target.
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            cascade::gui::drawTrackDetail(ht, &trackInfo_, receiverPositionKnown(), uiState_.rxLatDeg, uiState_.rxLonDeg);
            ImGui::EndTooltip();
        }

        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", r.id.c_str());
        // A CARRIED-FORWARD POSITION SAYS SO (0.99.78): the word beside the id, in the
        // same dim face, for a track whose source flagged its position an estimate -
        // the map draws the same track with a ring (MapView::draw).
        if (cascade::core::trackIsEstimated(ht.t)) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", cascade::gui::estimatedRowNote(tr).c_str());
            // That the word was drawn beside THIS id, for a bounded run (tests/test_unplaced_list_app.cpp); only put together when the census is on.
            if (cascade::gui::census::enabled()) { cascade::gui::census::note("tracklist:estimated:", r.id); }
        }

        // --- the details button ----------------------------------------------
        // Everything the six deleted columns said, on demand, in a window that
        // has room to spell it out. The button is drawn AFTER the row-spanning
        // Selectable and the Selectable allows overlap, so a press here opens
        // the details and does NOT also fly the map: clicking a row to follow
        // an aircraft and getting a dialog instead would be the regression this
        // change most has to avoid.
        ImGui::TableNextColumn();
        if (ImGui::SmallButton(detailsLabel)) {
            detailsTrackId_ = r.id;
            detailsOpen_ = true;
        }

        ImGui::PopStyleVar();
        ImGui::PopID();
    }

    // --- HEARD WITHOUT A POSITION, AFTER EVERY PLACED ROW (0.99.75) ----------------
    // An aircraft the receiver hears and has not yet placed (its position message
    // has not arrived): the owner wanted these counted and listed rather than
    // invisible until the position turns up. Always below the placed rows, whatever
    // the sort - they have no distance or bearing to be ordered by - and DIMMED:
    // fainter than a placed row at the same age, and fading further on the same
    // ageMs rule as one of its kind until it is dropped.
    //
    // A ROW, NOT A BUTTON TO NOWHERE. Clicking a placed row flies the map to the
    // target; there is nowhere to fly to, so the label is plain text with a tooltip
    // (the same detail block, whose position line says "no position yet") and the
    // only press is the Details button, which opens that block in its window. The
    // second line is the altitude where one is known and the words that stand where
    // the distance and the bearing would. With "On screen only" ticked there are no
    // such rows: they are never on screen (heardRows above).
    if (!mapListOnScreenOnly_) {
        constexpr float kHeardRowDim = 0.6f;
        for (std::size_t i = 0; i < heard.size(); ++i) {
            const cascade::core::HostTrack& ht = heard[i];
            const cascade::core::TrackPresentation pres =
                cascade::core::trackPresentation(ht.t);
            if (!pres.visible) { continue; }
            // The id and label are third-party fixed arrays: bounded, not trusted to
            // be terminated.
            const std::string id(ht.t.id, ::strnlen(ht.t.id, CASCADE_TRACK_ID_CHARS));
            const std::string label =
                (ht.t.label[0] != '\0')
                    ? std::string(ht.t.label, ::strnlen(ht.t.label, CASCADE_TRACK_LABEL_CHARS))
                    : id;
            const std::string note = cascade::gui::unplacedRowNote(
                ht.t.altM, cascade::gui::detailTrackKind(ht.t.kind), tr);
            ImGui::TableNextRow();
            // After the placed rows' ids (their indexes are 0..tracks.size()-1).
            ImGui::PushID(static_cast<int>(tracks.size() + i));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                ImGui::GetStyle().Alpha * pres.alpha * kHeardRowDim);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label.c_str());
            if (cascade::gui::census::enabled()) {
                cascade::gui::census::rect("tracklist:heardrow:" + id, ImGui::GetItemRectMin().x,
                                           ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                           ImGui::GetItemRectMax().y);
            }
            bool hovered = ImGui::IsItemHovered();
            ImGui::TextDisabled("%s", note.c_str());
            hovered = hovered || ImGui::IsItemHovered();
            if (hovered) {
                ImGui::BeginTooltip();
                cascade::gui::drawTrackDetail(ht, &trackInfo_, receiverPositionKnown(), uiState_.rxLatDeg, uiState_.rxLonDeg);
                ImGui::EndTooltip();
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", id.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(detailsLabel)) {
                detailsTrackId_ = id;
                detailsOpen_ = true;
            }
            ImGui::PopStyleVar();
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    ImGui::PopStyleVar();  // CellPadding

    // The one thing the list cannot say, said once under the table rather than
    // in a cell per row. It matters MORE now than when there were distance and
    // bearing columns to sit blank: sorting by distance with no receiver
    // position produces an order that looks arbitrary, and this is the line
    // that explains it.
    if (!receiverPositionKnown()) {
        ImGui::TextDisabled("%s", tr("Distance and bearing need the RX position above."));
    }
}

}  // namespace cascade::gui
