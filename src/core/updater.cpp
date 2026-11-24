// Implementation of core/updater.hpp. See that header for what this
// deliberately does not do.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/updater.hpp"
#include "core/sys_hash.hpp"
#include "core/sys_manifest_sig.hpp"   // wire-c

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/package_identity.hpp"
#include "core/plugin_repo.hpp"
#include "core/plugin_repo_data.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace cascade::core {

namespace {

// A manifest is a few kilobytes of text. The cap is generous for that and
// still small enough that a hostile server cannot make us allocate.
constexpr std::uint64_t kMaxManifestBytes = 256 * 1024;

// An installer is a few megabytes. Well above what we ship, far below anything
// that would fill a disk.
constexpr std::uint64_t kMaxInstallerBytes = 256ull * 1024ull * 1024ull;

// The only host an update may come from. The manifest supplies a URL, and a
// manifest is exactly the thing an attacker who could impersonate the endpoint
// would control - so the URL is checked against this rather than trusted. It
// is not sufficient on its own (the sha256 is what finally decides), but it
// keeps a redirect to somewhere unrelated from ever being attempted.
constexpr char kAllowedHost[] = "foxsdr.com";

// A string (or boolean) field of a JSON object, or empty (false) when it is
// absent OR HAS ANOTHER TYPE. Not nlohmann's value(key, default): that returns
// the default only for an absent key and THROWS (type_error 302) for a key
// that is present with a number, null or object in it - and this runs on the
// update check's worker thread, whose result the GUI thread collects with
// future::get(), which rethrows. A service answering {"version": 5} must be a
// refusal, not the end of the process.
std::string stringField(const json& j, const char* key) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

bool boolField(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

// `s` cut to at most `maxBytes` WITHOUT splitting a UTF-8 sequence: backing off over
// continuation bytes (10xxxxxx) lands on the lead byte of the character the cut would have
// broken, and that whole character goes - the same rule as user_presets.cpp's cleanLabel.
// A team message is drawn word-wrapped, and a half letter at the end of a line is garbage.
std::string clipUtf8(std::string s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) { return s; }
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0u) == 0x80u) { --cut; }
    s.resize(cut);
    return s;
}

// wire-c begin (wiring lane C row S6; core/sys_manifest_sig.hpp): THE SIGNATURE ON THE UPDATE MANIFEST. When the program carries an owner key (kOwnerKeyHex in
// sys_manifest_sig.cpp, EMPTY until the owner supplies one) the body must be a signed envelope whose payload is the manifest, and the manifest that is parsed is
// the payload; anything else - unsigned, signed by another key, altered, a format this build does not know - is refused with one sentence and counted as a failed
// check. With NO key the body is parsed exactly as before (signatures are not enforced yet: a decision the owner has not made), so today's behaviour is unchanged.
std::mutex g_manifestKeysMutex;
bool g_manifestKeysOverridden = false;
std::vector<cascade::core::sys::Ed25519PublicKey> g_manifestKeysForTest;

bool unwrapSignedManifest(std::string& body, std::string& error) {
    std::vector<cascade::core::sys::Ed25519PublicKey> keys;
    {
        std::lock_guard<std::mutex> lk(g_manifestKeysMutex);
        keys = g_manifestKeysOverridden ? g_manifestKeysForTest : cascade::core::sys::ownerTrustedKeys();
    }
    if (keys.empty()) { return true; }
    std::string payload;
    const cascade::core::sys::SigResult r = cascade::core::sys::verifyEnvelope(body, keys, payload);
    if (r != cascade::core::sys::SigResult::Ok) {
        diagWarnf("update: the manifest signature check refused the answer (%s)", cascade::core::sys::sigResultName(r));
        error = "the update information is not signed by FoxSDR";
        return false;
    }
    body = std::move(payload);
    return true;
}
// wire-c end

// The canned answer's file (FOXSDR_FAKE_UPDATE_MANIFEST), at most a manifest's worth of it.
// False when it is not there or cannot be opened.
bool readCannedManifest(std::string& body) {
    const char* const path = std::getenv("FOXSDR_FAKE_UPDATE_MANIFEST");
    if (path == nullptr || *path == '\0') { return false; }
    std::ifstream in(path, std::ios::binary);
    if (!in) { return false; }
    body.assign(static_cast<std::size_t>(kMaxManifestBytes), '\0');
    in.read(&body[0], static_cast<std::streamsize>(kMaxManifestBytes));
    body.resize(static_cast<std::size_t>(in.gcount()));
    return true;
}

