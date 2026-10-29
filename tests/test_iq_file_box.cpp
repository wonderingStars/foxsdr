// test_iq_file_box.cpp - the pure parts of the I/Q-file row's 2026-10-29 fix
// (gui/iq_file_box.hpp, gui/paste_files.hpp's firstDroppedFile).
//
//   [A] typedIqPath: the box's text as the path Open tries - whitespace and one
//       pair of double quotes off, an inner quote kept, nothing from nothing.
//   [B] iqOpenReasonForLog: the open's refusal with the path replaced, so the
//       log never carries a file name (PRIVACY.md); a message without the path
//       passes unchanged; an empty path changes nothing; a path that is itself
//       the replacement cannot loop.
//   [C] isWavName: a dropped file's name decides play or import.
//   [D] firstDroppedFile: a DROPFILES block as Explorer writes one (UTF-16, a
//       double-NUL list) to its first path in UTF-8 - a non-ASCII name, a pair
//       of surrogates, a second file ignored; a narrow block; and every way a
//       block is not one (short, offset off the end, no terminator, nothing).
//   [E] the scripted clipboard is what the paste reads while it is set.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gui/iq_file_box.hpp"
#include "gui/paste_files.hpp"
#include "test_check.hpp"

using cascade::gui::firstDroppedFile;
using cascade::gui::iqOpenReasonForLog;
using cascade::gui::isWavName;
using cascade::gui::typedIqPath;

namespace {

// A DROPFILES block: the 20-byte header, then the list. `wide` writes the
// paths as UTF-16LE from the code points given; a narrow block writes bytes.
std::vector<unsigned char> dropBlock(const std::vector<std::vector<std::uint32_t>>& paths, bool wide,
                                     bool terminate = true, std::uint32_t offset = 20) {
    std::vector<unsigned char> b(20, 0);
    std::memcpy(b.data(), &offset, 4);
    const std::uint32_t fWide = wide ? 1u : 0u;
    std::memcpy(b.data() + 16, &fWide, 4);
    const auto put16 = [&b](std::uint32_t u) {
        b.push_back(static_cast<unsigned char>(u & 0xFF));
        b.push_back(static_cast<unsigned char>((u >> 8) & 0xFF));
    };
    for (const auto& p : paths) {
        for (std::uint32_t cp : p) {
            if (wide) {
                if (cp >= 0x10000) {
                    const std::uint32_t v = cp - 0x10000;
                    put16(0xD800 + (v >> 10));
                    put16(0xDC00 + (v & 0x3FF));
                } else {
                    put16(cp);
                }
            } else {
                b.push_back(static_cast<unsigned char>(cp));
            }
        }
        if (wide) { put16(0); } else { b.push_back(0); }
    }
    if (terminate) {
        if (wide) { put16(0); } else { b.push_back(0); }
    }
    return b;
}

std::vector<std::uint32_t> cps(const std::string& ascii) {
    return std::vector<std::uint32_t>(ascii.begin(), ascii.end());
}

}  // namespace

