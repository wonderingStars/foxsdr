// kit_publish.cpp - the plugin kit's publisher (kit_publish.hpp): the kit's state as the window's models, the window's gestures as the
// kit's commands. SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "kit_publish.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

#include "core/i18n.hpp"
#include "core/plugin_ui_data.hpp"
#include "core/utf8_text.hpp"
#include "gui/track_metrics.hpp"

namespace foxsdr::slintkit {

namespace {

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }
std::string fmt(const char* f, double v) { std::string s; cascade::core::formatUtf8(s, f, v); return s; }   // a std::string, never a fixed buffer (test_i18n)
std::string n(std::size_t v) { return std::to_string(v); }
std::int64_t clockMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string h(const std::string& s) { return std::to_string(std::hash<std::string>{}(s)); }
template <class T> std::shared_ptr<slint::Model<T>> model(std::vector<T> v) { return std::make_shared<slint::VectorModel<T>>(std::move(v)); }
std::shared_ptr<slint::Model<slint::SharedString>> strings(const std::vector<std::string>& v) {
    std::vector<slint::SharedString> out;
    for (const auto& s : v) { out.push_back(ss(s)); }
    return model(std::move(out));
}
std::string cell(const char* c) { return std::string(c, ::strnlen(c, CASCADE_PANEL_CELL_CHARS)); }

// A store model with every list present and empty (what every page of another kind carries, and what the page of kind 9 shows before the EXTEND publisher's first
// hand-over): a model that was never set is not trusted to be an empty list by every repeater behind it.
::StoreModel blankStore() {
    ::StoreModel s{};
    s.sites = model(std::vector<::StoreSite>{});
    s.categories = model(std::vector<::StoreCategory>{});
    s.updates = model(std::vector<::UpdateRow>{});
    s.footnotes = model(std::vector<slint::SharedString>{});
    s.cleanup_lines = model(std::vector<slint::SharedString>{});
    s.plugin.badges = model(std::vector<::StoreBadge>{});
    s.plugin.screenshots = model(std::vector<slint::Image>{});
    return s;
}
const ::StoreModel& blankStoreModel() {
    static const ::StoreModel kBlank = blankStore();
    return kBlank;
}

// The same for the Patch presets page (kind 10): the row list present and empty.
::PatchPresetsModel blankPatchPresets() {
    ::PatchPresetsModel p{};
    p.rows = model(std::vector<::PresetRow>{});
    return p;
}
const ::PatchPresetsModel& blankPatchPresetsModel() {
    static const ::PatchPresetsModel kBlank = blankPatchPresets();
    return kBlank;
}

// The same for the Transmit page (kind 11): the three arrays present and empty.
::TransmitPageModel blankTransmit() {
    ::TransmitPageModel t{};
    t.modes = model(std::vector<::TxChoice>{});
    t.inputs = model(std::vector<::TxChoice>{});
    t.arm_labels = model(std::vector<slint::SharedString>{});
    return t;
}
const ::TransmitPageModel& blankTransmitModel() {
    static const ::TransmitPageModel kBlank = blankTransmit();
    return kBlank;
}

ChipTone toneOfState(FittedState s) {
    switch (s) {
        case FittedState::Fed: return ChipTone::Good;
        case FittedState::Refused: return ChipTone::Err;
        case FittedState::NotFed: return ChipTone::Warn;
        default: return ChipTone::Neutral;
    }
}
LampState lampOfState(FittedState s) {
    return s == FittedState::Fed ? LampState::Green : s == FittedState::Refused ? LampState::Red : s == FittedState::NotFed ? LampState::Amber : LampState::Off;
}
slint::Color kindColour(std::uint32_t kind) {
    switch (kind) {
        case CASCADE_TRACK_AIRCRAFT: return slint::Color::from_argb_uint8(255, 0xf5, 0xa5, 0x24);
        case CASCADE_TRACK_VESSEL: return slint::Color::from_argb_uint8(255, 0x7a, 0xd7, 0xff);
        case CASCADE_TRACK_STATION: return slint::Color::from_argb_uint8(255, 0x3d, 0xdc, 0x84);
        case CASCADE_TRACK_SATELLITE: return slint::Color::from_argb_uint8(255, 0xe6, 0xe8, 0xec);
        default: return slint::Color::from_argb_uint8(255, 0xc9, 0xce, 0xd6);
    }
}
cascade::gui::TrackKind kindOf(std::uint32_t kind) {
    return kind == CASCADE_TRACK_SATELLITE ? cascade::gui::TrackKind::Satellite : kind == CASCADE_TRACK_VESSEL ? cascade::gui::TrackKind::Vessel
         : kind == CASCADE_TRACK_AIRCRAFT ? cascade::gui::TrackKind::Aircraft : cascade::gui::TrackKind::Other;
}

// The table rows a panel or an instrument's memory publishes: heading and separator rows keep their kind; a row that asks to be a key and
// whose command the plugin registered is pressable (core/plugin_ui_data.hpp's rule).
std::vector<TableRow> tableRows(const std::vector<CascadePanelRow>& rows, std::size_t columns, const std::vector<cascade::core::HostCommand>& registered, std::string& sig) {
    std::vector<TableRow> out;
    out.reserve(rows.size());
    for (const CascadePanelRow& r : rows) {
        TableRow t{};
        std::vector<slint::SharedString> cells;
        if (r.kind == CASCADE_ROW_HEADING) { t.kind = RowKind::Heading; cells.push_back(ss(cell(r.cells[0]))); sig += "H" + cell(r.cells[0]); }
        else if (r.kind == CASCADE_ROW_SEPARATOR) { t.kind = RowKind::Separator; sig += "S"; }
        else {
            t.kind = RowKind::Cells;
            for (std::size_t c = 0; c < columns && c < CASCADE_PANEL_MAX_COLUMNS; ++c) { const std::string s = cell(r.cells[c]); cells.push_back(ss(s)); sig += s; sig += '|'; }
            t.pressable = cascade::core::panelRowPressable(r, registered) != 0u;
            sig += t.pressable ? "K" : "";
        }
        t.tone = (r.flags & CASCADE_ROW_FLAG_WARN) ? RowTone::Warn : (r.flags & CASCADE_ROW_FLAG_GOOD) ? RowTone::Good : (r.flags & CASCADE_ROW_FLAG_MUTED) ? RowTone::Muted : RowTone::Normal;
        t.band = (out.size() % 2u) == 1u;
        t.cells = model(std::move(cells));
        sig += std::to_string(static_cast<int>(t.tone)) + ";";
        out.push_back(std::move(t));
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
KitPublisher::KitPublisher(MainWindow& ui, PluginKit& kit) : ui_(ui), kit_(kit), pagesModel_(std::make_shared<slint::VectorModel<PageTab>>()), storeModel_(blankStore()), patchPresetsModel_(blankPatchPresets()), transmitModel_(blankTransmit()) {
    globe_ = std::make_unique<GlobePublisher>(kit_);
    ui_.set_pages(pagesModel_);
}
KitPublisher::~KitPublisher() = default;

KitPublisher::Page* KitPublisher::findPage(const std::string& id) {
    for (Page& p : pages_) { if (p.id == id) { return &p; } }
    return nullptr;
}

KitPublisher::Page& KitPublisher::openPage(const std::string& id, int kind, const std::string& plugin, const std::string& file, const std::string& title) {
    if (Page* p = findPage(id)) { return *p; }
    Page p;
    p.id = id;
    p.kind = kind;
    p.plugin = plugin;
    p.file = file;
    p.title = title;
    p.tx = 60.f + 24.f * static_cast<float>(pages_.size() % 6);
    p.ty = 40.f + 24.f * static_cast<float>(pages_.size() % 6);
    pages_.push_back(std::move(p));
    syncOpenSet();
    return pages_.back();
}

void KitPublisher::closePage(const std::string& id) {
    const int shown = currentPage();
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].id != id) { continue; }
        pages_.erase(pages_.begin() + static_cast<std::ptrdiff_t>(i));
        if (shown == static_cast<int>(i)) { ui_.set_view_index(0); }
        else if (shown > static_cast<int>(i)) { ui_.set_view_index(2 + shown); }   // the page before it keeps its place
        break;
    }
    syncOpenSet();
}

void KitPublisher::showPage(const std::string& id) {
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].id == id) { if (!pages_[i].torn) { ui_.set_view_index(3 + static_cast<int>(i)); } return; }
    }
}

int KitPublisher::currentPage() const {
    const int v = ui_.get_view_index();
    return v >= 3 && v - 3 < static_cast<int>(pages_.size()) ? v - 3 : -1;
}

// The kit's open set (the plugin-use table, the map pages' open flags) follows the pages.
void KitPublisher::syncOpenSet() {
    OpenWindows& open = kit_.open();
    open.windows.clear();
    open.mapPages.clear();
    open.decoderOutput = false;
    for (const Page& p : pages_) {
        if (p.kind == 1) { open.decoderOutput = true; }
        else if (p.kind == 2) { open.windows.insert(windowKey("image", p.plugin)); }
        else if (p.kind == 3) { open.windows.insert(windowKey("panel", p.plugin)); }
        else if (p.kind == 4) { open.windows.insert(windowKey("instrument", p.plugin, p.title)); }
        else if (p.kind == 5) { open.mapPages.insert(p.plugin); }
    }
    for (MapPage& pg : kit_.mapPages()) { pg.open = open.mapPages.count(pg.plugin) > 0; }
}

// A preset applied, or a window row pressed: the windows the module DECLARES (app_window.cpp openPluginWindowsFor), and the Decoder output
// for a text decoder when `withOutput`.
void KitPublisher::openWindowsFor(const PluginRow& r, bool withOutput) {
    if (!r.loaded || r.stopped) { return; }
    if ((r.caps & CASCADE_CAP_TRACK_SOURCE) != 0u) { openPage("map:" + r.name, 5, r.name, r.file, mapTitle(r.name)); }
    if ((r.caps & CASCADE_CAP_IMAGE_DECODER) != 0u) { openPage("image:" + r.name, 2, r.name, r.file, imageTitle(r.name)); }
    if ((r.caps & CASCADE_CAP_PANEL) != 0u) {
        const cascade::core::HostPanel* panel = kit_.panelOf(r.name, std::string());
        const std::string title = !r.panelTitle.empty() ? r.panelTitle : panel != nullptr ? panel->title : r.name;
        openPage("panel:" + r.name, 3, r.name, r.file, title);
    }
    if ((r.caps & CASCADE_CAP_INSTRUMENT) != 0u) {
        const cascade::core::HostInstrument* in = kit_.instrumentOf(r.name, std::string());
        const std::string title = !r.instrumentTitle.empty() ? r.instrumentTitle : in != nullptr ? in->title : r.name;
        openPage("instrument:" + r.name + "|" + title, 4, r.name, r.file, title);
    }
    if (withOutput && (r.caps & CASCADE_CAP_DECODER) != 0u && (r.caps & (CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_PANEL)) == 0u) {
        openPage("output", 1, "", "", cascade::i18n::tr("Decoder output"));
    }
}

const PluginRow* KitPublisher::rowOf(const Page& p) const {
    if (!p.file.empty()) { return rowByFile(kit_.lists().rows, p.file); }
    return rowByName(kit_.lists().rows, p.plugin);
}

std::size_t KitPublisher::openMapPages() const {
    std::size_t k = 0;
    for (const Page& p : pages_) { if (p.kind == 5) { ++k; } }
    return k;
}

void KitPublisher::openFromEnv(const std::string& spec) { envSpec_ = spec; envApplied_ = spec.empty(); }

void KitPublisher::setViewPrefsHooks(std::function<std::string()> load, std::function<void(const std::string&)> save) { globe_->setViewPrefsHooks(std::move(load), std::move(save)); }

std::string KitPublisher::globeState(const std::string& what) const {
    if (what == "status") { return globe_->statusLine(); }
    static const std::pair<const char*, const char*> names[] = {{"propagation", "Propagation Tools"}, {"ft8", "FT8"}, {"goes", "GOES HRIT / LRIT"}, {"solar", "Solar System View"}, {"satellites", "Satellites"}};
    for (const auto& n : names) { if (what == n.first) { return globe_->stateText(n.second); } }
    return "globe.propagation|ft8|goes|solar|satellites|status, not globe." + what;
}

