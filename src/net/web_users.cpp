// See net/web_users.hpp for what this holds and why it never touches a file or a console.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_users.hpp"

#include <algorithm>

#include "net/web_control.hpp"

namespace cascade::net {
namespace {

std::string toUrlToken(const std::vector<std::uint8_t>& bytes) {
    std::string s = base64Encode(bytes);
    for (char& c : s) {
        if (c == '+') { c = '-'; }
        else if (c == '/') { c = '_'; }
    }
    while (!s.empty() && s.back() == '=') { s.pop_back(); }
    return s;
}

// Decodes one UTF-8 sequence at s[i]; returns its length and the code point, or 0 for an invalid sequence (overlong, surrogate, too large,
// truncated).
std::size_t decodeUtf8(const std::string& s, std::size_t i, std::uint32_t& cp) {
    const auto b = [&](std::size_t k) { return static_cast<std::uint8_t>(s[k]); };
    const std::uint8_t c = b(i);
    if (c < 0x80) { cp = c; return 1; }
    std::size_t len = 0;
    std::uint32_t min = 0;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; min = 0x10000; }
    else { return 0; }
    if (i + len > s.size()) { return 0; }
    for (std::size_t k = 1; k < len; ++k) {
        if ((b(i + k) & 0xC0) != 0x80) { return 0; }
        cp = (cp << 6) | (b(i + k) & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { return 0; }
    return len;
}

// Characters that change how text is DRAWN without being visible: they are how a line is made to read differently from what it says.
bool isInvisibleControl(std::uint32_t cp) {
    return (cp >= 0x200B && cp <= 0x200F) ||   // zero-width space/joiners, direction marks
           (cp >= 0x202A && cp <= 0x202E) ||   // embeddings and overrides
           (cp >= 0x2060 && cp <= 0x2064) ||   // word joiner, invisible operators
           (cp >= 0x2066 && cp <= 0x2069) ||   // isolates
           cp == 0xFEFF || cp == 0x00AD ||     // byte order mark, soft hyphen
           (cp >= 0x80 && cp <= 0x9F);         // C1 controls
}

bool nameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

}  // namespace

const char* auditWord(AuditKind k) {
    switch (k) {
        case AuditKind::Join: return "join";
        case AuditKind::Leave: return "leave";
        case AuditKind::Expired: return "expired";
        case AuditKind::Kick: return "kick";
        case AuditKind::Ban: return "ban";
        case AuditKind::Unban: return "unban";
        case AuditKind::RefusedJoin: return "refused-join";
        case AuditKind::RefusedOp: return "refused-op";
        case AuditKind::Note: return "note";
        case AuditKind::Chat: return "chat";
    }
    return "unknown";
}

std::string foldCase(const std::string& s) {
    std::string o = s;
    for (char& c : o) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return o;
}

bool sanitizeListenerName(const std::string& raw, std::string& out) {
    if (raw.empty() || raw.size() > kMaxListenerNameLength) { return false; }
    if (raw.front() == ' ' || raw.back() == ' ') { return false; }
    bool prevSpace = false;
    for (char c : raw) {
        if (c == ' ') {
            if (prevSpace) { return false; }
            prevSpace = true;
            continue;
        }
        prevSpace = false;
        if (!nameChar(c)) { return false; }
    }
    out = raw;
    return true;
}

ChatResult sanitizeText(const std::string& raw, std::size_t maxBytes, std::string& out) {
    std::string cleaned;
    cleaned.reserve(raw.size());
    bool lastSpace = true;  // swallows leading spaces
    for (std::size_t i = 0; i < raw.size();) {
        std::uint32_t cp = 0;
        const std::size_t n = decodeUtf8(raw, i, cp);
        if (n == 0) { return ChatResult::BadText; }
        const bool space = cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp < 0x20 || cp == 0x7F || cp == 0x2028 || cp == 0x2029;
        if (space) {
            if (!lastSpace) { cleaned.push_back(' '); lastSpace = true; }
        } else if (!isInvisibleControl(cp)) {
            cleaned.append(raw, i, n);
            lastSpace = false;
        }
        i += n;
    }
    while (!cleaned.empty() && cleaned.back() == ' ') { cleaned.pop_back(); }
    if (cleaned.empty()) { return ChatResult::Empty; }
    if (cleaned.size() > maxBytes) { return ChatResult::TooLong; }
    out = std::move(cleaned);
    return ChatResult::Ok;
}

std::string escapeHtml(const std::string& text) {
    std::string o;
    o.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            case '`': o += "&#96;"; break;
            default: o.push_back(c);
        }
    }
    return o;
}