// 64 lower- or upper-case hex digits and nothing else. Written here rather
// than borrowed because PluginRepo's copy is file-local to that translation
// unit; the rule is four lines and duplicating it is better than widening that
// class's surface for one predicate.
bool wellFormedSha256(const std::string& s) {
    if (s.size() != 64) { return false; }
    for (char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) { return false; }
    }
    return true;
}

bool urlIsOnAllowedHost(const std::string& url) {
    // THE HOST IS CUT WHERE A URL'S AUTHORITY ENDS - at '/', '?' or '#' - by the plugin
    // store's own host function, which also refuses user-info, a backslash, a space or a
    // control character outright and keeps a non-default port in the answer (so it never
    // equals the bare host below). This used to cut at '/' alone, so
    // "https://other.example?.foxsdr.com/x" looked like a foxsdr.com subdomain here
    // and like other.example to the HTTP client (security review R1, finding F3).
    const std::string host = pluginSiteHost(url);
    if (host.empty()) { return false; }
    // Exactly the host, or a subdomain of it. "evilfoxsdr.com" must not pass,
    // which a plain suffix test would allow.
    if (host == kAllowedHost) { return true; }
    const std::string dotted = std::string(".") + kAllowedHost;
    return host.size() > dotted.size() &&
           host.compare(host.size() - dotted.size(), dotted.size(), dotted) == 0;
}

std::vector<int> numericParts(const std::string& v, std::string& pre) {
    pre.clear();
    std::string num = v;
    const std::size_t cut = num.find_first_of("-+");
    if (cut != std::string::npos) {
        pre = num.substr(cut + 1);
        num = num.substr(0, cut);
    }
    std::vector<int> out;
    std::size_t i = 0;
    while (i <= num.size()) {
        const std::size_t dot = num.find('.', i);
        const std::string part =
            num.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
        if (part.empty()) { break; }
        int value = 0;
        bool ok = true;
        for (char c : part) {
            if (c < '0' || c > '9') { ok = false; break; }
            value = value * 10 + (c - '0');
            if (value > 1000000) { ok = false; break; }
        }
        if (!ok) { break; }  // junk: stop rather than invent an ordering
        out.push_back(value);
        if (dot == std::string::npos) { break; }
        i = dot + 1;
    }
    return out;
}

}  // namespace

bool wellFormedVersion(const std::string& v) {
    // A STRUCTURAL PARSE, not a charset filter. A filter that only banned odd
    // characters would still accept "..", and ".." is the whole attack: this
    // string names the downloaded installer and that file is then executed.
    if (v.empty() || v.size() > 64) { return false; }

    // Dotted numeric core: at least one part, each 1 to 9 digits and nothing
    // else. An empty part is what "1..2" and a leading dot look like.
    std::size_t i = 0;
    int parts = 0;
    while (true) {
        std::size_t digits = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') { ++i; ++digits; }
        if (digits == 0 || digits > 9) { return false; }
        if (++parts > 4) { return false; }
        if (i < v.size() && v[i] == '.') { ++i; continue; }
        break;
    }
    if (i == v.size()) { return true; }  // a plain release: 0.58.0

    // Optional pre-release suffix: one hyphen, then dot-separated segments of
    // letters and digits - 0.57.0-nightly.20260819.b97092e. No segment may be
    // empty, which is what keeps ".." out, and nothing outside [0-9A-Za-z.]
    // gets in at all, which is what keeps separators, colons and spaces out.
    if (v[i] != '-') { return false; }
    ++i;
    std::size_t segment = 0;
    for (; i < v.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(v[i]);
        if (c == '.') {
            if (segment == 0) { return false; }
            segment = 0;
            continue;
        }
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z');
        if (!alnum) { return false; }
        ++segment;
    }
    return segment != 0;
}

bool wellFormedMessageId(const std::string& id) {
    // A charset is the right test here (unlike a version there is no structure to get
    // wrong): the id is never a path, only a key into a list, so what matters is that
    // nothing outside [A-Za-z0-9_-] gets in and that it is short.
    if (id.empty() || id.size() > kMaxTeamMessageIdBytes) { return false; }
    for (const char ch : id) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
        if (!ok) { return false; }
    }
    return true;
}

