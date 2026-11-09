// The plugin kit's pure logic (src/slint/kit_logic.hpp) against a STUB TABLE: the lists and facts a running engine would publish for a
// handful of plugins, filled by hand, and the rules the ImGui window applies to them held here - the list reader's joins, the fitted
// state's precedence and sentences, the chip words, the plugin-use table, the preset-lit test, the settings form's machine, the
// decoder-lines tail, the map pages' book-keeping. No Slint, no engine.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "kit_logic.hpp"
#include "test_check.hpp"

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace foxsdr::slintkit;

namespace {

// A table of lists and facts a test fills in. Every list entry is a FoxListItem as read_list would fill it.
class StubTable : public TableReader {
public:
    std::map<std::uint32_t, std::vector<FoxListItem>> lists;
    std::map<std::string, std::string> facts;
    int listReads = 0;

    std::int32_t readList(std::uint32_t list, std::uint32_t index, FoxListItem* out) override {
        ++listReads;
        const auto it = lists.find(list);
        const std::int32_t n = it == lists.end() ? 0 : static_cast<std::int32_t>(it->second.size());
        if (out != nullptr && static_cast<std::int32_t>(index) < n) { *out = it->second[index]; }
        return n;
    }
    bool fact(const std::string& key, std::string& out) override {
        const auto it = facts.find(key);
        if (it == facts.end()) { out.clear(); return false; }
        out = it->second;
        return true;
    }
    FoxListItem& add(std::uint32_t list, std::uint64_t id, const char* name, const char* detail, std::uint32_t flags = 0, double v0 = 0, double v1 = 0, double v2 = 0, double v3 = 0) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        it.id = id;
        std::snprintf(it.name, sizeof(it.name), "%s", name);
        std::snprintf(it.detail, sizeof(it.detail), "%s", detail);
        it.flags = flags;
        it.value[0] = v0; it.value[1] = v1; it.value[2] = v2; it.value[3] = v3;
        lists[list].push_back(it);
        return lists[list].back();
    }
};

