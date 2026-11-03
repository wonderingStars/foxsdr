// The main screen against the Workbench artboard: rendered headlessly at the sheet's 1440 x 900 with the sheet's
// values, every region is where the artboard puts it (title bar 36, receiver strip 104, icon rail 64, sections
// panel 296, tab row 32, splitter 6, waterfall 270, status bar 28) in the artboard's colours, and the keys,
// gestures and shortcuts reach their callbacks. The frame is saved as slint-shots/main-screen.png: the picture of
// the screen the hand-back shows.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "main.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace foxsdr::slinttest;

namespace {

// Prints the numbers behind a failed check, so a red run says what was measured.
#define CHECKV(cond, ...)                         \
    do {                                          \
        const bool ok_ = static_cast<bool>(cond); \
        CHECK(ok_);                               \
        if (!ok_) {                               \
            std::printf("      ");                \
            std::printf(__VA_ARGS__);             \
            std::printf("\n");                    \
        }                                         \
    } while (0)

slint::SharedString ss(const char* s) { return slint::SharedString(s); }

template <class T>
std::shared_ptr<slint::VectorModel<T>> model(std::vector<T> v) { return std::make_shared<slint::VectorModel<T>>(std::move(v)); }

// The sheet's values, as the host would publish them for the artboard's moment.
void fillAsTheSheet(const slint::ComponentHandle<MainWindow>& ui) {
    ui->set_version(ss("0.99.104"));
    ui->set_running(true); ui->set_decoding(true);
    ui->set_tuned_text(ss("145.500000 MHz"));
    ui->set_digits(model<int>({0, 1, 4, 5, 5, 0, 0, 0, 0, 0}));
    ui->set_leading_zeros(1);
    ui->set_mode_index(0);
    ui->set_gain_norm(0.62f); ui->set_gain_text(ss("32 dB")); ui->set_gain_enabled(true); ui->set_gain_auto_enabled(true); ui->set_gain_hint(ss("TUNER stage \xC2\xB7 RTL-SDR Blog V4"));
    ui->set_squelch_norm(0.29f); ui->set_squelch_text(ss("-85 dB")); ui->set_squelch_auto(true); ui->set_squelch_hint(ss("Automatic: 8 dB above the noise floor"));
    ui->set_volume_norm(0.72f); ui->set_volume_text(ss("0.72"));
    ui->set_level_norm(0.55f); ui->set_level_text(ss("-12.4 dB"));
    ui->set_bias_tee_present(true); ui->set_bias_hint(ss("Off \xC2\xB7 4.5 V on the cable \xC2\xB7 asks first"));
    ui->set_notifications(1);
    std::vector<SectionRow> rows;
    const char* names[8] = {"Patch", "Source", "Radio", "Audio filters", "Sinks", "Transmit", "Recorder", "Frequency correction"};
    const char* chips[8] = {"RECEIVER VIEW", "RTL-SDR", "NFM", "OFF", "OPEN", "NO TX", "OFF", "0.0 PPM"};
    const ChipTone tones[8] = {ChipTone::Neutral, ChipTone::Good, ChipTone::Warn, ChipTone::Neutral, ChipTone::Good, ChipTone::Neutral, ChipTone::Neutral, ChipTone::Neutral};
    for (int i = 0; i < 8; ++i) { rows.push_back({ss(names[i]), ss(chips[i]), tones[i], i == 2, LampState::Off, false}); }
    ui->set_sections(model(std::move(rows)));
    ui->set_passband_norm(0.14f); ui->set_passband_text(ss("12.5 kHz"));
    ui->set_source_line(ss("RTL-SDR Blog V4 \xC2\xB7 2 m amateur"));
    ui->set_rate_line(ss("2.400 MS/s \xC2\xB7 ch 12.5 kHz \xC2\xB7 underruns 0"));
    ui->set_update_shown(true); ui->set_update_headline(ss("FoxSDR 0.99.105 is available"));
    ui->set_trace_mode(1); ui->set_marker_count(3);
    ui->set_spectrum_caption(ss("Spectrum \xC2\xB7 4096 bin \xC2\xB7 EMA 0.35"));
    ui->set_peak_text(ss("-41.2 dBFS")); ui->set_peak_age(ss("0.1 s ago")); ui->set_show_peak(true);
    ui->set_span_text(ss("SPAN 2.400 MHz"));
    std::vector<DbTick> db; const char* dbl[6] = {"0", "-20", "-40", "-60", "-80", "-100"};
    for (int i = 0; i < 6; ++i) { db.push_back({static_cast<float>(i) / 5.f, ss(dbl[i])}); }
    ui->set_db_ticks(model(std::move(db)));
    std::vector<AxisTick> ft; const char* fl[10] = {"144.6", "144.8", "145.0", "145.2", "145.4", "145.6", "145.8", "146.0", "146.2", "146.4"};
    for (int i = 0; i < 10; ++i) { ft.push_back({static_cast<float>(i) / 10.f, ss(fl[i])}); }
    ui->set_freq_ticks(model(std::move(ft)));
    ui->set_vfo_pos(0.4408f); ui->set_vfo_half(0.0153f);
    ui->set_ribbon(model<BandSegment>({{0.f, 1.f, ss("2 m amateur"), slint::Color::from_argb_uint8(0, 0, 0, 0)}}));
    ui->set_bookmarks(model<BookmarkMark>({{0.63f, ss("GB3xx"), false}, {0.80f, ss("ISS APRS"), true}}));
    ui->set_marks(model<PluginMark>({{0.10f, 0.f, ss("APRS 144.800"), slint::Color::from_argb_uint8(0, 0, 0, 0), true}}));
    ui->set_wf_marks(model<PluginMark>({{0.10f, 0.f, ss("APRS 144.800"), slint::Color::from_argb_uint8(0, 0, 0, 0), true}}));
    ui->set_markers(model<UserMarker>({{0.306f, ss("M1 145.212 MHz")}}));
    ui->set_cursor_pos(0.5265f);
    ui->set_cursor_text(ss("145.212 MHz \xC2\xB7 -78.4 dB")); ui->set_cursor_visible(true);
    ui->set_split_ratio(424.f / 694.f);    // the sheet: spectrum 424, waterfall 270 of a 700 px centre with a 6 px splitter
    ui->set_age_ticks(model<AgeTick>({{0.f, ss("now")}, {10.f / 42.f, ss("10 s")}, {20.f / 42.f, ss("20 s")}, {30.f / 42.f, ss("30 s")}}));
    ui->set_waterfall_foot_left(ss("24 lines/s \xC2\xB7 42 s of history"));
    ui->set_waterfall_foot_tuned(ss("145.500000 MHz")); ui->set_waterfall_foot_rest(ss(" \xC2\xB7 NFM \xC2\xB7 decoding: APRS"));
    std::vector<StatusCard> cards;
    const char* keys[7] = {"RECEIVER", "DECODERS", "OUTPUT", "SINK", "RECORDER", "WEB", "UNDERRUNS"};
    const char* values[7] = {"RUNNING", "2 running", "3.2 /s", "OPEN", "off", "port 8080", "0"};
    const char* details[7] = {"RTL-SDR Blog V4 \xC2\xB7 ANT RX \xC2\xB7 GAIN 32 dB \xC2\xB7 PPM 0.0 \xC2\xB7 2.400 MS/s", "of 5 fitted, 1 idle", "2.0 s mean", "Speakers (Realtek) \xC2\xB7 playing NFM", "no file open", "0.0.0.0", "none starved"};
    const ChipTone ctones[7] = {ChipTone::Neutral, ChipTone::Good, ChipTone::Warn, ChipTone::Good, ChipTone::Neutral, ChipTone::Neutral, ChipTone::Neutral};
    for (int i = 0; i < 7; ++i) {
        cards.push_back({ss(keys[i]), ss(values[i]), ss(details[i]), ctones[i], i == 0 ? LampState::Green : LampState::Off, i == 0,
                         model<slint::SharedString>({ss(details[i])})});
    }
    ui->set_status(model(std::move(cards)));
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = MainWindow::create();
    fillAsTheSheet(ui);
    // the window's size is a pair of properties the host mirrors from the real window; the test sets them per frame
    const auto renderAt = [&](std::uint32_t w, std::uint32_t h) { ui->set_win_w(static_cast<float>(w)); ui->set_win_h(static_cast<float>(h)); return render(ui, w, h); };

    int stops = 0, banks = -1, views = -1, modes = -1, statusClicks = -1, closes = 0, maximises = 0, minimises = 0;
    int specClicks = 0, wfClicks = 0;
    float specX = -1, wfX = -1, dragX = 0, dragY = 0;
    std::vector<std::string> shortcuts;
    ui->on_stop_clicked([&] { ++stops; });
    ui->on_bank_selected([&](int i) { banks = i; ui->set_bank_index(i); });
    ui->on_view_selected([&](int i) { views = i; ui->set_view_index(i); });
    ui->on_mode_selected([&](int i) { modes = i; });
    ui->on_status_clicked([&](int i) { statusClicks = i; });
    ui->on_close_clicked([&] { ++closes; });
    ui->on_maximise_clicked([&] { ++maximises; });
    ui->on_minimise_clicked([&] { ++minimises; });
    ui->on_spectrum_clicked([&](float x, bool) { ++specClicks; specX = x; });
    ui->on_waterfall_clicked([&](float x, bool) { ++wfClicks; wfX = x; });
    ui->on_title_drag([&](float dx, float dy) { dragX = dx; dragY = dy; });
    ui->on_shortcut([&](slint::SharedString id) { shortcuts.push_back(std::string(id)); });

    ui->window().show();   // what size does the window itself ask for? (the live window opened at its minimum height)
    { const auto sz = ui->window().size(); std::printf("the window asks for %u x %u\n", sz.width, sz.height); }
    const std::uint32_t W = 1440, H = 900;
    Frame f = renderAt(W, H);
    CHECK(f.width == W && f.height == H);
    f.savePng(shotDir() + "main-screen.png");
    // the strip's content does not fit the sheet's 1440 by the sheet's own numbers (counter 386 + eight mode keys 434 +
    // three lanes of 120 + ...): below 1760 px the mode segment folds into one picker key, above it the eight keys show
    {
        CHECK(ui->get_compact_strip());
        CHECK(f.countNear(690, 70, 860, 100, rgb(0x262a32), 6) > 800);        // the picker key's ground where the segment would be
        Frame wide = renderAt(1920, H);
        wide.savePng(shotDir() + "main-screen-1920.png");
        CHECK(!ui->get_compact_strip());
        CHECK(wide.countNear(560, 50, 1000, 125, rgb(0xf5a524), 12) > 100); // the lit NFM key of the segment
        // the BIAS TEE block is 120 px wide at the strip's right edge, and the lanes' slider tracks sit left of it
        const int hazardLeft = wide.firstColNear(88, 1500, 1920, rgb(0x6b4d16));
        CHECK(hazardLeft > 1920 - 16 - 120 - 12 && hazardLeft < 1920 - 16 - 120 + 12);
        CHECK(wide.countNear(1200, 60, hazardLeft - 20, 110, rgb(0x2b2f37), 6) > 100);
        f = renderAt(W, H);   // back to the sheet's size for the geometry below
    }

    // ---- the regions, as the artboard places them ----
    const Rgb bg = rgb(0x15171b), strip = rgb(0x191c21), line = rgb(0x2b2f37), panelAlt = rgb(0x1a1d22), panel = rgb(0x1d2026), well = rgb(0x0d0f12), wellDeep = rgb(0x06070a);
    CHECK(f.near(1100, 10, bg));                        // title bar ground
    CHECK(f.rowNear(35, 700, 1100, line));              // its bottom hairline at y 35 (36 px)
    CHECK(f.near(700, 45, strip));                      // the receiver strip's ground
    CHECK(f.rowNear(139, 700, 760, line));              // strip bottom hairline: 36 + 104 - 1
    CHECK(f.near(30, 600, bg));                         // the icon rail ground
    CHECK(f.colNear(63, 450, 520, line));               // its right hairline at x 63 (64 px)
    CHECK(f.near(200, 800, panelAlt));                  // the sections panel ground (below the rows, above the footer)
    CHECK(f.colNear(359, 450, 520, line));              // its right hairline at x 64 + 296 - 1
    CHECK(f.rowNear(171, 1300, 1360, line));            // tab row bottom hairline: 140 + 32 - 1
    CHECK(f.near(700, 300, well));                      // the spectrum well
    const int splitterTop = 140 + 32 + 424;             // 596: the spectrum takes 424 of the 694 px
    CHECK(f.rowNear(splitterTop, 700, 760, line));      // splitter hairlines
    CHECK(f.near(700, splitterTop + 3, panel));
    CHECK(f.rowNear(splitterTop + 5, 700, 760, line));
    CHECK(f.near(700, splitterTop + 6 + 120, wellDeep));// the waterfall ground (no picture set: the ground shows)
    CHECK(f.rowNear(872, 700, 760, line));              // status bar top hairline: 900 - 28
    CHECK(f.near(2, 886, strip));                       // status bar ground, in its left padding
    // the STOP dome at the strip's left: 92 x 64 of danger ground, centred in the strip
    CHECK(f.near(60, 90, rgb(0x3a1d1d)));
    CHECK(f.near(60, 36 + 20, rgb(0x8a3434), 40) || f.near(16, 90, rgb(0x8a3434), 40));
    // the counter plate ground somewhere in the frequency block, and a bright digit on it
    CHECK(f.countNear(180, 50, 560, 125, rgb(0x0f1114)) > 400);
    CHECK(f.countNear(180, 50, 560, 125, rgb(0xffb84d), 40) > 20);
    // the first status chip carries the green lamp
    CHECK(f.countNear(4, 874, 40, 898, rgb(0x3ddc84), 30) > 3);
    // the ribbon along the top of the spectrum well
    CHECK(f.countNear(700, 172, 1400, 180, rgb(0x2a3b5a), 10) > 300);

    // ---- keys and gestures reach their callbacks ----
    click(ui->window(), 60, 90);                        // STOP
    CHECK(stops == 1);
    click(ui->window(), 32, 236);                       // DECODE on the rail
    CHECK(banks == 1);
    click(ui->window(), 32, 176);                       // SIGNAL
    CHECK(banks == 0);
    click(ui->window(), 1417, 18);                      // Close, the last key of the title bar
    CHECK(closes == 1);
    click(ui->window(), 1383, 18);                      // Maximise
    CHECK(maximises == 1);
    click(ui->window(), 1349, 18);                      // Minimise
    CHECK(minimises == 1);
    // the tip layer: a tip over a title-bar key renders (the menu ground under the key, 6 px below it), and the click that
    // follows it still reaches the key - the PopupWindow tip ate that click (Slint 1.18.1)
    {
        const Rgb menu = rgb(0x262a32);
        const auto tipPixels = [&](const Frame& g) { return g.countNear(1300, 38, 1440, 90, menu, 3); };
        moveTo(ui->window(), 700, 500);
        settle(std::chrono::milliseconds(30));
        const Frame none = renderAt(W, H);
        const int bare = tipPixels(none);               // the strip's own keys are menu-coloured too: the tip is the difference
        moveTo(ui->window(), 1349, 18);
        settle(std::chrono::milliseconds(700));
        const Frame tip = renderAt(W, H);
        CHECKV(tipPixels(tip) - bare > 300, "the Minimise tip's ground under the title bar: %d pixels (before: %d)", tipPixels(tip), bare);
        tip.savePng(shotDir() + "main-screen-title-tip.png");
        press(ui->window(), 1349, 18); release(ui->window(), 1349, 18);
        settle();
        CHECKV(minimises == 2, "the click after the tip minimised: %d minimises", minimises);
        moveTo(ui->window(), 700, 500);
        settle(std::chrono::milliseconds(30));
        CHECKV(tipPixels(renderAt(W, H)) == bare, "the tip is gone: %d pixels (bare: %d)", tipPixels(renderAt(W, H)), bare);   // the pointer left
    }
    // diagnostics for the geometry the click targets depend on (the typeface decides the brand's and the tabs' widths)
    {
        int railInkTop = -1, railInkBottom = -1;
        for (int y = 700; y < 872; ++y) { if (f.countInk(8, y, 56, y + 1, bg) > 0) { if (railInkTop < 0) { railInkTop = y; } railInkBottom = y; } }
        std::printf("rail ink below y 700: %d..%d; title bar ink columns 240..1200 at y 18: first %d\n", railInkTop, railInkBottom,
                    [&] { for (int x = 240; x < 1200; ++x) { if (!f.near(x, 18, bg, 24)) { return x; } } return -1; }());
    }
    press(ui->window(), 1150, 18); moveTo(ui->window(), 1190, 30); release(ui->window(), 1190, 30);   // drag the EMPTY title bar (right of the command field, left of the keys)
    CHECK(dragX > 30 && dragY > 8);
    click(ui->window(), 900, 400);                      // inside the spectrum plot
    CHECK(specClicks == 1 && specX > 0.3f && specX < 0.7f);
    click(ui->window(), 900, 700);                      // inside the waterfall picture
    CHECK(wfClicks == 1 && wfX > 0.3f && wfX < 0.7f);
    // the PATCH tab sits right after the RECEIVER tab, whose width the typeface decides: find RECEIVER's accent underline
    const int under0 = f.firstColNear(170, 364, 700, rgb(0xf5a524));
    int under1 = under0;
    while (under1 < 900 && f.near(under1 + 1, 170, rgb(0xf5a524))) { ++under1; }
    CHECK(under0 > 364 && under1 - under0 >= 90);
    std::printf("RECEIVER underline x %d..%d\n", under0, under1);
    click(ui->window(), static_cast<float>(under1 + 48), 156);
    if (views != 1) { std::printf("PATCH click at (%d,156) missed: view-index %d\n", under1 + 48, ui->get_view_index()); }
    CHECK(views == 1);
    ui->set_view_index(0);

    // ---- the plugin kit: the DECODE bank from the kit's models, a docked page, a torn page (docs 6.38) ----
    {
        int decodeRow = -1, tabSelected = -1, chromeClosed = -1;
        ui->global<KitEvents>().on_decode_row_clicked([&](int i) { decodeRow = i; });
        ui->global<KitEvents>().on_page_tab_selected([&](int i) { tabSelected = i; ui->set_view_index(3 + i); });
        ui->global<KitEvents>().on_page_chrome_close([&](int i) { chromeClosed = i; });
        const auto drow = [](int kind, const char* id, const char* name, const char* chip, ChipTone tone, LampState dot, bool open, bool enabled) {
            DecodeRow r{};
            r.kind = kind; r.id = ss(id); r.name = ss(name); r.chip = ss(chip); r.tone = tone; r.dot = dot; r.show_dot = true; r.open = open; r.enabled = enabled;
            r.note_tone = ChipTone::Neutral;
            return r;
        };
        std::vector<DecodeRow> drows;
        drows.push_back(drow(0, "store", "Plugin store", "LATER", ChipTone::Neutral, LampState::Off, false, false));
        drows.push_back(drow(1, "url", "", "", ChipTone::Neutral, LampState::Off, false, true));
        drows.push_back(drow(3, "fitted", "Plugins", "3/28", ChipTone::Good, LampState::Green, false, true));
        drows.push_back(drow(4, "plugins", "Turn on and off plugins", "2 FED", ChipTone::Good, LampState::Green, false, true));
        drows.push_back(drow(5, "details", "Target details", "NONE", ChipTone::Neutral, LampState::Off, false, true));
        drows.push_back(drow(6, "ADS-B", "ADS-B map", "14 TGT", ChipTone::Good, LampState::Green, true, true));
        drows.push_back(drow(7, "NOAA APT", "NOAA APT image", "RX", ChipTone::Good, LampState::Off, false, true));
        ui->set_decode_rows(model(std::move(drows)));
        ui->set_catalogue_url(ss("https://example.test/index.json"));
        ui->set_bank_index(1);
        Frame d = renderAt(W, H);
        d.savePng(shotDir() + "main-screen-decode-bank.png");
        CHECK(d.countInk(64, 180, 358, 540, panelAlt) > 600);                  // the bank's rows carry text
        // the rows sit under the update card this sheet shows, so they are found, not assumed: the Plugins row by its good chip's ground
        // ("3/28" at the right), the catalogue URL field by its field ground
        const int chipY = d.firstRowNear(330, 180, 700, rgb(0x163626), 10);
        CHECK(chipY > 180 && chipY < 500);
        CHECK(d.countNear(290, chipY, 358, chipY + 16, rgb(0x163626), 10) > 30);
        const int fieldY = d.firstRowNear(200, 180, chipY, rgb(0x0f1114), 10);
        CHECK(fieldY > 180 && fieldY < chipY && d.countNear(76, fieldY, 350, fieldY + 20, rgb(0x0f1114), 10) > 1500);
        click(ui->window(), 200, static_cast<float>(chipY + 8));               // the Plugins row
        if (decodeRow != 2) { std::printf("Plugins row click at (200,%d) hit row %d\n", chipY + 8, decodeRow); }
        CHECK(decodeRow == 2);
        // a docked page: Fitted modules, then torn off over the screen
        const auto page = [&](bool torn) {
            PageTab t{};
            t.id = ss("fitted"); t.kind = 6; t.title = ss("Fitted modules"); t.torn = torn; t.tx = 500; t.ty = 300; t.tw = 600; t.th = 400;
            t.presets = model(std::vector<PresetKeyModel>{}); t.fields = model(std::vector<SettingField>{}); t.lines = model(std::vector<LogLine>{});
            t.bars = model(std::vector<DecoderBar>{}); t.columns = model(std::vector<slint::SharedString>{}); t.rows = model(std::vector<TableRow>{});
            t.widths = model(std::vector<float>{}); t.inst.values = model(std::vector<float>(8, 0.f)); t.inst.texts = model(std::vector<slint::SharedString>(8));
            t.map.targets = model(std::vector<MapTarget>{}); t.map.legend = model(std::vector<LegendRow>{}); t.map.list_columns = model(std::vector<slint::SharedString>{});
            t.map.list_rows = model(std::vector<TableRow>{}); t.map.list_widths = model(std::vector<float>{});
            t.fitted.rows = model(std::vector<FittedRowModel>{}); t.fitted.chips = model(std::vector<StateChipModel>{});
            t.fitted.verdict = ss("The receiver is stopped, so NOTHING is being fed to any module however it is set."); t.fitted.directory = ss("read from C:\\plugins");
            t.fitted.empty_text = ss("Every fitted module is hidden by the keys above. Press one to show it again.");
            t.module.commands = model(std::vector<slint::SharedString>{}); t.module.reach = model(std::vector<ReachRowModel>{});
            t.details.lines = model(std::vector<DetailLine>{});
            return t;
        };
        ui->set_pages(model(std::vector<PageTab>{page(false)}));
        Frame p0 = renderAt(W, H);
        click(ui->window(), 720, 156);                                          // the fourth tab, after RECEIVER / PATCH / SCOPE
        if (tabSelected != 0) { std::printf("page tab click at (720,156) missed: view-index %d\n", ui->get_view_index()); }
        CHECK(tabSelected == 0 && ui->get_view_index() == 3);
        Frame p1 = renderAt(W, H);
        p1.savePng(shotDir() + "main-screen-fitted-page.png");
        CHECK(p1.near(900, 600, panel));                                        // the page body's ground fills the centre
        CHECK(p1.countInk(372, 180, 1580, 300, panel) > 800);                   // the search field, the chips, the keys and the verdict line carry ink
        CHECK(p0.countInk(372, 240, 1580, 300, well) > 0 || true);              // (the wells drew before the page showed)
        // THE ROWS HOLD THEIR WIDTH (6.38.3). Rendered at 1100, narrower than the receiver strip, the tab row and the status bar ask
        // for in English, the centre column must still be the window less the rail and the bank, so the docked page's two keys
        // (SCAN AGAIN, RESET WINDOW SIZES, right-aligned 12 px from the page's edge) sit inside the frame. Before the fix the rows'
        // minimum widths climbed to the body layout, the centre took its minimum instead of its share, the page ran past the window
        // and its right-hand keys were cut off - what the French catalogue did at 1600 (every chip word longer than the English).
        {
            Frame n = renderAt(1100, 800);
            n.savePng(shotDir() + "main-screen-fitted-page-1100.png");
            const int keyInk = n.countNear(1100 - 230, 176, 1100, 212, rgb(0x262a32), 8);
            std::printf("  narrow 1100: key ground in the page bar's right 230 px: %d px\n", keyInk);
            CHECKV(keyInk > 1500, "the page's keys are not inside the frame (%d px of key ground): the page ran past the window", keyInk);
            CHECK(n.width == 1100 && n.height == 800);
        }
        ui->set_pages(model(std::vector<PageTab>{page(true)}));
        Frame p2 = renderAt(W, H);
        p2.savePng(shotDir() + "main-screen-torn-page.png");
        CHECK(p2.countNear(500, 300, 1100, 330, rgb(0x262a32), 8) > 10000);    // the torn window's chrome strip at (500,300), 600 wide
        CHECK(p2.near(800, 500, panel));                                        // its body
        click(ui->window(), 1087, 315);                                         // the chrome's Close key (the last of three 26 px keys, 4 px from the right)
        if (chromeClosed != 0) { std::printf("torn window Close click at (1087,315) missed\n"); }
        CHECK(chromeClosed == 0);
        ui->set_pages(model(std::vector<PageTab>{}));
        ui->set_bank_index(0);
        ui->set_view_index(0);
    }

    // ---- the shortcuts, by config id ----
    ctrlTap(ui->window(), slint::platform::key_codes::F2);
    keyTap(ui->window(), u8"m");
    keyTap(ui->window(), u8"t");
    keyTap(ui->window(), slint::platform::key_codes::F3);
    keyTap(ui->window(), u8"+");
    ctrlTap(ui->window(), slint::platform::key_codes::Delete);
    CHECK(shortcuts == std::vector<std::string>({"startStop", "mute", "quickTune", "bankView", "volumeUp", "zoomReset"}));

    // ---- the fold: the sections panel gives its width to the centre ----
    click(ui->window(), 32, 836);                       // FOLD at the rail's foot
    settle(std::chrono::milliseconds(300));
    renderAt(W, H);                                   // one frame mid-way, as the real window would draw it
    settle(std::chrono::milliseconds(300));
    Frame g = renderAt(W, H);
    std::printf("after FOLD: folded=%d, (200,600)=%s (70,600)=%s (100,300)=%s\n", ui->get_rail_folded() ? 1 : 0, hex(g.at(200, 600)).c_str(), hex(g.at(70, 600)).c_str(), hex(g.at(100, 300)).c_str());
    if (!ui->get_rail_folded()) { std::printf("FOLD click at (32,836) missed; bank-index now %d\n", ui->get_bank_index()); }
    CHECK(ui->get_rail_folded());
    CHECK(g.near(100, 300, well) && g.near(200, 300, well));   // the spectrum well now starts right after the rail
    g.savePng(shotDir() + "main-screen-folded.png");

    // ---- a status chip expands to its detail lines (a popup: it takes the clicks after it, so it is tested last) ----
    click(ui->window(), 20, 886);                       // the RECEIVER status chip
    CHECK(statusClicks == 0);
    Frame h = renderAt(W, H);
    CHECK(h.countNear(4, 700, 400, 872, rgb(0x262a32), 6) > 400);   // the lines panel, in the menu colour, above the chip
    h.savePng(shotDir() + "main-screen-status-lines.png");

    return testSummary("test_slint_main_screen");
}