bool updateManifestFakeSet() {
    // A PRODUCT run ignores the seam (security review R1, finding F4): engine/process_boot.cpp calls lockFakePackageSeams() for a run with no --frames and no --seconds.
    if (!fakePackageSeamsOpen()) { return false; }
    const char* const v = std::getenv("FOXSDR_FAKE_UPDATE_MANIFEST");
    return v != nullptr && *v != '\0';
}

int compareVersions(const std::string& a, const std::string& b) {
    std::string aPre, bPre;
    const std::vector<int> an = numericParts(a, aPre);
    const std::vector<int> bn = numericParts(b, bPre);

    for (std::size_t i = 0; i < an.size() || i < bn.size(); ++i) {
        const int x = (i < an.size()) ? an[i] : 0;  // missing parts are zero
        const int y = (i < bn.size()) ? bn[i] : 0;
        if (x != y) { return x < y ? -1 : 1; }
    }
    if (aPre.empty() && bPre.empty()) { return 0; }
    // A release beats a pre-release of the same number.
    if (aPre.empty()) { return 1; }
    if (bPre.empty()) { return -1; }
    if (aPre < bPre) { return -1; }
    if (aPre > bPre) { return 1; }
    return 0;
}

bool parseUpdateManifest(const std::string& text, const std::string& currentVersion,
                         UpdateInfo& out, std::string& error) {
    out = UpdateInfo{};
    error.clear();

    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "the update service returned something that is not JSON";
        return false;
    }

    out.version = stringField(j, "version");
    if (out.version.empty()) {
        error = "the update service named no version";
        return false;
    }
    out.url = stringField(j, "url");
    out.sha256 = stringField(j, "sha256");

    // THE THREE FIELDS THAT MAKE A DOWNLOAD SAFE. Each is refused rather than
    // warned about, because a manifest missing any of them cannot be acted on
    // and pretending otherwise is how an updater becomes an exploit.
    if (!wellFormedVersion(out.version)) {
        error = "refusing an update version that does not parse: \"" + out.version + "\"";
        return false;
    }
    if (!wellFormedSha256(out.sha256)) {
        error = "the update service gave no usable checksum for " + out.version;
        return false;
    }
    if (!urlIsOnAllowedHost(out.url)) {
        error = "refusing an update URL that is not on " + std::string(kAllowedHost) + ": \"" +
                out.url + "\"";
        return false;
    }

    if (j.contains("size") && j["size"].is_number_unsigned()) {
        out.sizeBytes = j["size"].get<std::uint64_t>();
    }

    // Recomputed here rather than believed: the server says "newer", but this
    // build knows its own version, and a server that got it wrong (or a stale
    // cache) must not be able to talk anyone into a downgrade.
    out.newer = compareVersions(out.version, currentVersion) > 0;

    // THE KILL SWITCH (field-early-warning): the versions the service has withdrawn. Kept one
    // entry at a time (only a well-formed version, so nothing odd is stored or compared), at
    // most kMaxWithdrawn, and never a reason to refuse the manifest. A version on the list is
    // not offered however the service words the rest, and the version running being on it - or
    // the service saying so, `current_withdrawn` - is what makes the window recommend an update.
    if (j.contains("withdrawn") && j["withdrawn"].is_array()) {
        for (const json& w : j["withdrawn"]) {
            if (out.withdrawn.size() >= kMaxWithdrawn) { break; }
            if (!w.is_string()) { continue; }
            const std::string v = w.get<std::string>();
            if (wellFormedVersion(v)) { out.withdrawn.push_back(v); }
        }
    }
    auto isWithdrawn = [&out](const std::string& v) {
        return std::find(out.withdrawn.begin(), out.withdrawn.end(), v) != out.withdrawn.end();
    };
    out.offerWithdrawn = isWithdrawn(out.version);
    out.runningWithdrawn = isWithdrawn(currentVersion) || boolField(j, "current_withdrawn");
    if (out.offerWithdrawn) { out.newer = false; }

    if (j.contains("notes") && j["notes"].is_array()) {
        for (const json& n : j["notes"]) {
            if (!n.is_object()) { continue; }
            ReleaseNote note;
            note.version = stringField(n, "version");
            note.date = stringField(n, "date");
            note.critical = boolField(n, "critical");
            if (n.contains("notes") && n["notes"].is_array()) {
                for (const json& line : n["notes"]) {
                    if (line.is_string()) { note.notes.push_back(line.get<std::string>()); }
                }
            }
            if (!note.version.empty() && !note.notes.empty()) {
                out.notes.push_back(std::move(note));
            }
        }
    }

    // MESSAGES FROM THE TEAM (0.99.79), present whether or not `newer`: that is what they
    // are for. Each is kept or dropped ON ITS OWN, the policy malformed notes already
    // follow, so one broken entry costs only itself and never the manifest or the messages
    // beside it. What is kept is bounded - at most kMaxTeamMessages, each with at most
    // kMaxTeamMessageLines lines clipped to kMaxTeamMessageLineBytes - because the answer is
    // data from a service over the network and the window draws all of it. A message with
    // an id that fails wellFormedMessageId is dropped, because that id is written into the
    // settings file; one with no line of text is dropped, because there is nothing to say.
    // A blank line is skipped rather than drawn as an empty bullet. The first ten USABLE
    // messages are kept, so a broken entry does not take a place.
    if (j.contains("messages") && j["messages"].is_array()) {
        for (const json& m : j["messages"]) {
            if (out.messages.size() >= kMaxTeamMessages) { break; }
            if (!m.is_object()) { continue; }
            TeamMessage msg;
            msg.id = stringField(m, "id");
            if (!wellFormedMessageId(msg.id)) { continue; }
            msg.date = clipUtf8(stringField(m, "date"), kMaxTeamMessageDateBytes);
            msg.important = boolField(m, "important");
            if (m.contains("lines") && m["lines"].is_array()) {
                for (const json& line : m["lines"]) {
                    if (msg.lines.size() >= kMaxTeamMessageLines) { break; }
                    if (!line.is_string()) { continue; }
                    std::string text =
                        clipUtf8(line.get<std::string>(), kMaxTeamMessageLineBytes);
                    if (!text.empty()) { msg.lines.push_back(std::move(text)); }
                }
            }
            if (!msg.lines.empty()) { out.messages.push_back(std::move(msg)); }
        }
    }

    // MESSAGES FOR ONE RADIO (0.99.110, core/radio_message.hpp): kept or dropped ON ITS OWN like the team messages. An id that fails wellFormedMessageId, a salt
    // or hash that is not lower-case hex of the right length, or text with nothing printable left is dropped; the text is sanitised and clamped HERE, on receipt;
    // the first kMaxRadioMessages usable entries are kept. Nothing about a radio is read from the entry: it is only a hash this copy tries its own radios against.
    if (j.contains("radioMessages") && j["radioMessages"].is_array()) {
        auto lowerHex = [](const std::string& s, std::size_t lo, std::size_t hi) {
            if (s.size() < lo || s.size() > hi) { return false; }
            for (const char c : s) {
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; }
            }
            return true;
        };
        for (const json& m : j["radioMessages"]) {
            if (out.radioMessages.size() >= cascade::core::kMaxRadioMessages) { break; }
            if (!m.is_object()) { continue; }
            cascade::core::RadioMessage rm;
            rm.id = stringField(m, "id");
            if (!wellFormedMessageId(rm.id)) { continue; }
            rm.salt = stringField(m, "salt");
            rm.hash = stringField(m, "hash");
            if (!lowerHex(rm.salt, cascade::core::kMinRadioMessageSaltChars, cascade::core::kMaxRadioMessageSaltChars) || !lowerHex(rm.hash, 64, 64)) { continue; }
            rm.date = clipUtf8(stringField(m, "date"), kMaxTeamMessageDateBytes);
            rm.text = cascade::core::sanitiseRadioMessageText(stringField(m, "text"));
            if (rm.text.empty()) { continue; }
            out.radioMessages.push_back(std::move(rm));
        }
    }

    // Also recomputed, for the same reason.
    out.critical = false;
    if (out.newer) {
        for (const ReleaseNote& n : out.notes) {
            if (n.critical) { out.critical = true; }
        }
    }
    return true;
}

