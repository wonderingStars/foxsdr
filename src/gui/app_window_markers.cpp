// app_window_markers.cpp - THE WATERFALL'S MARKERS: notes dropped at a frequency of interest WITHOUT tuning to it (stub campaign B10, MARKERS-A; master 0.99.57).
// AppWindow members, kept out of app_window.cpp because they are one subject (and because that file is at the object-file limit).
//
// THE REQUEST (a user, 2026-09-30): "I don't always want to tune to an interesting frequency, just take note of it for later examination ... a right click
// menu on the waterfall display that would give you an option to drop a marker on the current cursor position ... clear all markers, list all markers and
// maybe copy all markers to the clipboard." Right-click the waterfall - the receiver's or a patch Display part's - and drop one; list them, copy them, clear
// them. A MARKER IS NOT A BOOKMARK: a pencil tick made while looking at something else - a frequency, a number to call it by, the moment it was made and a
// note added later. The list window can turn one into a bookmark.
//
// WHERE THE LIST LIVES. In master the window kept it (freqMarkers_.add from a menu item). It is the ENGINE's now (Engine::freqMarkers_, engine_markers.cpp):
// the window works it through FOXAPP_OP_MARKER_DROP / _REMOVE / _CLEAR / _NOTE (docs/api-0.3-additions.md 9.8) and reads it as the list FREQ_MARKERS with the
// whole notes as facts, again only when fact.markers.version moves (a decoder printing a line moves listSeq, which is not the list's signal - the bookmarks'
// rule). What the window keeps is the copy it draws from (markerCache_), the menu's request and the note being typed.
//
// WHAT IS HELD HERE, each by tests/test_freq_markers_app.cpp (the real binary, a scripted pointer, markers.json read back) and the engine's by
// tests/test_engine_api_markers.cpp:
//   - A RIGHT-CLICK OPENS THE MENU AND DROPS NOTHING BY ITSELF. requestMarkerMenu only remembers where the click landed; "Drop marker here" is the only thing
//     that sends a DROP;
//   - the marker is dropped at the frequency under the pointer, rounded to the coarsest 1-2-5 step no wider than a pixel (core::roundMarkerHz), in ABSOLUTE
//     hertz: on a patch Display part the part works in offsets from its radio's centre, so its view is put back into absolute hertz before the menu is asked for;
//   - the numbers M1, M2, ... are the engine's and never reused (the window never invents one: it shows the number the list gave);
//   - a LONG PRESS tunes nothing (gui/press_gesture.hpp, in drawCenterPanels): Android has no right button, its touch layer turns a finger held still for
//     450 ms into "left up, right down", and the left half used to tune first.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "core/demod_names.hpp"
#include "core/freq_markers.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/fonts.hpp"
#include "gui/freq_marker_layout.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"
#include "gui/ui_scale.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

namespace cmd = cascade::core::cmd;

// --- the window's copy of the list -------------------------------------------------------------------------------------------------------