std::string KitPublisher::pagesLine() const {
    std::string s = "pages:";
    for (const Page& p : pages_) { s += " " + p.title + (p.torn ? "(torn)" : ""); }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------------------
std::shared_ptr<slint::Model<PresetKeyModel>> KitPublisher::presetModel(const PluginRow& r, const FoxReceiverState& state, bool mine, std::string& sig) {
    std::vector<PresetKeyModel> keys;
    for (const Preset& p : r.presets) {
        if (!mine && p.mine) { continue; }
        PresetKeyModel k{};
        k.label = ss(p.label);
        k.lit = presetLit(p, state.centreHz, state.tunedHz);
        k.mine = p.mine;
        k.tooltip = ss(presetTooltip(r, p, k.lit));
        sig += p.label + (k.lit ? "*" : "") + (p.mine ? "m" : "") + ";";
        keys.push_back(std::move(k));
    }
    return model(std::move(keys));
}

std::shared_ptr<slint::Model<SettingField>> KitPublisher::fieldsModel(const std::string& file, std::string& sig) {
    std::vector<SettingField> fields;
    const SettingsSet* set = settingsFor(kit_.lists().settings, file);
    if (set == nullptr) { return model(std::move(fields)); }
    for (const SettingSpec& s : set->fields) {
        SettingField f{};
        f.id = static_cast<int>(s.id);
        f.key = ss(s.key);
        f.label = ss(s.label);
        f.kind = s.kind == CASCADE_SETTING_NUMBER ? SettingKind::Number : s.kind == CASCADE_SETTING_BOOL ? SettingKind::Bool : s.kind == CASCADE_SETTING_PATH ? SettingKind::Path
               : s.kind == 4u ? SettingKind::Password : SettingKind::Text;
        const std::string shown = kit_.edits().shown(file, s.key, s.value);
        f.value = ss(shown);
        f.checked = shown == "1";
        f.placeholder = ss(s.placeholder);
        f.max_length = static_cast<int>(s.maxLength);
        f.error = ss(kit_.edits().error(file, s.key));
        f.pending = kit_.edits().pending(file, s.key);
        sig += s.key + "=" + shown + (f.pending ? "?" : "") + std::string(f.error) + ";";
        fields.push_back(std::move(f));
    }
    // THE SAME MODEL OBJECT EACH TIME, its rows set in place where they changed (the forms family: a replaced model recreates every
    // field and loses the text being typed; a row set in place keeps the box and only moves what differs).
    auto& cached = fieldModels_[file];
    if (!cached || cached->row_count() != fields.size()) {
        cached = std::make_shared<slint::VectorModel<SettingField>>(std::move(fields));
        return cached;
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const SettingField& now = fields[i];
        const SettingField was = *cached->row_data(i);
        if (was.id != now.id || was.value != now.value || was.error != now.error || was.pending != now.pending || was.checked != now.checked ||
            was.label != now.label || was.kind != now.kind || was.placeholder != now.placeholder || was.max_length != now.max_length) {
            cached->set_row_data(i, now);
        }
    }
    return cached;
}

slint::Image KitPublisher::pictureImage(const std::string& plugin) {
    const cascade::core::HostImage* img = kit_.imageOf(plugin);
    PictureBuffer& pb = pictures_[plugin];
    if (img == nullptr || img->width == 0u || img->height == 0u) { return slint::Image(); }
    if (pb.revision != img->revision || pb.w != img->width || pb.h != img->height) {
        if (pb.w != img->width || pb.h != img->height) { pb.buffer = slint::SharedPixelBuffer<slint::Rgba8Pixel>(img->width, img->height); pb.w = img->width; pb.h = img->height; }
        slint::Rgba8Pixel* px = pb.buffer.begin();
        const std::size_t count = static_cast<std::size_t>(img->width) * img->height;
        const bool rgb = img->format == CASCADE_IMAGE_RGB24;
        const std::size_t need = count * (rgb ? 3u : 1u);
        if (img->pixels.size() >= need) {
            for (std::size_t i = 0; i < count; ++i) {
                if (rgb) { px[i] = {img->pixels[3 * i], img->pixels[3 * i + 1], img->pixels[3 * i + 2], 255}; }
                else { const std::uint8_t g = img->pixels[i]; px[i] = {g, g, g, 255}; }
            }
        }
        pb.revision = img->revision;
    }
    return slint::Image(pb.buffer);
}

KitPublisher::MapSurface& KitPublisher::surfaceFor(const std::string& plugin) {
    auto it = surfaces_.find(plugin);
    if (it == surfaces_.end()) { it = surfaces_.emplace(plugin, std::make_unique<MapSurface>()).first; }
    return *it->second;
}

void KitPublisher::fillPrelude(const Page& p, PageTab& t, std::string& sig) {
    const PluginRow* r = rowOf(p);
    t.has_presets = r != nullptr && !r->presets.empty();
    t.presets = t.has_presets ? presetModel(*r, state_, true, sig) : model(std::vector<PresetKeyModel>{});
    const SettingsSet* set = r != nullptr ? settingsFor(kit_.lists().settings, r->file) : nullptr;
    t.has_settings = set != nullptr && !set->fields.empty();
    t.fields = t.has_settings ? fieldsModel(r->file, sig) : model(std::vector<SettingField>{});
    t.settings_open = p.settingsOpen;
    sig += p.settingsOpen ? "o" : "c";
}

void KitPublisher::fillOutput(PageTab& t, std::string& sig) {
    std::vector<LogLine> lines;
    const auto& ring = kit_.decoderLines();
    for (const DecoderLine& l : ring) {
        LogLine x{};
        x.plugin = ss(l.plugin);
        x.text = ss(l.text);
        x.tone = l.tone == 1 ? LineTone::Warn : l.tone == 2 ? LineTone::Err : LineTone::Normal;
        lines.push_back(std::move(x));
    }
    sig += "L" + n(ring.size()) + (ring.empty() ? "" : std::to_string(ring.back().id));
    t.lines = model(std::move(lines));
    t.count_text = ss(cascade::core::formatText(ring.size() == 1 ? cascade::i18n::tr("%d line") : cascade::i18n::tr("%d lines"), static_cast<int>(ring.size())));
    std::vector<DecoderBar> bars;
    for (const PluginRow& r : kit_.lists().rows) {
        if (!r.loaded || r.stopped || r.presets.empty() || (r.caps & CASCADE_CAP_DECODER) == 0u) { continue; }
        DecoderBar b{};
        b.name = ss(r.name);
        b.file = ss(r.file);
        b.presets = presetModel(r, state_, true, sig);
        bars.push_back(std::move(b));
    }
    t.bars = model(std::move(bars));
}

void KitPublisher::fillPicture(const Page& p, PageTab& t, std::string& sig) {
    const cascade::core::HostImage* img = kit_.imageOf(p.plugin);
    t.have = img != nullptr && img->width > 0u;
    t.picture = pictureImage(p.plugin);
    if (t.have) {
        t.status = ss(cascade::core::formatText(img->complete ? cascade::i18n::tr("%ux%u  complete  frame %llu") : cascade::i18n::tr("%ux%u  receiving...  frame %llu"), static_cast<unsigned>(img->width), static_cast<unsigned>(img->height), static_cast<unsigned long long>(img->sequence)));
        t.interactive = img->interactive;
        t.pic_w = static_cast<int>(img->width);
        t.pic_h = static_cast<int>(img->height);
        sig += "P" + std::to_string(img->revision) + (img->complete ? "c" : "r");
    } else {
        t.status = ss(cascade::i18n::tr("Waiting for the first image..."));
        t.interactive = false;
        t.pic_w = 0; t.pic_h = 0;
        sig += "P0";
    }
    t.save_enabled = t.have;
    t.save_note = ss(p.saveNote);
    sig += p.saveNote;
}

void KitPublisher::fillPanel(const Page& p, PageTab& t, std::string& sig) {
    const cascade::core::HostPanel* panel = kit_.panelOf(p.plugin, p.title);
    const PluginRow* r = rowOf(p);
    static const std::vector<cascade::core::HostCommand> kNone;
    std::vector<slint::SharedString> cols;
    std::vector<TableRow> rows;
    if (panel != nullptr) {
        for (const std::string& c : panel->headings) { cols.push_back(ss(c)); sig += c + "|"; }
        rows = tableRows(panel->rows, panel->headings.size(), r != nullptr ? r->commands : kNone, sig);
    }
    t.columns = model(std::move(cols));
    t.rows = model(std::move(rows));
    std::vector<float> widths(p.widths.begin(), p.widths.end());
    t.widths = model(std::move(widths));
    t.empty_text = ss(panel == nullptr ? (r != nullptr && r->asleep ? cascade::i18n::tr("Idle: nothing is using this plugin yet.") : cascade::i18n::tr("Nothing to show yet.")) : cascade::i18n::tr("Nothing to show yet."));
}

void KitPublisher::fillInstrument(const Page& p, PageTab& t, std::string& sig) {
    const cascade::core::HostInstrument* in = kit_.instrumentOf(p.plugin, p.title);
    const PluginRow* r = rowOf(p);
    static const std::vector<cascade::core::HostCommand> kNone;
    InstrumentState s{};
    std::vector<float> values(8, 0.f);
    std::vector<slint::SharedString> texts(8);
    t.inst_title = ss(p.title);
    t.inst_kind = in != nullptr ? static_cast<int>(in->kind) : 0;
    t.obs_course = p.obsCourse;
    if (in != nullptr) {
        s.have = in->have;
        s.alert = in->have && (in->state.flags & CASCADE_INSTRUMENT_FLAG_ALERT) != 0u;
        s.lock = in->have && (in->state.flags & CASCADE_INSTRUMENT_FLAG_LOCK) != 0u;
        s.low_batt = in->have && (in->state.flags & CASCADE_INSTRUMENT_FLAG_LOW_BATT) != 0u;
        s.seq = static_cast<int>(in->state.seq);
        for (int i = 0; i < 8; ++i) { values[static_cast<std::size_t>(i)] = static_cast<float>(in->state.values[i]); texts[static_cast<std::size_t>(i)] = ss(std::string(in->state.text[i], ::strnlen(in->state.text[i], CASCADE_INSTRUMENT_TEXT_CHARS))); }
        // NEW: the event arrived while the page was not showing; the page marks it seen as it draws (the chip of a closed page keeps saying NEW)
        const std::string key = windowKey("instrument", p.plugin, p.title);
        s.unread = in->have && kit_.instrumentSeen(key) != in->state.seq;
        kit_.markInstrumentSeen(key, in->state.seq);
        sig += "I" + std::to_string(in->state.seq) + (in->have ? "h" : "-") + std::to_string(in->state.flags);
        for (int i = 0; i < 8; ++i) { sig += fmt("%.4g,", in->state.values[i]) + std::string(texts[static_cast<std::size_t>(i)]) + "|"; }
        std::vector<slint::SharedString> cols;
        for (const std::string& c : in->headings) { cols.push_back(ss(c)); sig += c; }
        t.columns = model(std::move(cols));
        t.rows = model(tableRows(in->rows, in->headings.size(), r != nullptr ? r->commands : kNone, sig));
    } else {
        s.have = false;
        t.columns = model(std::vector<slint::SharedString>{});
        t.rows = model(std::vector<TableRow>{});
        sig += "I-";
    }
    s.values = model(std::move(values));
    s.texts = model(std::move(texts));
    s.now_ms = static_cast<int>(nowMs_ % 1000000);
    if (s.alert) { sig += std::to_string((nowMs_ / 250) % 2); }   // the blink
    if (in != nullptr && in->kind == CASCADE_INSTRUMENT_FAX) {
        const cascade::core::HostImage* img = kit_.imageOf(p.plugin);
        s.chart = pictureImage(p.plugin);
        s.chart_complete = img != nullptr && img->complete;
        if (img != nullptr) { sig += "C" + std::to_string(img->revision); }
    }
    t.inst = s;
    std::vector<float> widths(p.widths.begin(), p.widths.end());
    t.widths = model(std::move(widths));
    t.empty_text = ss(cascade::i18n::tr("Nothing to show yet."));
}

void KitPublisher::fillMap(const Page& p, PageTab& t, const FoxReceiverState& state, std::string& sig) {
    MapSurface& s = surfaceFor(p.plugin);
    MapPage* pg = findMapPage(kit_.mapPages(), p.plugin);
    MapModel m{};
    const bool home = (state.flags & FOXAPI_RX_POSITION_SET) != 0u;
    std::vector<const cascade::core::HostTrack*> mine;
    for (const cascade::core::HostTrack& tr : kit_.tracks()) { if (tr.plugin == p.plugin) { mine.push_back(&tr); } }
    // the view: fitted to the first tracks, then the operator's
    if (pg != nullptr && !pg->viewFitted && !mine.empty()) {
        std::vector<cascade::core::HostTrack> copy;
        for (const auto* tr : mine) { copy.push_back(*tr); }
        if (fitView(copy, pg->centreLat, pg->centreLon, pg->spanDeg, s.h > 0 ? static_cast<double>(s.w) / s.h : 1.6)) { pg->viewFitted = true; }
    }
    if (pg != nullptr && !pg->followedId.empty()) {
        for (const auto* tr : mine) { if (pg->followedId == tr->t.id && cascade::core::trackHasPosition(tr->t)) { pg->centreLat = tr->t.latDeg; pg->centreLon = tr->t.lonDeg; break; } }
    }
    // the compositor: the basemap when attached (Mercator), else the coastline (equirectangular); satellites always the coastline
    BasemapClient& bm = kit_.basemap();
    const bool tiles = bm.active() && !(pg != nullptr && pg->satellite);
    if (tiles != s.sourceSet || (tiles && bm.attribution() != s.sourceAttribution)) {
        s.sourceSet = tiles;
        s.sourceAttribution = tiles ? bm.attribution() : std::string();
        if (tiles) {
            s.comp.setTileSource([&bm](std::uint32_t z, std::uint32_t x, std::uint32_t y, std::vector<std::uint8_t>& rgb) { return static_cast<int>(bm.rawTile(z, x, y, rgb)); },
                                 bm.tileSize(), bm.minZoom(), bm.maxZoom());
        } else {
            s.comp.setTileSource(slintmap::TileSource{}, 256, 0, 0);
        }
        s.comp.setMercator(tiles);
    }
    if (s.w > 0 && s.h > 0) {
        s.comp.setSize(s.w, s.h);
        if (pg != nullptr) { s.comp.setView(pg->centreLat, pg->centreLon, pg->spanDeg); }
        s.comp.setLayers(!tiles, true);
        s.comp.setTrailLook(look_.trailWidthPx, look_.iconPx, look_.trailStyle);   // the Display settings: trail width (to the wingspan) and style
        s.comp.setHome(home, state.rxLatDeg, state.rxLonDeg);
        if (s.coverageOn && home) {
            std::vector<double> km(static_cast<std::size_t>(cascade::gui::CoverageMap::kBuckets), 0.0);
            for (int b = 0; b < cascade::gui::CoverageMap::kBuckets; ++b) { km[static_cast<std::size_t>(b)] = kit_.coverage().maxKm(b); }
            s.comp.setCoverage(km);
        } else { s.comp.setCoverage({}); }
        std::vector<slintmap::Trail> trails;
        if (s.trailsOn) {
            for (const cascade::core::HostPath& path : kit_.paths()) {
                if (path.plugin != p.plugin) { continue; }
                slintmap::Trail tr;
                for (const CascadePathPoint& pt : path.points) { tr.points.emplace_back(pt.latDeg, pt.lonDeg); }
                const slint::Color c = kindColour(path.kind);
                tr.rgba = (static_cast<std::uint32_t>(c.red()) << 24) | (static_cast<std::uint32_t>(c.green()) << 16) | (static_cast<std::uint32_t>(c.blue()) << 8) | 0xb0u;
                tr.dashed = (path.flags & CASCADE_PATH_FLAG_DASHED) != 0u;
                tr.closed = (path.flags & CASCADE_PATH_FLAG_CLOSED) != 0u;
                trails.push_back(std::move(tr));
            }
        }
        s.comp.setTrails(trails);
        auto& buf = s.buffers[static_cast<std::size_t>(s.which)];
        if (buf.width() != static_cast<std::uint32_t>(s.w) || buf.height() != static_cast<std::uint32_t>(s.h)) { buf = slint::SharedPixelBuffer<slint::Rgba8Pixel>(static_cast<std::uint32_t>(s.w), static_cast<std::uint32_t>(s.h)); }
        if (s.comp.compose(reinterpret_cast<std::uint8_t*>(buf.begin()), s.w, s.h)) {
            sig += "B" + std::to_string(nowMs_);
            s.which ^= 1;
            auto& other = s.buffers[static_cast<std::size_t>(s.which)];
            if (other.width() != buf.width() || other.height() != buf.height()) { other = slint::SharedPixelBuffer<slint::Rgba8Pixel>(buf.width(), buf.height()); }
            std::memcpy(other.begin(), buf.begin(), sizeof(slint::Rgba8Pixel) * buf.width() * buf.height());
        }
        m.base = slint::Image(s.buffers[static_cast<std::size_t>(s.which ^ 1)]);
    }
    // the targets as items, the list as rows
    std::vector<MapTarget> targets;
    std::vector<cascade::gui::TrackRow> rows;
    s.targetIds.clear();
    std::size_t placed = 0;
    for (std::size_t i = 0; i < mine.size(); ++i) {
        const cascade::core::HostTrack& tr = *mine[i];
        const cascade::core::TrackPresentation pres = cascade::core::trackPresentation(tr.t);
        if (!pres.visible) { continue; }
        const bool hasPos = cascade::core::trackHasPosition(tr.t);
        double fx = -1, fy = -1;
        bool onScreen = false;
        if (hasPos) {
            ++placed;
            onScreen = s.comp.project(tr.t.latDeg, tr.t.lonDeg, fx, fy);
            if (onScreen && targets.size() < 300) {
                MapTarget mt{};
                mt.id = ss(tr.t.id);
                mt.x = static_cast<float>(fx);
                mt.y = static_cast<float>(fy);
                mt.heading = std::isfinite(tr.t.courseDeg) ? static_cast<float>(tr.t.courseDeg) : -1.f;
                mt.kind = static_cast<int>(tr.t.kind);
                mt.label = ss(tr.t.label[0] != '\0' ? std::string(tr.t.label) : std::string(tr.t.id));
                mt.alpha = pres.alpha;
                mt.selected = pg != nullptr && pg->selectedId == tr.t.id;
                mt.followed = pg != nullptr && pg->followedId == tr.t.id;
                mt.estimated = cascade::core::trackIsEstimated(tr.t);
                mt.emergency = (tr.t.flags & CASCADE_TRACK_FLAG_EMERGENCY) != 0u;
                mt.size = static_cast<float>(slintmap::targetIconPx(static_cast<int>(tr.t.kind), look_));   // the Display setting's icon size: an aircraft's only
                if (s.altColours && tr.t.kind == CASCADE_TRACK_AIRCRAFT && std::isfinite(tr.t.altM)) {
                    const auto& band = cascade::gui::altBandStyle(cascade::gui::altitudeBandIndex(tr.t.altM));
                    mt.color = slint::Color::from_argb_uint8(255, band.r, band.g, band.b);
                } else { mt.color = kindColour(tr.t.kind); }
                sig += std::string(tr.t.id) + fmt("%.3f", fx) + fmt("%.3f", fy) + (mt.selected ? "s" : "") + (mt.followed ? "f" : "") + ";";
                s.targetIds.emplace_back(tr.t.id);
                targets.push_back(std::move(mt));
            }
        }
        if (s.onScreenOnly && hasPos && !onScreen) { continue; }
        cascade::gui::TrackRow row;
        row.label = tr.t.label[0] != '\0' ? std::string(tr.t.label) : std::string(tr.t.id);
        row.id = tr.t.id;
        row.altM = tr.t.altM;
        row.speedMps = tr.t.speedMps;
        row.courseDeg = tr.t.courseDeg;
        if (home && hasPos) {
            row.distanceKm = cascade::gui::greatCircleKm(state.rxLatDeg, state.rxLonDeg, tr.t.latDeg, tr.t.lonDeg);
            row.bearingDeg = cascade::gui::initialBearingDeg(state.rxLatDeg, state.rxLonDeg, tr.t.latDeg, tr.t.lonDeg);
        }
        row.ageMs = tr.t.ageMs;
        row.source = i;
        rows.push_back(std::move(row));
    }
    std::size_t unplaced = 0;
    for (const cascade::core::HostTrack& tr : kit_.unplacedTracks()) { if (tr.plugin == p.plugin && cascade::core::trackPresentation(tr.t).visible) { ++unplaced; } }
    cascade::gui::sortTrackRows(rows, cascade::gui::trackSortKeyForMenuIndex(s.sortKey), s.sortAscending);
    std::vector<TableRow> listRows;
    s.listIds.clear();
    for (const auto& row : rows) {
        const cascade::core::HostTrack& tr = *mine[row.source];
        TableRow t2{};
        std::string note;
        if (!cascade::core::trackHasPosition(tr.t)) { note = cascade::gui::unplacedRowNote(tr.t.altM, kindOf(tr.t.kind), cascade::i18n::tr); }
        else if (cascade::core::trackIsEstimated(tr.t)) { note = cascade::gui::estimatedRowNote(cascade::i18n::tr); }
        else if (std::isfinite(row.distanceKm)) { note = fmt("%.0f km ", row.distanceKm) + fmt("%03.0f", row.bearingDeg); }
        t2.cells = model(std::vector<slint::SharedString>{ss(row.label), ss(row.id), ss(note)});
        t2.kind = RowKind::Cells;
        t2.tone = pg != nullptr && pg->selectedId == row.id ? RowTone::Key : RowTone::Normal;
        t2.band = (listRows.size() % 2u) == 1u;
        t2.pressable = true;
        s.listIds.push_back(row.id);
        sig += row.label + row.id + note + ";";
        listRows.push_back(std::move(t2));
    }
    for (const cascade::core::HostTrack& tr : kit_.unplacedTracks()) {
        if (tr.plugin != p.plugin || !cascade::core::trackPresentation(tr.t).visible) { continue; }
        TableRow t2{};
        t2.cells = model(std::vector<slint::SharedString>{ss(tr.t.label[0] != '\0' ? std::string(tr.t.label) : std::string(tr.t.id)), ss(tr.t.id), ss(cascade::gui::unplacedRowNote(tr.t.altM, kindOf(tr.t.kind), cascade::i18n::tr))});
        t2.kind = RowKind::Cells;
        t2.tone = RowTone::Muted;
        t2.band = (listRows.size() % 2u) == 1u;
        t2.pressable = false;
        s.listIds.emplace_back(tr.t.id);
        sig += std::string(tr.t.id) + "u;";
        listRows.push_back(std::move(t2));
    }
    m.targets = model(std::move(targets));
    m.list_columns = model(std::vector<slint::SharedString>{ss(cascade::i18n::tr("Callsign")), ss(cascade::i18n::tr("ID")), ss(cascade::i18n::tr("Note"))});
    m.list_rows = model(std::move(listRows));
    m.list_widths = model(std::vector<float>{90.f, 80.f, 0.f});
    m.count_text = ss(countText(rows.size(), placed, unplaced));
    std::vector<LegendRow> legend;
    if (s.altColours) {
        for (int b = 0; b < cascade::gui::kAltBandCount; ++b) { const auto& st = cascade::gui::altBandStyle(b); legend.push_back({ss(st.label), slint::Color::from_argb_uint8(255, st.r, st.g, st.b)}); }
    }
    m.legend = model(std::move(legend));
    const double mpp = s.comp.metresPerPixel();
    if (mpp > 0 && s.w > 0) {
        const double steps[] = {100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000, 2000000, 5000000};
        double pick = steps[0];
        for (double st : steps) { if (st / mpp <= 0.3 * s.w) { pick = st; } }
        m.scale_frac = static_cast<float>(pick / mpp / s.w);
        m.scale_text = ss(pick >= 1000 ? fmt("%.0f km", pick / 1000) : fmt("%.0f m", pick));
    }
    m.attribution = ss(tiles ? bm.attribution() : cascade::i18n::tr("Coastline: Natural Earth (public domain)"));
    m.zoom_in = pg != nullptr && pg->spanDeg > slintmap::TileCompositor::kMinSpanDeg;
    m.zoom_out = pg != nullptr && pg->spanDeg < slintmap::TileCompositor::kMaxSpanDeg;
    m.pick_armed = s.pickArmed;
    m.hover_text = ss(s.hoverText);
    m.following = ss(pg != nullptr && !pg->followedId.empty() ? cascade::core::formatText(cascade::i18n::tr("following %s"), pg->followedId.c_str()) : std::string());
    m.coverage_on = s.coverageOn;
    m.trails_on = s.trailsOn;
    m.alt_colours = s.altColours;
    m.on_screen_only = s.onScreenOnly;
    m.coverage_empty = kit_.coverage().empty();
    m.coverage_status = ss(!home ? std::string(cascade::i18n::tr("set the RX position to measure coverage")) : kit_.coverage().empty() ? std::string(cascade::i18n::tr("nothing heard yet"))
                           : cascade::core::formatText(cascade::i18n::tr("%d of %d bearings, best %.0f km"), static_cast<int>(kit_.coverage().filledBuckets()), static_cast<int>(cascade::gui::CoverageMap::kBuckets), kit_.coverage().peakKm()));
    m.sort_label = ss(cascade::gui::trackSortKeyName(cascade::gui::trackSortKeyForMenuIndex(s.sortKey)));
    m.sort_ascending = s.sortAscending;
    m.rx_known = home;
    m.rx_text = ss(home ? "RX " + fmt("%.4f", state.rxLatDeg) + " " + fmt("%.4f", state.rxLonDeg) : std::string(cascade::i18n::tr("no receiver position: Set RX here, then click the map")));
    sig += std::string(m.count_text) + std::string(m.following) + std::string(m.coverage_status) + (s.pickArmed ? "P" : "") + s.hoverText + std::string(m.scale_text) + std::string(m.attribution);
    sig += std::to_string(s.coverageOn) + std::to_string(s.trailsOn) + std::to_string(s.altColours) + std::to_string(s.onScreenOnly) + std::to_string(s.sortKey) + std::to_string(s.sortAscending);
    sig += "L" + std::to_string(look_.iconPx) + "." + std::to_string(look_.trailWidthPx) + "." + std::to_string(static_cast<int>(look_.trailStyle));   // the icon size is in the targets: a change of look is a change of page
    t.map = m;
}

void KitPublisher::fillFitted(PageTab& t, const FoxReceiverState& state, std::string& sig) {
    const bool running = (state.flags & FOXAPI_RX_RUNNING) != 0u;
    FittedModel f{};
    std::vector<FittedRowModel> rows;
    std::array<int, 6> counts{};
    std::string lowerSearch = fittedSearch_;
    for (char& c : lowerSearch) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    const auto matches = [&](const PluginRow& r) {
        if (lowerSearch.empty()) { return true; }
        std::string hay = r.name + " " + r.file + " " + r.version;
        for (char& c : hay) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        return hay.find(lowerSearch) != std::string::npos;
    };
    std::vector<const PluginRow*> ordered;
    for (const PluginRow& r : kit_.lists().rows) { ordered.push_back(&r); }
    std::stable_sort(ordered.begin(), ordered.end(), [](const PluginRow* a, const PluginRow* b) { return a->category < b->category; });
    for (const PluginRow* rp : ordered) {
        const PluginRow& r = *rp;
        const FittedState st = fittedState(r, running);
        ++counts[static_cast<std::size_t>(fittedStateIndex(st))];
        if (fittedHidden_.count(fittedStateIndex(st)) > 0 || !matches(r)) { continue; }
        FittedRowModel m{};
        m.file = ss(r.file);
        m.name = ss(r.caps != 0u ? r.name : r.file);
        m.version = ss(r.version);
        m.group = ss(r.category);
        m.state = ss(fittedStateWord(st));
        m.tone = toneOfState(st);
        m.dot = lampOfState(st);
        std::string note;
        ChipTone noteTone = ChipTone::Neutral;
        if (!r.loaded) { note = r.error.empty() ? std::string(cascade::i18n::tr("The host refused this file and recorded no reason.")) : r.error; noteTone = ChipTone::Err; }
        else if (!r.integrity.empty()) { note = r.integrity; noteTone = ChipTone::Err; }
        else if (!r.notice.empty()) { note = r.notice; noteTone = r.noticeLevel >= CASCADE_LOG_ERROR ? ChipTone::Err : ChipTone::Warn; }
        else if (r.originWord == "orphan") { note = cascade::i18n::tr("Not installed from the plugin store."); noteTone = ChipTone::Warn; }
        m.note = ss(note);
        m.note_tone = noteTone;
        m.can_stop = r.loaded;
        m.offers_start = fittedOffersStart(r);
        m.can_remove = true;
        const bool armed = armedRemove_ == r.file && nowMs_ - armedAtMs_ < 5000;
        m.armed = armed;
        m.remove_label = ss(armed ? cascade::i18n::tr("CONFIRM") : r.originWord == "orphan" ? cascade::i18n::tr("REMOVE FILE") : cascade::i18n::tr("REMOVE"));
        sig += r.file + std::string(m.state) + note + (armed ? "A" : "") + ";";
        rows.push_back(std::move(m));
    }
    f.rows = model(std::move(rows));
    static const char* kChipWords[6] = {FOX_TR_NOOP("FED"), FOX_TR_NOOP("IDLE"), FOX_TR_NOOP("NOT DECODING"), FOX_TR_NOOP("TAKES NO SIGNAL"), FOX_TR_NOOP("STOPPED"), FOX_TR_NOOP("REFUSED")};
    std::vector<StateChipModel> chips;
    for (int i = 0; i < 6; ++i) {
        StateChipModel c{};
        c.label = ss(cascade::i18n::tr(kChipWords[i]));
        c.count = counts[static_cast<std::size_t>(i)];
        c.tone = i == 0 ? ChipTone::Good : i == 2 ? ChipTone::Warn : i == 5 ? ChipTone::Err : ChipTone::Neutral;
        c.lamp = c.count > 0 && (i == 0 || i == 2 || i == 5) ? (i == 0 ? LampState::Green : i == 2 ? LampState::Amber : LampState::Red) : LampState::Off;
        c.on = fittedHidden_.count(i) == 0;
        sig += std::to_string(c.count) + (c.on ? "+" : "-");
        chips.push_back(std::move(c));
    }
    f.chips = model(std::move(chips));
    f.search = ss(fittedSearch_);
    // the verdict sentence and, beside it, the folder (the component joins them with a middle dot: no folder twice)
    f.verdict = ss(running ? cascade::i18n::tr("The receiver is running, so a module with a matched decoder is being fed.") : cascade::i18n::tr("The receiver is stopped, so NOTHING is being fed to any module however it is set."));
    f.directory = ss(kit_.lists().pluginDir.empty() ? std::string(cascade::i18n::tr("No directory has been scanned yet.")) : cascade::core::formatText(cascade::i18n::tr("read from %s"), kit_.lists().pluginDir.c_str()));
    f.report = ss(kit_.lists().installReport);
    f.error = ss(kit_.lists().installError);
    f.empty_text = ss(kit_.lists().rows.empty() ? cascade::i18n::tr("No module the host can load was found in the folder above, so nothing is fitted. The plugin store is where one is fitted from.")
                                                 : cascade::i18n::tr("Every fitted module is hidden by the keys above. Press one to show it again."));
    f.cleanup_label = ss("");
    sig += std::string(f.verdict) + std::string(f.report) + std::string(f.error) + fittedSearch_;
    t.fitted = f;
}

void KitPublisher::fillModule(const Page& p, PageTab& t, const FoxReceiverState& state, std::string& sig) {
    const PluginRow* r = rowByFile(kit_.lists().rows, p.file);
    ModuleModel m{};
    m.file = ss(p.file);
    if (r == nullptr) {
        m.name = ss(p.title);
        m.sentence = ss(cascade::i18n::tr("This module is no longer fitted."));
        t.module = m;
        sig += "gone";
        return;
    }
    const bool running = (state.flags & FOXAPI_RX_RUNNING) != 0u;
    const FittedState st = fittedState(*r, running);
    m.name = ss(r->caps != 0u ? r->name : r->file);
    m.maker_line = ss((r->author.empty() ? std::string(cascade::i18n::tr("maker not stated")) : r->author) + (r->version.empty() ? std::string() : "  \xC2\xB7  " + cascade::core::formatText(cascade::i18n::tr("version %s"), r->version.c_str())));
    m.state_word = ss(fittedStateWord(st));
    m.state_tone = toneOfState(st);
    m.lamp = lampOfState(st);
    m.offers_start = fittedOffersStart(*r);
    m.can_run = r->loaded;
    m.tune_shown = r->loaded && (r->caps & CASCADE_CAP_HOST_CLIENT) != 0u;
    m.tune_on = r->tuneAllowed;
    m.settings_shown = r->loaded && r->askedSettings;
    m.settings_on = r->settingsAllowed;
    const bool txGranted = (r->txBits & 32u) != 0u;
    m.transmit_shown = r->loaded && ((r->txBits & 2u) != 0u || txGranted);
    m.transmit_on = txGranted;
    m.settings_note = ss(m.settings_shown ? cascade::i18n::tr("Radio settings lets this module change the mode, bandwidth, squelch, gains, sample rate, volume and mute, and start or stop the receiver. It is separate from receiver control.") : "");
    m.transmit_note = ss(m.transmit_shown ? cascade::i18n::tr("The transmit grant alone keys nothing: ARM FOR PLUGINS on the TRANSMIT page must be open as well (5, 15 or 60 minutes).") : "");
    m.tx_state = ss(r->loaded ? txPlateSentence(r->txBits, kit_.lists().txArmed) : "");
    m.tx_hot = (r->txBits & 64u) != 0u;
    std::vector<slint::SharedString> commands;
    if (r->loaded && !r->stopped) { for (const auto& c : r->commands) { commands.push_back(ss(c.label)); } }
    m.commands = model(std::move(commands));
    std::vector<ReachRowModel> reach;
    for (const ReachRow& row : reachRows(r->caps)) { ReachRowModel x; x.key = ss(row.key); x.detail = ss(row.detail); x.outward = row.outward; reach.push_back(std::move(x)); }
    m.reach_lead = ss(r->loaded ? cascade::i18n::tr("Declared by the maker, not enforced. A fitted module is loaded into this application's own process and runs with every privilege the application has: there is no sandbox and no permission model. This list is what the module says it PROVIDES, not a limit on what it can take.")
                                : cascade::i18n::tr("Not known here, and nothing is routed to it: the host refused the file before a descriptor was read."));
    m.reach = model(std::move(reach));
    m.reach_note = ss(!r->loaded ? "" : (r->caps & (CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_TX_AUDIO)) != 0u
                      ? cascade::i18n::tr("The tune grant above, the radio-settings grant and the transmit grant are the only permissions this console does enforce: without them every request to retune, to change the receiver's settings, or to transmit, is refused. Nothing else in the list is a gate.")
                      : cascade::i18n::tr("The only permissions this console enforces are the per-module tune, radio-settings and transmit grants, and this module asks for none of them. Nothing else in the list is a gate."));
    m.sentence = ss(fittedStateSentence(*r, running));
    m.refused = st == FittedState::Refused;
    m.fault = ss(r->fault);
    m.keep_shown = r->loaded && !r->stopped && capsOfferKeepRunning(r->caps);
    m.keep_running = r->runMode == 1;
    m.orphan = ss(r->originWord == "orphan" ? cascade::i18n::tr("This file was not installed from the plugin store - it has no install record and the catalogue cannot update it.") : "");
    m.integrity = ss(r->integrity);
    m.origin = ss(r->originWord == "site" ? cascade::core::formatText(cascade::i18n::tr("Installed from %s, not from the FoxSDR catalogue."), r->originSite.c_str())
                  : r->originWord == "removed-site" ? cascade::core::formatText(cascade::i18n::tr("Installed from %s, a site you removed. It gets no updates."), r->originSite.c_str()) : std::string());
    m.notice = ss(r->loaded ? r->notice : "");
    m.notice_err = r->noticeLevel >= CASCADE_LOG_ERROR;
    m.file_path = ss(r->path.empty() ? std::string(cascade::i18n::tr("not recorded")) : r->path);
    m.abi_text = ss(abiText(r->abiVersion, static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION)));
    m.fitted_text = ss(fittedDateText(r->fittedAtUnix));
    m.catalogue_note = ss(r->hasRecord && !r->catalogueKey.empty() ? cascade::core::formatText(cascade::i18n::tr("Catalogue entry: %s (the store's pictures and description come with a later beta)"), r->catalogueKey.c_str()) : std::string());
    sig += std::string(m.state_word) + std::string(m.sentence) + std::string(m.tx_state) + std::string(m.notice) + std::to_string(r->tuneAllowed) + std::to_string(r->settingsAllowed) + std::to_string(r->txBits) + std::to_string(r->runMode) + std::to_string(r->commands.size());
    t.module = m;
}