std::string updateEndpoint() {
    // The override is for bounded runs and tests only: a PRODUCT run ignores it (security review R1, finding F4; see updateManifestFakeSet).
    if (fakePackageSeamsOpen()) {
#if defined(_WIN32)
        char buf[512] = {0};
        const DWORD n = ::GetEnvironmentVariableA("FOXSDR_UPDATE_URL", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) { return std::string(buf, n); }
#else
        const char* v = std::getenv("FOXSDR_UPDATE_URL");
        if (v != nullptr && *v != '\0') { return std::string(v); }
#endif
    }
    return "https://foxsdr.com/api/update";
}

std::string updateChannelParam(const std::string& setting) {
    return setting == "beta" ? std::string("beta") : std::string();
}

bool checkForUpdate(const std::string& baseUrl, const std::string& currentVersion,
                    const std::string& channel, UpdateInfo& out, std::string& error) {
    out = UpdateInfo{};
    error.clear();
    // THE TEST SEAM (updater.hpp, FOXSDR_FAKE_UPDATE_MANIFEST): the answer is a file, no URL
    // is built and fetchText is not called. The log line says so and carries neither the path
    // nor anything the file said. Counted as a failed check, like the real one, when it fails.
    if (updateManifestFakeSet()) {
        diagLogf("update: FOXSDR_FAKE_UPDATE_MANIFEST set - canned answer, foxsdr.com was not "
                 "asked");
        std::string canned;
        if (!readCannedManifest(canned)) {
            error = "the canned update manifest could not be read";
            health::noteUpdateCheckFailed();
            return false;
        }
        if (!unwrapSignedManifest(canned, error) || !parseUpdateManifest(canned, currentVersion, out, error)) {   // wire-c: the signature check first
            health::noteUpdateCheckFailed();
            return false;
        }
        return true;
    }
    if (baseUrl.empty()) {
        error = "no update endpoint configured";
        return false;
    }
    // The running version is the only thing sent. It is a query parameter so
    // the server can say what changed since; nothing else identifies anyone.
    std::string url = baseUrl + "?v=" + currentVersion;
    if (!channel.empty()) { url += "&channel=" + channel; }

    // COUNTED, ANONYMOUSLY (0.99.64, core/health_events.hpp): a check that did
    // not complete - the service could not be reached, or did not answer with a
    // manifest this build will act on. Nothing of what it said is kept.
    std::string body;
    if (!PluginRepo::fetchText(url, kMaxManifestBytes, body, error)) {
        health::noteUpdateCheckFailed();
        return false;
    }
    if (!unwrapSignedManifest(body, error) || !parseUpdateManifest(body, currentVersion, out, error)) {   // wire-c: the signature check first
        health::noteUpdateCheckFailed();
        return false;
    }
    return true;
}

