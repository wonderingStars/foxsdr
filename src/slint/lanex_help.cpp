// lanex_help.cpp - see lanex_help.hpp. Every sentence below goes through the translation call; the tools/i18n_keys.py extraction finds the FOX_TR_NOOP literals.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanex_help.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "core/i18n.hpp"

namespace foxsdr::slintlanex {

namespace {

// VERIFICATION KEY (what each sentence was checked against; the same table is in docs/multi-radio-records/lane-x.md):
//   T   the text of a control's own tooltip, copied letter for letter from the .slint file that shows it (the product already says it)
//   C   a sentence checked against the host code named in the record
const std::vector<HelpTopic> kTopics = {
    // ------------------------------------------------------------------ the window
    {"start", FOX_TR_NOOP("Finding your way around"), {
        FOX_TR_NOOP("The rail at the left holds five banks: SIGNAL, DECODE, TOOLS, EXTEND and SYSTEM. By default the keys F1 to F5 open them in turn; the Key bindings page changes the keys."),   // C: main.slint RailKey hints, system_logic.cpp key table
        FOX_TR_NOOP("Each bank is a list of sections. Click a section's name to open it, and click it again to fold it."),   // C: on_section_clicked toggles openSections
        FOX_TR_NOOP("The brand at the left of the title bar opens the App menu: About FoxSDR, Key bindings, Save a screenshot, Help topics, the manual online, Keyboard shortcuts, Recordings and Quit."),   // C: main.cpp menu 601 to 610
        FOX_TR_NOOP("Help topics also opens with Ctrl+F1, and Esc closes it."),   // C: key_dispatch.cpp, main.cpp escape
    }, -1, -1, "bank rail section menu start begin getting started keys F1"},
    {"tune", FOX_TR_NOOP("Tuning and zooming"), {
        FOX_TR_NOOP("Click the spectrum or the waterfall to tune to that frequency. Double-click either one to show the whole span again."),   // C: clickTune, spectrum_double_clicked
        FOX_TR_NOOP("The mouse wheel over either one zooms in and out. With Shift held it steps the tuned frequency by the mode's step instead, except while the Airband monitor listens, when Shift and the wheel zoom as well."),   // C: wheel lambda
        FOX_TR_NOOP("Right-click the spectrum for the Trace menu, and the waterfall for the marker menu."),   // C: spectrum_menu_requested, waterfall_menu_requested
    }, -1, -1, "tune frequency click wheel zoom spectrum waterfall marker trace menu"},
    {"layouts", FOX_TR_NOOP("Layouts"), {
        FOX_TR_NOOP("The Layouts key in the title bar opens a menu of arrangements of the window. Choosing one applies it."),   // C: main.cpp menu ids 1000 and up
        FOX_TR_NOOP("Save the current layout... stores the open bank and sections and how the radios are laid out under a name you type, and Manage layouts... lists them. Two layouts come with the program: Airband and HF DX."),   // C: main.cpp 1072 to 1073, layouts_prefs.cpp 86 and 88
    }, -1, -1, "layout arrangement save airband hf dx"},
    {"radios", FOX_TR_NOOP("Several radios"), {
        FOX_TR_NOOP("With more than one radio open, the radio layouts show them side by side or in a grid, each pane with its own spectrum and waterfall. A window that is too small shows one radio at a time and says so."),   // C: workspace_logic.cpp pane layouts, main.cpp narrow notice
    }, -1, -1, "radios multi pane grid side by side several"},

    // ------------------------------------------------------------------ SIGNAL
    {"sig-patch", FOX_TR_NOOP("Patch"), {
        FOX_TR_NOOP("A patch wires parts of the signal chain together on the PATCH tab above the spectrum. The Patch row's chip reads RECEIVER VIEW normally and PATCH RUNNING while a patch runs."),   // C: main.cpp rail chip for bank 0 row 0
    }, 0, 0, "patch wire parts graph chain tab"},
    {"sig-source", FOX_TR_NOOP("Source"), {
        FOX_TR_NOOP("Source says where the signal comes from: a radio, the sound card, an I/Q file or a network radio. Its chip reads GENERATOR while no radio is open."),   // C: signal_bank.slint labels Device, Sound card, I/Q file, rtl_tcp host and port; main.cpp chip
        FOX_TR_NOOP("An up- or down-converter between the antenna and this radio. FoxSDR then shows and tunes the frequency on the air, and drives the radio at the converted one."),   // T
        FOX_TR_NOOP("Divides the sample rate by a power of two before anything else sees it: a narrower span, less work for the computer, and less noise in each sample - about 3 dB for every halving. The same as decimation in Airspy's own software."),   // T
        FOX_TR_NOOP("Sends about 4.5 V up the antenna cable to power an amplifier at the mast. Leave it off unless you have one: equipment that is not expecting power on the connector can be damaged by it."),   // T
    }, 0, 1, "source radio device sound card iq file converter decimation rtl_tcp pluto bias tee amplifier"},
    {"sig-radio", FOX_TR_NOOP("Radio"), {
        FOX_TR_NOOP("Mode keys are on the receiver strip above (Ctrl+F … Ctrl+R)."),   // T: main.slint line 670 (the ellipsis is the file's own U+2026)
        FOX_TR_NOOP("The Passband slider sets the width of the channel, and Tune step is the size of one step of the tune-up and tune-down keys."),   // C: main.cpp on_passband_changed, tuneStepUp
        FOX_TR_NOOP("Decode the 38 kHz difference channel when a 19 kHz pilot tone is present. Unticked, the sound is forced to mono."),   // T
    }, 0, 2, "radio mode passband tune step stereo band plan fm"},
    {"sig-audio", FOX_TR_NOOP("Audio filters"), {
        FOX_TR_NOOP("Audio filters holds noise reduction, a notch, an automatic notch and the noise blanker. The chip reads ON while noise reduction or either notch is on."),   // C: main.cpp chip (FOXAPI_RX_NR | NOTCH | AUTO_NOTCH); labels in signal_bank.slint
        FOX_TR_NOOP("How the audio level follows the signal. Auto is the original behaviour; Fast, Medium and Slow differ in how quickly the gain recovers after a loud signal; Off holds the gain still at the Fixed gain value."),   // T
        FOX_TR_NOOP("Removes short wide-band clicks - ignition, electric fences, switching - before the filter smears them. It changes what the audio, meter, scope and decoders of this radio hear; recordings are not touched."),   // T
    }, 0, 3, "audio filter noise reduction notch blanker clicks agc"},
    {"sig-sinks", FOX_TR_NOOP("Sinks"), {
        FOX_TR_NOOP("Sinks holds the sound output. The chip reads OPEN while an output is open and CLOSED otherwise."),   // C: main.cpp chip (FOXAPI_RX_SINK_OPEN)
        FOX_TR_NOOP("How much sound is held back before it plays. A larger buffer rides out a slow computer, at the cost of a longer delay between the radio and the speakers. AUTOMATIC starts at 120 ms and deepens itself, to at most 960 ms, when this computer falls behind. After a quarter of an hour of quiet minutes it eases back one step at a time."),   // T
    }, 0, 4, "sinks sound output audio buffer speakers"},
    {"sig-record", FOX_TR_NOOP("Recorder"), {
        FOX_TR_NOOP("The Recorder section starts and stops recording. Its chip reads REC while a recording runs."),   // C: main.cpp chip (lists.recording)
        FOX_TR_NOOP("Recordings... in the App menu lists the recorder's folder, newest first; its Open key plays an I/Q take as the I/Q file."),   // C: lanec_publish.cpp recordings list
    }, 0, 6, "recorder record recording wav iq files"},
    {"sig-ppm", FOX_TR_NOOP("Frequency correction"), {
        FOX_TR_NOOP("Corrects the error of this radio's crystal, in parts per million. Positive when stations show up below their real frequency. Tune to a signal whose frequency you know exactly and change the value until it sits where it should. The switch is for every radio; the value is remembered for each radio."),   // T
    }, 0, 7, "frequency correction ppm crystal error"},

    // ------------------------------------------------------------------ DECODE
    {"dec-store", FOX_TR_NOOP("Plugin store"), {
        FOX_TR_NOOP("The Plugin store lists the plugins you can fit. Search the catalogue by name, open a plugin's page for its details, and use the < BROWSE key to come back to the list."),   // C: extend_bank.slint labels "Search the catalogue", "< BROWSE"; key_dispatch.hpp escapeStep
    }, 1, 0, "plugin store catalogue install fit site"},
    {"dec-fitted", FOX_TR_NOOP("Fitted modules"), {
        FOX_TR_NOOP("Fitted modules lists the plugins installed on this computer. Its chip counts them."),   // C: main.cpp chip (lists.fittedPlugins)
    }, 1, 1, "fitted modules plugins installed"},
    {"dec-decoders", FOX_TR_NOOP("Decoders"), {
        FOX_TR_NOOP("Decoders lists the decoder plugins. Its chip reads the number running, or IDLE when none is."),   // C: main.cpp chip (state.decodersRunning)
    }, 1, 2, "decoders decoder plugins running idle"},
    {"dec-output", FOX_TR_NOOP("Decoder output"), {
        FOX_TR_NOOP("Decoder output holds the lines the decoders print. Its chip counts them, and the Copy key puts every held line on the clipboard."),   // C: main.cpp chip (decoderLinesTotal), output.slint copy-text
    }, 1, 5, "decoder output lines copy clipboard text"},

    // ------------------------------------------------------------------ TOOLS
    {"tools-scope", FOX_TR_NOOP("Demod scope"), {
        FOX_TR_NOOP("Opens the bench oscilloscope: the demodulated audio on a\nten-by-eight graticule, its spectrum, and the channel I/Q as\ntwo traces and as a vector display. The time base, the\nattenuator and the input selector are on the page itself."),   // T
    }, 2, 0, "demod scope oscilloscope audio iq vector"},
    {"tools-bookmarks", FOX_TR_NOOP("Bookmarks"), {
        FOX_TR_NOOP("Bookmarks keeps your frequencies with a name, a group and a mode. Its chip counts them."),   // C: main.cpp chip (lists.bookmarks.size())
        FOX_TR_NOOP("Adds every entry of an SDR# frequencies.xml, or of a CSV with a\nfrequency column (and name, group, mode, bandwidth if it has them).\nImporting the same file again adds nothing twice."),   // T
        FOX_TR_NOOP("Marks every bookmark inside the spectrum's span, named where\nthere is room. Only the ones on screen are looked at, so a list\nof tens of thousands costs nothing."),   // T
    }, 2, 2, "bookmarks frequencies groups import csv xml sdr# marks"},
    {"tools-scanner", FOX_TR_NOOP("Scanner"), {
        FOX_TR_NOOP("Scans the rows ticked in the Bookmarks list, in frequency order,\ntuning each one's mode and bandwidth with it."),   // T
        FOX_TR_NOOP("Silences the receiver while the scan steps and lets you hear it when it stops on a signal."),   // T
        FOX_TR_NOOP("Adds a line to scan-log.csv in the settings folder for each signal the scan stops on: the time, the frequency and how long it was heard."),   // T
        FOX_TR_NOOP("Passes over the frequency the scan is on for ten minutes. In a scan of the ticked rows the row is unticked and ticked again when the time is up."),   // T
    }, 2, 3, "scanner scan lockout log squelch"},
    {"tools-airband", FOX_TR_NOOP("Airband"), {
        FOX_TR_NOOP("Airband listens to an airport's air traffic control. Type the airport's code, such as KORD, ORD, EGLL or LHR, and press Look up: its frequencies join the frequency list as a group of their own."),   // C: tools_bank.slint "Airport code", "Look up", placeholder "airport code - KORD, ORD, EGLL, LHR"
        FOX_TR_NOOP("Nearest airports lists the airports closest to the receiver's position, and LISTEN plays the ticked frequencies."),   // C: tools_bank.slint labels "Nearest airports", "LISTEN"; the receiver latitude and longitude are the radar fold's own fields
    }, 2, 4, "airband airport air traffic control atc listen icao"},
    {"tools-export", FOX_TR_NOOP("Export"), {
        FOX_TR_NOOP("The Export section writes the waterfall of the selected radio as a file: a picture, or the levels as CSV or FSW data. A file that exists is never overwritten."),   // C: export_logic.cpp (formats, "(2)" rule)
    }, 2, 5, "export waterfall picture csv fsw data save"},

    // ------------------------------------------------------------------ EXTEND
    {"ext-web", FOX_TR_NOOP("Web access"), {
        FOX_TR_NOOP("Web access serves the receiver's page to a browser. Its chip reads PORT and the number while it is listening, and OFF otherwise."),   // C: main.cpp chip (lists.webListening); extend_bank.slint "Serve a browser page"
        FOX_TR_NOOP("A password protects the page; the Show key shows what you typed, and Hide hides it again."),   // C: extend_bank.slint "Set password", "Show the password", "Hide the password"
    }, 3, 0, "web access browser page port password remote"},
    {"ext-cat", FOX_TR_NOOP("CAT"), {
        FOX_TR_NOOP("CAT lets logging and digital-mode programs read and set the frequency and the mode over the network. Its chip reads PORT and the number while it is listening, and OFF otherwise."),   // C: main.cpp chip (lists.catListening); extend_bank.slint "Accept CAT connections"
    }, 3, 1, "cat rigctl rigctld control port logging"},

    // ------------------------------------------------------------------ SYSTEM
    {"sys-display", FOX_TR_NOOP("Display"), {
        FOX_TR_NOOP("The colours of the waterfall and its strength key. The Blue-yellow safe map can be told apart by people who confuse red and green."),   // T
        FOX_TR_NOOP("How fast the waterfall scrolls. A slow line keeps the strongest level of every frame since the last line, so a short signal is not lost."),   // T
        FOX_TR_NOOP("Name the strongest signals on the spectrum with their frequency and level."),   // T
        FOX_TR_NOOP("Frame the spectrum and the waterfall on a band or on the tuned frequency."),   // T
        FOX_TR_NOOP("How the tuned-frequency counter and the typed frequency group their digits. Typing a frequency accepts either style."),   // T
        FOX_TR_NOOP("Black grounds, white lines and text, one yellow accent. It is also used whenever Windows' own high-contrast setting is on."),   // T
    }, 4, 0, "display colour map waterfall speed peak labels band zoom digit grouping high contrast"},
    {"sys-keys", FOX_TR_NOOP("Key bindings"), {
        FOX_TR_NOOP("Key bindings lists every action with its key and lets you change it; a change takes effect at once. Shift+F1 lists the keys in use."),   // C: key_dispatch.hpp (a rebinding takes effect the moment the capture saves it), "Press a key..." in system_bank.slint, kShortcutOverlay
    }, 4, 5, "key bindings keyboard shortcuts rebind"},
    {"sys-diag", FOX_TR_NOOP("Diagnostics"), {
        FOX_TR_NOOP("Copy diagnostics puts the diagnostics report on the clipboard, and Open reports folder opens the folder of the reports FoxSDR has written."),   // C: system_bank.slint "Copy diagnostics", "Open reports folder"
    }, 4, 6, "diagnostics report crash freeze copy log"},
    {"sys-usage", FOX_TR_NOOP("Usage reporting"), {
        FOX_TR_NOOP("The Send anonymous usage reports switch turns the usage reports on or off. Show what is sent shows what a report holds."),   // C: system_bank.slint "Send anonymous usage reports", "Show what is sent"
        FOX_TR_NOOP("When a radio does not open and Usage reporting is on, FoxSDR sends one report about that failure, every time it happens, whether or not Diagnostics is on. With Usage reporting off it sends nothing about your radios."),   // C: PRIVACY.md "Radio failure reports and the radio journal", first sentence; owner, 2026-11-24: off sends nothing (wording pending approval, PRIVACY-FOR-APPROVAL-0.99.115-OFF.md)
    }, 4, 7, "usage reporting privacy anonymous reports send"},
    {"controllers", FOX_TR_NOOP("Controllers"), {
        FOX_TR_NOOP("A ShuttleXpress or a PowerMate can tune the radio and press a few of the window's keys. Nothing is opened until you turn this on and choose the device."),   // C: knob_host.hpp, lanex_publish.cpp knobRows
        FOX_TR_NOOP("The jog tunes, the ring scans faster the further it is turned, and the five buttons in order are: smaller step, larger step, next mode, mute, record."),   // C: input/mapping.cpp shuttleXpressPreset
        FOX_TR_NOOP("Turning it tunes; pressing it mutes."),   // C: input/mapping.cpp powerMatePreset
    }, -1, -1, "controllers knob shuttlexpress powermate jog dial midi"},
    {"sys-bench", FOX_TR_NOOP("Speed test"), {
        FOX_TR_NOOP("The Performance section holds a speed test of the signal processing on this computer. It times the spectrum transform, the channel filter, every demodulator and the resamplers, one after another, and shows how many times faster than real time each one runs."),   // C: core/dsp_benchmark.hpp
        FOX_TR_NOOP("The test uses one processor core for about ten seconds, can be cancelled at any time, and changes no setting."),   // C: lanex_bench.cpp (one worker thread, an item at a time, no setting is written)
    }, 4, 11, "speed test benchmark performance dsp processor"},
};

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

}  // namespace

const std::vector<HelpTopic>& helpTopics() { return kTopics; }

int topicForSection(int bank, int row) {
    if (bank < 0 || row < 0) { return -1; }   // a topic tied to no section (bank -1) is never "the topic of a section"
    for (std::size_t i = 0; i < kTopics.size(); ++i) {
        if (kTopics[i].bank == bank && kTopics[i].row == row) { return static_cast<int>(i); }
    }
    return -1;
}

int topicIndex(const std::string& id) {
    for (std::size_t i = 0; i < kTopics.size(); ++i) {
        if (id == kTopics[i].id) { return static_cast<int>(i); }
    }
    return -1;
}

std::string sentence(const char* english) {
    std::string s = cascade::i18n::tr(english);
    for (char& c : s) { if (c == '\n') { c = ' '; } }
    return s;
}

std::string titleOf(const HelpTopic& t) { return cascade::i18n::tr(t.title); }

std::vector<int> searchTopics(const std::string& query) {
    std::vector<std::string> words;
    {
        std::string cur;
        for (const char c : query) {
            if (std::isspace(static_cast<unsigned char>(c))) { if (!cur.empty()) { words.push_back(lower(cur)); cur.clear(); } }
            else { cur += c; }
        }
        if (!cur.empty()) { words.push_back(lower(cur)); }
    }
    std::vector<int> all;
    for (std::size_t i = 0; i < kTopics.size(); ++i) { all.push_back(static_cast<int>(i)); }
    if (words.empty()) { return all; }
    std::vector<std::pair<int, int>> scored;   // (score, index): a smaller score is a better answer
    for (const int i : all) {
        const HelpTopic& t = kTopics[static_cast<std::size_t>(i)];
        const std::string title = lower(titleOf(t));
        std::string body;
        for (const char* b : t.body) { body += lower(sentence(b)); body += ' '; }
        const std::string kw = lower(t.keywords);
        int score = 0;
        bool every = true;
        for (const std::string& w : words) {
            if (title.find(w) != std::string::npos) { score += 0; }
            else if (kw.find(w) != std::string::npos) { score += 2; }
            else if (body.find(w) != std::string::npos) { score += 4; }
            else { every = false; break; }
        }
        if (every) { scored.emplace_back(score, i); }
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<int> out;
    for (const auto& s : scored) { out.push_back(s.second); }
    return out;
}

}  // namespace foxsdr::slintlanex
