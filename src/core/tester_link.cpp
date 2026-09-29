// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/tester_link.hpp"

#include "core/tester_usage.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace cascade::core {

std::string parseBetaLinkUrl(const std::string& arg) {
    std::string s = arg;
    // ONE LAYER of matching surrounding quotes only - a shell/shortcut
    // quoting artefact around argv[1], not something to unwrap repeatedly.
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    if (s.empty() || s.size() > kMaxBetaLinkUrlChars) { return std::string(); }
    // Control bytes rejected outright, before anything else looks at the
    // content - a URL that would otherwise match is still refused if it
    // carries one.
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) { return std::string(); }
    }
    // TWO SPELLINGS OF ONE URL. The portal navigates to foxsdr://beta?t=,
    // but Windows' ShellExecute - and so every browser's hand-off there -
    // normalises the empty path after the "beta" authority to "/" and starts
    // the handler with foxsdr://beta/?t= (seen on a real 0.99.50 install,
    // 2026-09-29: every Windows click was being dropped as unrecognised).
    // Exactly that one slash and nothing else: any other path is still
    // refused.
    static constexpr char kPrefix[] = "foxsdr://beta?t=";
    static constexpr char kSlashPrefix[] = "foxsdr://beta/?t=";
    static constexpr std::size_t kTokenLen = 40;
    // EXACT LENGTH, EXACT PREFIX - this is what makes an extra query
    // parameter, a path segment, or trailing junk after a valid token all
    // fail here rather than needing their own separate checks: none of them
    // can produce a string of exactly prefix + kTokenLen bytes that also
    // starts with that prefix and ends in 40 valid hex characters.
    std::size_t prefixLen = 0;
    for (const char* prefix : {kPrefix, kSlashPrefix}) {
        const std::size_t len = std::char_traits<char>::length(prefix);
        if (s.size() == len + kTokenLen && s.compare(0, len, prefix) == 0) {
            prefixLen = len;
            break;
        }
    }
    if (prefixLen == 0) { return std::string(); }
    const std::string token = s.substr(prefixLen);
    if (!validAppToken(token)) { return std::string(); }
    return token;
}

std::string linkRequestPath(const std::string& configDir) {
    return (fs::path(configDir) / "link-request").string();
}

bool writeLinkRequestFile(const std::string& configDir, const std::string& token) {
    if (configDir.empty() || !validAppToken(token)) { return false; }
    std::error_code ec;
    fs::create_directories(fs::path(configDir), ec);

    // TEMP NAME, THEN RENAME. A reader polling linkRequestPath() at 1 Hz must
    // never observe a partially written file - the same atomicity discipline
    // ConfigStore's own writer and crash_upload's sidecars use elsewhere in
    // this codebase.
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const long pid = static_cast<long>(::getpid());
#endif
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path tmp =
        fs::path(configDir) / (".link-request." + std::to_string(pid) + "." +
                               std::to_string(stamp) + ".tmp");
#if defined(_WIN32)
    // Same-user-only by the OS's default ACL, as the header comment says.
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { return false; }
        f.write(token.data(), static_cast<std::streamsize>(token.size()));
        if (!f) {
            fs::remove(tmp, ec);
            return false;
        }
    }
#else
    // EXPLICIT 0600, not a bare ofstream create (which lands at whatever the
    // process umask leaves - 644 under the common 022 umask, world-readable).
    // This is a one-shot credential file living beside config.json, which
    // this codebase already treats as same-user-only; the mode is asked for
    // directly rather than trusted to the umask of whatever process happens
    // to launch the activation.
    {
        const int fd = ::open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd < 0) { return false; }
        const ssize_t n = ::write(fd, token.data(), token.size());
        const bool ok = n == static_cast<ssize_t>(token.size());
        ::close(fd);
        if (!ok) {
            fs::remove(tmp, ec);
            return false;
        }
    }