const AppWindow::MarkerListCache& AppWindow::markerLists() {
    MarkerListCache& c = markerCache_;
    if (uiSession_ == nullptr || c.listSeq == uiState_.listSeq) { return c; }
    c.listSeq = uiState_.listSeq;
    // THE ROWS ARE COPIED WHEN THE LIST CHANGED, not whenever listSeq moves: the list's own counter says so.
    readFactText("fact.markers.version", c.factText);
    const std::string version = c.factText;
    if (c.version.empty() || version != c.version) {
        const int32_t n = std::max<int32_t>(0, engineApi_->read_list(uiSession_, FOXAPI_LIST_FREQ_MARKERS, 0, nullptr));
        c.rows.clear();
        c.rows.reserve(static_cast<std::size_t>(n));
        FoxListItem it{};
        for (int32_t i = 0; i < n; ++i) {
            it = FoxListItem{};
            it.structSize = sizeof(it);
            // `got <= i` is the end of a list that changed under the read: what was read stands, nothing stale is kept.
            if (engineApi_->read_list(uiSession_, FOXAPI_LIST_FREQ_MARKERS, static_cast<uint32_t>(i), &it) <= i) { break; }
            cascade::core::FreqMarker m;
            m.number = static_cast<int>(it.id);
            m.freqHz = it.value[0];
            m.notedUnix = static_cast<std::int64_t>(it.value[1]);
            // The item's name is the note cut at 95 bytes; the whole note is the fact, which keeps its room.
            c.factKey = "fact.markers.note." + std::to_string(it.id);
            readFactText(c.factKey.c_str(), c.factText);
            m.note = c.factText;
            c.rows.push_back(std::move(m));
        }
        ++c.copies;
        // A publish between the version and the rows would leave rows newer than the version they are filed under: ask again, and if the list moved,
        // forget the version so the next refresh copies the rows again.
        readFactText("fact.markers.version", c.factText);
        if (c.factText == version) {
            c.version = version;
        } else {
            c.version.clear();
        }
    }
    readFactText("fact.markers.error", c.error);
    return c;
}

int AppWindow::markerNearHz(double hz, double tolHz) {
    if (!std::isfinite(hz)) { return 0; }
    int best = 0;
    double bestD = 0.0;
    for (const cascade::core::FreqMarker& m : markerLists().rows) {
        const double d = std::fabs(m.freqHz - hz);
        if (d <= tolHz && (best == 0 || d < bestD)) {
            best = m.number;
            bestD = d;
        }
    }
    return best;
}

// --- on the waterfall ----------------------------------------------------------------------------------------------------------------------