// Four plugins: a fed text decoder with presets (APRS), an asleep picture decoder (APT), a stopped panel+instrument module (POCSAG) and a
// refused file (FT8 1.0 for a newer host); one user preset for APRS; one stale grant; two settings fields for APRS.
StubTable sample() {
    StubTable t;
    t.add(FOXAPI_LIST_PLUGINS, 1, "APRS / AX.25 Packet Decoder", "aprs-decoder-1.0.2-abi3-win-x64.dll", FOXAPI_ITEM_ACTIVE | FOXAPI_ITEM_TUNE_ALLOWED | FOXAPI_ITEM_MATCHED,
          CASCADE_CAP_DECODER | CASCADE_CAP_PRESET | CASCADE_CAP_SETTINGS_UI | CASCADE_CAP_HOST_CLIENT, 123456, 2, 1);
    t.add(FOXAPI_LIST_PLUGINS, 2, "NOAA Weather Pictures (APT)", "apt-decoder-1.0.0-abi3-win-x64.dll", FOXAPI_ITEM_ACTIVE, CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_PRESET, 0, 0, 4);
    t.add(FOXAPI_LIST_PLUGINS, 3, "POCSAG", "pocsag-decoder-1.0.5-abi3-win-x64.dll", FOXAPI_ITEM_ACTIVE | FOXAPI_ITEM_STOPPED,
          CASCADE_CAP_IQ_DECODER | CASCADE_CAP_PANEL | CASCADE_CAP_INSTRUMENT | CASCADE_CAP_PRESET, 0, 0, 2);
    t.add(FOXAPI_LIST_PLUGINS, 4, "ft8-decoder-1.0.0-abi3-win-x64.dll", "ft8-decoder-1.0.0-abi3-win-x64.dll", 0, 0, 0, 0, 0);
    t.add(FOXAPI_LIST_DECODERS, 1, "APRS / AX.25 Packet Decoder", "", FOXAPI_ITEM_ACTIVE | FOXAPI_ITEM_FED | FOXAPI_ITEM_MUTES);
    t.add(FOXAPI_LIST_DECODERS, 2, "NOAA Weather Pictures (APT)", "asleep: nothing is using it", 0);
    t.add(FOXAPI_LIST_DECODERS, 3, "POCSAG", "stopped by you", 0);
    t.add(FOXAPI_LIST_PRESETS, 0, "APRS Europe 144.800", "APRS / AX.25 Packet Decoder", 0, 144.8e6, 0, 0);
    t.add(FOXAPI_LIST_PRESETS, 1, "", "APRS / AX.25 Packet Decoder", 0, 144.39e6, 0, 12500);
    t.add(FOXAPI_LIST_PRESETS, 0, "NOAA 19 137.100", "NOAA Weather Pictures (APT)", FOXAPI_ITEM_DEVICE_CENTRE, 137.1e6, 0, 0);
    t.add(FOXAPI_LIST_USER_PRESETS, 7, "145.5000 MHz", "aprs-decoder", 0, 145.5e6, 0, 0);
    t.add(FOXAPI_LIST_MUTE_CAUSES, 0, "APRS / AX.25 Packet Decoder", "aprs-decoder-1.0.2-abi3-win-x64.dll", FOXAPI_ITEM_ACTIVE, 1);
    t.add(FOXAPI_LIST_STALE_GRANTS, 0, "old-tracker-0.9.0-abi3-win-x64.dll", "", FOXAPI_ITEM_REFUSED);
    t.add(FOXAPI_LIST_PLUGIN_COMMANDS, 4, "CLEAR LOG", "aprs-decoder-1.0.2-abi3-win-x64.dll");
    t.add(FOXAPI_LIST_PLUGIN_COMMANDS, 9, "SEND BEACON", "aprs-decoder-1.0.2-abi3-win-x64.dll");
    t.add(FOXAPI_LIST_PLUGIN_SETTINGS, 1, "Callsign filter", "aprs-decoder-1.0.2-abi3-win-x64.dll", 0, 0, 16);
    t.add(FOXAPI_LIST_PLUGIN_SETTINGS, 2, "Log to file", "aprs-decoder-1.0.2-abi3-win-x64.dll", 0, 2, 1);
    t.add(FOXAPI_LIST_STORE_BLOCKED, 0, "adsb-decoder-1.4.0-abi3-win-x64.dll", "needs FoxSDR 0.99");
    t.facts["fact.plugin.aprs-decoder-1.0.2-abi3-win-x64.dll.record"] = "aprs-decoder\t3\t1762128000\tvoice-data";
    t.facts["fact.plugin.aprs-decoder-1.0.2-abi3-win-x64.dll.version"] = "1.0.2";
    t.facts["fact.plugin.aprs-decoder-1.0.2-abi3-win-x64.dll.author"] = "FOX & SCHIRMYVER";
    t.facts["fact.plugin.aprs-decoder-1.0.2-abi3-win-x64.dll.path"] = "C:\\plugins\\aprs-decoder-1.0.2-abi3-win-x64.dll";
    t.facts["fact.plugin.aprs-decoder-1.0.2-abi3-win-x64.dll.notice"] = "bad checksum, frame dropped";
    t.facts["fact.plugin.pocsag-decoder-1.0.5-abi3-win-x64.dll.windows"] = "panel\tPager memory\ninstrument\tPOCSAG pager";
    t.facts["fact.plugin.ft8-decoder-1.0.0-abi3-win-x64.dll.error"] = "built for plugin ABI 5; this build is ABI 4";
    t.facts["fact.plugin.setting.1.spec"] = std::string("callsign_filter") + '\x1F' + "e.g. M0ABC";
    t.facts["fact.plugin.setting.1"] = "M0ABC";
    t.facts["fact.plugin.setting.2.spec"] = std::string("log_to_file") + '\x1F' + "";
    t.facts["fact.plugin.setting.2"] = "1";
    t.facts["fact.blocked.adsb-decoder-1.4.0-abi3-win-x64.dll.message"] = "adsb-decoder 1.4.0 is retired: needs FoxSDR 0.99";
    t.facts["fact.blocked.adsb-decoder-1.4.0-abi3-win-x64.dll.id"] = "adsb-decoder";
    t.facts["fact.mute.subject"] = "APRS / AX.25 Packet Decoder";
    t.facts["fact.mute.keptRunning"] = "1";
    t.facts["fact.plugin.lastDenied"] = "aprs-decoder-1.0.2-abi3-win-x64.dll";
    t.facts["fact.plugin.dir"] = "C:\\plugins";
    t.facts["fact.store.url"] = "https://example.test/index.json";
    return t;
}

