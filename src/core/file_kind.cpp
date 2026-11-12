// file_kind.cpp - see file_kind.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/file_kind.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <set>
#include <string_view>
#include <system_error>

#include "core/freq_import.hpp"
#include "core/plugin_module_check.hpp"
#include "source/iq_file_source.hpp"

namespace cascade::core {

namespace fs = std::filesystem;

const char* fileKindWord(FileKind k) {
    switch (k) {
        case FileKind::Unknown: return "unknown";
        case FileKind::Unreadable: return "unreadable";
        case FileKind::Missing: return "missing";
        case FileKind::Folder: return "folder";
        case FileKind::IqRecording: return "iq-recording";
        case FileKind::Wav: return "wav";
        case FileKind::Rf64: return "rf64";
        case FileKind::SigMfPair: return "sigmf-pair";
        case FileKind::SigMfMeta: return "sigmf-meta";
        case FileKind::BookmarkCsv: return "bookmark-csv";
        case FileKind::BookmarkXml: return "bookmark-xml";
        case FileKind::PluginPackage: return "plugin-package";
    }
    return "unknown";
}

namespace {

std::uint32_t rd16(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8); }
std::uint32_t rd32(const std::uint8_t* p) { return rd16(p) | (rd16(p + 2) << 16); }

bool tagIs(const std::uint8_t* p, const char* t) { return std::memcmp(p, t, 4) == 0; }

bool endsWithNoCase(const std::string& s, std::string_view suffix) {
    if (s.size() < suffix.size()) { return false; }
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const char a = s[s.size() - suffix.size() + i];
        if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(suffix[i]))) { return false; }
    }
    return true;
}

std::string lowerKey(const std::string& s) {
    std::string k = s;
    for (char& c : k) {
        if (c == '/') { c = '\\'; }
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return k;
}

bool isPlainShortAscii(const std::string& s) {
    if (s.size() >= 240) { return false; }
    for (char c : s) {
        if (static_cast<unsigned char>(c) >= 0x80 || c == 0) { return false; }
    }
    return true;
}

// The first bytes of a file, and its size. false: cannot be opened.
bool readHead(const fs::path& p, std::vector<std::uint8_t>& head, std::uint64_t& size) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { return false; }
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (end < 0) { return false; }
    size = static_cast<std::uint64_t>(end);
    in.seekg(0, std::ios::beg);
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(size, kHeadBytes));
    head.assign(want, 0);
    if (want > 0) {
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(want));
        head.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    }
    return true;
}

bool readWhole(const fs::path& p, std::uint64_t size, std::vector<std::uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { return false; }
    out.assign(static_cast<std::size_t>(size), 0);
    if (size > 0) {
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
        if (static_cast<std::uint64_t>(in.gcount()) != size) { return false; }
    }
    return true;
}

bool hasNul(const std::uint8_t* d, std::size_t n) { return n > 0 && std::memchr(d, 0, n) != nullptr; }

bool isImageMagic(const std::vector<std::uint8_t>& h) {
    if (h.size() >= 2 && h[0] == 'M' && h[1] == 'Z') { return true; }
    if (h.size() >= 4 && h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') { return true; }
    if (h.size() >= 4) {
        const std::uint32_t m = rd32(h.data());
        if (m == 0xFEEDFACEu || m == 0xFEEDFACFu || m == 0xCEFAEDFEu || m == 0xCFFAEDFEu || m == 0xBEBAFECAu) { return true; }
    }
    return false;
}

FileClass make(FileKind k, const std::string& path, std::string detail) {
    FileClass c;
    c.kind = k;
    c.path = path;
    c.detail = std::move(detail);
    return c;
}

}  // namespace

WavHeader parseWavHeader(const std::uint8_t* d, std::size_t n) {
    WavHeader h;
    if (d == nullptr || n < 12) { return h; }
    if (tagIs(d, "RIFF")) { h.riff = true; }
    else if (tagIs(d, "RF64")) { h.rf64 = true; }
    else { return h; }
    if (!tagIs(d + 8, "WAVE")) {
        h.riff = h.rf64 = false;
        return h;
    }
    std::uint64_t pos = 12;
    for (std::size_t chunk = 0; chunk < kMaxChunks; ++chunk) {
        if (pos + 8 > n) { break; }
        const std::uint8_t* c = d + pos;
        const std::uint64_t size = rd32(c + 4);
        if (tagIs(c, "fmt ")) {
            if (size < 16) { h.problem = "the fmt chunk is too small"; return h; }
            if (pos + 8 + 16 > n) { break; }
            h.haveFmt = true;
            h.formatTag = rd16(c + 8);
            h.channels = rd16(c + 10);
            h.sampleRateHz = rd32(c + 12);
            h.bitsPerSample = rd16(c + 22);
        } else if (tagIs(c, "data")) {
            h.haveData = true;
            if (!h.haveFmt) { h.problem = "the data chunk comes before the fmt chunk"; }
            return h;
        }
        pos += 8 + size + (size & 1u);
    }
    return h;
}