bool listenerMayTune(const ControlRequest& req) {
    if (req.empty()) { return false; }
    ControlRequest rest = req;
    // THE TUNING FIELDS, and only these: where the receiver listens and how wide. Everything else - start/stop, volume and mute (the desk's
    // speakers), squelch, the audio chain, the source and its gains, the recorder, bookmarks added or removed, the scanner, plugins and above
    // all the transmit key - stays the owner's.
    rest.centerHz.reset();
    rest.vfoOffsetHz.reset();
    rest.mode.reset();
    rest.bandwidthHz.reset();
    rest.bookmarkTune.reset();
    // (ControlRequest::empty() does not list acknowledgeNotice - alone it does nothing - so it is named here: it must not ride along either.)
    return rest.empty() && !rest.acknowledgeNotice;
}

// --- WebRoom -----------------------------------------------------------------------------------------------------------------------------

void WebRoom::configure(const RoomConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_ = cfg;
    cfg_.maxListeners = std::min(std::max<std::size_t>(cfg_.maxListeners, 1), kMaxListenersCeiling);
    if (cfg_.idleSeconds < 0) { cfg_.idleSeconds = 0; }
    if (cfg_.sessionSeconds < 0) { cfg_.sessionSeconds = 0; }
    owner_.revokeAll();
    listeners_.clear();
    bannedNames_.clear();
    bannedAddresses_.clear();
    chat_.clear();
    chatSeq_ = 0;
    chatTimes_.clear();
    note_ = OwnerNote{};
    audit_.clear();
    auditSeq_ = 0;
}

RoomConfig WebRoom::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cfg_;
}

void WebRoom::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    owner_.revokeAll();
    listeners_.clear();
    bannedNames_.clear();
    bannedAddresses_.clear();
    chat_.clear();
    chatTimes_.clear();
    note_ = OwnerNote{};
    audit_.clear();
}

bool WebRoom::issueOwner(std::int64_t now, std::string& tokenOut, std::string& error) {
    return owner_.issue(now, tokenOut, error);
}

void WebRoom::auditLocked(AuditKind kind, const std::string& name, std::int64_t now) {
    AuditEvent e;
    e.seq = ++auditSeq_;
    e.t = now;
    e.kind = kind;
    e.name = name;
    audit_.push_back(std::move(e));
    while (audit_.size() > kAuditRing) { audit_.pop_front(); }
}

void WebRoom::audit(AuditKind kind, const std::string& name, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    auditLocked(kind, name, now);
}

void WebRoom::eraseListenerLocked(std::size_t index) {
    chatTimes_.erase(listeners_[index].key);
    listeners_.erase(listeners_.begin() + static_cast<std::ptrdiff_t>(index));
}

std::size_t WebRoom::purgeLocked(std::int64_t now) {
    std::size_t ended = 0;
    for (std::size_t i = 0; i < listeners_.size();) {
        const Listener& l = listeners_[i];
        const bool tooOld = cfg_.sessionSeconds > 0 && now - l.joinedAt >= cfg_.sessionSeconds;
        const bool idle = cfg_.idleSeconds > 0 && now - l.lastSeen >= cfg_.idleSeconds;
        if (tooOld || idle || kSessionTtlSeconds <= now - l.joinedAt) {
            auditLocked(AuditKind::Expired, l.name, now);
            eraseListenerLocked(i);
            ++ended;
        } else {
            ++i;
        }
    }
    return ended;
}

std::size_t WebRoom::tick(std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return purgeLocked(now);
}