#endif
    const fs::path dst(linkRequestPath(configDir));
    fs::rename(tmp, dst, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

namespace {

// Same conversion crash_upload.cpp's own fileEpoch() uses - kept as its own
// tiny copy rather than exported across translation units for one call site.
std::time_t fileWriteEpoch(const fs::path& p) {
    std::error_code ec;
    const auto ft = fs::last_write_time(p, ec);
    if (ec) { return 0; }
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(sys);
}

}  // namespace

std::string claimLinkRequestFile(const std::string& configDir, std::time_t now) {
    if (configDir.empty()) { return std::string(); }
    if (now == 0) { now = std::time(nullptr); }
    const fs::path path(linkRequestPath(configDir));
    std::error_code ec;
    if (!fs::exists(path, ec)) { return std::string(); }

    // CLAIM BY RENAME - atomic on both platforms, so two instances polling in
    // the same instant can never both claim the same request: whichever
    // loses the race gets ENOENT from rename() and returns "", exactly as if
    // there had been nothing there.
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const long pid = static_cast<long>(::getpid());
#endif
    const fs::path claimed =
        fs::path(configDir) / (".link-request.claimed." + std::to_string(pid));
    fs::rename(path, claimed, ec);
    if (ec) { return std::string(); }

    const std::time_t wroteAt = fileWriteEpoch(claimed);
    const bool stale = (wroteAt > 0) && (now - wroteAt > kLinkRequestMaxAgeSec);

    std::string token;
    if (!stale) {
        std::ifstream f(claimed, std::ios::binary);
        if (f) {
            std::string buf(kMaxLinkRequestBytes, '\0');
            f.read(&buf[0], static_cast<std::streamsize>(buf.size()));
            buf.resize(static_cast<std::size_t>(f.gcount()));
            token = extractAppToken(buf);
        }
    }
    fs::remove(claimed, ec);
    return token;
}

bool claimPrimaryInstanceAt(const std::string& identity) {
#if defined(_WIN32)
    // Leaked on purpose - held for the life of the process, reclaimed by the
    // OS at exit, matching every other "for the life of this process"
    // resource in this codebase. `Local\` scopes it to this logon session,
    // so a different user on a shared box can never observe or squat it
    // (PORTAL-LINK-VERDICT.md finding 2's fix).
    const HANDLE h = ::CreateMutexA(nullptr, FALSE, identity.c_str());
    // Could not even ask - fail OPEN, not closed. "Not primary" makes a link
    // activation write its file and exit without ever starting a GUI to
    // claim it (main.cpp: !primaryInstance means "assume another instance
    // will poll for this"), which for a mutex failure is nobody - the link
    // would be silently lost. Continuing as a normal launch is the answer
    // that still works when the one thing this function exists to answer
    // could not itself be answered.
    if (h == nullptr) { return true; }
    return ::GetLastError() != ERROR_ALREADY_EXISTS;
#else
    // Leaked on purpose, same reasoning. flock() is per-open-file-description,
    // so holding the fd for the process's life is what keeps the lock held;
    // a lock file the process never explicitly unlinks is harmless - the
    // NEXT process to check simply re-creates/re-locks it.
    const int fd = ::open(identity.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) { return false; }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    return true;
#endif
}

bool claimPrimaryInstance(const std::string& configDir) {
#if defined(_WIN32)
    (void)configDir;
    return claimPrimaryInstanceAt("Local\\FoxSDR-instance");
#else
    return claimPrimaryInstanceAt(configDir.empty() ? "/tmp/foxsdr-instance.lock"
                                                    : configDir + "/instance.lock");
#endif
}

namespace {
constexpr char kDefaultBetaApiBaseUrl[] = "https://foxsdr.com";
}  // namespace

std::string betaApiBaseUrl() {
#if defined(_WIN32)
    char buf[512] = {0};
    const DWORD n = ::GetEnvironmentVariableA("FOXSDR_BETA_API_URL", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) { return std::string(buf, n); }
#else
    const char* env = std::getenv("FOXSDR_BETA_API_URL");
    if (env != nullptr && env[0] != '\0') { return std::string(env); }
#endif
    return std::string(kDefaultBetaApiBaseUrl);
}

std::string sanitizeTesterName(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20u || u == 0x7Fu) { continue; }
        out.push_back(c);
    }
    if (out.size() > kMaxTesterNameBytes) {
        std::size_t cut = kMaxTesterNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0u) == 0x80u) { --cut; }
        out.resize(cut);
    }
    while (!out.empty() && out.back() == ' ') { out.pop_back(); }
    return out;
}

