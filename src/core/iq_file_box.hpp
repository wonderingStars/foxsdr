// core/iq_file_box.hpp - what the Source section's I/Q-file row does with the text
// in its box, and what it may say about it in the log. No ImGui, no disk.
//
// WHY THIS EXISTS (a tester's report, FoxSDR 0.99.101 on Windows 10,
// 2026-10-29): "Can the IQ-file source be fixed: 1. the Open does nothing
// 2. Not possible to paste in a WAV-file in the IQ-file field." His log had
// no I/Q-file line at all, which was itself the clue. The row was a text box
// and an Open key, and Open only ever tried the text in the box: with the box
// empty it handed "" to the worker, whose refusal became a red "cannot open
// file: " with nothing after the colon, several controls further down, and no
// log line. There was no way to CHOOSE a file. The paste half: a WAV file
// copied in Explorer sits on the clipboard as a list of files, not as text, and
// the window library's clipboard read knows only text, so Ctrl+V did nothing;
// and Explorer's "Copy as path" puts the path in double quotes, which the open
// then faithfully refused as part of the name (gui/paste_files.hpp has the
// clipboard half).
//
// What is here is the part of the fix that is pure and worth pinning on its
// own: the text in the box made into a path to open, the open's refusal made
// into a log line that names no file, and the question a dropped file raises.
//
// (Moved to core/ when the engine split put the open on the engine: the refusal
// is logged by the engine, and the engine includes nothing of the interface. gui/iq_file_box.hpp
// re-exports these names in cascade::gui.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_IQ_FILE_BOX_HPP
#define CASCADE_CORE_IQ_FILE_BOX_HPP

#include <cctype>
#include <string>

namespace cascade::core {

// The path Open tries for what the box holds: the text with its surrounding
// whitespace (a pasted line often ends in a line break, which a single-line box
// shows as a space) and ONE pair of surrounding double quotes removed - what
// Explorer's "Copy as path" puts there, and what a shell would take off. A quote
// inside the name stays: it is the name's. Empty for a box that holds nothing
// worth opening (nothing, spaces, or a pair of quotes around nothing).
inline std::string typedIqPath(const std::string& typed) {
    const auto trim = [](std::string s) {
        std::size_t b = 0;
        while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) { ++b; }
        std::size_t e = s.size();
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) { --e; }
        return s.substr(b, e - b);
    };
    std::string t = trim(typed);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') { t = trim(t.substr(1, t.size() - 2)); }
    return t;
}

// The open's refusal as the log may carry it: `error` with every occurrence of
// `path` replaced by "(the file)". IqFileSource's messages name the path in two
// of their forms ("cannot open file: <path>", "not a RIFF/WAVE file: <path>") and
// PRIVACY.md promises the log "never the name or path of a file you opened"; the
// full sentence stays on screen, where the person already knows what they chose.
// The same rule the bookmark import's log line follows (app_window_disk_work.cpp).
inline std::string iqOpenReasonForLog(const std::string& error, const std::string& path) {
    std::string why = error;
    if (path.empty()) { return why; }
    for (std::size_t at = why.find(path); at != std::string::npos; at = why.find(path, at)) {
        why.replace(at, path.size(), "(the file)");
        at += 10;  // past "(the file)", so a path that is itself "(the file)" cannot loop
    }
    return why;
}

// Whether a file dropped on the window is a recording to PLAY rather than a
// frequency list to import: its name ends in ".wav", in any case. The I/Q file
// source reads RIFF/WAVE and nothing else (source/iq_file_source.hpp), and no
// frequency list is a WAV, so the extension alone decides. Name arithmetic
// only: the disk is not asked.
inline bool isWavName(const std::string& path) {
    static constexpr char kWav[] = ".wav";
    if (path.size() < 4) { return false; }
    for (std::size_t i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(path[path.size() - 4 + i]);
        if (std::tolower(c) != static_cast<unsigned char>(kWav[i])) { return false; }
    }
    return true;
}

}  // namespace cascade::core

#endif  // CASCADE_CORE_IQ_FILE_BOX_HPP