void AppWindow::drawFreqMarkers(double loHz, double hiHz, float x0, float y0, float width, float height, float scale, const ImVec4* avoid) {
    const std::vector<cascade::core::FreqMarker>& rows = markerLists().rows;
    if (rows.empty() || !(width > 0.0f) || !(height > 0.0f)) { return; }
    namespace th = cascade::gui::theme;
    struct Vis {
        const cascade::core::FreqMarker* m;
        float x;
    };
    std::vector<Vis> vis;
    for (const cascade::core::FreqMarker& m : rows) {
        float x = 0.0f;
        if (cascade::gui::markerScreenX(m.freqHz, loHz, hiHz, x0, width, x)) { vis.push_back({&m, x}); }
    }
    if (vis.empty()) { return; }
    std::sort(vis.begin(), vis.end(), [](const Vis& a, const Vis& b) { return a.x < b.x; });

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // THE FOOT IS WHERE THE PICTURE CAN BE SEEN TO END, not where its layout
    // says it does. The receiver's waterfall was once laid out 12 px past the
    // bottom of its view (drawCenterPanels now subtracts the item spacing),
    // and the first rendered check put every bottom-row tab under the
    // cabinet's frame; a picture overhanging its clip must not do that again.
    const float visibleBottom = std::min(y0 + height, dl->GetClipRectMax().y);
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + width, y0 + height), true);
    // IVORY ON ENAMEL, a colour nothing else on the waterfall uses: the VFO
    // line and the bookmarks are amber, the plugins' marks gold, and the
    // colour map runs phosphor to cream - a marker has to read as the user's
    // own pencil mark against any of them.
    const ImU32 lineCol = th::withAlpha(th::kIvory, 0.60f);
    const ImU32 plateCol = th::withAlpha(th::kEnamelDark, 0.90f);
    const ImU32 rimCol = th::withAlpha(th::kIvory, 0.80f);
    const ImU32 inkCol = th::kIvory;
    ImFont* font = cascade::gui::fonts::ui();
    // The font in force, so a patch part's tabs grow and shrink with its
    // canvas exactly as its own lettering does.
    const float fontPx = ImGui::GetFontSize() * 0.8f;
    const auto textSize = [&](const char* s) {
        return font != nullptr ? font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, s) : ImGui::CalcTextSize(s);
    };
    const float padX = 3.0f * scale;
    const float padY = 1.0f * scale;
    const float tabH = textSize("Hg").y + 2.0f * padY;
    const float rowPitch = tabH + 2.0f * scale;
    const float foot = visibleBottom - 2.0f * scale;
    // Never more than half the picture in tabs: the markers annotate the
    // waterfall, they must not become it.
    const int tabRows = std::min(cascade::gui::kMarkerTabMaxRows, static_cast<int>((0.5f * (visibleBottom - y0)) / rowPitch));

    std::vector<std::string> labels;
    std::vector<cascade::gui::MarkerTabIn> tabsIn;
    labels.reserve(vis.size());
    tabsIn.reserve(vis.size());
    for (const Vis& v : vis) {
        labels.push_back("M" + std::to_string(v.m->number) + " " + cascade::core::markerMhzText(v.m->freqHz));
        tabsIn.push_back({v.x, textSize(labels.back().c_str()).x + 2.0f * padX});
    }
    // Rows whose height the `avoid` plate reaches start with its extent taken.
    std::vector<std::vector<std::pair<float, float>>> occupied(static_cast<std::size_t>(std::max(tabRows, 0)));
    if (avoid != nullptr && avoid->z > avoid->x && avoid->w > avoid->y) {
        for (int r = 0; r < tabRows; ++r) {
            const float bottom = foot - static_cast<float>(r) * rowPitch;
            if (bottom - tabH < avoid->w && bottom > avoid->y) { occupied[static_cast<std::size_t>(r)].emplace_back(avoid->x, avoid->z); }
        }
    }
    const std::vector<cascade::gui::MarkerTabOut> tabs = cascade::gui::layoutMarkerTabs(tabsIn, x0, x0 + width, tabRows, 3.0f, occupied);

    // Dashed, so a marker never reads as the VFO's solid line or as a signal.
    const float dash = 4.0f * scale;
    const float gap = 3.0f * scale;
    for (const Vis& v : vis) {
        for (float y = y0; y < y0 + height; y += dash + gap) {
            dl->AddLine(ImVec2(v.x, y), ImVec2(v.x, std::min(y + dash, y0 + height)), lineCol, std::max(1.0f, scale));
        }
    }

    // The tooltip names the marker nearest the pointer - over its tab, or
    // within a few pixels of its line.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool pointerIn = ImGui::IsWindowHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && mouse.x >= x0 && mouse.x <= x0 + width &&
                           mouse.y >= y0 && mouse.y <= y0 + height;
    const cascade::core::FreqMarker* hovered = nullptr;
    float hoveredD = 4.0f * scale;
    for (std::size_t i = 0; i < vis.size(); ++i) {
        const cascade::gui::MarkerTabOut& t = tabs[i];
        if (t.row >= 0) {
            const float bottom = foot - static_cast<float>(t.row) * rowPitch;
            const float top = bottom - tabH;
            dl->AddRectFilled(ImVec2(t.a, top), ImVec2(t.e, bottom), plateCol, 2.0f * scale);
            dl->AddRect(ImVec2(t.a, top), ImVec2(t.e, bottom), rimCol, 2.0f * scale, 0, 1.0f);
            dl->AddText(font, fontPx, ImVec2(t.a + padX, top + padY), inkCol, labels[i].c_str());
            cascade::gui::census::note("fmk:tab:", vis[i].m->number);
            cascade::gui::census::rect("fmk:tab:", vis[i].m->number, t.a, top, t.e, bottom);
            if (pointerIn && mouse.x >= t.a && mouse.x <= t.e && mouse.y >= top && mouse.y <= bottom) {
                hovered = vis[i].m;
                hoveredD = -1.0f;  // a tab under the pointer beats any line
            }
        } else {
            cascade::gui::census::note("fmk:line:", vis[i].m->number);
        }
        const float d = std::fabs(mouse.x - vis[i].x);
        if (pointerIn && hoveredD >= 0.0f && d < hoveredD) {
            hovered = vis[i].m;
            hoveredD = d;
        }
    }
    dl->PopClipRect();

    if (hovered != nullptr) {
        std::string tip = "M" + std::to_string(hovered->number) + "  " + cascade::core::markerMhzText(hovered->freqHz) + " MHz";
        if (!hovered->note.empty()) { tip += "\n" + hovered->note; }
        tip += "\n";
        tip += tr("Right-click for the marker menu");
        ImGui::SetTooltip("%s", tip.c_str());
    }
}