const PluginRow& rowNamed(const KitLists& k, const char* name) {
    static const PluginRow none{};
    for (const PluginRow& r : k.rows) { if (r.name == name) { return r; } }
    return none;
}

}  // namespace

int main() {
    // ---- the reader's joins ----
    {
        StubTable t = sample();
        KitLists k;
        ReadOptions o; o.fittedFacts = true; o.settings = true; o.blocked = true;
        readKitLists(t, k, o);
        CHECK(k.rows.size() == 4);
        const PluginRow& aprs = rowNamed(k, "APRS / AX.25 Packet Decoder");
        CHECK(aprs.loaded && aprs.decoder && aprs.fed && aprs.mutes && aprs.tuneAllowed && aprs.matched);
        CHECK(aprs.runId == "aprs-decoder");
        CHECK(aprs.category == "voice-data");
        CHECK(aprs.hasRecord && aprs.catalogueKey == "aprs-decoder" && aprs.abiVersion == 3 && aprs.fittedAtUnix == 1762128000);
        CHECK(aprs.presets.size() == 3 && aprs.ownPresets == 2);
        CHECK(aprs.presets[0].label == "APRS Europe 144.800" && aprs.presets[0].hz == 144.8e6 && !aprs.presets[0].mine);
        CHECK(aprs.presets[1].label == "APRS / AX.25 Packet Decoder");   // an empty label falls back to the plugin's name
        CHECK(aprs.presets[1].bwHz == 12500 && aprs.presets[1].index == 1);
        CHECK(aprs.presets[2].mine && aprs.presets[2].index == 1000 && aprs.presets[2].label == "145.5000 MHz");
        CHECK(aprs.commands.size() == 2 && aprs.commands[1].id == 9 && aprs.commands[1].label == "SEND BEACON");
        CHECK(aprs.version == "1.0.2" && aprs.author == "FOX & SCHIRMYVER" && aprs.notice == "bad checksum, frame dropped");
        const PluginRow& apt = rowNamed(k, "NOAA Weather Pictures (APT)");
        CHECK(apt.asleep && apt.runMode == 0 && apt.decoder && !apt.fed && apt.idle == "asleep: nothing is using it");
        CHECK(apt.presets.size() == 1 && apt.presets[0].deviceCentre);
        const PluginRow& pocsag = rowNamed(k, "POCSAG");
        CHECK(pocsag.stopped && pocsag.runMode == 2 && pocsag.panelTitle == "Pager memory" && pocsag.instrumentTitle == "POCSAG pager");
        const PluginRow& ft8 = rowNamed(k, "ft8-decoder-1.0.0-abi3-win-x64.dll");
        CHECK(!ft8.loaded && ft8.error == "built for plugin ABI 5; this build is ABI 4");
        CHECK(k.muteCauses.size() == 1 && k.muteCauses[0].key == "aprs-decoder-1.0.2-abi3-win-x64.dll");
        CHECK(k.staleGrants.size() == 1 && k.staleGrants[0].refused);
        CHECK(k.settings.size() == 1 && k.settings[0].file == aprs.file && k.settings[0].fields.size() == 2);
        CHECK(k.settings[0].fields[0].key == "callsign_filter" && k.settings[0].fields[0].placeholder == "e.g. M0ABC" && k.settings[0].fields[0].value == "M0ABC" && k.settings[0].fields[0].maxLength == 16);
        CHECK(k.settings[0].fields[1].kind == 2 && k.settings[0].fields[1].value == "1" && k.settings[0].fields[1].id == 2);
        CHECK(k.blocked.size() == 1 && k.blocked[0].message == "adsb-decoder 1.4.0 is retired: needs FoxSDR 0.99" && k.blocked[0].id == "adsb-decoder");
        CHECK(k.muteSubject == "APRS / AX.25 Packet Decoder" && k.muteKeptRunning && k.lastDenied == aprs.file && k.pluginDir == "C:\\plugins");
        CHECK(k.catalogueUrl == "https://example.test/index.json");
        CHECK(fileForName(k.rows, "POCSAG") == "pocsag-decoder-1.0.5-abi3-win-x64.dll");
        CHECK(fileForName(k.rows, "ft8-decoder-1.0.0-abi3-win-x64.dll").empty());   // a refused file is never a loaded name
        CHECK(rowByFile(k.rows, "apt-decoder-1.0.0-abi3-win-x64.dll") == &apt);
        CHECK(settingsFor(k.settings, aprs.file) == &k.settings[0] && settingsFor(k.settings, "x") == nullptr);
        // without the fitted facts the facts are not read at all
        StubTable t2 = sample();
        KitLists k2;
        readKitLists(t2, k2, ReadOptions{});
        CHECK(rowNamed(k2, "APRS / AX.25 Packet Decoder").version.empty() && rowNamed(k2, "APRS / AX.25 Packet Decoder").commands.empty());
        CHECK(k2.settings.empty() && k2.blocked.empty());
    }

    // ---- the fitted state's precedence, words and sentences ----
    {
        StubTable t = sample();
        KitLists k;
        ReadOptions o; o.fittedFacts = true;
        readKitLists(t, k, o);
        const PluginRow& aprs = rowNamed(k, "APRS / AX.25 Packet Decoder");
        const PluginRow& apt = rowNamed(k, "NOAA Weather Pictures (APT)");
        const PluginRow& pocsag = rowNamed(k, "POCSAG");
        const PluginRow& ft8 = rowNamed(k, "ft8-decoder-1.0.0-abi3-win-x64.dll");
        CHECK(fittedState(aprs, true) == FittedState::Fed);
        CHECK(fittedState(aprs, false) == FittedState::NotFed);   // the receiver's run state gates FED
        CHECK(fittedState(apt, true) == FittedState::Idle);
        CHECK(fittedState(pocsag, true) == FittedState::Stopped);
        CHECK(fittedState(ft8, true) == FittedState::Refused);
        PluginRow basemap; basemap.loaded = true; basemap.caps = CASCADE_CAP_BASEMAP;
        CHECK(fittedState(basemap, true) == FittedState::NoSignal);
        CHECK(std::string(fittedStateWord(FittedState::NotFed)) == "NOT DECODING");
        CHECK(std::string(fittedStateWord(FittedState::NoSignal)) == "TAKES NO SIGNAL");
        CHECK(fittedStateIndex(FittedState::Fed) == 0 && fittedStateIndex(FittedState::Refused) == 5);
        CHECK(fittedOffersStart(apt) && fittedOffersStart(pocsag) && !fittedOffersStart(aprs));
        CHECK(fittedStateSentence(ft8, true) == "built for plugin ABI 5; this build is ABI 4");
        CHECK(fittedStateSentence(aprs, false).find("starting the receiver is all this needs") != std::string::npos);
        CHECK(fittedStateSentence(apt, true).find("idle") != std::string::npos);
        PluginRow notFed = aprs; notFed.fed = false; notFed.idle = "the receiver is at 2.4 MS/s and it wants 48 kS/s";
        CHECK(fittedStateSentence(notFed, true) == notFed.idle);
        CHECK(fittedVerdictLine(true, "C:\\plugins").find("Read from C:\\plugins.") != std::string::npos);
        CHECK(fittedVerdictLine(false, "").find("No directory has been scanned yet.") != std::string::npos);
        CHECK(abiText(0, 4) == "not recorded" && abiText(4, 4) == "4, matches this build" && abiText(5, 4) == "5, this build needs 4");
        CHECK(fittedDateText(0).empty() && fittedDateText(1762128000).compare(0, 7, "fitted ") == 0);
        const auto reach = reachRows(aprs.caps);
        CHECK(reach.size() == 4 && reach[0].key == "TEXT DECODER" && reach[2].key == "PRESETS" && reach[1].outward);
        CHECK(capsOfferKeepRunning(aprs.caps) && !capsOfferKeepRunning(CASCADE_CAP_AUDIO_PROCESSOR) && !capsOfferKeepRunning(CASCADE_CAP_HOST_CLIENT));
        CHECK(txPlateSentence(0, false).empty());
        CHECK(txPlateSentence(2, false) == "It has asked to transmit and is not allowed to.");
        CHECK(txPlateSentence(2 | 32, true).compare(0, 5, "ARMED") == 0);
        CHECK(txPlateSentence(32 | 64, true).compare(0, 6, "ON AIR") == 0);
    }

    // ---- the chip words ----
    {
        cascade::core::HostImage img; img.width = 640; img.height = 480; img.complete = false;
        CHECK(imageChip(nullptr, true) == "IDLE" && imageChip(nullptr, false) == "WAIT" && imageChip(&img, false) == "RX");
        img.complete = true;
        CHECK(imageChip(&img, false) == "IMG");
        cascade::core::HostPanel panel; panel.rows.resize(7);
        CHECK(panelChip(nullptr) == "IDLE" && panelChip(&panel) == "7 ROW");
        cascade::core::HostInstrument pager; pager.kind = CASCADE_INSTRUMENT_PAGER; pager.have = true; pager.state.values[0] = 3;
        CHECK(instrumentChip(nullptr, false) == "IDLE" && instrumentChip(&pager, true) == "NEW" && instrumentChip(&pager, false) == "3 MSG");
        pager.have = false;
        CHECK(instrumentChip(&pager, false) == "WAIT");
        cascade::core::HostInstrument vor; vor.kind = CASCADE_INSTRUMENT_NAV_BEARING; vor.have = true; vor.state.values[0] = 274; vor.state.flags = CASCADE_INSTRUMENT_FLAG_LOCK;
        CHECK(instrumentChip(&vor, false) == "274");
        vor.state.flags = 0;
        CHECK(instrumentChip(&vor, false) == "NAV");
        cascade::core::HostInstrument fax; fax.kind = CASCADE_INSTRUMENT_FAX; fax.have = true; std::snprintf(fax.state.text[0], sizeof(fax.state.text[0]), "PICTURE");
        CHECK(instrumentChip(&fax, false) == "PICTURE");
        cascade::core::HostInstrument meter; meter.kind = CASCADE_INSTRUMENT_METER; meter.have = true; meter.rows.resize(12);
        CHECK(instrumentChip(&meter, false) == "12 MTR");
        cascade::core::HostInstrument printer; printer.kind = CASCADE_INSTRUMENT_TELEPRINTER; printer.have = true; std::snprintf(printer.state.text[0], sizeof(printer.state.text[0]), "G-EZBX");
        CHECK(instrumentChip(&printer, false) == "G-EZBX");
        cascade::core::HostInstrument other; other.kind = 42; other.have = true;
        CHECK(instrumentChip(&other, false) == "RX");
        CHECK(fittedChipWord(3, 28) == "3/28" && decodersChipWord(2) == "2 FED" && targetsChipWord(14) == "14 TGT");
    }

    // ---- the plugin-use table ----
    {
        StubTable t = sample();
        KitLists k;
        readKitLists(t, k, ReadOptions{});
        OpenWindows open;
        CHECK(pluginUseText(k.rows, open).empty());
        open.windows.insert(windowKey("instrument", "POCSAG", "POCSAG pager"));
        open.decoderOutput = true;
        const std::string text = pluginUseText(k.rows, open);
        CHECK(text == "aprs-decoder-1.0.2-abi3-win-x64.dll\t8\npocsag-decoder-1.0.5-abi3-win-x64.dll\t9\n");
        OpenWindows map; map.mapPages.insert("APRS / AX.25 Packet Decoder");   // not a track source: no bit
        CHECK(pluginUseText(k.rows, map).empty());
        // THE RADAR SCOPE'S BIT (docs 6.57; FOXAPP_PLUGIN_USE_RADAR_SCOPE = 4): while the scope shows, EVERY track source is in use - an AUTO ADS-B plugin sleeps without it and the scope stays empty -
        // and nothing that is not a track source is
        {
            KitLists r;
            PluginRow tracks;
            tracks.loaded = true;
            tracks.file = "adsb-decoder-1.9.1-abi3-win-x64.dll";
            tracks.name = "ADS-B";
            tracks.caps = CASCADE_CAP_TRACK_SOURCE;
            PluginRow audio;
            audio.loaded = true;
            audio.file = "cw-decoder-1.1.0-abi3-win-x64.dll";
            audio.name = "CW";
            audio.caps = CASCADE_CAP_DECODER;
            r.rows.push_back(tracks);
            r.rows.push_back(audio);
            OpenWindows none;
            CHECK(pluginUseText(r.rows, none).empty());
            OpenWindows scope;
            scope.radarScope = true;
            CHECK(pluginUseText(r.rows, scope) == "adsb-decoder-1.9.1-abi3-win-x64.dll\t4\n");
            scope.mapPages.insert("ADS-B");   // the bits add: a map page and the scope together
            CHECK(pluginUseText(r.rows, scope) == "adsb-decoder-1.9.1-abi3-win-x64.dll\t6\n");
        }
        CHECK(windowBelongsTo(windowKey("image", "NOAA Weather Pictures (APT)"), "NOAA Weather Pictures (APT)"));
        CHECK(!windowBelongsTo(windowKey("panel", "POCSAG"), "POCSAG pager"));
        CHECK(windowBelongsTo(windowKey("instrument", "POCSAG", "POCSAG pager"), "POCSAG"));
    }

    // ---- presets: lit, tooltips, the save label ----
    {
        Preset p; p.hz = 144.8e6;
        CHECK(presetLit(p, 144.0e6, 144.8e6) && presetLit(p, 144.0e6, 144.804e6) && !presetLit(p, 144.0e6, 144.81e6));
        p.deviceCentre = true;
        CHECK(presetLit(p, 144.8e6, 100e6) && !presetLit(p, 144.0e6, 144.8e6));
        Preset wide; wide.hz = 1090e6; wide.bwHz = 2.4e6;
        CHECK(presetLit(wide, 0, 1091e6) && !presetLit(wide, 0, 1091.3e6));
        Preset none; none.hz = 0;
        CHECK(!presetLit(none, 0, 0));
        PluginRow text; text.name = "APRS"; text.caps = CASCADE_CAP_DECODER;
        CHECK(presetTooltip(text, p, false) == "Tune to 144.8000 MHz, start this plugin and open the Decoder output window");
        PluginRow pic; pic.name = "APT"; pic.caps = CASCADE_CAP_IMAGE_DECODER;
        CHECK(presetTooltip(pic, p, false) == "Tune to 144.8000 MHz, start this plugin and open its windows");
        PluginRow bare; bare.name = "X"; bare.caps = CASCADE_CAP_HOST_CLIENT;
        CHECK(presetTooltip(bare, p, false) == "Tune to 144.8000 MHz and start this plugin");
        CHECK(presetTooltip(text, p, true) == "Already tuned to this preset of APRS.");
        Preset mine; mine.mine = true; mine.hz = 145.5e6;
        CHECK(presetTooltip(text, mine, false) == "Tune to your preset for APRS. Right-click to forget it.");
        CHECK(presetSaveLabel(145.5e6) == "+ Save 145.5000 MHz as a preset");
    }

    // ---- the settings form's machine ----
    {
        CHECK(verdictFromAnswer(FOXAPI_OK, 0) == Verdict::Accepted && verdictFromAnswer(FOXAPI_NO_CHANGE, 0) == Verdict::Accepted);
        CHECK(verdictFromAnswer(-3, 2) == Verdict::NotANumber && verdictFromAnswer(-3, 4) == Verdict::StoreFull && verdictFromAnswer(FOXAPI_BUSY, 0) == Verdict::StoreError);
        CHECK(std::string(verdictSentence(Verdict::TooLong)) == "Not saved: longer than this field allows.");
        CHECK(std::string(verdictSentence(Verdict::Accepted)).empty());
        SettingsEdits e;
        CHECK(e.shown("f", "k", "old") == "old" && !e.pending("f", "k") && e.error("f", "k").empty());
        e.sent("f", "k", 77, "new");
        CHECK(e.pending("f", "k") && e.shown("f", "k", "old") == "new" && e.error("f", "k").empty());
        CHECK(!e.deliver(78, Verdict::Accepted));   // no box waits for that ticket
        CHECK(e.deliver(77, Verdict::Accepted) && !e.pending("f", "k"));
        CHECK(e.shown("f", "k", "old") == "new");   // held while the table catches up
        for (int i = 0; i < kSettingHoldTicks; ++i) { e.tick(); }
        CHECK(e.shown("f", "k", "old") == "old");
        e.sent("f", "k", 80, "3,x");
        CHECK(e.deliver(80, Verdict::NotANumber));
        CHECK(e.shown("f", "k", "old") == "old" && e.error("f", "k") == "Not saved: that is not a number.");
        e.sent("f", "k", 81, "4");                   // a new edit clears the old refusal while it is pending
        CHECK(e.error("f", "k").empty());
        e.refusedAtOnce("g", "z");
        CHECK(e.error("g", "z") == "Not saved: the store refused it." && e.size() == 2);
    }

    // ---- the decoder lines' tail ----
    {
        StubTable t;
        for (std::uint64_t id = 1; id <= 6; ++id) { t.add(FOXAPI_LIST_DECODER_LINES, id, "aprs", id == 3 ? "WARN  bad checksum" : id == 5 ? "ERROR lost sync" : "M0ABC>APRS"); }
        std::uint64_t lastId = 0;
        std::deque<DecoderLine> ring;
        CHECK(readDecoderLinesTail(t, lastId, ring, 500) == 6 && lastId == 6 && ring.size() == 6 && ring.front().id == 1 && ring.back().id == 6);
        CHECK(ring[2].tone == 1 && ring[4].tone == 2 && ring[0].tone == 0 && ring[0].plugin == "aprs");
        CHECK(readDecoderLinesTail(t, lastId, ring, 500) == 0);   // nothing newer
        t.add(FOXAPI_LIST_DECODER_LINES, 7, "aprs", "newer");
        t.add(FOXAPI_LIST_DECODER_LINES, 8, "aprs", "newest");
        CHECK(readDecoderLinesTail(t, lastId, ring, 4) == 2 && ring.size() == 4 && ring.front().id == 5 && ring.back().text == "newest" && lastId == 8);
        CHECK(lineTone("WARN  x") == 1 && lineTone("ERROR x") == 2 && lineTone("INFO  x") == 0 && lineTone("warning") == 0);
    }

    // ---- the map pages ----
    {
        StubTable t = sample();
        KitLists k;
        readKitLists(t, k, ReadOptions{});
        std::vector<MapPage> pages;
        syncMapPages(pages, {"ADS-B", "AIS"}, k.rows);
        CHECK(pages.size() == 2 && pages[0].plugin == "ADS-B" && !pages[0].open);
        pages[0].open = true; pages[0].spanDeg = 2.5;
        syncMapPages(pages, {"AIS", "ADS-B"}, k.rows);   // the order of the names does not reorder or reset the pages
        CHECK(pages.size() == 2 && pages[0].plugin == "ADS-B" && pages[0].open && pages[0].spanDeg == 2.5);
        syncMapPages(pages, {"AIS"}, k.rows);
        CHECK(pages.size() == 1 && pages[0].plugin == "AIS" && findMapPage(pages, "ADS-B") == nullptr && findMapPage(pages, "AIS") == &pages[0]);
        std::vector<cascade::core::HostTrack> tracks(2);
        tracks[0].t.latDeg = 51.0; tracks[0].t.lonDeg = -1.0;
        tracks[1].t.latDeg = 52.0; tracks[1].t.lonDeg = 1.0;
        double lat = 0, lon = 0, span = 0;
        CHECK(fitView(tracks, lat, lon, span, 1.5));
        CHECK_NEAR(lat, 51.5, 1e-9);
        CHECK_NEAR(lon, 0.0, 1e-9);
        CHECK(span > 2.0 && span < 3.0);
        std::vector<cascade::core::HostTrack> unplaced(1);
        unplaced[0].t.flags = CASCADE_TRACK_FLAG_NO_POSITION; unplaced[0].t.latDeg = 0; unplaced[0].t.lonDeg = 0;
        CHECK(!fitView(unplaced, lat, lon, span, 1.5));
        CHECK(imageTitle("APT") == "APT image" && mapTitle("ADS-B") == "ADS-B map");
        CHECK(countText(14, 14, 0) == "14 targets" && countText(1, 1, 0) == "1 target" && countText(3, 14, 2) == "3 of 14 targets, 2 heard without a position");
    }

    return testSummary("test_slint_host_kit");
}