bool looksLikeSigMfMeta(const std::uint8_t* d, std::size_t n) {
    if (d == nullptr || n == 0 || hasNul(d, n)) { return false; }
    std::size_t i = 0;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) { i = 3; }
    while (i < n && std::isspace(d[i])) { ++i; }
    if (i >= n || d[i] != '{') { return false; }
    const std::string_view text(reinterpret_cast<const char*>(d) + i, n - i);
    const std::size_t g = text.find("\"global\"");
    if (g == std::string_view::npos) { return false; }
    std::size_t colon = g + 8;
    while (colon < text.size() && std::isspace(static_cast<unsigned char>(text[colon]))) { ++colon; }
    if (colon >= text.size() || text[colon] != ':') { return false; }
    return text.find("\"core:datatype\"", colon) != std::string_view::npos;
}

fs::path fsPathFromUtf8(const std::string& utf8) {
    std::u8string u(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    fs::path p(u);
#if defined(_WIN32)
    const std::wstring w = p.native();
    const bool already = w.rfind(L"\\\\?\\", 0) == 0 || w.rfind(L"\\\\.\\", 0) == 0;
    const bool driveAbs = w.size() >= 3 && std::iswalpha(w[0]) && w[1] == L':' && (w[2] == L'\\' || w[2] == L'/');
    const bool unc = w.size() >= 3 && (w[0] == L'\\' || w[0] == L'/') && (w[1] == L'\\' || w[1] == L'/');
    if (!already && w.size() >= 248) {
        std::wstring n = w;
        std::replace(n.begin(), n.end(), L'/', L'\\');
        if (driveAbs) { return fs::path(L"\\\\?\\" + n); }
        if (unc) { return fs::path(L"\\\\?\\UNC\\" + n.substr(2)); }
    }
#endif
    return p;
}

FileClass classifyFile(const std::string& utf8Path) {
    try {
        if (utf8Path.empty()) { return make(FileKind::Missing, utf8Path, "no file was named"); }
        const fs::path p = fsPathFromUtf8(utf8Path);
        std::error_code ec;
        const fs::file_status st = fs::status(p, ec);
        if (ec || st.type() == fs::file_type::not_found) { return make(FileKind::Missing, utf8Path, "the file is gone"); }
        if (st.type() == fs::file_type::directory) { return make(FileKind::Folder, utf8Path, "a folder"); }
        if (st.type() != fs::file_type::regular) { return make(FileKind::Unknown, utf8Path, "not an ordinary file"); }

        std::vector<std::uint8_t> head;
        std::uint64_t size = 0;
        if (!readHead(p, head, size)) { return make(FileKind::Unreadable, utf8Path, "the file could not be opened"); }

        // A SigMF data file has no signature of its own: it is the data of the metadata document beside it.
        if (endsWithNoCase(utf8Path, ".sigmf-data")) {
            const std::string meta = utf8Path.substr(0, utf8Path.size() - 11) + ".sigmf-meta";
            std::vector<std::uint8_t> mh;
            std::uint64_t ms = 0;
            if (readHead(fsPathFromUtf8(meta), mh, ms) && looksLikeSigMfMeta(mh.data(), mh.size())) {
                FileClass c = make(FileKind::SigMfPair, meta, "a SigMF recording: metadata and data");
                c.dataPath = utf8Path;
                return c;
            }
            return make(FileKind::Unknown, utf8Path, "a SigMF data file with no metadata file beside it");
        }

        const WavHeader w = parseWavHeader(head.data(), head.size());
        if (w.rf64) {
            FileClass c = make(FileKind::Rf64, utf8Path, "an RF64 file (larger than 4 GiB); the I/Q player does not read RF64");
            c.channels = w.channels;
            c.sampleRateHz = w.sampleRateHz;
            c.bitsPerSample = w.bitsPerSample;
            c.formatTag = w.formatTag;
            return c;
        }
        if (w.riff) {
            FileClass c = make(FileKind::Wav, utf8Path, "a sound file");
            c.channels = w.channels;
            c.sampleRateHz = w.sampleRateHz;
            c.bitsPerSample = w.bitsPerSample;
            c.formatTag = w.formatTag;
            if (!w.problem.empty()) {
                c.detail = "a sound file with a damaged header: " + w.problem;
                return c;
            }
            if (!w.haveFmt) {
                c.detail = "a sound file whose fmt chunk was not found";
                return c;
            }
            const bool iqFormat = w.channels == 2 && ((w.formatTag == 1 && w.bitsPerSample == 16) || (w.formatTag == 3 && w.bitsPerSample == 32));
            if (!iqFormat) {
                c.detail = w.channels == 2 ? "a stereo sound file the I/Q player cannot read (it plays 16-bit and float32)"
                                           : "a sound file with " + std::to_string(w.channels) + " channel(s), not an I/Q recording (two channels)";
                return c;
            }
            if (!w.haveData) {
                c.detail = "a two-channel sound file whose data chunk was not seen in its first 64 KiB";
                if (!isPlainShortAscii(utf8Path)) { return c; }
            }
            if (isPlainShortAscii(utf8Path)) {
                source::IqFileSource reader;
                if (!reader.open(utf8Path)) {
                    c.detail = std::string("a two-channel sound file the I/Q player refuses: ") + reader.lastError();
                    // Its message may name the path; the rule is that the detail never does.
                    for (std::size_t at = c.detail.find(utf8Path); at != std::string::npos; at = c.detail.find(utf8Path, at)) {
                        c.detail.replace(at, utf8Path.size(), "(the file)");
                    }
                    return c;
                }
            }
            c.kind = FileKind::IqRecording;
            c.detail = "an I/Q recording";
            return c;
        }

        if (isImageMagic(head)) {
            if (size > kMaxModuleBytes) { return make(FileKind::Unknown, utf8Path, "a program file larger than any plugin"); }
            std::vector<std::uint8_t> bytes;
            if (!readWhole(p, size, bytes)) { return make(FileKind::Unreadable, utf8Path, "the file could not be read"); }
            const ModuleCheck mc = checkModuleBytes(bytes.data(), bytes.size());
            if (mc.ok()) { return make(FileKind::PluginPackage, utf8Path, "a plugin module"); }
            return make(FileKind::Unknown, utf8Path, "a program or library that is not a plugin for this machine: " + mc.detail);
        }

        if (looksLikeSigMfMeta(head.data(), head.size())) {
            const std::string base = endsWithNoCase(utf8Path, ".sigmf-meta") ? utf8Path.substr(0, utf8Path.size() - 11) : std::string();
            if (!base.empty()) {
                std::error_code ec2;
                if (fs::is_regular_file(fsPathFromUtf8(base + ".sigmf-data"), ec2)) {
                    FileClass c = make(FileKind::SigMfPair, utf8Path, "a SigMF recording: metadata and data");
                    c.dataPath = base + ".sigmf-data";
                    return c;
                }
            }
            return make(FileKind::SigMfMeta, utf8Path, "a SigMF metadata file with no .sigmf-data file beside it");
        }

        if (!head.empty() && !hasNul(head.data(), head.size())) {
            if (size > kMaxListBytes) { return make(FileKind::Unknown, utf8Path, "a text file too large to be a frequency list"); }
            std::vector<std::uint8_t> bytes;
            if (!readWhole(p, size, bytes)) { return make(FileKind::Unreadable, utf8Path, "the file could not be read"); }
            const ImportResult r = importFrequencyList(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            if (!r.items.empty()) {
                // wire-b begin: the description names the format the importer recognised (SDRuno bank, gqrx, CHIRP ...), not always "SDR# XML"
                const std::string what = r.format == "CSV" ? std::string("a frequency list (CSV)")
                                       : (r.format == "SDR# XML" || r.format.empty()) ? std::string("a frequency list (SDR# XML)")
                                                                                       : "a frequency list (" + r.format + ")";
                // wire-b end
                FileClass c = make(r.format == "CSV" ? FileKind::BookmarkCsv : FileKind::BookmarkXml, utf8Path, what);
                c.entries = r.items.size();
                return c;
            }
        }
        return make(FileKind::Unknown, utf8Path, "not a kind of file this program opens");
    } catch (...) {
        return make(FileKind::Unreadable, utf8Path, "the file could not be examined");
    }
}

std::vector<FileClass> classifyDrop(const std::vector<std::string>& paths, std::size_t maxFiles, std::size_t* dropped) {
    std::vector<FileClass> out;
    std::set<std::string> pairs;  // meta paths already emitted as a pair
    const std::size_t take = std::min(paths.size(), maxFiles);
    for (std::size_t i = 0; i < take; ++i) {
        FileClass c = classifyFile(paths[i]);
        if (c.kind == FileKind::SigMfPair) {
            if (!pairs.insert(lowerKey(c.path)).second) { continue; }
        }
        out.push_back(std::move(c));
    }
    if (dropped != nullptr) { *dropped = paths.size() - take; }
    return out;
}

}  // namespace cascade::core