void AppWindow::drawFreqMarkerLines(double loHz, double hiHz, float x0, float y0, float width, float height, float scale) {
    const std::vector<cascade::core::FreqMarker>& rows = markerLists().rows;
    if (rows.empty() || !(width > 0.0f) || !(height > 0.0f)) { return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + width, y0 + height), true);
    // Fainter than on the waterfall: the trace is read by its shape, and a
    // full-strength line through every peak would compete with it.
    const ImU32 col = cascade::gui::theme::withAlpha(cascade::gui::theme::kIvory, 0.35f);
    const float dash = 4.0f * scale;
    const float gap = 3.0f * scale;
    for (const cascade::core::FreqMarker& m : rows) {
        float x = 0.0f;
        if (!cascade::gui::markerScreenX(m.freqHz, loHz, hiHz, x0, width, x)) { continue; }
        for (float y = y0; y < y0 + height; y += dash + gap) {
            dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + dash, y0 + height)), col, std::max(1.0f, scale));
        }
        cascade::gui::census::note("fmk:specline:", m.number);
    }
    dl->PopClipRect();
}

// --- the menu ----------------------------------------------------------------------------------------------------------------------------------

void AppWindow::requestMarkerMenu(double hz, double hzPerPx) {
    if (!std::isfinite(hz) || !std::isfinite(hzPerPx) || !(hzPerPx > 0.0)) { return; }
    // ONLY REMEMBERS WHERE THE CLICK LANDED: opening the menu changes nothing, and a long press (Android's right button) must not drop a marker on the
    // way to it. "Drop marker here" in drawMarkerMenu is the one thing that sends a DROP.
    markerMenuStepHz_ = cascade::core::markerStepHz(hzPerPx);
    markerMenuHz_ = cascade::core::roundMarkerHz(hz, hzPerPx);
    // "Remove" offers the marker within a few pixels of the click, the same
    // reach the tooltip has.
    markerMenuNear_ = markerNearHz(hz, 6.0 * hzPerPx);
    markerMenuPending_ = true;
}