DetailsModel KitPublisher::detailsModel(const FoxReceiverState& state, std::string& sig) {
    DetailsModel d{};
    d.chip = ss(detailsId_.empty() ? std::string(cascade::i18n::tr("NONE")) : detailsId_.substr(0, 10));
    d.lamp = LampState::Off;
    d.gone = false;
    d.can_go = false;
    d.following = false;
    std::vector<DetailLine> lines;
    if (detailsId_.empty()) {
        d.empty_hint = ss(cascade::i18n::tr("No target chosen. Click a target in the map's list, press its Details button, or follow one - it will be shown here."));
        d.heading = ss("");
        sig += "none";
    } else {
        const cascade::core::HostTrack* found = nullptr;
        for (const cascade::core::HostTrack& tr : kit_.tracks()) {
            if (detailsId_ == tr.t.id && cascade::core::trackPresentation(tr.t).visible) { found = &tr; break; }
        }
        if (found == nullptr) {
            d.gone = true;
            d.heading = ss(detailsId_);
            sig += "gone" + detailsId_;
        } else {
            cascade::gui::TrackDetailInput in;
            in.label = found->t.label[0] != '\0' ? std::string(found->t.label) : std::string(found->t.id);
            in.id = found->t.id;
            in.source = found->plugin;
            in.kind = kindOf(found->t.kind);
            in.latDeg = found->t.latDeg;
            in.lonDeg = found->t.lonDeg;
            in.hasPosition = cascade::core::trackHasPosition(found->t);
            in.estimated = cascade::core::trackIsEstimated(found->t);
            in.altM = found->t.altM;
            in.speedMps = found->t.speedMps;
            in.courseDeg = found->t.courseDeg;
            in.ageMs = found->t.ageMs;
            in.hasHome = (state.flags & FOXAPI_RX_POSITION_SET) != 0u;
            in.homeLatDeg = state.rxLatDeg;
            in.homeLonDeg = state.rxLonDeg;
            in.infoActive = kit_.trackInfo().active();
            if (in.infoActive) {
                const cascade::gui::TrackInfoCache::Info* info = kit_.trackInfo().get(in.id, found->t.kind);
                in.infoPending = info == nullptr;
                in.infoKnown = info != nullptr && info->known;
                if (in.infoKnown) { in.registration = info->registration; in.typeCode = info->typeCode; in.typeName = info->typeName; in.operatorName = info->operatorName; in.country = info->country; }
            }
            for (const cascade::gui::TrackDetailLine& l : cascade::gui::buildTrackDetailLines(in)) {
                if (l.heading) { d.heading = ss(l.text); continue; }
                DetailLine x{};
                x.text = ss(l.text);
                x.known = l.known;
                x.heading = l.heading;
                x.separator_after = l.separatorAfter;
                sig += l.text + (l.known ? "" : "?") + ";";
                lines.push_back(std::move(x));
            }
            if (std::string(d.heading).empty()) { d.heading = ss(in.label); }
            d.lamp = LampState::Green;
            d.can_go = in.hasPosition;
            MapPage* pg = findMapPage(kit_.mapPages(), found->plugin);
            d.following = pg != nullptr && pg->followedId == detailsId_;
            sig += (d.following ? "F" : "") + std::string(d.heading);
        }
    }
    d.lines = model(std::move(lines));
    return d;
}