BetaLinkResolved resolveAppTokenName(const std::string& baseUrl, const std::string& appToken,
                                     const std::shared_ptr<UploadCancel>& cancel) {
    BetaLinkResolved out;
    if (baseUrl.empty() || appToken.empty() || !cancel) { return out; }
    const AuthRequestResult r =
        authRequestBounded("GET", baseUrl + "/api/beta/app-token/me", appToken, "", cancel);
    if (!r.attempted || r.cancelled) { return out; }
    if (r.status == 404) {
        out.outcome = BetaLinkOutcome::Invalid;
        return out;
    }
    if (r.status < 200 || r.status >= 300) { return out; }  // NetworkError-shaped: leave as-is
    const nlohmann::json j = nlohmann::json::parse(r.body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { return out; }
    const auto it = j.find("name");
    if (it == j.end() || !it->is_string()) { return out; }
    out.outcome = BetaLinkOutcome::Ok;
    out.name = sanitizeTesterName(it->get<std::string>());
    return out;
}

struct BetaLinkNameSender::Shared {
    std::atomic<bool> inFlight{false};
    std::mutex mu;
    std::optional<BetaLinkResolved> result;
};

BetaLinkNameSender::~BetaLinkNameSender() {
    // Best-effort: unblocks an in-flight worker promptly. Never waits for it
    // - see TesterUsageSender's header comment for why detaching is safe.
    if (cancel_) { cancel_->cancel(); }
}

bool BetaLinkNameSender::busy() const { return shared_ && shared_->inFlight.load(); }

void BetaLinkNameSender::send(const std::string& baseUrl, const std::string& appToken) {
    if (baseUrl.empty() || appToken.empty() || busy()) { return; }
    cancel_ = std::make_shared<UploadCancel>();
    shared_ = std::make_shared<Shared>();
    shared_->inFlight.store(true);
    auto shared = shared_;
    auto cancel = cancel_;
    std::thread([baseUrl, appToken, shared, cancel]() {
        BetaLinkResolved r;
        try {
            r = resolveAppTokenName(baseUrl, appToken, cancel);
        } catch (...) {
            // r stays default-constructed (NetworkError)
        }
        if (!cancel->cancelled()) {
            std::lock_guard<std::mutex> lk(shared->mu);
            shared->result = r;
        }
        shared->inFlight.store(false);
    }).detach();
}

std::optional<BetaLinkResolved> BetaLinkNameSender::takeResult() {
    if (!shared_) { return std::nullopt; }
    std::lock_guard<std::mutex> lk(shared_->mu);
    if (!shared_->result) { return std::nullopt; }
    BetaLinkResolved r = std::move(*shared_->result);
    shared_->result.reset();
    return r;
}

BetaMigrationResult exchangePortalToken(const std::string& baseUrl, const std::string& portalToken,
                                        const std::shared_ptr<UploadCancel>& cancel) {
    BetaMigrationResult out;
    if (baseUrl.empty() || portalToken.empty() || !cancel) { return out; }
    // No body content is meaningful to this endpoint - the credential IS the
    // bearer header - but authRequestBounded's POST path still needs a
    // non-empty payload to send, matching postBounded's own "empty means
    // nothing to do" convention elsewhere in this codebase.
    const AuthRequestResult r =
        authRequestBounded("POST", baseUrl + "/api/beta/app-token", portalToken, "{}", cancel);
    if (!r.attempted || r.cancelled) { return out; }
    if (r.status == 404 || r.status == 401) {
        out.outcome = BetaMigrationOutcome::Invalid;
        return out;
    }
    if (r.status < 200 || r.status >= 300) { return out; }
    const nlohmann::json j = nlohmann::json::parse(r.body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { return out; }
    const auto tokIt = j.find("appToken");
    const auto nameIt = j.find("name");
    if (tokIt == j.end() || !tokIt->is_string()) { return out; }
    const std::string appToken = tokIt->get<std::string>();
    if (!validAppToken(appToken)) { return out; }  // the site owes us its own shape
    out.outcome = BetaMigrationOutcome::Ok;
    out.appToken = appToken;
    if (nameIt != j.end() && nameIt->is_string()) {
        out.name = sanitizeTesterName(nameIt->get<std::string>());
    }
    return out;
}

struct BetaMigrationSender::Shared {
    std::atomic<bool> inFlight{false};
    std::mutex mu;
    std::optional<BetaMigrationResult> result;
};

BetaMigrationSender::~BetaMigrationSender() {
    if (cancel_) { cancel_->cancel(); }
}

bool BetaMigrationSender::busy() const { return shared_ && shared_->inFlight.load(); }

void BetaMigrationSender::send(const std::string& baseUrl, const std::string& portalToken) {
    if (baseUrl.empty() || portalToken.empty() || busy()) { return; }
    cancel_ = std::make_shared<UploadCancel>();
    shared_ = std::make_shared<Shared>();
    shared_->inFlight.store(true);
    auto shared = shared_;
    auto cancel = cancel_;
    std::thread([baseUrl, portalToken, shared, cancel]() {
        BetaMigrationResult r;
        try {
            r = exchangePortalToken(baseUrl, portalToken, cancel);
        } catch (...) {
        }
        if (!cancel->cancelled()) {
            std::lock_guard<std::mutex> lk(shared->mu);
            shared->result = r;
        }
        shared->inFlight.store(false);
    }).detach();
}

std::optional<BetaMigrationResult> BetaMigrationSender::takeResult() {
    if (!shared_) { return std::nullopt; }
    std::lock_guard<std::mutex> lk(shared_->mu);
    if (!shared_->result) { return std::nullopt; }
    BetaMigrationResult r = std::move(*shared_->result);
    shared_->result.reset();
    return r;
}

}  // namespace cascade::core