void AppWindow::drawMarkerMenu() {
    static constexpr const char* kMenuId = "##freq_marker_menu";
    if (markerMenuPending_) {
        markerMenuPending_ = false;
        ImGui::OpenPopup(kMenuId);
    }
    if (!ImGui::BeginPopup(kMenuId)) { return; }
    const MarkerListCache& list = markerLists();
    const std::string here = cascade::core::formatMarkerMhz(markerMenuHz_, markerMenuStepHz_) + " MHz";
    // Each item's rectangle, for tests/test_freq_markers_app to aim at.
    const auto itemRect = [](const char* name) {
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        cascade::gui::census::rect(name, a.x, a.y, b.x, b.y);
    };
    cascade::gui::census::note("fmk:menu:at:", here);
    const bool full = list.rows.size() >= cascade::core::FreqMarkers::kMaxMarkers;
    if (ImGui::MenuItem(tr("Drop marker here"), here.c_str(), false, markerMenuHz_ > 0.0 && !full)) {
        // Two of this view's pixels either side is "the same signal": a second
        // right-click on it names the marker already there instead of
        // stacking another on top. The engine judges it and numbers it.
        uiSubmit(cmd::makeNum(FOXAPP_OP_MARKER_DROP, markerMenuHz_, 2.0 * markerMenuStepHz_));
    }
    itemRect("fmk:menu:drop");
    if (markerMenuNear_ != 0) {
        std::string label;
        cascade::core::formatUtf8(label, tr("Remove marker M%d"), markerMenuNear_);
        if (ImGui::MenuItem(label.c_str())) { uiSubmit(cmd::makeInt(FOXAPP_OP_MARKER_REMOVE, markerMenuNear_)); }
        itemRect("fmk:menu:remove");
    }
    ImGui::Separator();
    const std::string count = std::to_string(list.rows.size());
    if (ImGui::MenuItem(tr("List markers..."), count.c_str())) { markerWindowOpen_ = true; }
    itemRect("fmk:menu:list");
    if (ImGui::MenuItem(tr("Copy markers to clipboard"), nullptr, false, !list.rows.empty())) {
        ImGui::SetClipboardText(cascade::core::formatMarkersClipboard(list.rows).c_str());
    }
    // Clearing is the one thing here that cannot be taken back, so it asks
    // once more, in a submenu the pointer has to travel into.
    if (ImGui::BeginMenu(tr("Clear all markers"), !list.rows.empty())) {
        std::string yes;
        cascade::core::formatUtf8(yes, tr("Yes, remove all %zu"), list.rows.size());
        if (ImGui::MenuItem(yes.c_str())) { uiSubmit(cmd::make(FOXAPP_OP_MARKER_CLEAR)); }
        ImGui::EndMenu();
    }
    // (The display range block of master's 0.99.76 ends this menu, as it ends the spectrum's: batch B11's drawDisplayMenuBlock.)
    ImGui::EndPopup();
}

// --- the list window ---------------------------------------------------------------------------------------------------------------------------

