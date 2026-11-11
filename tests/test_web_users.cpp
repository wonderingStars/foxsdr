// Tests for net/web_users.hpp: the owner/listener rooms, limits, bans, chat, note and audit trail (feat/web-secure).
//
// Pure computation with an injected clock, so every limit is driven deterministically. The argument is a source root (CTest passes it): the
// last test reads this module's own source to prove it opens no file and prints nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "net/web_control.hpp"
#include "net/web_users.hpp"
#include "test_check.hpp"

using namespace cascade::net;

namespace {

std::string g_root;

RoomConfig on(std::size_t cap = 4) {
    RoomConfig c;
    c.listenersEnabled = true;
    c.maxListeners = cap;
    return c;
}

std::string join(WebRoom& r, const std::string& name, const std::string& addr, std::int64_t now = 100, JoinResult* res = nullptr) {
    std::string tok, err;
    const JoinResult j = r.joinListener(now, name, addr, "admin", tok, err);
    if (res != nullptr) { *res = j; }
    return j == JoinResult::Ok ? tok : std::string();
}

void testNames() {
    std::string out;
    CHECK(sanitizeListenerName("Alice", out) && out == "Alice");
    CHECK(sanitizeListenerName("a b.c-d_e9", out));
    CHECK(!sanitizeListenerName("", out));
    CHECK(!sanitizeListenerName(" lead", out));
    CHECK(!sanitizeListenerName("trail ", out));
    CHECK(!sanitizeListenerName("two  spaces", out));
    CHECK(!sanitizeListenerName("<b>x</b>", out));
    CHECK(!sanitizeListenerName("a\nb", out));
    CHECK(!sanitizeListenerName(std::string("a\0b", 3), out));
    CHECK(!sanitizeListenerName("caf\xc3\xa9", out));                 // non-ASCII is refused, not normalised
    CHECK(sanitizeListenerName(std::string(kMaxListenerNameLength, 'x'), out));
    CHECK(!sanitizeListenerName(std::string(kMaxListenerNameLength + 1, 'x'), out));
}

void testText() {
    std::string out;
    CHECK(sanitizeText("hello  world", 200, out) == ChatResult::Ok && out == "hello world");
    CHECK(sanitizeText("  a\r\nb\tc  ", 200, out) == ChatResult::Ok && out == "a b c");
    CHECK(sanitizeText("", 200, out) == ChatResult::Empty);
    CHECK(sanitizeText(" \n\t ", 200, out) == ChatResult::Empty);
    CHECK(sanitizeText(std::string(201, 'x'), 200, out) == ChatResult::TooLong);
    CHECK(sanitizeText(std::string(200, 'x'), 200, out) == ChatResult::Ok);
    CHECK(sanitizeText("bad \xff byte", 200, out) == ChatResult::BadText);
    CHECK(sanitizeText("overlong \xc0\x80", 200, out) == ChatResult::BadText);
    CHECK(sanitizeText("surrogate \xed\xa0\x80", 200, out) == ChatResult::BadText);
    CHECK(sanitizeText("truncated \xe2\x82", 200, out) == ChatResult::BadText);
    CHECK(sanitizeText("caf\xc3\xa9 ok", 200, out) == ChatResult::Ok && out == "caf\xc3\xa9 ok");
    // The bidirectional override that makes "evil.exe" read "exe.live" is dropped, and so is a zero-width space.
    CHECK(sanitizeText("a\xe2\x80\xae" "b\xe2\x80\x8b" "c", 200, out) == ChatResult::Ok && out == "abc");
    CHECK(sanitizeText(std::string("nul\0inside", 10), 200, out) == ChatResult::Ok && out == "nul inside");
    CHECK(escapeHtml("<script>alert('x')&\"`</script>") == "&lt;script&gt;alert(&#39;x&#39;)&amp;&quot;&#96;&lt;/script&gt;");
    CHECK(escapeHtml("plain") == "plain");
    CHECK(escapeHtml("") == "");
}

void testDisabledByDefault() {
    WebRoom r;
    r.configure(RoomConfig{});
    JoinResult j{};
    CHECK(join(r, "bob", "10.0.0.5", 100, &j).empty());
    CHECK(j == JoinResult::Disabled);
    Identity who;
    who.role = Role::Owner;
    CHECK(r.postChat(who, "hi", 100) == ChatResult::Disabled);
    CHECK(r.setNote("hi", 100) == ChatResult::Disabled);
}

void testJoinRules() {
    WebRoom r;
    r.configure(on(3));
    JoinResult j{};
    const std::string t1 = join(r, "Alice", "10.0.0.1", 100, &j);
    CHECK(j == JoinResult::Ok && !t1.empty());
    join(r, "alice", "10.0.0.2", 100, &j);                 // the same name in another case
    CHECK(j == JoinResult::NameTaken);
    join(r, "ADMIN", "10.0.0.3", 100, &j);                 // the owner's account name, any case
    CHECK(j == JoinResult::ReservedName);
    join(r, "bad<name>", "10.0.0.3", 100, &j);
    CHECK(j == JoinResult::BadName);
    CHECK(!join(r, "Bob", "10.0.0.2", 100, &j).empty());
    CHECK(!join(r, "Carol", "10.0.0.3", 100, &j).empty());
    join(r, "Dave", "10.0.0.4", 100, &j);                  // the cap is 3
    CHECK(j == JoinResult::Full);
    CHECK(r.listenerCount(100) == 3);
    // A refused join leaves a name-free trail (no address).
    for (const AuditEvent& e : r.auditSince(0)) { CHECK(e.name.find("10.0.0") == std::string::npos); }
    // A seat freed by a sign-out can be taken again.
    CHECK(r.revoke(t1, 101));
    CHECK(r.listenerCount(101) == 2);
    CHECK(!join(r, "Dave", "10.0.0.4", 101, &j).empty());
}

void testConcurrentJoinsAtTheCap() {
    for (const std::size_t cap : {static_cast<std::size_t>(4), kMaxListenersCeiling}) {
        WebRoom r;
        r.configure(on(cap));
        std::atomic<int> ok{0}, full{0}, other{0};
        std::vector<std::thread> ts;
        const int n = static_cast<int>(cap) * 3;
        for (int i = 0; i < n; ++i) {
            ts.emplace_back([&, i] {
                std::string tok, err;
                const JoinResult j = r.joinListener(100, "user" + std::to_string(i), "10.1.0." + std::to_string(i), "admin", tok, err);
                if (j == JoinResult::Ok) { ++ok; } else if (j == JoinResult::Full) { ++full; } else { ++other; }
            });
        }
        for (std::thread& t : ts) { t.join(); }
        CHECK(ok == static_cast<int>(cap));
        CHECK(full == n - static_cast<int>(cap));
        CHECK(other == 0);
        CHECK(r.listenerCount(100) == cap);
    }
    // The ceiling holds whatever the config says, and the owner can still sign in with a full room.
    WebRoom r;
    RoomConfig c = on(1000);
    r.configure(c);
    CHECK(r.config().maxListeners == kMaxListenersCeiling);
    for (std::size_t i = 0; i < kMaxListenersCeiling; ++i) { CHECK(!join(r, "u" + std::to_string(i), "10.2.0.1").empty()); }
    std::string tok, err;
    CHECK(r.issueOwner(100, tok, err));
    Identity who;
    CHECK(r.identify(tok, 100, true, who) && who.role == Role::Owner);
    c.maxListeners = 0;
    r.configure(c);
    CHECK(r.config().maxListeners == 1);
}

void testIdentifyAndLimits() {
    WebRoom r;
    RoomConfig c = on();
    c.idleSeconds = 60;
    c.sessionSeconds = 600;
    r.configure(c);
    const std::string tok = join(r, "Bob", "10.0.0.9", 1000);
    Identity who;
    CHECK(r.identify(tok, 1000, true, who));
    CHECK(who.role == Role::Listener && who.name == "Bob" && who.address == "10.0.0.9");
    CHECK(!r.identify("not-a-token", 1000, true, who) && who.role == Role::None);
    CHECK(!r.identify("", 1000, true, who));
    // Activity resets the idle clock ...
    CHECK(r.identify(tok, 1050, true, who));
    CHECK(r.identify(tok, 1100, true, who));
    // ... a stream's re-check (touch = false) does not: the abandoned tab is found idle although its audio still plays.
    CHECK(r.identify(tok, 1150, false, who));
    CHECK(!r.identify(tok, 1161, false, who));
    CHECK(r.listenerCount(1161) == 0);
    // The session limit ends a listener who is never idle.
    const std::string t2 = join(r, "Carol", "10.0.0.8", 2000);
    for (std::int64_t t = 2030; t < 2600; t += 30) { CHECK(r.identify(t2, t, true, who)); }
    CHECK(!r.identify(t2, 2600, true, who));
    // 0 = no limit (the 12 h cookie lifetime of the owner's store still applies, see kSessionTtlSeconds).
    c.idleSeconds = 0;
    c.sessionSeconds = 0;
    r.configure(c);
    const std::string t3 = join(r, "Dan", "10.0.0.7", 3000);
    CHECK(r.identify(t3, 3000 + 6 * 3600, false, who));
    CHECK(!r.identify(t3, 3000 + kSessionTtlSeconds, false, who));
    // The idle tick ends sessions with nobody asking.
    c.idleSeconds = 10;
    r.configure(c);
    join(r, "Eve", "10.0.0.6", 5000);
    CHECK(r.tick(5005) == 0);
    CHECK(r.tick(5010) == 1);
    bool expired = false;
    for (const AuditEvent& e : r.auditSince(0)) { expired = expired || e.kind == AuditKind::Expired; }
    CHECK(expired);
}

void testOwnerSession() {
    WebRoom r;
    r.configure(on());
    std::string tok, err;
    CHECK(r.issueOwner(100, tok, err));
    Identity who;
    CHECK(r.identify(tok, 100, true, who) && who.role == Role::Owner && who.name.empty());
    CHECK(r.ownerSessions() == 1);
    CHECK(r.revoke(tok, 101));
    CHECK(!r.identify(tok, 101, true, who));
    // A listener's token never reads as the owner's, and the owner's is not a listener.
    const std::string lt = join(r, "Bob", "10.0.0.1");
    CHECK(r.identify(lt, 100, true, who) && who.role == Role::Listener);
    r.revokeAll();
    CHECK(!r.identify(lt, 100, true, who));
    CHECK(r.listenerCount(100) == 0);
}

void testBansEndLiveSessions() {
    WebRoom r;
    r.configure(on(8));
    const std::string a = join(r, "Alice", "10.0.0.1");
    const std::string b = join(r, "Bob", "10.0.0.2");
    const std::string c = join(r, "Carol", "10.0.0.1");   // shares Alice's address
    Identity who;
    ModResult m = r.banAddress("10.0.0.1", 200);
    CHECK(m.ok && m.ended == 2);
    CHECK(!r.identify(a, 200, false, who));
    CHECK(!r.identify(c, 200, false, who));
    CHECK(r.identify(b, 200, false, who));
    JoinResult j{};
    join(r, "Zed", "10.0.0.1", 201, &j);
    CHECK(j == JoinResult::BannedAddress);
    m = r.banName("BOB", 202);
    CHECK(m.ok && m.ended == 1);
    CHECK(!r.identify(b, 202, false, who));
    join(r, "bob", "10.0.0.50", 203, &j);
    CHECK(j == JoinResult::BannedName);
    CHECK(r.isBannedName("Bob") && r.isBannedAddress("10.0.0.1"));
    CHECK(!r.banName("bad<name>", 204).ok);
    CHECK(!r.banAddress("", 204).ok);
    CHECK(r.unbanName("bob", 205) && r.unbanAddress("10.0.0.1", 205));
    CHECK(!r.unbanName("bob", 205));
    CHECK(!join(r, "bob", "10.0.0.50", 206).empty());
    CHECK(!join(r, "Zed", "10.0.0.1", 206).empty());
    // Kick ends a session but bans nothing.
    CHECK(r.kick("zed", 207).ended == 1);
    CHECK(!r.isBannedName("zed"));
    CHECK(!join(r, "zed", "10.0.0.1", 208).empty());
    CHECK(r.kick("nobody", 209).ended == 0);
    CHECK(!r.kick("", 209).ok);
    // The ban lists are bounded.
    WebRoom big;
    big.configure(on());
    for (std::size_t i = 0; i < kMaxBansPerList; ++i) { CHECK(big.banAddress("192.0.2." + std::to_string(i), 1).ok); }
    CHECK(!big.banAddress("198.51.100.1", 1).ok);
    CHECK(big.banAddress("192.0.2.0", 1).ok);   // an address already on the list is not "one more"
}

void testChat() {
    WebRoom r;
    RoomConfig c = on();
    r.configure(c);
    Identity bob;
    bob.role = Role::Listener;
    bob.name = "Bob";
    CHECK(r.postChat(bob, "hi", 100) == ChatResult::Disabled);   // OFF by default
    c.chatEnabled = true;
    r.configure(c);
    CHECK(r.postChat(bob, "hi", 100) == ChatResult::Ok);
    CHECK(r.postChat(bob, "<img src=x onerror=alert(1)> & \"q\"", 101) == ChatResult::Ok);
    CHECK(r.postChat(bob, "third", 102) == ChatResult::Ok);
    CHECK(r.postChat(bob, "fourth", 103) == ChatResult::RateLimited);   // 3 per 10 s
    CHECK(r.postChat(bob, "fourth", 110) == ChatResult::Ok);            // the first has left the window
    Identity alice = bob;
    alice.name = "Alice";
    CHECK(r.postChat(alice, "mine", 103) == ChatResult::Ok);            // the limit is per person
    CHECK(r.postChat(alice, "", 104) == ChatResult::Empty);
    CHECK(r.postChat(alice, std::string(201, 'x'), 104) == ChatResult::TooLong);
    CHECK(r.postChat(alice, "\xff", 104) == ChatResult::BadText);
    Identity none;
    CHECK(r.postChat(none, "hi", 104) == ChatResult::Disabled);          // not signed in
    const std::vector<ChatLine> lines = r.chatSince(0);
    CHECK(lines.size() == 5);
    bool escaped = false;
    for (const ChatLine& l : lines) {
        CHECK(l.html.find('<') == std::string::npos && l.html.find('>') == std::string::npos && l.html.find('"') == std::string::npos);
        escaped = escaped || l.html == "&lt;img src=x onerror=alert(1)&gt; &amp; &quot;q&quot;";
    }
    CHECK(escaped);
    CHECK(r.chatSince(r.chatSeq()).empty());
    CHECK(r.chatSince(lines[2].seq).size() == 2);
    // The owner speaks as "owner", and the word cannot be a listener's name (the name set has no '*', and 'owner' is the account's).
    Identity owner;
    owner.role = Role::Owner;
    CHECK(r.postChat(owner, "welcome", 105) == ChatResult::Ok);
    CHECK(r.chatSince(0).back().owner && r.chatSince(0).back().name == "owner");
    // The ring keeps kChatRing lines and no more.
    for (std::int64_t t = 1000; t < 1000 + 4 * static_cast<std::int64_t>(kChatRing); t += 11) {
        for (int k = 0; k < 3; ++k) { r.postChat(owner, "x" + std::to_string(t) + std::to_string(k), t); }
    }
    CHECK(r.chatSince(0).size() == kChatRing);
}

void testNote() {
    WebRoom r;
    r.configure(on());
    CHECK(r.note().seq == 0 && r.note().html.empty());
    CHECK(r.setNote("Closing at <noon> & later", 100) == ChatResult::Ok);
    CHECK(r.note().html == "Closing at &lt;noon&gt; &amp; later" && r.note().seq == 1);
    CHECK(r.setNote(std::string(201, 'x'), 100) == ChatResult::TooLong);
    CHECK(r.note().seq == 1);
    CHECK(r.setNote("", 101) == ChatResult::Ok);
    CHECK(r.note().html.empty() && r.note().seq == 2);
}

void testAuditCarriesNoContent() {
    WebRoom r;
    RoomConfig c = on();
    c.chatEnabled = true;
    r.configure(c);
    const std::string tok = join(r, "Bob", "203.0.113.77");
    Identity who;
    CHECK(r.identify(tok, 100, true, who));
    r.postChat(who, "SECRET-CHAT-TEXT", 101);
    r.setNote("SECRET-NOTE-TEXT", 102);
    r.banAddress("203.0.113.77", 103);
    r.audit(AuditKind::RefusedOp, "Bob", 104);
    const std::vector<AuditEvent> ev = r.auditSince(0);
    CHECK(ev.size() >= 5);
    for (const AuditEvent& e : ev) {
        CHECK(e.name.find("203.0.113") == std::string::npos);
        CHECK(e.name.find("SECRET") == std::string::npos);
        CHECK(std::string(auditWord(e.kind)).find("SECRET") == std::string::npos);
    }
    // The ring is bounded.
    for (int i = 0; i < 500; ++i) { r.audit(AuditKind::RefusedOp, "x", 200 + i); }
    CHECK(r.auditSince(0).size() == kAuditRing);
}

void testListenerMayTune() {
    using F = void (*)(ControlRequest&);
    // Every field of the request, one at a time. The five tuning fields are the only ones a listener may ever set.
    const std::vector<std::pair<const char*, F>> tuning = {
        {"centerHz", [](ControlRequest& q) { q.centerHz = 100e6; }},
        {"vfoOffsetHz", [](ControlRequest& q) { q.vfoOffsetHz = 1e3; }},
        {"mode", [](ControlRequest& q) { q.mode = cascade::dsp::DemodMode::NFM; }},
        {"bandwidthHz", [](ControlRequest& q) { q.bandwidthHz = 12500; }},
        {"bookmarkTune", [](ControlRequest& q) { q.bookmarkTune = 0; }},
    };
    const std::vector<std::pair<const char*, F>> refused = {
        {"running", [](ControlRequest& q) { q.running = false; }},
        {"squelchDb", [](ControlRequest& q) { q.squelchDb = -50; }},
        {"volume", [](ControlRequest& q) { q.volume = 1; }},
        {"muted", [](ControlRequest& q) { q.muted = true; }},
        {"dbMin", [](ControlRequest& q) { q.dbMin = -100; }},
        {"dbMax", [](ControlRequest& q) { q.dbMax = 0; }},
        {"deemphasisIndex", [](ControlRequest& q) { q.deemphasisIndex = 1; }},
        {"nrEnabled", [](ControlRequest& q) { q.nrEnabled = true; }},
        {"nrStrength", [](ControlRequest& q) { q.nrStrength = 0.5; }},
        {"notchEnabled", [](ControlRequest& q) { q.notchEnabled = true; }},
        {"notchFreqHz", [](ControlRequest& q) { q.notchFreqHz = 1000; }},
        {"notchQ", [](ControlRequest& q) { q.notchQ = 30; }},
        {"autoNotch", [](ControlRequest& q) { q.autoNotch = true; }},
        {"stereoEnabled", [](ControlRequest& q) { q.stereoEnabled = true; }},
        {"sourceKind", [](ControlRequest& q) { q.sourceKind = "siggen"; }},
        {"soapyArgs", [](ControlRequest& q) { q.soapyArgs = "driver=rtlsdr"; }},
        {"antenna", [](ControlRequest& q) { q.antenna = "RX"; }},
        {"sampleRateHz", [](ControlRequest& q) { q.sampleRateHz = 2e6; }},
        {"gainName", [](ControlRequest& q) { q.gainName = "LNA"; }},
        {"gainDb", [](ControlRequest& q) { q.gainDb = 10; }},
        {"agc", [](ControlRequest& q) { q.agc = true; }},
        {"scanDevices", [](ControlRequest& q) { q.scanDevices = true; }},
        {"recordIq", [](ControlRequest& q) { q.recordIq = true; }},
        {"recordAudio", [](ControlRequest& q) { q.recordAudio = true; }},
        {"bookmarkAdd", [](ControlRequest& q) { q.bookmarkAdd = "x"; }},
        {"bookmarkRemove", [](ControlRequest& q) { q.bookmarkRemove = 0; }},
        {"scannerActive", [](ControlRequest& q) { q.scannerActive = true; }},
        {"scannerSkip", [](ControlRequest& q) { q.scannerSkip = true; }},
        {"scanStartHz", [](ControlRequest& q) { q.scanStartHz = 1e6; }},
        {"scanStopHz", [](ControlRequest& q) { q.scanStopHz = 2e6; }},
        {"scanStepHz", [](ControlRequest& q) { q.scanStepHz = 1e3; }},
        {"pluginFetch", [](ControlRequest& q) { q.pluginFetch = true; }},
        {"pluginInstall", [](ControlRequest& q) { q.pluginInstall = "id"; }},
        {"acknowledgeNotice", [](ControlRequest& q) { q.acknowledgeNotice = true; }},
        {"pluginRemove", [](ControlRequest& q) { q.pluginRemove = "x.dll"; }},
        {"pluginTuneName", [](ControlRequest& q) { q.pluginTuneName = "p"; }},
        {"pluginTuneAllowed", [](ControlRequest& q) { q.pluginTuneAllowed = true; }},
        {"pluginPresetName", [](ControlRequest& q) { q.pluginPresetName = "p"; }},
        {"pluginPresetIndex", [](ControlRequest& q) { q.pluginPresetIndex = 0; }},
        {"transmitPtt", [](ControlRequest& q) { q.transmitPtt = true; }},
    };
    CHECK(!listenerMayTune(ControlRequest{}));
    for (const auto& f : tuning) {
        ControlRequest q;
        f.second(q);
        CHECK(!q.empty());
        const bool ok = listenerMayTune(q);
        if (!ok) { std::printf("tuning field refused: %s\n", f.first); }
        CHECK(ok);
    }
    ControlRequest every;
    for (const auto& f : refused) {
        ControlRequest q;
        f.second(q);
        // the lambda really sets something ControlRequest::empty() sees (acknowledgeNotice alone is the one it does not list: it does
        // nothing without pluginInstall, and listenerMayTune names it itself)
        CHECK(!q.empty() || std::string(f.first) == "acknowledgeNotice");
        const bool ok = listenerMayTune(q);
        if (ok) { std::printf("NOT refused: %s\n", f.first); }
        CHECK(!ok);
        // ... and riding along with a legitimate tuning field changes nothing.
        ControlRequest mixed;
        mixed.centerHz = 100e6;
        f.second(mixed);
        CHECK(!listenerMayTune(mixed));
        f.second(every);
    }
    for (const auto& f : tuning) { f.second(every); }
    CHECK(!listenerMayTune(every));
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void testSourceOpensNoFileAndPrintsNothing() {
    for (const char* f : {"src/net/web_users.cpp", "src/net/web_users.hpp"}) {
        const std::string text = slurp(g_root + "/" + f);
        CHECK(!text.empty());
        for (const char* bad : {"fopen", "ofstream", "fstream", "fprintf", "printf", "puts(", "std::cout", "std::cerr", "stderr", "stdout",
                                "fwrite", "OutputDebugString", "CreateFile", "_open(", "syslog"}) {
            const bool found = text.find(bad) != std::string::npos;
            if (found) { std::printf("%s contains %s\n", f, bad); }
            CHECK(!found);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    g_root = argc > 1 ? argv[1] : "../..";
    testNames();
    testText();
    testDisabledByDefault();
    testJoinRules();
    testConcurrentJoinsAtTheCap();
    testIdentifyAndLimits();
    testOwnerSession();
    testBansEndLiveSessions();
    testChat();
    testNote();
    testAuditCarriesNoContent();
    testListenerMayTune();
    testSourceOpensNoFileAndPrintsNothing();
    return testSummary("test_web_users");
}