int main() {
    std::printf("test_iq_file_box\n");

    std::printf("[A] typedIqPath\n");
    CHECK(typedIqPath("C:\\rec\\a.wav") == "C:\\rec\\a.wav");
    CHECK(typedIqPath("\"C:\\rec\\a.wav\"") == "C:\\rec\\a.wav");             // Explorer's "Copy as path"
    CHECK(typedIqPath("  \"C:\\rec\\a b.wav\" \r\n") == "C:\\rec\\a b.wav");  // a pasted line's ending
    CHECK(typedIqPath("\" C:\\rec\\a.wav \"") == "C:\\rec\\a.wav");           // spaces inside the quotes
    CHECK(typedIqPath("C:\\rec\\say \"hi\".wav") == "C:\\rec\\say \"hi\".wav");  // an inner quote is the name's
    CHECK(typedIqPath("\"C:\\rec\\a.wav") == "\"C:\\rec\\a.wav");  // one quote: not a pair, left alone
    CHECK(typedIqPath("\"") == "\"");
    CHECK(typedIqPath("").empty());
    CHECK(typedIqPath("   ").empty());
    CHECK(typedIqPath("\"\"").empty());
    CHECK(typedIqPath("\" \"").empty());
    CHECK(typedIqPath("/home/u/rec.wav") == "/home/u/rec.wav");

    std::printf("[B] iqOpenReasonForLog\n");
    CHECK(iqOpenReasonForLog("cannot open file: C:\\Users\\me\\rec.wav", "C:\\Users\\me\\rec.wav") ==
          "cannot open file: (the file)");
    CHECK(iqOpenReasonForLog("not a RIFF/WAVE file: /home/u/x.wav", "/home/u/x.wav") ==
          "not a RIFF/WAVE file: (the file)");
    CHECK(iqOpenReasonForLog("missing fmt chunk", "/home/u/x.wav") == "missing fmt chunk");
    CHECK(iqOpenReasonForLog("cannot open file: ", "") == "cannot open file: ");
    CHECK(iqOpenReasonForLog("a a a", "a") == "(the file) (the file) (the file)");
    CHECK(iqOpenReasonForLog("x (the file) y", "(the file)") == "x (the file) y");  // cannot loop

    std::printf("[C] isWavName\n");
    CHECK(isWavName("C:\\rec\\a.wav"));
    CHECK(isWavName("C:\\rec\\A.WAV"));
    CHECK(isWavName("/home/u/take.Wav"));
    CHECK(!isWavName("C:\\rec\\frequencies.xml"));
    CHECK(!isWavName("C:\\rec\\list.csv"));
    CHECK(!isWavName("wav"));
    CHECK(!isWavName(".wav") == false);  // a bare ".wav" is still named as one; the open decides
    CHECK(!isWavName(""));
    CHECK(!isWavName("C:\\rec\\a.wave"));

    std::printf("[D] firstDroppedFile\n");
    {
        // What Explorer puts on the clipboard for one copied file: a wide list.
        const auto b = dropBlock({cps("C:\\rec\\take.wav")}, true);
        CHECK(firstDroppedFile(b.data(), b.size()) == "C:\\rec\\take.wav");
    }
    {
        // Two files: the first is the one pasted.
        const auto b = dropBlock({cps("C:\\rec\\one.wav"), cps("C:\\rec\\two.wav")}, true);
        CHECK(firstDroppedFile(b.data(), b.size()) == "C:\\rec\\one.wav");
    }
    {
        // A non-ASCII name - an umlaut (one UTF-16 unit) and an emoji (a
        // surrogate pair) - comes out as UTF-8.
        std::vector<std::uint32_t> p = cps("C:\\Aufnahmen\\");
        p.push_back(0x00FC);  // u with diaeresis
        p.push_back(0x1F4FB);  // radio
        const auto tail = cps(".wav");
        p.insert(p.end(), tail.begin(), tail.end());
        const auto b = dropBlock({p}, true);
        CHECK(firstDroppedFile(b.data(), b.size()) == "C:\\Aufnahmen\\\xC3\xBC\xF0\x9F\x93\xBB.wav");
    }
    {
        // A narrow (code-page) list, ASCII only, is read too.
        const auto b = dropBlock({cps("C:\\rec\\narrow.wav")}, false);
        CHECK(firstDroppedFile(b.data(), b.size()) == "C:\\rec\\narrow.wav");
    }
    {
        // The list may start further in than the header's 20 bytes.
        auto b = dropBlock({cps("C:\\x.wav")}, true, true, 24);
        b.insert(b.begin() + 20, 4, static_cast<unsigned char>(0xEE));  // padding the offset skips
        CHECK(firstDroppedFile(b.data(), b.size()) == "C:\\x.wav");
    }
    // Not a block: too short, nothing, an offset past the end, a list with no
    // terminator inside the block, an empty list.
    {
        const auto b = dropBlock({cps("C:\\x.wav")}, true);
        CHECK(firstDroppedFile(b.data(), 19).empty());
        CHECK(firstDroppedFile(nullptr, b.size()).empty());
        CHECK(firstDroppedFile(b.data(), 0).empty());
        // The first path's NUL cut off: the path runs to the end of the block.
        CHECK(firstDroppedFile(b.data(), 20 + 2 * 8).empty());
        // Half a unit: still no terminator seen.
        CHECK(firstDroppedFile(b.data(), 20 + 2 * 8 + 1).empty());
    }
    {
        const auto b = dropBlock({cps("C:\\x.wav")}, true, true, 4000);
        CHECK(firstDroppedFile(b.data(), b.size()).empty());
    }
    {
        const auto b = dropBlock({cps("C:\\x.wav")}, true, true, static_cast<std::uint32_t>(dropBlock({cps("C:\\x.wav")}, true).size()));
        CHECK(firstDroppedFile(b.data(), b.size()).empty());  // the offset IS the size
    }
    {
        const auto b = dropBlock({}, true);  // an empty list: one double NUL
        CHECK(firstDroppedFile(b.data(), b.size()).empty());
        const auto n = dropBlock({}, false);
        CHECK(firstDroppedFile(n.data(), n.size()).empty());
    }
    {
        const auto b = dropBlock({cps("C:\\x.wav")}, false, false);  // narrow, no terminator at all
        CHECK(firstDroppedFile(b.data(), b.size() - 1).empty());
    }

    std::printf("[E] the scripted clipboard\n");
    // While set, the paste reads it and asks nothing else - so this touches no
    // real clipboard. (Cleared again so nothing is left behind for a later read.)
    cascade::gui::setScriptedClipboard("C:\\rec\\scripted.wav");
    const char* t = cascade::gui::clipboardTextForPaste();
    CHECK(t != nullptr && std::string(t) == "C:\\rec\\scripted.wav");
    cascade::gui::setScriptedClipboard("");

    return testSummary("test_iq_file_box");
}