void KitPublisher::fillDetails(PageTab& t, const FoxReceiverState& state, std::string& sig) { t.details = detailsModel(state, sig); }

// The Plugin store page (kind 9): the model the EXTEND publisher handed over, whole. Its signature is the hand-over's revision (the publisher sets a model only when it
// differs from the one before).
void KitPublisher::fillStore(PageTab& t, std::string& sig) {
    t.store = storeModel_;
    t.chip = ss(storeChip_.empty() ? std::string(cascade::i18n::tr("IDLE")) : storeChip_);
    sig += "S" + std::to_string(storeRevision_);
}

// The Patch presets page (kind 10): the model the PATCH publisher handed over, whole; its signature is the hand-over's revision.
void KitPublisher::fillPatchPresets(PageTab& t, std::string& sig) {
    t.patch_presets = patchPresetsModel_;
    sig += "P" + std::to_string(patchPresetsRevision_);
}

KitPublisher::PatchPresetsView KitPublisher::patchPresetsView() const {
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].id != "patch-presets") { continue; }
        return pages_[i].torn || currentPage() == static_cast<int>(i) ? PatchPresetsView::Shown : PatchPresetsView::Docked;
    }
    return PatchPresetsView::Closed;
}

void KitPublisher::setPatchPresetsModel(const ::PatchPresetsModel& model) {
    patchPresetsModel_ = model;
    ++patchPresetsRevision_;
}

// Opens the page TORN OFF at the window's own size (the ImGui window is a floating 440 x 460) and leaves the patch view showing; an open page is left as it is.
void KitPublisher::openPatchPresetsPage() {
    if (findPage("patch-presets") != nullptr) { return; }
    Page& p = openPage("patch-presets", 10, "", "", cascade::i18n::tr("PATCH PRESETS"));
    p.torn = true;
    p.tw = 440.f;
    p.th = 460.f;
}

void KitPublisher::closePatchPresetsPage() { closePage("patch-presets"); }

// The Transmit page (kind 11): the model the TRANSMIT publisher handed over, whole; its signature is the hand-over's revision. The tab's chip is the lamp word.
void KitPublisher::fillTransmit(PageTab& t, std::string& sig) {
    t.transmit = transmitModel_;
    t.chip = ss(transmitChip_);
    sig += "X" + std::to_string(transmitRevision_);
}

KitPublisher::TransmitView KitPublisher::transmitView() const {
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].id != "transmit") { continue; }
        return pages_[i].torn || currentPage() == static_cast<int>(i) ? TransmitView::Shown : TransmitView::Docked;
    }
    return TransmitView::Closed;
}

// LIVE: the page's controls are drawn. A torn page is drawn unless it is rolled up; a docked one only while it is the tab showing.
bool KitPublisher::transmitLive() const {
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].id != "transmit") { continue; }
        return pages_[i].torn ? !pages_[i].rolled : currentPage() == static_cast<int>(i);
    }
    return false;
}

void KitPublisher::setTransmitModel(const ::TransmitPageModel& model, const std::string& chip, ::ChipTone tone) {
    transmitModel_ = model;
    transmitChip_ = chip;
    transmitChipTone_ = tone;
    ++transmitRevision_;
}

// Opens the page TORN OFF at the window's own size (the ImGui window is a floating 560 x 640); an open page is left as it is.
void KitPublisher::openTransmitPage() {
    if (findPage("transmit") != nullptr) { return; }
    Page& p = openPage("transmit", 11, "", "", cascade::i18n::tr("TRANSMIT"));
    p.torn = true;
    p.tw = 560.f;
    p.th = 640.f;
}

void KitPublisher::closeTransmitPage() { closePage("transmit"); }

KitPublisher::StoreView KitPublisher::storeView() const {
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].id != "store") { continue; }
        return pages_[i].torn || currentPage() == static_cast<int>(i) ? StoreView::Shown : StoreView::Docked;
    }
    return StoreView::Closed;
}

void KitPublisher::setStoreModel(const ::StoreModel& model) {
    storeModel_ = model;
    ++storeRevision_;
}

void KitPublisher::setStoreRail(const std::string& word, ::ChipTone tone, bool lit) {
    storeChip_ = word;
    storeChipTone_ = tone;
    storeLit_ = lit;
}

void KitPublisher::openStorePage() {
    openPage("store", 9, "", "", cascade::i18n::tr("PLUGIN STORE"));
    showPage("store");
}

