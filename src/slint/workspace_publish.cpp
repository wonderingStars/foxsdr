// The radio workspace's publisher - see workspace_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "workspace_publish.hpp"

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "host_logic.hpp"
#include "radio_mix_logic.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

namespace foxsdr::slintworkspace {

using namespace foxsdr::slinthost;

namespace {

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

std::string f5(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.5f|", v);
    return b;
}

template <class T>
std::shared_ptr<slint::Model<T>> emptyModel() { return std::make_shared<slint::VectorModel<T>>(); }

// A pane row with every model present (a null model in a struct is not a thing the pane can read) and the empty strings it starts with.
WorkspaceCell blankCell() {
    WorkspaceCell c{};   // value-initialised: a generated struct leaves its scalars as they were
    c.occupied = false;
    c.pane.freq_ticks = emptyModel<AxisTick>();
    c.pane.db_ticks = emptyModel<DbTick>();
    c.pane.age_ticks = emptyModel<AgeTick>();
    c.pane.bands = emptyModel<BandSegment>();
    c.pane.bookmarks = emptyModel<BookmarkMark>();
    c.pane.marks = emptyModel<PluginMark>();
    c.pane.wf_marks = emptyModel<PluginMark>();
    c.pane.markers = emptyModel<UserMarker>();
    c.pane.show_vfo = true;
    c.pane.cursor_pos = -1;
    return c;
}

}  // namespace

WorkspacePublisher::WorkspacePublisher(MainWindow& ui) : ui_(ui) {
    std::vector<WorkspaceCell> rows;
    for (int i = 0; i < kMaxRadios; ++i) {
        rows_[static_cast<std::size_t>(i)] = blankCell();
        rows_[static_cast<std::size_t>(i)].label = ss(cascade::core::formatText(cascade::i18n::tr("Radio %d"), i + 1));
        rows_[static_cast<std::size_t>(i)].card_label = ss(addCardSlotName(i + 1));
        rows_[static_cast<std::size_t>(i)].pane.radio = i + 1;
        rows.push_back(rows_[static_cast<std::size_t>(i)]);
    }
    cells_ = std::make_shared<slint::VectorModel<WorkspaceCell>>(std::move(rows));
    ui_.global<WorkspaceModel>().set_cells(cells_);
}

void WorkspacePublisher::setRow(int slot, bool occupied) {
    rows_[static_cast<std::size_t>(slot)].occupied = occupied;
    cells_->set_row_data(static_cast<std::size_t>(slot), rows_[static_cast<std::size_t>(slot)]);
    published_[static_cast<std::size_t>(slot)] = true;
    ++rowsSet_;
}

void WorkspacePublisher::publishLayout(const Workspace& ws, int winW, int winH, int ribbonSize) {
    auto& g = ui_.global<WorkspaceModel>();
    const int shown = ws.shownLayout(winW, winH);
    if (shown != shown_) { shown_ = shown; g.set_shown_layout(shown); }
    if (ws.layout() != layout_) { layout_ = ws.layout(); g.set_layout(layout_); }
    if (ws.page() != page_) { page_ = ws.page(); g.set_page(page_); }
    if (ws.radioCount() != count_) { count_ = ws.radioCount(); g.set_radio_count(count_); }
    if (ws.selectedRadio() != selected_) { selected_ = ws.selectedRadio(); g.set_selected_radio(selected_); }
    if (ribbonSize != ribbon_) { ribbon_ = ribbonSize; g.set_ribbon_size(ribbonSize); }
    const std::string notice = ws.fellBack(winW, winH) ? smallWindowNotice() : std::string();
    if (notice != notice_) { notice_ = notice; g.set_notice(ss(notice)); }
    // the page keys: one per page of the chosen layout, only when there is more than one AND a multi-pane layout is on screen (screen stage S5): where One is shown (the layout, or a window under 1280 x 720)
    // a page is a radio and the keys would be "Radios 1-1" ... "Radios 8-8", eight keys that do what the selector, the lamps and Ctrl+1 to Ctrl+8 do and are wider than the bar of a small window
    std::string sig;
    std::vector<slint::SharedString> labels, tips;
    const int pages = ws.pageCount();
    const int size = pageSizeOf(ws.layout());
    if (pages > 1 && shown != kLayoutOne) {
        for (int p = 0; p < pages; ++p) {
            const int first = p * size + 1, last = std::min(kMaxRadios, p * size + size);
            const std::string l = pageLabel(first, last), t = pageTip(first, last);
            sig += l + "|" + t + ";";
            labels.push_back(ss(l));
            tips.push_back(ss(t));
        }
    }
    if (sig != pagesSig_) {
        pagesSig_ = sig;
        g.set_page_labels(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(labels)));
        g.set_page_tips(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(tips)));
    }
    // which slots hold a radio: a row is set again only when its occupancy changed (its pane figures are publishPane's)
    for (int i = 0; i < kMaxRadios; ++i) {
        const bool occ = ws.occupied(i);
        if (!published_[static_cast<std::size_t>(i)] || rows_[static_cast<std::size_t>(i)].occupied != occ) { setRow(i, occ); }
    }
}