void AppWindow::drawMarkerWindow() {
    // THE NOTE BEING TYPED is the window's text until the engine's list has it back (see markerNoteHold_), for at most thirty frames.
    if (markerNoteHold_.frames > 0) {
        bool back = false;
        for (const cascade::core::FreqMarker& m : markerLists().rows) {
            back = back || (m.number == markerNoteHold_.number && m.note == markerNoteHold_.text);
        }
        if (back) {
            markerNoteHold_.frames = 0;
        } else {
            --markerNoteHold_.frames;
        }
    }
    if (!markerWindowOpen_) { return; }
    const MarkerListCache& list = markerLists();
    ImGui::SetNextWindowSize(ImVec2(cascade::gui::uiscale::px(620.0f), cascade::gui::uiscale::px(340.0f)), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(trId("Markers##freq_markers"), &markerWindowOpen_)) {
        cascade::gui::census::note("fmk:window");
        if (!list.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::bad());
            ImGui::TextWrapped("%s", list.error.c_str());
            ImGui::PopStyleColor();
        }
        const float footerH = ImGui::GetFrameHeightWithSpacing();
        if (list.rows.empty()) {
            ImGui::TextWrapped("%s", tr("No markers yet. Right-click the waterfall and choose "
                                        "Drop marker here."));
        } else {
            // In frequency order: the list is for working through the band.
            std::vector<const cascade::core::FreqMarker*> rows;
            rows.reserve(list.rows.size());
            for (const cascade::core::FreqMarker& m : list.rows) { rows.push_back(&m); }
            std::stable_sort(rows.begin(), rows.end(),
                             [](const cascade::core::FreqMarker* a, const cascade::core::FreqMarker* b) { return a->freqHz < b->freqHz; });
            int removeNo = 0;
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
            if (ImGui::BeginTable("##fmk_table", 5, flags, ImVec2(0.0f, -footerH))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("#");
                ImGui::TableSetupColumn(tr("Frequency"));
                ImGui::TableSetupColumn(tr("Noted"));
                ImGui::TableSetupColumn(tr("Note"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("##actions");
                ImGui::TableHeadersRow();
                for (const cascade::core::FreqMarker* m : rows) {
                    ImGui::PushID(m->number);
                    cascade::gui::census::note("fmk:row:", m->number);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("M%d", m->number);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s MHz", cascade::core::markerMhzText(m->freqHz).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(cascade::core::formatMarkerTime(m->notedUnix).c_str());
                    ImGui::TableNextColumn();
                    std::string note = (markerNoteHold_.frames > 0 && markerNoteHold_.number == m->number) ? markerNoteHold_.text : m->note;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputTextWithHint("##note", tr("add a note"), &note)) {
                        // A KEYSTROKE AT A TIME, as master saved it: the engine writes half a second after the last change. The box keeps what was typed
                        // until the list has it back.
                        uiSubmit(cmd::makeText(FOXAPP_OP_MARKER_NOTE, note, m->number));
                        markerNoteHold_.number = m->number;
                        markerNoteHold_.text = note;
                        markerNoteHold_.frames = 30;
                    }
                    cascade::gui::census::rect("fmk:note:", m->number, ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                               ImGui::GetItemRectMax().y);
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton(tr("Tune"))) { uiSubmit(cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, m->freqHz)); }
                    cascade::gui::census::rect("fmk:tune:", m->number, ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                               ImGui::GetItemRectMax().y);
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", tr("Tune the receiver here")); }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr("Bookmark"))) {
                        // The note names it when there is one; otherwise its marker number. In the current mode and bandwidth, as master added it. A
                        // LIST row (BOOKMARK_ADD_ROWS 1): no range is judged - the marker was dropped where the picture showed - and the same row twice is
                        // added once.
                        std::string name = !m->note.empty() ? m->note : "M" + std::to_string(m->number);
                        for (char& ch : name) {
                            if (ch == '\t' || ch == '\n' || ch == '\r') { ch = ' '; }
                        }
                        const char* mode = uiState_.demodMode >= 1u && uiState_.demodMode <= 8u ? cascade::core::kDemodNames[uiState_.demodMode] : "AM";
                        char head[96];
                        std::snprintf(head, sizeof(head), "%.6f", m->freqHz);
                        const std::string row = std::string(head) + '\t' + mode + '\t' + std::to_string(static_cast<long long>(std::llround(tunedBandwidthHz()))) +
                                                "\t0\t\t" + name;
                        uiSubmit(cmd::makeText(FOXAPP_OP_BOOKMARK_ADD_ROWS, row, 1));
                    }
                    cascade::gui::census::rect("fmk:bookmark:", m->number, ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                               ImGui::GetItemRectMax().y);
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", tr("Add it to the Bookmarks, in the current mode")); }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) { removeNo = m->number; }
                    cascade::gui::census::rect("fmk:remove:", m->number, ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                               ImGui::GetItemRectMax().y);
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", tr("Remove this marker")); }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (removeNo != 0) { uiSubmit(cmd::makeInt(FOXAPP_OP_MARKER_REMOVE, removeNo)); }
        }
        ImGui::BeginDisabled(list.rows.empty());
        if (ImGui::Button(tr("Copy markers to clipboard"))) { ImGui::SetClipboardText(cascade::core::formatMarkersClipboard(list.rows).c_str()); }
        ImGui::SameLine();
        if (ImGui::Button(tr("Clear all markers"))) { ImGui::OpenPopup("##fmk_clear"); }
        cascade::gui::census::rect("fmk:clear", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("##fmk_clear")) {
            std::string yes;
            cascade::core::formatUtf8(yes, tr("Yes, remove all %zu"), list.rows.size());
            if (ImGui::MenuItem(yes.c_str())) { uiSubmit(cmd::make(FOXAPP_OP_MARKER_CLEAR)); }
            cascade::gui::census::rect("fmk:clear:yes", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                       ImGui::GetItemRectMax().y);
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

}  // namespace cascade::gui