PageTab KitPublisher::pageModel(const Page& p, const FoxReceiverState& state, std::string& sig) {
    PageTab t{};
    t.id = ss(p.id);
    t.kind = p.kind;
    {   // WHICH PAGES HAVE A 3D VIEW (docs 6.64): the Survey Engine's panel (by the plugin's title) and the Radiosondes' map; a page of another plugin is untouched
        std::string lowered = p.title + " " + p.plugin;
        for (char& ch : lowered) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
        if (p.kind == 3 && lowered.find("survey") != std::string::npos) { t.view3d = 1; }
        else if (p.kind == 5 && lowered.find("radiosonde") != std::string::npos) { t.view3d = 2; }
    }
    t.title = ss(p.title);
    t.chip = ss("");
    t.chip_tone = ChipTone::Neutral;
    t.torn = p.torn;
    t.tx = p.tx; t.ty = p.ty; t.tw = p.tw; t.th = p.th;
    t.rolled = p.rolled;
    t.follow = p.follow;
    t.presets = model(std::vector<PresetKeyModel>{});
    t.fields = model(std::vector<SettingField>{});
    t.lines = model(std::vector<LogLine>{});
    t.bars = model(std::vector<DecoderBar>{});
    t.columns = model(std::vector<slint::SharedString>{});
    t.rows = model(std::vector<TableRow>{});
    t.widths = model(std::vector<float>{});
    t.inst.values = model(std::vector<float>(8, 0.f));
    t.inst.texts = model(std::vector<slint::SharedString>(8));
    t.map.targets = model(std::vector<MapTarget>{});
    t.map.legend = model(std::vector<LegendRow>{});
    t.map.list_columns = model(std::vector<slint::SharedString>{});
    t.map.list_rows = model(std::vector<TableRow>{});
    t.map.list_widths = model(std::vector<float>{});
    t.fitted.rows = model(std::vector<FittedRowModel>{});
    t.fitted.chips = model(std::vector<StateChipModel>{});
    t.module.commands = model(std::vector<slint::SharedString>{});
    t.module.reach = model(std::vector<ReachRowModel>{});
    t.details.lines = model(std::vector<DetailLine>{});
    t.store = blankStoreModel();
    t.patch_presets = blankPatchPresetsModel();
    t.transmit = blankTransmitModel();
    sig = p.id + (p.torn ? "T" : "") + fmt("%.0f", p.tx) + fmt("%.0f", p.ty) + fmt("%.0f", p.tw) + fmt("%.0f", p.th) + (p.rolled ? "r" : "") + (p.follow ? "f" : "");
    const PluginRow* r = rowOf(p);
    bool chipIdle = false;   // the chip says IDLE or WAIT (decided from the state: the words themselves are translated)
    switch (p.kind) {
        case 1: fillOutput(t, sig); break;
        case 2: { fillPrelude(p, t, sig); fillPicture(p, t, sig); const auto* img = kit_.imageOf(p.plugin); chipIdle = img == nullptr || img->width == 0u; t.chip = ss(imageChip(img, r != nullptr && r->asleep)); } break;
        case 3: { fillPrelude(p, t, sig); fillPanel(p, t, sig); const auto* panel = kit_.panelOf(p.plugin, p.title); chipIdle = panel == nullptr; t.chip = ss(panelChip(panel)); } break;
        case 4: { fillPrelude(p, t, sig); fillInstrument(p, t, sig); const auto* in = kit_.instrumentOf(p.plugin, p.title); chipIdle = in == nullptr || !in->have; t.chip = ss(instrumentChip(in, false)); } break;
        case 5: fillPrelude(p, t, sig); fillMap(p, t, state, sig); { MapPage* pg = findMapPage(kit_.mapPages(), p.plugin); t.chip = ss(targetsChipWord(pg != nullptr ? pg->visibleCount : 0)); } break;
        case 6: fillFitted(t, state, sig); break;
        case 7: fillModule(p, t, state, sig); break;
        case 8: fillDetails(t, state, sig); break;
        case 9: fillStore(t, sig); break;
        case 10: fillPatchPresets(t, sig); break;
        case 11: fillTransmit(t, sig); break;
        default: break;
    }
    t.chip_tone = p.kind == 11 ? transmitChipTone_ : p.kind == 9 ? storeChipTone_ : chipIdle || std::string(t.chip) == "IDLE" || std::string(t.chip) == "WAIT" || std::string(t.chip).empty() ? ChipTone::Neutral : ChipTone::Good;   // the engine's own chip words (teleprinter, beacon, meter, weather) stay English
    sig += std::string(t.chip);
    globe_->fill(p.plugin, p.kind, state, t.globe, sig);   // the 3D view a plugin page offers beside its own (docs 6.65)
    return t;
}

// ---------------------------------------------------------------------------------------------------------------------------------
void KitPublisher::publishPages(const FoxReceiverState& state) {
    std::string setSig;
    for (const Page& p : pages_) { setSig += p.id + ";"; }
    const bool rebuilt = setSig != pagesSig_;
    if (rebuilt) {
        pagesSig_ = setSig;
        std::vector<PageTab> tabs;
        for (const Page& p : pages_) { std::string sig; tabs.push_back(pageModel(p, state, sig)); sigs_.last["page:" + p.id] = sig; }
        pagesModel_ = std::make_shared<slint::VectorModel<PageTab>>(std::move(tabs));
        ui_.set_pages(pagesModel_);
        return;
    }
    const int current = currentPage();
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        const Page& p = pages_[i];
        // only a page that shows is built: the current one, and every torn one
        if (!p.torn && static_cast<int>(i) != current) { continue; }
        std::string sig;
        PageTab t = pageModel(p, state, sig);
        if (sigs_.changed("page:" + p.id, sig)) { pagesModel_->set_row_data(i, std::move(t)); }
    }
}

void KitPublisher::publishDecodeBank(const FoxReceiverState& state) {
    const KitLists& k = kit_.lists();
    const bool running = (state.flags & FOXAPI_RX_RUNNING) != 0u;
    std::vector<DecodeRow> rows;
    std::string sig;
    const auto add = [&](int kind, const std::string& id, const std::string& name, const std::string& chip, ChipTone tone, LampState dot, bool showDot, bool open, bool enabled, const std::string& tip,
                         const std::string& note = std::string(), ChipTone noteTone = ChipTone::Neutral, const std::string& key = std::string(), const std::string& key2 = std::string()) {
        DecodeRow r{};
        r.kind = kind; r.id = ss(id); r.name = ss(name); r.chip = ss(chip); r.tone = tone; r.dot = dot; r.show_dot = showDot; r.open = open; r.enabled = enabled; r.tooltip = ss(tip);
        r.note = ss(note); r.note_tone = noteTone; r.key_label = ss(key); r.key2_label = ss(key2);
        sig += std::to_string(kind) + id + name + chip + std::to_string(static_cast<int>(tone)) + std::to_string(static_cast<int>(dot)) + (open ? "o" : "c") + (enabled ? "e" : "d") + note + key + key2 + ";";
        rows.push_back(std::move(r));
    };
    std::size_t fed = 0, decoders = 0, refused = 0;
    for (const PluginRow& r : k.rows) { if (!r.loaded) { ++refused; continue; } if (r.decoder) { ++decoders; if (r.fed && running) { ++fed; } } }
    // THE PLUGIN STORE ROW (drawPluginStoreSection): a key that opens the page of kind 9, its chip the EXTEND publisher's (BUSY / IDLE / "N UPD" / OK), its lamp green
    // while the page is up and amber while a transfer moves or updates wait. The tooltip is the ImGui row's own.
    {
        const bool open = findPage("store") != nullptr;
        add(0, "store", cascade::i18n::tr("Plugin store"), storeChip_.empty() ? std::string(cascade::i18n::tr("IDLE")) : storeChip_, storeChipTone_,
            open ? LampState::Green : storeLit_ ? LampState::Yellow : LampState::Off, true, open, true,
            cascade::i18n::tr("Opens the plugin store: the catalogue, what each module reaches for,\nwhat it costs to fit, and the updates the catalogue offers.\nThe first time you open it in a session it reads the catalogue;\nnothing is fetched at startup."));
    }
    add(1, "url", "", "", ChipTone::Neutral, LampState::Off, false, false, true, "");
    if (!k.enforceError.empty()) { add(10, "enforce", "", "", ChipTone::Neutral, LampState::Off, false, false, true, "", k.enforceError, ChipTone::Err); }
    for (const BlockedRow& b : k.blocked) {
        const bool armed = armedRemove_ == "blocked:" + b.file && nowMs_ - armedAtMs_ < 5000;
        add(2, b.file, b.file, "", ChipTone::Err, LampState::Red, true, false, true, "", b.message, ChipTone::Err, b.id.empty() ? "" : cascade::i18n::tr("Update"), armed ? cascade::i18n::tr("Confirm delete") : cascade::i18n::tr("Remove"));
    }
    add(3, "fitted", refused > 0 ? cascade::core::formatText(cascade::i18n::tr("Plugins (%zu refused)"), refused) : std::string(cascade::i18n::tr("Plugins")), fittedChipWord(fed, decoders), fed > 0 ? ChipTone::Good : ChipTone::Neutral, fed > 0 ? LampState::Green : LampState::Off, true, findPage("fitted") != nullptr, true,
        cascade::i18n::tr("Opens the fitted modules page: what is installed, which modules are being fed, which were refused and why, and the keys that start, stop and remove them. Start tunes the radio to the decoder's first preset unless it is already there."));
    if (refused > 0) { add(10, "refusednote", "", "", ChipTone::Neutral, LampState::Off, false, false, true, "", cascade::core::formatText(refused == 1 ? cascade::i18n::tr("%zu module was found and refused. The page says why.") : cascade::i18n::tr("%zu modules were found and refused. The page says why."), refused), ChipTone::Err); }
    add(4, "plugins", cascade::i18n::tr("Turn on and off plugins"), decodersChipWord(fed), fed > 0 ? ChipTone::Good : ChipTone::Neutral, fed > 0 ? LampState::Green : LampState::Off, true, decodeOpen_["plugins"], true, "");
    add(5, "details", cascade::i18n::tr("Target details"), detailsId_.empty() ? std::string(cascade::i18n::tr("NONE")) : detailsId_.substr(0, 10), ChipTone::Neutral, LampState::Off, true, decodeOpen_["details"], true, "");
    bool anyMap = false;
    for (const MapPage& pg : kit_.mapPages()) {
        anyMap = true;
        add(6, pg.plugin, mapTitle(pg.plugin), targetsChipWord(pg.visibleCount), pg.visibleCount > 0 ? ChipTone::Good : ChipTone::Neutral, pg.open ? LampState::Green : LampState::Off, true, pg.open, true,
            pg.satellite ? cascade::i18n::tr("Opens the satellites window: receiver position, overlays, trail style, coverage, the target register and the map. Opening it also tunes and starts the plugin, just like its preset.")
                         : cascade::i18n::tr("Opens this plugin's map: its targets and their trails, the receiver position and the coverage overlay. Nothing opens it for you - opening it also tunes and starts the plugin, just like its preset."));
    }
    if (!anyMap) {
        add(6, "", cascade::i18n::tr("Target maps"), cascade::i18n::tr("NONE"), ChipTone::Neutral, LampState::Off, true, false, false, "");
        bool anyTracker = false;
        for (const PluginRow& r : k.rows) { if (r.loaded && (r.caps & CASCADE_CAP_TRACK_SOURCE) != 0u) { anyTracker = true; } }
        add(10, "mapnote", "", "", ChipTone::Neutral, LampState::Off, false, false, true, "", anyTracker ? cascade::i18n::tr("A tracker is fitted and stopped or asleep: start it, or open its map from a preset.") : cascade::i18n::tr("A tracker plugin reports the positions a map draws - install one from the plugin store."), ChipTone::Neutral);
    }
    const char* kRowTip = cascade::i18n::tr("Opens this plugin's window. Nothing opens it for you; close it from its key and it stays closed - opening it also tunes and starts the decoder, just like its preset.");
    for (const PluginRow& r : k.rows) {
        if (!r.loaded || r.stopped) { continue; }
        if ((r.caps & CASCADE_CAP_IMAGE_DECODER) != 0u) {
            const bool open = findPage("image:" + r.name) != nullptr;
            const auto* img = kit_.imageOf(r.name);
            const std::string chip = imageChip(img, r.asleep);
            add(7, r.name, imageTitle(r.name), chip, img != nullptr && img->width != 0u ? ChipTone::Good : ChipTone::Neutral, open ? LampState::Green : LampState::Off, true, open, true, kRowTip);
        }
        if ((r.caps & CASCADE_CAP_PANEL) != 0u) {
            const cascade::core::HostPanel* panel = kit_.panelOf(r.name, std::string());
            const std::string title = !r.panelTitle.empty() ? r.panelTitle : panel != nullptr ? panel->title : r.name;
            const bool open = findPage("panel:" + r.name) != nullptr;
            add(8, r.name, title, panelChip(panel), panel != nullptr && !panel->rows.empty() ? ChipTone::Good : ChipTone::Neutral, open ? LampState::Green : LampState::Off, true, open, true, kRowTip);
        }
        if ((r.caps & CASCADE_CAP_INSTRUMENT) != 0u) {
            const cascade::core::HostInstrument* in = kit_.instrumentOf(r.name, std::string());
            const std::string title = !r.instrumentTitle.empty() ? r.instrumentTitle : in != nullptr ? in->title : r.name;
            const std::string key = windowKey("instrument", r.name, title);
            const bool open = findPage("instrument:" + r.name + "|" + title) != nullptr;
            const bool unread = in != nullptr && in->have && kit_.instrumentSeen(key) != in->state.seq;
            const std::string chip = instrumentChip(in, unread);
            add(9, r.name + "|" + title, title, chip, unread ? ChipTone::Warn : in == nullptr || !in->have || chip == "WAIT" || chip == "IDLE" ? ChipTone::Neutral : ChipTone::Good, unread ? LampState::Yellow : open ? LampState::Green : LampState::Off, true, open, true, kRowTip);
        }
    }
    for (const cascade::core::HostInstrument& in : kit_.instruments()) {
        if (in.plugin != "Demonstration") { continue; }
        const bool open = findPage("instrument:" + in.plugin + "|" + in.title) != nullptr;
        add(9, in.plugin + "|" + in.title, cascade::core::formatText(cascade::i18n::tr("%s (DEMO)"), in.title.c_str()), instrumentChip(&in, false), ChipTone::Warn, open ? LampState::Green : LampState::Off, true, open, true, kRowTip);
    }
    if (sigs_.changed("decode-rows", sig)) { ui_.set_decode_rows(model(std::move(rows))); }

    // the "Turn on and off plugins" drawer's entries and notes
    std::vector<PluginEntry> entries;
    std::string esig;
    std::vector<const PluginRow*> railRows;
    for (const PluginRow& r : k.rows) { if (r.loaded && (r.ownPresets > 0 || r.decoder)) { railRows.push_back(&r); } }
    bool grouped = false;
    for (const PluginRow* r : railRows) { if (!r->category.empty()) { grouped = true; } }
    if (grouped) { std::stable_sort(railRows.begin(), railRows.end(), [](const PluginRow* a, const PluginRow* b) { return a->category < b->category; }); }
    std::string lastGroup;
    for (const PluginRow* rp : railRows) {
        const PluginRow& r = *rp;
        PluginEntry e{};
        e.file = ss(r.file);
        e.name = ss(r.name);
        e.group = ss(grouped ? (r.category.empty() ? std::string(cascade::i18n::tr("OTHER")) : r.category) : "");
        e.show_group = grouped && std::string(e.group) != lastGroup;
        lastGroup = e.group;
        e.presets = presetModel(r, state, true, esig);
        e.save_label = ss(presetSaveLabel(state.tunedHz));
        e.save_shown = r.decoder;
        e.note = ss(presetNote_);
        e.decoder = r.decoder;
        e.offers_stop = !r.stopped && !r.asleep;
        e.idle = r.asleep && !r.stopped;
        e.mutes = r.mutes;
        {   // THE RUNS-ON PICKER: one key per open radio, shown only when more than one is open
            const RunsOnModel ro = runsOnFor(r, radioMask_);
            std::vector<::RunsOnKey> keys;
            for (const RunsOnKey& k2 : ro.keys) {
                ::RunsOnKey key{};
                key.label = ss("R" + std::to_string(k2.radio));
                key.accessible = ss(cascade::core::formatText(cascade::i18n::tr("Radio %d: %s"), k2.radio, r.name.c_str()));
                key.on = k2.on;
                keys.push_back(key);
            }
            e.runs_on = model(std::move(keys));
            e.runs_on_label = ss(ro.shown() ? cascade::i18n::tr("Runs on") : "");
            e.runs_on_tip = ss(ro.shown() ? cascade::i18n::tr("The radio this plugin listens to. Choose another to move it there.") : "");
            esig += "r" + std::to_string(ro.shown() ? ro.current : 0) + "m" + std::to_string(radioMask_) + ";";
        }
        const SettingsSet* set = settingsFor(k.settings, r.file);
        e.has_settings = set != nullptr && !set->fields.empty();
        e.settings_open = entrySettingsOpen_[r.file];
        e.fields = e.has_settings ? fieldsModel(r.file, esig) : model(std::vector<SettingField>{});
        esig += r.file + std::string(e.group) + (e.show_group ? "G" : "") + (e.offers_stop ? "S" : "") + (e.idle ? "I" : "") + (e.mutes ? "M" : "") + (e.settings_open ? "o" : "") + ";";
        entries.push_back(std::move(e));
    }
    esig += presetNote_ + fmt("%.0f", state.tunedHz);
    if (sigs_.changed("entries", esig)) { ui_.set_plugin_entries(model(std::move(entries))); }
    // the notes and keys of the drawer
    std::size_t runningNow = 0;
    std::vector<std::string> runningKeys;
    bool anyTextIdle = false, anyRunning = false;
    for (const PluginRow& r : k.rows) {
        if (!r.loaded || !r.decoder) { continue; }
        if (r.fed && running && !r.stopped) { ++runningNow; runningKeys.push_back(r.file); anyRunning = true; }
        if ((r.caps & CASCADE_CAP_DECODER) != 0u && r.asleep) { anyTextIdle = true; }
    }
    std::string decodeNote;
    if (decoders == 0) { decodeNote = cascade::i18n::tr("No fitted module carries a decoder of any kind, so there is nothing here to decode with. The Plugin store row above is where one is fitted from."); }
    else if (!running) { decodeNote = cascade::i18n::tr("Nothing is decoding: the receiver is stopped."); }
    else if (!anyRunning) { decodeNote = cascade::i18n::tr("No decoder is running."); }
    ui_.set_decode_note(ss(decodeNote));
    const bool outputOffered = !kit_.decoderLines().empty() || anyRunning || anyTextIdle;
    ui_.set_output_key_label(ss(!outputOffered ? "" : findPage("output") != nullptr ? cascade::i18n::tr("Hide decoder output") : cascade::i18n::tr("Show decoder output")));
    ui_.set_lines_text(ss(state.decoderLinesTotal > 0 ? cascade::core::formatText(state.decoderLinesTotal == 1 ? cascade::i18n::tr("%llu line decoded") : cascade::i18n::tr("%llu lines decoded"), static_cast<unsigned long long>(state.decoderLinesTotal)) : std::string()));
    ui_.set_tail_note(ss(kit_.decoderLines().size() >= 500 ? cascade::core::formatText(cascade::i18n::tr("The window holds the newest %d lines."), 500) : std::string()));
    ui_.set_cost_note(ss(runningNow >= 2 ? cascade::core::formatText(cascade::i18n::tr("%d decoders are running at once. They share one machine, and several at a time is what makes the audio stutter."), static_cast<int>(runningNow)) : std::string()));
    ui_.set_stop_all_label(ss(runningNow >= 2 ? cascade::core::formatText(cascade::i18n::tr("STOP ALL %d RUNNING"), static_cast<int>(runningNow)) : std::string()));
    ui_.set_empty_entries_text(ss(railRows.empty() ? cascade::i18n::tr("No fitted module publishes a preset or is fed a signal.") : ""));
    // the Receiver control card
    std::string refusal;
    if (!k.lastDenied.empty()) {
        const PluginRow* d = rowByFile(k.rows, k.lastDenied);
        refusal = cascade::core::formatText(cascade::i18n::tr("\"%s\" asked to tune the receiver and was refused."), (d != nullptr ? d->name : k.lastDenied).c_str());
    }
    ui_.set_refusal(ss(refusal));
    std::vector<::StaleGrant> stale;
    std::string ssig;
    for (const StaleGrant& g : k.staleGrants) { ::StaleGrant x; x.file = ss(g.file); x.refused = g.refused; x.granted = true; ssig += g.file + (g.refused ? "r" : ""); stale.push_back(std::move(x)); }
    if (sigs_.changed("stale", ssig)) { ui_.set_stale(model(std::move(stale))); }
    // the Target details drawer
    std::string dsig;
    DetailsModel d = detailsModel(state, dsig);
    if (sigs_.changed("details", dsig)) { ui_.set_details(d); }
    if (catalogueUrl_ != k.catalogueUrl) { catalogueUrl_ = k.catalogueUrl; ui_.set_catalogue_url(ss(catalogueUrl_)); }
}