// wire-c begin: the test seam of the signature check (updater.hpp)
void setManifestTrustedKeysForTest(const std::vector<std::array<std::uint8_t, 32>>* keys) {
    std::lock_guard<std::mutex> lk(g_manifestKeysMutex);
    g_manifestKeysOverridden = keys != nullptr;
    g_manifestKeysForTest.clear();
    if (keys != nullptr) { g_manifestKeysForTest.assign(keys->begin(), keys->end()); }
}
// wire-c end

bool downloadUpdate(const UpdateInfo& info, std::string& outPath, std::string& error,
                    std::atomic<float>* progress, std::atomic<bool>* cancel) {
    outPath.clear();
    error.clear();
    // Reset before anything can read it: a second attempt after a failed one
    // would otherwise start its bar wherever the first one stopped.
    if (progress != nullptr) {
        progress->store(0.0f, std::memory_order_relaxed);
    }
    if (info.offerWithdrawn) {
        error = "that version has been withdrawn and is not offered";
        return false;
    }
    if (!info.newer || info.url.empty()) {
        error = "there is no update to download";
        return false;
    }
    // Re-checked here as well as at parse time: downloadUpdate is public, and
    // a caller that built an UpdateInfo by hand must not be able to skip the
    // host rule - or the version rule, which matters more here, because the
    // version below becomes a path and that path is later executed.
    if (!wellFormedVersion(info.version)) {
        error = "refusing an update version that does not parse: \"" + info.version + "\"";
        return false;
    }
    if (!urlIsOnAllowedHost(info.url)) {
        error = "refusing an update URL that is not on " + std::string(kAllowedHost);
        return false;
    }

    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec) {
        error = "no temporary directory available";
        health::noteUpdateDownloadFailed();
        return false;
    }
    dir /= "foxsdr-update";
    fs::create_directories(dir, ec);

    // Named after the version so a second attempt overwrites rather than
    // accumulating installers in the user's temp directory.
    const fs::path dest = dir / ("foxsdr-setup-" + info.version + ".exe");

    // COUNTED, ANONYMOUSLY (0.99.64, core/health_events.hpp), and told apart by
    // the failure's KIND rather than its text: a download that did not complete
    // (the network, the disk) and bytes that did not hash to the published
    // digest are different problems with different owners. A cancel is neither.
    PluginRepo::FetchFailure why = PluginRepo::FetchFailure::None;
    if (!PluginRepo::fetchVerifiedFile(info.url, info.sha256, dest.string(), kMaxInstallerBytes,
                                       error, progress, cancel, &why)) {
        switch (why) {
            case PluginRepo::FetchFailure::Transfer:
            case PluginRepo::FetchFailure::Place: health::noteUpdateDownloadFailed(); break;
            case PluginRepo::FetchFailure::Verify: health::noteUpdateVerifyFailed(); break;
            default: break;  // cancelled, refused before any transfer, none
        }
        return false;
    }
    // The bar sat at whatever the last chunk made it; the file is verified and
    // named, so say so rather than stopping at 99%.
    if (progress != nullptr) {
        progress->store(1.0f, std::memory_order_relaxed);
    }
    outPath = dest.string();
    recordVerifiedInstaller(outPath, info.sha256);   // F1: what the launchers re-check and hold right before they start it
    return true;
}