void WorkspacePublisher::publishPane(RadioView& rv, const PaneContext& ctx) {
    const int slot = std::clamp(rv.slot, 0, kMaxRadios - 1);
    WorkspaceCell& cell = rows_[static_cast<std::size_t>(slot)];
    RadioPaneData& d = cell.pane;
    const PaneFigures& fig = rv.fig;
    Signatures& sg = rv.paneSigs;
    bool dirty = !published_[static_cast<std::size_t>(slot)];
    const PaneTexts& t = ctx.texts;

    // THE WORDS and the flags, as one signature
    {
        const std::string sig = t.prefix + "|" + t.selectLabel + "|" + t.stopLabel + "|" + t.spectrumLabel + "|" + t.waterfallLabel + "|" + t.resizeLabel + "|" + t.closeLabel + "|" + t.runTip + "|" + t.decTip + "|" +
                                t.muteTip + "|" + t.failTip + "|" + t.device + "|" + t.frequency + "|" + t.mode + "|" + t.gain + "|" + t.squelch + "|" + t.volume + "|" + t.rate + "|" + ctx.decoder + "|" + ctx.notice + "|" +
                                (t.running ? "r" : "-") + (t.decoding ? "d" : "-") + (t.muted ? "m" : "-") + (t.failed ? "f" : "-") + (t.biasTee ? "b" : "-") + std::to_string(ctx.traceMode) + "|" + (ctx.linkShown ? "L" : "-") + std::to_string(ctx.link) + ctx.linkLabel + "|" + ctx.linkTip;
        if (sg.changed("pane.words", sig)) {
            d.radio = slot + 1;
            d.prefix = ss(t.prefix);
            d.select_label = ss(t.selectLabel);
            d.stop_label = ss(t.stopLabel);
            d.spectrum_label = ss(t.spectrumLabel);
            d.waterfall_label = ss(t.waterfallLabel);
            d.resize_label = ss(t.resizeLabel);
            d.removable = slot > 0;                    // radio 1 is always there (RADIO_REMOVE refuses it)
            d.close_label = ss(t.closeLabel);
            d.run_tip = ss(t.runTip);
            d.dec_tip = ss(t.decTip);
            d.mute_tip = ss(t.muteTip);
            d.fail_tip = ss(t.failTip);
            d.device = ss(t.device);
            d.frequency = ss(t.frequency);
            d.mode = ss(t.mode);
            d.running = t.running;
            d.decoding = t.decoding;
            d.muted = t.muted;
            d.failed = t.failed;
            d.gain = ss(t.gain);
            d.squelch = ss(t.squelch);
            d.volume = ss(t.volume);
            d.rate = ss(t.rate);
            d.bias_tee = t.biasTee;
            d.decoder = ss(ctx.decoder);
            d.notice = ss(ctx.notice);
            d.trace_mode = ctx.traceMode;
            d.link_shown = ctx.linkShown;
            d.link = ctx.link;
            d.link_label = ss(ctx.linkLabel);
            d.link_tip = ss(ctx.linkTip);
            dirty = true;
        }
    }
    // THE PICTURES and what comes with a new frame
    if (fig.newFrame) {
        d.trace = rv.wells.traceImage();
        d.picture = rv.wells.waterfallImage();
        rv.wells.flip();
        d.span = ss(fig.spanText);
        d.peak = ss(fig.showPeak ? fig.peakText : std::string());
        d.foot_left = ss(fig.footLeft);
        if (sg.changed("pane.age", [&] { std::string s; for (const auto& tk : fig.ageTicks) { s += f5(tk.frac) + tk.label + ";"; } return s; }())) {
            std::vector<AgeTick> ticks;
            for (const auto& tk : fig.ageTicks) { ticks.push_back({static_cast<float>(tk.frac == 0.0 ? 22.0 / std::max<std::uint32_t>(1u, rv.wells.size.wfH) : tk.frac), ss(tk.label)}); }
            d.age_ticks = std::make_shared<slint::VectorModel<AgeTick>>(std::move(ticks));
        }
        dirty = true;
    }
    // THE AXES and overlays follow the view every tick
    {
        std::string sig;
        for (const auto& tk : fig.freqTicks) { sig += f5(tk.frac) + tk.label + ";"; }
        if (sg.changed("pane.freq", sig)) {
            std::vector<AxisTick> ft;
            for (const auto& tk : fig.freqTicks) { ft.push_back({static_cast<float>(tk.frac), ss(tk.label)}); }
            d.freq_ticks = std::make_shared<slint::VectorModel<AxisTick>>(std::move(ft));
            d.axis_unit = ss(axisUnit(fig.view.spanHz));
            dirty = true;
        }
    }
    {
        std::string sig;
        for (const auto& tk : fig.dbTicks) { sig += f5(tk.frac) + tk.label + ";"; }
        if (sg.changed("pane.db", sig)) {
            std::vector<DbTick> dt;
            for (const auto& tk : fig.dbTicks) { dt.push_back({static_cast<float>(tk.frac), ss(tk.label)}); }
            d.db_ticks = std::make_shared<slint::VectorModel<DbTick>>(std::move(dt));
            dirty = true;
        }
    }
    if (d.vfo_pos != fig.vfoPos || d.vfo_half != fig.vfoHalf) { d.vfo_pos = fig.vfoPos; d.vfo_half = fig.vfoHalf; dirty = true; }
    {
        std::string sig;
        for (const auto& b : fig.ribbon) { sig += f5(b.from) + f5(b.to) + b.label + ";"; }
        if (sg.changed("pane.ribbon", sig)) {
            std::vector<BandSegment> ribbon;
            for (const auto& b : fig.ribbon) { ribbon.push_back({b.from, b.to, ss(b.label), slint::Color::from_argb_uint8(0, 0, 0, 0)}); }
            d.bands = std::make_shared<slint::VectorModel<BandSegment>>(std::move(ribbon));
            dirty = true;
        }
    }
    {
        std::string sig;
        for (const auto& b : fig.bookmarks) { sig += f5(b.pos) + b.label + (b.favourite ? "*" : "") + std::to_string(b.row) + ";"; }
        if (sg.changed("pane.bookmarks", sig)) {
            std::vector<BookmarkMark> bms;
            for (const auto& b : fig.bookmarks) { bms.push_back({b.pos, ss(b.label), b.favourite, b.row}); }
            d.bookmarks = std::make_shared<slint::VectorModel<BookmarkMark>>(std::move(bms));
            dirty = true;
        }
    }
    {
        const auto toMark = [](const FigureMark& m) { return PluginMark{m.pos, m.width, ss(m.label), slint::Color::from_argb_uint8(m.a, m.r, m.g, m.b), m.dashed}; };
        std::string sig, wsig;
        for (const auto& m : fig.marks) { sig += f5(m.pos) + f5(m.width) + m.label + (m.dashed ? "d" : "") + ";"; }
        for (const auto& m : fig.wfMarks) { wsig += f5(m.pos) + f5(m.width) + ";"; }
        if (sg.changed("pane.marks", sig)) {
            std::vector<PluginMark> marks;
            for (const auto& m : fig.marks) { marks.push_back(toMark(m)); }
            d.marks = std::make_shared<slint::VectorModel<PluginMark>>(std::move(marks));
            dirty = true;
        }
        if (sg.changed("pane.wfmarks", wsig)) {
            std::vector<PluginMark> wf;
            for (const auto& m : fig.wfMarks) { wf.push_back(toMark(m)); }
            d.wf_marks = std::make_shared<slint::VectorModel<PluginMark>>(std::move(wf));
            dirty = true;
        }
    }
    {
        std::string sig;
        for (const auto& m : fig.markers) { sig += f5(m.pos) + m.label + ";"; }
        if (sg.changed("pane.markers", sig)) {
            std::vector<UserMarker> ums;
            for (const auto& m : fig.markers) { ums.push_back({m.pos, ss(m.label)}); }
            d.markers = std::make_shared<slint::VectorModel<UserMarker>>(std::move(ums));
            dirty = true;
        }
    }
    {
        const std::string tuned = fig.footTuned, rest = fig.footRest;
        if (sg.changed("pane.foot", tuned + "|" + rest)) { d.foot_tuned = ss(tuned); d.foot_rest = ss(rest); dirty = true; }
    }
    {
        const bool visible = ctx.hoverX >= 0;
        if (sg.changed("pane.cursor", std::string(visible ? "1" : "0") + f5(ctx.hoverX) + ctx.cursorText)) {
            d.cursor_visible = visible;
            d.cursor_pos = ctx.hoverX;
            d.cursor_text = ss(ctx.cursorText);
            dirty = true;
        }
    }
    if (dirty) { setRow(slot, true); }
}

}  // namespace foxsdr::slintworkspace