JoinResult WebRoom::joinListener(std::int64_t now, const std::string& rawName, const std::string& address, const std::string& ownerName,
                                 std::string& tokenOut, std::string& error) {
    error.clear();
    tokenOut.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cfg_.listenersEnabled) { return JoinResult::Disabled; }
    std::string name;
    if (!sanitizeListenerName(rawName, name)) {
        auditLocked(AuditKind::RefusedJoin, std::string(), now);
        return JoinResult::BadName;
    }
    const std::string key = foldCase(name);
    if (key == foldCase(ownerName)) {
        auditLocked(AuditKind::RefusedJoin, std::string(), now);
        return JoinResult::ReservedName;
    }
    const std::string addr = foldCase(address);
    if (std::find(bannedAddresses_.begin(), bannedAddresses_.end(), addr) != bannedAddresses_.end()) {
        auditLocked(AuditKind::RefusedJoin, std::string(), now);
        return JoinResult::BannedAddress;
    }
    if (std::find(bannedNames_.begin(), bannedNames_.end(), key) != bannedNames_.end()) {
        auditLocked(AuditKind::RefusedJoin, name, now);
        return JoinResult::BannedName;
    }
    purgeLocked(now);
    for (const Listener& l : listeners_) {
        if (l.key == key) { return JoinResult::NameTaken; }
    }
    if (listeners_.size() >= cfg_.maxListeners) {
        auditLocked(AuditKind::RefusedJoin, std::string(), now);
        return JoinResult::Full;
    }
    std::vector<std::uint8_t> raw(kTokenBytes);
    if (!randomBytes(raw.data(), raw.size(), error)) { return JoinResult::Error; }
    const std::string token = toUrlToken(raw);
    std::vector<std::uint8_t> digest;
    if (!sha256(reinterpret_cast<const std::uint8_t*>(token.data()), token.size(), digest, error)) { return JoinResult::Error; }
    Listener l;
    l.tokenHash = std::move(digest);
    l.name = name;
    l.key = key;
    l.address = address;
    l.joinedAt = now;
    l.lastSeen = now;
    listeners_.push_back(std::move(l));
    auditLocked(AuditKind::Join, name, now);
    tokenOut = token;
    return JoinResult::Ok;
}

bool WebRoom::identify(const std::string& token, std::int64_t now, bool touch, Identity& out) {
    out = Identity{};
    if (token.empty()) { return false; }
    if (owner_.validate(token, now)) {
        out.role = Role::Owner;
        return true;
    }
    std::vector<std::uint8_t> digest;
    std::string ignored;
    if (!sha256(reinterpret_cast<const std::uint8_t*>(token.data()), token.size(), digest, ignored)) { return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    purgeLocked(now);
    bool found = false;
    for (Listener& l : listeners_) {
        // (no early exit: the scan reads every entry, as SessionStore::validate does)
        if (constantTimeEquals(l.tokenHash, digest)) {
            found = true;
            if (touch) { l.lastSeen = now; }
            out.role = Role::Listener;
            out.name = l.name;
            out.address = l.address;
        }
    }
    return found;
}

bool WebRoom::revoke(const std::string& token, std::int64_t now) {
    bool any = owner_.revoke(token);
    if (token.empty()) { return any; }
    std::vector<std::uint8_t> digest;
    std::string ignored;
    if (!sha256(reinterpret_cast<const std::uint8_t*>(token.data()), token.size(), digest, ignored)) { return any; }
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < listeners_.size(); ++i) {
        if (constantTimeEquals(listeners_[i].tokenHash, digest)) {
            auditLocked(AuditKind::Leave, listeners_[i].name, now);
            eraseListenerLocked(i);
            return true;
        }
    }
    return any;
}

void WebRoom::revokeAll() {
    owner_.revokeAll();
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.clear();
    chatTimes_.clear();
}

std::size_t WebRoom::ownerSessions() const { return owner_.size(); }

std::size_t WebRoom::listenerCount(std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    purgeLocked(now);
    return listeners_.size();
}

std::vector<ListenerInfo> WebRoom::listeners(std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    purgeLocked(now);
    std::vector<ListenerInfo> out;
    out.reserve(listeners_.size());
    for (const Listener& l : listeners_) {
        ListenerInfo i;
        i.name = l.name;
        i.address = l.address;
        i.signedInSeconds = now - l.joinedAt;
        i.idleSeconds = now - l.lastSeen;
        out.push_back(std::move(i));
    }
    return out;
}

std::size_t WebRoom::endWhereLocked(bool byName, const std::string& key, std::int64_t now, AuditKind why) {
    std::size_t ended = 0;
    for (std::size_t i = 0; i < listeners_.size();) {
        const std::string& k = byName ? listeners_[i].key : foldCase(listeners_[i].address);
        if (k == key) {
            auditLocked(why, listeners_[i].name, now);
            eraseListenerLocked(i);
            ++ended;
        } else {
            ++i;
        }
    }
    return ended;
}

ModResult WebRoom::kick(const std::string& name, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    ModResult r;
    r.ok = !name.empty();
    if (r.ok) { r.ended = endWhereLocked(true, foldCase(name), now, AuditKind::Kick); }
    return r;
}

ModResult WebRoom::banName(const std::string& name, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    ModResult r;
    std::string clean;
    if (!sanitizeListenerName(name, clean)) { return r; }
    const std::string key = foldCase(clean);
    if (std::find(bannedNames_.begin(), bannedNames_.end(), key) == bannedNames_.end()) {
        if (bannedNames_.size() >= kMaxBansPerList) { return r; }
        bannedNames_.push_back(key);
    }
    r.ok = true;
    r.ended = endWhereLocked(true, key, now, AuditKind::Ban);
    if (r.ended == 0) { auditLocked(AuditKind::Ban, clean, now); }
    return r;
}