// ---- F1 (review R1, 0.99.114): the installer that runs is the installer that was verified ---------------------------------------------------------
namespace {
std::mutex g_verifiedInstallersMutex;
std::map<std::string, std::string> g_verifiedInstallers;   // path as downloadUpdate returned it -> lower-case sha256 it was verified against

std::string lowerHexOf(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'F') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return out;
}
}  // namespace

void recordVerifiedInstaller(const std::string& path, const std::string& sha256) {
    std::lock_guard<std::mutex> lk(g_verifiedInstallersMutex);
    g_verifiedInstallers[path] = lowerHexOf(sha256);
}

void InstallerHold::release() {
#if defined(_WIN32)
    if (handle_ != nullptr) { ::CloseHandle(static_cast<HANDLE>(handle_)); }
#endif
    handle_ = nullptr;
    held_ = false;
}

bool holdVerifiedInstaller(const std::string& path, InstallerHold& hold, std::string& error) {
    hold.release();
    error.clear();
    const auto refuse = [&error](const char* why) {
        error = why;
        diagWarnf("update: the installer was not started: %s", why);   // no path, no content
        return false;
    };
    std::string expected;
    {
        std::lock_guard<std::mutex> lk(g_verifiedInstallersMutex);
        const auto it = g_verifiedInstallers.find(path);
        if (it != g_verifiedInstallers.end()) { expected = it->second; }
    }
    if (expected.empty()) { return refuse("that file is not an installer this program verified"); }

    cascade::core::sys::Sha256 hasher;
    std::uint64_t total = 0;
    std::vector<char> buf(64 * 1024);
#if defined(_WIN32)
    std::wstring wide;
    try {
        wide = fs::path(path).wstring();
    } catch (...) {
        return refuse("the installer's path cannot be read");
    }
    if (wide.empty()) { return refuse("the installer's path is empty"); }
    // FILE_SHARE_READ only: while this handle lives nobody can open the file for writing or deleting, rename it or replace it; the shell and the loader only read it.
    // FILE_FLAG_OPEN_REPARSE_POINT so a link is opened AS a link and refused below, not followed to a target nobody checked.
    HANDLE h = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) { return refuse("the installer cannot be opened"); }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        ::CloseHandle(h);
        return refuse("the installer is not a plain file");
    }
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            ::CloseHandle(h);
            return refuse("the installer cannot be read");
        }
        if (got == 0) { break; }
        total += got;
        if (total > kMaxInstallerBytes) {
            ::CloseHandle(h);
            return refuse("the installer is larger than any this program downloads");
        }
        hasher.update(buf.data(), got);
    }
    if (cascade::core::sys::toHex(hasher.finish().data(), 32) != expected) {
        ::CloseHandle(h);
        return refuse("the installer is not the file that was verified");
    }
    hold.handle_ = h;
    hold.held_ = true;
    return true;
#else
    std::ifstream in(path, std::ios::binary);
    if (!in) { return refuse("the installer cannot be opened"); }
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) { break; }
        total += static_cast<std::uint64_t>(got);
        if (total > kMaxInstallerBytes) { return refuse("the installer is larger than any this program downloads"); }
        hasher.update(buf.data(), static_cast<std::size_t>(got));
    }
    if (cascade::core::sys::toHex(hasher.finish().data(), 32) != expected) { return refuse("the installer is not the file that was verified"); }
    hold.held_ = true;   // no share-mode lock off Windows: the digest check is all there is
    return true;
#endif
}

// UpdateCheckTask: a template now (AbandonableTask), defined in the header.

}  // namespace cascade::core