void KitPublisher::publishMuteQuestion() {
    const KitLists& k = kit_.lists();
    if (muteQuestionOpen_ && (!k.muteKeptRunning || k.muteCauses.empty())) { muteQuestionOpen_ = false; }
    if (muteQuestionOpen_) {
        std::string who;
        for (std::size_t i = 0; i < muteNames_.size(); ++i) {
            if (i == 0) { who = muteNames_[i]; }
            else if (i + 1 == muteNames_.size()) { who = cascade::core::formatText(cascade::i18n::tr("%s and %s"), who.c_str(), muteNames_[i].c_str()); }
            else { who += ", " + muteNames_[i]; }
        }
        ui_.set_mute_question_text(ss(cascade::core::formatText(cascade::i18n::tr("%s is still running and is muting the audio. Stop it so sound resumes?"), who.c_str())));
        ui_.set_mute_stop_label(ss(cascade::core::formatText(cascade::i18n::tr("Stop %s and resume sound"), who.c_str())));
    }
    ui_.set_mute_question_open(muteQuestionOpen_);
}

void KitPublisher::publish(const FoxReceiverState& state) {
    state_ = state;
    nowMs_ = clockMs();
    const KitEvents ev = kit_.takeEvents();
    for (const std::string& file : ev.presetApplied) {
        const PluginRow* r = rowByFile(kit_.lists().rows, file);
        if (r != nullptr) { openWindowsFor(*r, true); }
    }
    if (ev.muteTuneAway && !kit_.lists().muteCauses.empty()) {
        muteNames_.clear(); muteKeys_.clear();
        for (const MuteCause& c : kit_.lists().muteCauses) { muteNames_.push_back(c.name); muteKeys_.push_back(c.key); }
        muteQuestionOpen_ = true;
    }
    for (const std::string& line : ev.notices) { std::fprintf(stderr, "foxsdr-slint: %s\n", line.c_str()); }
    if (!armedRemove_.empty() && nowMs_ - armedAtMs_ >= 5000) { armedRemove_.clear(); }
    if (!envApplied_ && !kit_.lists().rows.empty()) {
        envApplied_ = true;
        std::string rest = envSpec_, last;
        while (!rest.empty()) {
            const std::size_t comma = rest.find(',');
            const std::string id = rest.substr(0, comma);
            rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
            if (id.empty()) { continue; }
            const std::size_t colon = id.find(':');
            const std::string kind = id.substr(0, colon), arg = colon == std::string::npos ? std::string() : id.substr(colon + 1);
            if (kind == "output") { openPage("output", 1, "", "", cascade::i18n::tr("Decoder output")); }
            else if (kind == "fitted") { openPage("fitted", 6, "", "", cascade::i18n::tr("Fitted modules")); }
            else if (kind == "details") { openPage("details", 8, "", "", cascade::i18n::tr("Target details")); }
            else if (kind == "store") { openPage("store", 9, "", "", cascade::i18n::tr("PLUGIN STORE")); }
            else if (kind == "patch-presets") { openPatchPresetsPage(); }
            else if (kind == "transmit") { openTransmitPage(); }
            else if (kind == "module") { const PluginRow* r = rowByFile(kit_.lists().rows, arg); if (r != nullptr) { openPage(id, 7, r->name, r->file, r->name); } }
            else {
                const std::size_t bar = arg.find('|');
                const std::string plugin = arg.substr(0, bar), title = bar == std::string::npos ? std::string() : arg.substr(bar + 1);
                const PluginRow* r = rowByName(kit_.lists().rows, plugin);
                if (r == nullptr) { std::fprintf(stderr, "foxsdr-slint: FOXSDR_SLINT_OPEN: no loaded plugin named \"%s\"\n", plugin.c_str()); continue; }
                if (kind == "image") { openPage(id, 2, plugin, r->file, imageTitle(plugin)); }
                else if (kind == "panel") { openPage(id, 3, plugin, r->file, !r->panelTitle.empty() ? r->panelTitle : title.empty() ? plugin : title); }
                else if (kind == "instrument") {
                    if ((r->caps & CASCADE_CAP_INSTRUMENT) == 0u) { std::fprintf(stderr, "foxsdr-slint: FOXSDR_SLINT_OPEN: \"%s\" declares no instrument\n", plugin.c_str()); continue; }
                    const std::string t = !title.empty() ? title : !r->instrumentTitle.empty() ? r->instrumentTitle : plugin;
                    openPage("instrument:" + plugin + "|" + t, 4, plugin, r->file, t);
                    last = "instrument:" + plugin + "|" + t;
                    continue;
                }
                else if (kind == "map") { openPage(id, 5, plugin, r->file, mapTitle(plugin)); }
            }
            last = id;
        }
        if (!last.empty()) { showPage(last); }
    }
    publishDecodeBank(state);
    globe_->tick(state);
    publishPages(state);
    publishMuteQuestion();
}

