// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "cjk_fonts.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

#include "core/i18n.hpp"
#include "core/scripts.hpp"
#include "gui/font_probe.hpp"

namespace foxsdr::slintfonts {
namespace {

namespace scripts = cascade::core::scripts;
namespace fontprobe = cascade::gui::fontprobe;

using Bytes = std::shared_ptr<std::vector<unsigned char>>;

// One bulk read of a font file, as gui/fonts.cpp readFileUncached does (a .ttc is 14-21 MB: no per-byte stream walk).
Bytes readFile(const std::string& path) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec)) { return nullptr; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { return nullptr; }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0) { return nullptr; }
    f.seekg(0, std::ios::beg);
    auto bytes = std::make_shared<std::vector<unsigned char>>(static_cast<std::size_t>(size));
    f.read(reinterpret_cast<char*>(bytes->data()), size);
    if (f.gcount() != size) { return nullptr; }
    return bytes;
}

std::string windowsFontPath(const std::string& fontDir, const std::string& file) {
    std::string root = fontDir;
    if (root.empty()) {
        const char* win = std::getenv("WINDIR");
        root = (win != nullptr && win[0] != '\0') ? std::string(win) + "\\Fonts" : std::string("C:\\Windows\\Fonts");
    }
    return root + "\\" + file;
}

}  // namespace

std::vector<Face> chooseFacesFor(std::string_view languageCode, const std::vector<unsigned int>& codePoints, bool windows, const std::string& fontDir) {
    std::vector<Face> out;
    std::vector<unsigned int> need;
    for (const unsigned int cp : codePoints) {
        if (scripts::isCjk(cp)) { need.push_back(cp); }
    }
    const scripts::CjkFace standard = scripts::cjkFaceFor(languageCode, need);
    if (standard == scripts::CjkFace::None) { return out; }
    const std::vector<unsigned int> probe = scripts::probeCodePoints(standard);
    // The classic's rule (cjkFacesFor): the first usable face is always taken, a later one - at most three in all - only while something the catalogue uses is still missing
    // and only for what it adds.
    std::vector<unsigned int> left = need;
    std::size_t taken = 0;
    for (const scripts::SystemFace& c : scripts::systemCandidates(standard, windows)) {
        if (taken >= 3) { break; }
        const std::string path = windows ? windowsFontPath(fontDir, c.path) : c.path;
        const Bytes b = readFile(path);
        if (b == nullptr) { continue; }
        const int index = fontprobe::pickFace(b->data(), b->size(), c.nameHint, probe);
        if (index < 0) { continue; }
        const fontprobe::Face face{b->data(), b->size(), index};
        const std::size_t before = left.size();
        left.erase(std::remove_if(left.begin(), left.end(), [&](unsigned int cp) { return fontprobe::has(face, cp); }), left.end());
        if (taken == 0 || left.size() < before) {
            out.push_back(Face{path, index, std::filesystem::path(path).filename().string() + "#" + std::to_string(index), fontprobe::names(face)});
            ++taken;
        }
        if (left.empty()) { break; }
    }
    return out;
}

std::vector<Face> chooseFaces(std::string_view languageCode, bool windows, const std::string& fontDir) {
    return chooseFacesFor(languageCode, cascade::i18n::codePoints(std::string(languageCode)), windows, fontDir);
}

std::string familyOf(const Face& face) {
    if (face.names.empty()) { return std::string(); }
    return face.names.size() >= 3 ? face.names.back() : face.names.front();
}

bool languageDrawable(const std::string& languageCode) {
    static std::mutex m;
    static std::map<std::string, bool> cache;
    const std::lock_guard<std::mutex> lock(m);
    if (const auto it = cache.find(languageCode); it != cache.end()) { return it->second; }
    const std::vector<unsigned int> cps = cascade::i18n::codePoints(languageCode);
    bool ok = true;
    if (scripts::cjkFaceFor(languageCode, cps) != scripts::CjkFace::None) { ok = !chooseFacesFor(languageCode, cps).empty(); }
    cache.emplace(languageCode, ok);
    return ok;
}

std::string languageUnavailableReason(const std::string& languageCode) {
    if (languageDrawable(languageCode)) { return std::string(); }
    const scripts::CjkFace standard = scripts::cjkFaceFor(languageCode, cascade::i18n::codePoints(languageCode));
#ifdef _WIN32
    const char* reason = scripts::missingFontReason(standard, true);
#else
    const char* reason = scripts::missingFontReason(standard, false);
#endif
    return reason != nullptr ? std::string(reason) : std::string();
}

bool faceHas(const Face& face, unsigned int codePoint) {
    const Bytes b = readFile(face.path);
    if (b == nullptr) { return false; }
    return fontprobe::has(fontprobe::Face{b->data(), b->size(), face.index}, codePoint);
}

bool bytesHave(const unsigned char* data, std::size_t len, int index, unsigned int codePoint) {
    return fontprobe::has(fontprobe::Face{data, len, index}, codePoint);
}

}  // namespace foxsdr::slintfonts