ModResult WebRoom::banAddress(const std::string& address, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    ModResult r;
    const std::string key = foldCase(address);
    if (key.empty() || key.size() > 64) { return r; }
    if (std::find(bannedAddresses_.begin(), bannedAddresses_.end(), key) == bannedAddresses_.end()) {
        if (bannedAddresses_.size() >= kMaxBansPerList) { return r; }
        bannedAddresses_.push_back(key);
    }
    r.ok = true;
    r.ended = endWhereLocked(false, key, now, AuditKind::Ban);
    if (r.ended == 0) { auditLocked(AuditKind::Ban, std::string(), now); }
    return r;
}

bool WebRoom::unbanName(const std::string& name, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = foldCase(name);
    const auto it = std::find(bannedNames_.begin(), bannedNames_.end(), key);
    if (it == bannedNames_.end()) { return false; }
    bannedNames_.erase(it);
    auditLocked(AuditKind::Unban, std::string(), now);
    return true;
}

bool WebRoom::unbanAddress(const std::string& address, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = foldCase(address);
    const auto it = std::find(bannedAddresses_.begin(), bannedAddresses_.end(), key);
    if (it == bannedAddresses_.end()) { return false; }
    bannedAddresses_.erase(it);
    auditLocked(AuditKind::Unban, std::string(), now);
    return true;
}

bool WebRoom::isBannedAddress(const std::string& address) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::find(bannedAddresses_.begin(), bannedAddresses_.end(), foldCase(address)) != bannedAddresses_.end();
}

bool WebRoom::isBannedName(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::find(bannedNames_.begin(), bannedNames_.end(), foldCase(name)) != bannedNames_.end();
}

std::vector<std::string> WebRoom::bannedNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bannedNames_;
}

std::vector<std::string> WebRoom::bannedAddresses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bannedAddresses_;
}

ChatResult WebRoom::setNote(const std::string& text, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cfg_.listenersEnabled) { return ChatResult::Disabled; }
    if (text.empty()) {
        note_.html.clear();
        ++note_.seq;
        return ChatResult::Ok;
    }
    std::string clean;
    const ChatResult r = sanitizeText(text, kMaxNoteBytes, clean);
    if (r != ChatResult::Ok) { return r; }
    note_.html = escapeHtml(clean);
    ++note_.seq;
    auditLocked(AuditKind::Note, std::string(), now);
    return ChatResult::Ok;
}

OwnerNote WebRoom::note() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return note_;
}

ChatResult WebRoom::postChat(const Identity& who, const std::string& text, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cfg_.chatEnabled || !cfg_.listenersEnabled || who.role == Role::None) { return ChatResult::Disabled; }
    std::string clean;
    const ChatResult r = sanitizeText(text, kMaxChatBytes, clean);
    if (r != ChatResult::Ok) { return r; }
    const bool owner = who.role == Role::Owner;
    // (the owner's key cannot be a listener's: names are letters, digits and _ - . only)
    const std::string key = owner ? std::string("*owner") : foldCase(who.name);
    std::deque<std::int64_t>& times = chatTimes_[key];
    while (!times.empty() && now - times.front() >= kChatWindowSeconds) { times.pop_front(); }
    if (times.size() >= kChatBurst) { return ChatResult::RateLimited; }
    times.push_back(now);
    ChatLine line;
    line.seq = ++chatSeq_;
    line.t = now;
    line.name = owner ? std::string("owner") : who.name;
    line.owner = owner;
    line.html = escapeHtml(clean);
    chat_.push_back(std::move(line));
    while (chat_.size() > kChatRing) { chat_.pop_front(); }
    auditLocked(AuditKind::Chat, owner ? std::string() : who.name, now);  // the event, never the text
    return ChatResult::Ok;
}

std::vector<ChatLine> WebRoom::chatSince(std::uint64_t afterSeq) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ChatLine> out;
    for (const ChatLine& l : chat_) {
        if (l.seq > afterSeq) { out.push_back(l); }
    }
    return out;
}

std::uint64_t WebRoom::chatSeq() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return chatSeq_;
}

std::vector<AuditEvent> WebRoom::auditSince(std::uint64_t afterSeq) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AuditEvent> out;
    for (const AuditEvent& e : audit_) {
        if (e.seq > afterSeq) { out.push_back(e); }
    }
    return out;
}

}  // namespace cascade::net