// ---------------------------------------------------------------------------------------------------------------------------------
void KitPublisher::wire() {
    globe_->wire(ui_, [this](int i) { return i >= 0 && i < static_cast<int>(pages_.size()) ? pages_[static_cast<std::size_t>(i)].plugin : std::string(); });
    // ---- the DECODE bank ----
    ui_.global<::KitEvents>().on_decode_row_clicked([this](int i) {
        auto rows = ui_.get_decode_rows();
        if (!rows || i < 0 || i >= static_cast<int>(rows->row_count())) { return; }
        const DecodeRow r = *rows->row_data(static_cast<std::size_t>(i));
        const std::string id(r.id);
        const PluginRow* row = nullptr;
        switch (r.kind) {
            case 0: if (findPage("store") != nullptr) { closePage("store"); } else { openStorePage(); } break;
            case 3: if (findPage("fitted") != nullptr) { closePage("fitted"); } else { openPage("fitted", 6, "", "", cascade::i18n::tr("Fitted modules")); showPage("fitted"); } break;
            case 4: decodeOpen_["plugins"] = !decodeOpen_["plugins"]; break;
            case 5: decodeOpen_["details"] = !decodeOpen_["details"]; break;
            case 6: {
                if (!r.enabled) { return; }
                row = rowByName(kit_.lists().rows, id);
                const std::string pid = "map:" + id;
                if (findPage(pid) != nullptr) { closePage(pid); }
                else { openPage(pid, 5, id, row != nullptr ? row->file : std::string(), mapTitle(id)); showPage(pid); if (row != nullptr) { kit_.autoPreset(row->file); } }
                break;
            }
            case 7: case 8: case 9: {
                std::string plugin = id, title;
                if (r.kind == 9) { const std::size_t bar = id.find('|'); plugin = id.substr(0, bar); title = bar == std::string::npos ? std::string() : id.substr(bar + 1); }
                row = rowByName(kit_.lists().rows, plugin);
                const std::string pid = r.kind == 7 ? "image:" + plugin : r.kind == 8 ? "panel:" + plugin : "instrument:" + plugin + "|" + title;
                if (findPage(pid) != nullptr) { closePage(pid); break; }
                const std::string pageTitle = r.kind == 7 ? imageTitle(plugin) : std::string(r.name);
                openPage(pid, r.kind == 7 ? 2 : r.kind == 8 ? 3 : 4, plugin, row != nullptr ? row->file : std::string(), pageTitle);
                showPage(pid);
                if (row != nullptr) { kit_.autoPreset(row->file); }
                break;
            }
            default: break;
        }
    });
    ui_.global<::KitEvents>().on_decode_row_key([this](int i) {
        auto rows = ui_.get_decode_rows();
        if (!rows || i < 0 || i >= static_cast<int>(rows->row_count())) { return; }
        const DecodeRow r = *rows->row_data(static_cast<std::size_t>(i));
        if (r.kind != 2) { return; }
        for (const BlockedRow& b : kit_.lists().blocked) { if (b.file == std::string(r.id) && !b.id.empty()) { kit_.updateBlocked(b.id); } }
    });
    ui_.global<::KitEvents>().on_decode_row_key2([this](int i) {
        auto rows = ui_.get_decode_rows();
        if (!rows || i < 0 || i >= static_cast<int>(rows->row_count())) { return; }
        const DecodeRow r = *rows->row_data(static_cast<std::size_t>(i));
        if (r.kind != 2) { return; }
        const std::string key = "blocked:" + std::string(r.id);
        if (armedRemove_ == key && nowMs_ - armedAtMs_ < 5000) { kit_.removeBlocked(std::string(r.id)); armedRemove_.clear(); }
        else { armedRemove_ = key; armedAtMs_ = nowMs_; }
    });
    ui_.global<::KitEvents>().on_decode_url_committed([this](slint::SharedString url) { kit_.setCatalogueUrl(std::string(url)); });
    ui_.global<::KitEvents>().on_decode_output_clicked([this] { if (findPage("output") != nullptr) { closePage("output"); } else { openPage("output", 1, "", "", cascade::i18n::tr("Decoder output")); showPage("output"); } });
    ui_.global<::KitEvents>().on_decode_stop_all([this] {
        std::vector<std::string> keys;
        const bool running = (state_.flags & FOXAPI_RX_RUNNING) != 0u;
        for (const PluginRow& r : kit_.lists().rows) { if (r.loaded && r.decoder && r.fed && running && !r.stopped) { keys.push_back(r.file); } }
        kit_.stopList(keys, false);
    });
    const auto entryRow = [this](int e) -> const PluginRow* {
        auto entries = ui_.get_plugin_entries();
        if (!entries || e < 0 || e >= static_cast<int>(entries->row_count())) { return nullptr; }
        return rowByFile(kit_.lists().rows, std::string(entries->row_data(static_cast<std::size_t>(e))->file));
    };
    ui_.global<::KitEvents>().on_decode_entry_preset([this, entryRow](int e, int i) {
        const PluginRow* r = entryRow(e);
        if (r == nullptr || i < 0 || i >= static_cast<int>(r->presets.size())) { return; }
        kit_.applyPreset(r->file, r->presets[static_cast<std::size_t>(i)]);
    });
    ui_.global<::KitEvents>().on_decode_entry_forgot([this, entryRow](int e, int i) {
        const PluginRow* r = entryRow(e);
        if (r == nullptr || i < 0 || i >= static_cast<int>(r->presets.size())) { return; }
        kit_.forgetPreset(r->file, r->presets[static_cast<std::size_t>(i)]);
        presetNote_ = cascade::core::formatText(cascade::i18n::tr("Forgot a preset for %s"), r->name.c_str());
    });
    ui_.global<::KitEvents>().on_decode_entry_save([this, entryRow](int e) {
        const PluginRow* r = entryRow(e);
        if (r == nullptr) { return; }
        kit_.savePreset(r->file);
        presetNote_ = cascade::core::formatText(cascade::i18n::tr("Saved %.4f MHz as a preset for %s"), state_.tunedHz / 1e6, r->name.c_str());
    });
    ui_.global<::KitEvents>().on_decode_entry_run([this, entryRow](int e) { const PluginRow* r = entryRow(e); if (r != nullptr) { kit_.startStop(r->file, !r->stopped && !r->asleep); } });
    ui_.global<::KitEvents>().on_decode_entry_runs_on([this, entryRow](int e, int radio) {
        const PluginRow* r = entryRow(e);
        if (r != nullptr && radio >= 1 && radio <= 8) { kit_.setPluginRadio(r->file, radio, !r->stopped); }   // a running plugin goes on running on its new radio; a stopped one stays stopped
    });
    ui_.global<::KitEvents>().on_decode_entry_mute([this, entryRow](int e, bool on) { const PluginRow* r = entryRow(e); if (r != nullptr) { kit_.setMute(r->file, on); } });
    ui_.global<::KitEvents>().on_decode_entry_settings_toggled([this, entryRow](int e) { const PluginRow* r = entryRow(e); if (r != nullptr) { entrySettingsOpen_[r->file] = !entrySettingsOpen_[r->file]; } });
    const auto settingOf = [this](const std::string& file, int id) -> const SettingSpec* {
        const SettingsSet* set = settingsFor(kit_.lists().settings, file);
        if (set == nullptr) { return nullptr; }
        for (const SettingSpec& s : set->fields) { if (static_cast<int>(s.id) == id) { return &s; } }
        return nullptr;
    };
    ui_.global<::KitEvents>().on_decode_entry_setting_committed([this, entryRow, settingOf](int e, int id, slint::SharedString text) {
        const PluginRow* r = entryRow(e); const SettingSpec* s = r != nullptr ? settingOf(r->file, id) : nullptr;
        if (s != nullptr) { kit_.editSetting(r->file, *s, std::string(text)); }
    });
    ui_.global<::KitEvents>().on_decode_entry_setting_toggled([this, entryRow, settingOf](int e, int id, bool on) {
        const PluginRow* r = entryRow(e); const SettingSpec* s = r != nullptr ? settingOf(r->file, id) : nullptr;
        if (s != nullptr) { kit_.editSetting(r->file, *s, on ? "1" : ""); }
    });
    ui_.global<::KitEvents>().on_decode_entry_browse([this](int, int) { std::fprintf(stderr, "foxsdr-slint: the file dialog for a PATH field is a later step; type the path\n"); });
    ui_.global<::KitEvents>().on_decode_grant_toggled([this](int i, bool on) {
        const auto& stale = kit_.lists().staleGrants;
        if (i < 0 || i >= static_cast<int>(stale.size())) { return; }
        kit_.setGrant(stale[static_cast<std::size_t>(i)].file, 1, on);
    });
    const auto detailsGo = [this] {
        for (const cascade::core::HostTrack& tr : kit_.tracks()) {
            if (detailsId_ != tr.t.id || !cascade::core::trackHasPosition(tr.t)) { continue; }
            const PluginRow* r = rowByName(kit_.lists().rows, tr.plugin);
            openPage("map:" + tr.plugin, 5, tr.plugin, r != nullptr ? r->file : std::string(), mapTitle(tr.plugin));
            MapPage* pg = findMapPage(kit_.mapPages(), tr.plugin);
            if (pg != nullptr) { pg->selectedId = detailsId_; pg->centreLat = tr.t.latDeg; pg->centreLon = tr.t.lonDeg; pg->viewFitted = true; }
            showPage("map:" + tr.plugin);
            return;
        }
    };
    const auto detailsFollow = [this] {
        for (const cascade::core::HostTrack& tr : kit_.tracks()) {
            if (detailsId_ != tr.t.id) { continue; }
            const PluginRow* r = rowByName(kit_.lists().rows, tr.plugin);
            openPage("map:" + tr.plugin, 5, tr.plugin, r != nullptr ? r->file : std::string(), mapTitle(tr.plugin));
            MapPage* pg = findMapPage(kit_.mapPages(), tr.plugin);
            if (pg != nullptr) { pg->followedId = pg->followedId == detailsId_ ? std::string() : detailsId_; }
            return;
        }
    };
    ui_.global<::KitEvents>().on_decode_details_go(detailsGo);
    ui_.global<::KitEvents>().on_decode_details_follow(detailsFollow);
    ui_.global<::KitEvents>().on_decode_details_close([this] { detailsId_.clear(); });

    // ---- the tab row and the torn windows ----
    ui_.global<::KitEvents>().on_page_tab_selected([this](int i) { if (i >= 0 && i < static_cast<int>(pages_.size())) { ui_.set_view_index(3 + i); } });
    ui_.global<::KitEvents>().on_page_tab_closed([this](int i) { if (i >= 0 && i < static_cast<int>(pages_.size())) { closePage(pages_[static_cast<std::size_t>(i)].id); } });
    ui_.global<::KitEvents>().on_page_tab_torn([this](int i, float, float) {
        if (i < 0 || i >= static_cast<int>(pages_.size())) { return; }
        Page& p = pages_[static_cast<std::size_t>(i)];
        p.torn = true;
        if (currentPage() == i) { ui_.set_view_index(0); }
    });
    ui_.on_add_window_clicked([this] { openPage("output", 1, "", "", cascade::i18n::tr("Decoder output")); showPage("output"); });
    ui_.global<::KitEvents>().on_page_chrome_close([this](int i) { if (i >= 0 && i < static_cast<int>(pages_.size())) { closePage(pages_[static_cast<std::size_t>(i)].id); } });
    ui_.global<::KitEvents>().on_page_chrome_rollup([this](int i) { if (i >= 0 && i < static_cast<int>(pages_.size())) { pages_[static_cast<std::size_t>(i)].rolled = !pages_[static_cast<std::size_t>(i)].rolled; } });
    ui_.global<::KitEvents>().on_page_chrome_dock([this](int i) { if (i >= 0 && i < static_cast<int>(pages_.size())) { pages_[static_cast<std::size_t>(i)].torn = false; ui_.set_view_index(3 + i); } });
    ui_.global<::KitEvents>().on_page_chrome_drag([this](int i, float dx, float dy) {
        if (i < 0 || i >= static_cast<int>(pages_.size())) { return; }
        Page& p = pages_[static_cast<std::size_t>(i)];
        p.tx = std::max(0.f, p.tx + dx);
        p.ty = std::max(0.f, p.ty + dy);
    });
    ui_.global<::KitEvents>().on_page_chrome_resize([this](int i, float dx, float dy) {
        if (i < 0 || i >= static_cast<int>(pages_.size())) { return; }
        Page& p = pages_[static_cast<std::size_t>(i)];
        p.tw = std::max(240.f, p.tw + dx);
        p.th = std::max(140.f, p.th + dy);
    });

    // ---- the pages' bodies ----
    const auto page = [this](int i) -> Page* { return i >= 0 && i < static_cast<int>(pages_.size()) ? &pages_[static_cast<std::size_t>(i)] : nullptr; };
    ui_.global<::KitEvents>().on_page_preset_pressed([this, page](int i, int k) {
        Page* p = page(i); const PluginRow* r = p != nullptr ? rowOf(*p) : nullptr;
        if (r != nullptr && k >= 0 && k < static_cast<int>(r->presets.size())) { kit_.applyPreset(r->file, r->presets[static_cast<std::size_t>(k)]); }
    });
    ui_.global<::KitEvents>().on_page_preset_forgot([this, page](int i, int k) {
        Page* p = page(i); const PluginRow* r = p != nullptr ? rowOf(*p) : nullptr;
        if (r != nullptr && k >= 0 && k < static_cast<int>(r->presets.size())) { kit_.forgetPreset(r->file, r->presets[static_cast<std::size_t>(k)]); }
    });
    ui_.global<::KitEvents>().on_page_save_clicked([this, page](int i) { Page* p = page(i); const PluginRow* r = p != nullptr ? rowOf(*p) : nullptr; if (r != nullptr) { kit_.savePreset(r->file); } });
    ui_.global<::KitEvents>().on_page_settings_toggled([page](int i) { if (Page* p = page(i)) { p->settingsOpen = !p->settingsOpen; } });
    ui_.global<::KitEvents>().on_page_setting_committed([this, page, settingOf](int i, int id, slint::SharedString text) {
        Page* p = page(i); const PluginRow* r = p != nullptr ? rowOf(*p) : nullptr; const SettingSpec* s = r != nullptr ? settingOf(r->file, id) : nullptr;
        if (s != nullptr) { kit_.editSetting(r->file, *s, std::string(text)); }
    });
    ui_.global<::KitEvents>().on_page_setting_toggled([this, page, settingOf](int i, int id, bool on) {
        Page* p = page(i); const PluginRow* r = p != nullptr ? rowOf(*p) : nullptr; const SettingSpec* s = r != nullptr ? settingOf(r->file, id) : nullptr;
        if (s != nullptr) { kit_.editSetting(r->file, *s, on ? "1" : ""); }
    });
    ui_.global<::KitEvents>().on_page_browse_clicked([](int, int) { std::fprintf(stderr, "foxsdr-slint: the file dialog for a PATH field is a later step; type the path\n"); });
    ui_.global<::KitEvents>().on_page_bar_preset_pressed([this](int, int bar, int k) {
        int b = 0;
        for (const PluginRow& r : kit_.lists().rows) {
            if (!r.loaded || r.stopped || r.presets.empty() || (r.caps & CASCADE_CAP_DECODER) == 0u) { continue; }
            if (b++ != bar) { continue; }
            if (k >= 0 && k < static_cast<int>(r.presets.size())) { kit_.applyPreset(r.file, r.presets[static_cast<std::size_t>(k)]); }
            return;
        }
    });
    ui_.global<::KitEvents>().on_page_bar_save_clicked([this](int, int bar) {
        int b = 0;
        for (const PluginRow& r : kit_.lists().rows) {
            if (!r.loaded || r.stopped || r.presets.empty() || (r.caps & CASCADE_CAP_DECODER) == 0u) { continue; }
            if (b++ == bar) { kit_.savePreset(r.file); return; }
        }
    });
    ui_.global<::KitEvents>().on_page_log_clear([this](int) { kit_.clearDecoderLines(); });
    ui_.global<::KitEvents>().on_page_log_follow([page](int i, bool on) { if (Page* p = page(i)) { p->follow = on; } });
    ui_.global<::KitEvents>().on_page_picture_pressed([this, page](int i, int x, int y) { if (Page* p = page(i)) { kit_.imagePress(p->plugin, x, y); } });
    ui_.global<::KitEvents>().on_page_picture_save([this, page](int i) { if (Page* p = page(i)) { p->saveNote = kit_.savePicture(p->plugin); } });
    ui_.global<::KitEvents>().on_page_row_pressed([this, page](int i, int row) {
        Page* p = page(i); const PluginRow* r = p != nullptr ? rowOf(*p) : nullptr;
        if (p == nullptr || r == nullptr) { return; }
        const std::vector<CascadePanelRow>* rows = nullptr;
        if (p->kind == 3) { const auto* panel = kit_.panelOf(p->plugin, p->title); if (panel != nullptr) { rows = &panel->rows; } }
        else if (p->kind == 4) { const auto* in = kit_.instrumentOf(p->plugin, p->title); if (in != nullptr) { rows = &in->rows; } }
        if (rows == nullptr || row < 0 || row >= static_cast<int>(rows->size())) { return; }
        const std::uint32_t id = cascade::core::panelRowPressable((*rows)[static_cast<std::size_t>(row)], r->commands);
        if (id != 0u) { kit_.pressCommand(r->file, id); }
    });
    ui_.global<::KitEvents>().on_page_row_clicked([](int, int) {});
    ui_.global<::KitEvents>().on_page_column_resized([page](int i, int c, float w) {
        if (Page* p = page(i)) { if (c >= 0 && c < 8) { if (p->widths.size() <= static_cast<std::size_t>(c)) { p->widths.resize(static_cast<std::size_t>(c) + 1, 0.f); } p->widths[static_cast<std::size_t>(c)] = w; } }
    });
    ui_.global<::KitEvents>().on_page_obs_changed([page](int i, int v) { if (Page* p = page(i)) { p->obsCourse = ((v % 360) + 360) % 360; } });
    // the map
    const auto surface = [this, page](int i) -> MapSurface* { Page* p = page(i); return p != nullptr && p->kind == 5 ? &surfaceFor(p->plugin) : nullptr; };
    const auto mapPage = [this, page](int i) -> MapPage* { Page* p = page(i); return p != nullptr && p->kind == 5 ? findMapPage(kit_.mapPages(), p->plugin) : nullptr; };
    ui_.global<::KitEvents>().on_page_map_target_clicked([this, surface, mapPage](int i, int t) {
        MapSurface* s = surface(i); MapPage* pg = mapPage(i);
        if (s == nullptr || pg == nullptr || t < 0 || t >= static_cast<int>(s->targetIds.size())) { return; }
        pg->selectedId = s->targetIds[static_cast<std::size_t>(t)];
        detailsId_ = pg->selectedId;
    });
    ui_.global<::KitEvents>().on_page_map_target_double_clicked([surface, mapPage](int i, int t) {
        MapSurface* s = surface(i); MapPage* pg = mapPage(i);
        if (s == nullptr || pg == nullptr || t < 0 || t >= static_cast<int>(s->targetIds.size())) { return; }
        const std::string& id = s->targetIds[static_cast<std::size_t>(t)];
        pg->followedId = pg->followedId == id ? std::string() : id;
    });
    ui_.global<::KitEvents>().on_page_map_hovered([this, surface, page](int i, int t) {
        MapSurface* s = surface(i); Page* p = page(i);
        if (s == nullptr || p == nullptr) { return; }
        s->hoverText.clear();
        if (t < 0 || t >= static_cast<int>(s->targetIds.size())) { return; }
        for (const cascade::core::HostTrack& tr : kit_.tracks()) {
            if (tr.plugin != p->plugin || s->targetIds[static_cast<std::size_t>(t)] != tr.t.id) { continue; }
            s->hoverText = std::string(tr.t.label[0] != '\0' ? tr.t.label : tr.t.id) + (std::isfinite(tr.t.altM) ? fmt(" \xC2\xB7 %.0f ft", tr.t.altM * 3.28084) : "") + (std::isfinite(tr.t.speedMps) ? fmt(" \xC2\xB7 %.0f kt", tr.t.speedMps * 1.94384) : "");
            break;
        }
    });
    ui_.global<::KitEvents>().on_page_map_panned([surface, mapPage](int i, float dx, float dy) {
        MapSurface* s = surface(i); MapPage* pg = mapPage(i);
        if (s == nullptr || pg == nullptr) { return; }
        if (!pg->followedId.empty()) { return; }   // a drag while following is refused (the ImGui window asks; this window keeps following until Stop following)
        double lat0, lon0, lat1, lon1;
        s->comp.unproject(0.5, 0.5, lat0, lon0);
        s->comp.unproject(0.5 - dx, 0.5 - dy, lat1, lon1);
        pg->centreLat = std::clamp(pg->centreLat + (lat1 - lat0), -85.0, 85.0);
        double lon = pg->centreLon + (lon1 - lon0);
        while (lon > 180) { lon -= 360; }
        while (lon < -180) { lon += 360; }
        pg->centreLon = lon;
    });
    const auto zoomBy = [mapPage](int i, double factor, float fx, float fy, MapSurface* s) {
        MapPage* pg = mapPage(i);
        if (pg == nullptr) { return; }
        double lat, lon;
        if (s != nullptr) { s->comp.unproject(fx, fy, lat, lon); }
        const double before = pg->spanDeg;
        pg->spanDeg = std::clamp(pg->spanDeg * factor, slintmap::TileCompositor::kMinSpanDeg, slintmap::TileCompositor::kMaxSpanDeg);
        if (s != nullptr && pg->followedId.empty()) {
            // keep the point under the pointer where it is
            const double k = pg->spanDeg / before;
            pg->centreLon = lon + (pg->centreLon - lon) * k;
            pg->centreLat = std::clamp(lat + (pg->centreLat - lat) * k, -85.0, 85.0);
        }
    };
    ui_.global<::KitEvents>().on_page_map_zoomed([surface, zoomBy](int i, int notches, float fx, float fy) { zoomBy(i, std::pow(0.85, notches), fx, fy, surface(i)); });
    ui_.global<::KitEvents>().on_page_map_zoom_key([surface, zoomBy](int i, int dir) { zoomBy(i, dir > 0 ? 0.5 : 2.0, 0.5f, 0.5f, surface(i)); });
    ui_.global<::KitEvents>().on_page_map_clicked([this, surface, mapPage](int i, float fx, float fy) {
        MapSurface* s = surface(i); MapPage* pg = mapPage(i);
        if (s == nullptr || pg == nullptr) { return; }
        if (s->pickArmed) {
            double lat, lon;
            s->comp.unproject(fx, fy, lat, lon);
            FoxCommand c{}; c.op = FOXAPI_OP_SET_POSITION; c.num[0] = lat; c.num[1] = lon;
            kit_.submit(c);
            s->pickArmed = false;
            return;
        }
        pg->selectedId.clear();
    });
    ui_.global<::KitEvents>().on_page_map_resized([surface](int i, float w, float h) { if (MapSurface* s = surface(i)) { s->w = std::max(16, static_cast<int>(w)); s->h = std::max(16, static_cast<int>(h)); } });
    ui_.global<::KitEvents>().on_page_map_list_clicked([this, surface, mapPage](int i, int row) {
        MapSurface* s = surface(i); MapPage* pg = mapPage(i);
        if (s == nullptr || pg == nullptr || row < 0 || row >= static_cast<int>(s->listIds.size())) { return; }
        pg->selectedId = s->listIds[static_cast<std::size_t>(row)];
        detailsId_ = pg->selectedId;
        for (const cascade::core::HostTrack& tr : kit_.tracks()) {
            if (tr.plugin == pg->plugin && pg->selectedId == tr.t.id && cascade::core::trackHasPosition(tr.t)) { pg->centreLat = tr.t.latDeg; pg->centreLon = tr.t.lonDeg; break; }
        }
    });
    ui_.global<::KitEvents>().on_page_map_list_double_clicked([surface, mapPage](int i, int row) {
        MapSurface* s = surface(i); MapPage* pg = mapPage(i);
        if (s == nullptr || pg == nullptr || row < 0 || row >= static_cast<int>(s->listIds.size())) { return; }
        const std::string& id = s->listIds[static_cast<std::size_t>(row)];
        pg->followedId = pg->followedId == id ? std::string() : id;
    });
    ui_.global<::KitEvents>().on_page_map_fit([this, surface, mapPage](int i) {
        MapSurface* s = surface(i); MapPage* pg = mapPage(i);
        if (s == nullptr || pg == nullptr) { return; }
        std::vector<cascade::core::HostTrack> mine;
        for (const cascade::core::HostTrack& tr : kit_.tracks()) { if (tr.plugin == pg->plugin && cascade::core::trackPresentation(tr.t).visible) { mine.push_back(tr); } }
        if (fitView(mine, pg->centreLat, pg->centreLon, pg->spanDeg, s->h > 0 ? static_cast<double>(s->w) / s->h : 1.6)) { pg->viewFitted = true; pg->followedId.clear(); }
    });
    ui_.global<::KitEvents>().on_page_map_set_rx([surface](int i) { if (MapSurface* s = surface(i)) { s->pickArmed = !s->pickArmed; } });
    ui_.global<::KitEvents>().on_page_map_on_screen([surface](int i, bool on) { if (MapSurface* s = surface(i)) { s->onScreenOnly = on; } });
    ui_.global<::KitEvents>().on_page_map_coverage([surface](int i, bool on) { if (MapSurface* s = surface(i)) { s->coverageOn = on; } });
    ui_.global<::KitEvents>().on_page_map_trails([surface](int i, bool on) { if (MapSurface* s = surface(i)) { s->trailsOn = on; } });
    ui_.global<::KitEvents>().on_page_map_alt_colours([surface](int i, bool on) { if (MapSurface* s = surface(i)) { s->altColours = on; } });
    ui_.global<::KitEvents>().on_page_map_reset_coverage([this](int) { kit_.coverage().reset(); });
    ui_.global<::KitEvents>().on_page_map_stop_following([mapPage](int i) { if (MapPage* pg = mapPage(i)) { pg->followedId.clear(); } });
    ui_.global<::KitEvents>().on_page_map_sort_clicked([surface](int i) { if (MapSurface* s = surface(i)) { s->sortKey = (s->sortKey + 1) % cascade::gui::kTrackSortKeyCount; } });
    ui_.global<::KitEvents>().on_page_map_sort_dir([surface](int i) { if (MapSurface* s = surface(i)) { s->sortAscending = !s->sortAscending; } });
    // Fitted modules
    const auto fittedRow = [this](int i, int row) -> const PluginRow* {
        Page* p = i >= 0 && i < static_cast<int>(pages_.size()) ? &pages_[static_cast<std::size_t>(i)] : nullptr;
        if (p == nullptr || p->kind != 6) { return nullptr; }
        auto pages = pagesModel_;
        if (!pages || static_cast<std::size_t>(i) >= pages->row_count()) { return nullptr; }
        const PageTab t = *pages->row_data(static_cast<std::size_t>(i));
        if (!t.fitted.rows || row < 0 || row >= static_cast<int>(t.fitted.rows->row_count())) { return nullptr; }
        return rowByFile(kit_.lists().rows, std::string(t.fitted.rows->row_data(static_cast<std::size_t>(row))->file));
    };
    ui_.global<::KitEvents>().on_page_fitted_row_clicked([this, fittedRow](int i, int row) {
        const PluginRow* r = fittedRow(i, row);
        if (r == nullptr) { return; }
        openPage("module:" + r->file, 7, r->name, r->file, r->caps != 0u ? r->name : r->file);
        showPage("module:" + r->file);
    });
    ui_.global<::KitEvents>().on_page_fitted_row_double_clicked([this, fittedRow](int i, int row) {
        const PluginRow* r = fittedRow(i, row);
        if (r == nullptr) { return; }
        if (r->loaded && !r->stopped && (r->caps & (CASCADE_CAP_PANEL | CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_INSTRUMENT | CASCADE_CAP_TRACK_SOURCE)) != 0u) {
            openWindowsFor(*r, false);
            kit_.autoPreset(r->file);
        } else {
            openPage("module:" + r->file, 7, r->name, r->file, r->caps != 0u ? r->name : r->file);
            showPage("module:" + r->file);
        }
    });
    ui_.global<::KitEvents>().on_page_fitted_run([this, fittedRow](int i, int row) { const PluginRow* r = fittedRow(i, row); if (r != nullptr && r->loaded) { kit_.startStop(r->file, !fittedOffersStart(*r)); } });
    ui_.global<::KitEvents>().on_page_fitted_remove([this, fittedRow](int i, int row) {
        const PluginRow* r = fittedRow(i, row);
        if (r == nullptr) { return; }
        if (armedRemove_ == r->file && nowMs_ - armedAtMs_ < 5000) {
            FoxCommand c{}; c.op = FOXAPI_OP_STORE_REMOVE; std::snprintf(c.text, sizeof(c.text), "%s", r->file.c_str());
            kit_.submit(c);
            armedRemove_.clear();
        } else { armedRemove_ = r->file; armedAtMs_ = nowMs_; }
    });
    ui_.global<::KitEvents>().on_page_fitted_chip([this](int, int chip) { if (fittedHidden_.count(chip) > 0) { fittedHidden_.erase(chip); } else { fittedHidden_.insert(chip); } });
    ui_.global<::KitEvents>().on_page_fitted_search([this](int, slint::SharedString text) { fittedSearch_ = std::string(text); });
    ui_.global<::KitEvents>().on_page_fitted_scan([this](int) { kit_.rescan(); });
    ui_.global<::KitEvents>().on_page_fitted_reset_windows([this](int) { for (Page& p : pages_) { p.tx = 60.f; p.ty = 40.f; p.tw = 720.f; p.th = 520.f; p.widths.clear(); } });
    ui_.global<::KitEvents>().on_page_fitted_cleanup([](int) {});
    // a module's page
    ui_.global<::KitEvents>().on_page_module_back([this, page](int i) { Page* p = page(i); if (p == nullptr) { return; } const std::string id = p->id; openPage("fitted", 6, "", "", cascade::i18n::tr("Fitted modules")); showPage("fitted"); closePage(id); });
    ui_.global<::KitEvents>().on_page_module_run([this, page](int i) { Page* p = page(i); const PluginRow* r = p != nullptr ? rowOf(*p) : nullptr; if (r != nullptr && r->loaded) { kit_.startStop(r->file, !fittedOffersStart(*r)); } });
    ui_.global<::KitEvents>().on_page_module_grant([this, page](int i, int kind, bool on) { Page* p = page(i); const PluginRow* r = p != nullptr ? rowOf(*p) : nullptr; if (r != nullptr) { kit_.setGrant(r->file, kind, on); } });
    ui_.global<::KitEvents>().on_page_module_command([this, page](int i, int c) {
        Page* p = page(i); const PluginRow* r = p != nullptr ? rowOf(*p) : nullptr;
        if (r != nullptr && c >= 0 && c < static_cast<int>(r->commands.size())) {
            // the Solar System View's ROTATE / TILT / ZOOM keys drive the 3D camera while the 3D view is on (docs 6.65): the plugin is not sent them then
            if (globe_->takeCommand(r->name, r->commands[static_cast<std::size_t>(c)].label)) { return; }
            kit_.pressCommand(r->file, r->commands[static_cast<std::size_t>(c)].id);
        }
    });
    ui_.global<::KitEvents>().on_page_module_reenable([this, page](int i) { Page* p = page(i); if (p != nullptr) { kit_.reenable(p->file); } });
    ui_.global<::KitEvents>().on_page_module_keep([this, page](int i, bool on) { Page* p = page(i); if (p != nullptr) { kit_.setRunMode(p->file, on ? 1 : 0); } });
    // Target details (the page)
    ui_.global<::KitEvents>().on_page_details_go([detailsGo](int) { detailsGo(); });
    ui_.global<::KitEvents>().on_page_details_follow([detailsFollow](int) { detailsFollow(); });
    ui_.global<::KitEvents>().on_page_details_close([this](int) { detailsId_.clear(); closePage("details"); });
    // the "Sound is muted" question
    ui_.global<::KitEvents>().on_mute_stop_clicked([this] { kit_.stopList(muteKeys_, true); muteQuestionOpen_ = false; ui_.set_mute_question_open(false); });
    ui_.global<::KitEvents>().on_mute_keep_clicked([this] { kit_.keepMuteRunning(); muteQuestionOpen_ = false; ui_.set_mute_question_open(false); });
}

}  // namespace foxsdr::slintkit
