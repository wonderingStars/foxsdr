// extend_logic.cpp - the EXTEND bank's pure logic and the Plugin store page's (extend_logic.hpp): the ImGui window's rules and sentences over the table's lists and facts, with
// no window and no engine. Every sentence is the drawer's own, through cascade::i18n::tr(); a format is composed with formatText/formatUtf8 into a std::string, never into a
// fixed buffer. Each effect function does what the ImGui control does and returns it as data (extend_logic.hpp has the table).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "extend_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string_view>

#include "core/add_all_plan.hpp"
#include "core/i18n.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_repo_data.hpp"
#include "core/plugin_store_reasons.hpp"
#include "core/server_ports.hpp"
#include "core/utf8_text.hpp"
#include "gui/store_first_open.hpp"

namespace foxsdr::slintextend {

using cascade::i18n::tr;
namespace cc = cascade::core;
namespace cmd = cascade::core::cmd;

namespace {

std::string T(const char* english) { return tr(english); }

std::string factOf(TableReader& t, const std::string& key) {
    std::string s;
    if (!t.fact(key, s)) { s.clear(); }
    return s;
}
bool flagOf(TableReader& t, const std::string& key) { return factOf(t, key) == "1"; }
int numberOf(TableReader& t, const std::string& key) { return std::atoi(factOf(t, key).c_str()); }

std::int32_t listSize(TableReader& t, std::uint32_t list) { return std::max<std::int32_t>(0, t.readList(list, 0, nullptr)); }

// Every entry of a list, in order; `got <= i` is the end of a list that changed under the read (what was read stands).
template <typename F>
void eachItem(TableReader& t, std::uint32_t list, F&& f) {
    const std::int32_t n = listSize(t, list);
    for (std::int32_t i = 0; i < n; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (t.readList(list, static_cast<std::uint32_t>(i), &it) <= i) { break; }
        f(i, it);
    }
}

Command make(const cmd::QueuedCommand& q) {
    Command c;
    c.q = q;
    c.grant = foxsdr::slintsignal::grantFor(q.c.op);
    c.valid = true;
    return c;
}
Command make(const FoxCommand& f) {
    cmd::QueuedCommand q;
    q.c = f;
    return make(q);
}
Effect commandEffect(const Command& c) {
    Effect e;
    e.commands.push_back(c);
    return e;
}

// The text cut to `bytes`, never through a character (the window's fixed edit buffers).
std::string cut(const std::string& s, std::size_t bytes) {
    if (s.size() <= bytes) { return s; }
    return s.substr(0, cc::utf8Floor(s.c_str(), bytes));
}

bool blank(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

// A whole number as an InputInt reads it: blanks off both ends, digits only, at most nine of them. False for anything else (an empty box included).
bool parsePort(const std::string& text, int& out) {
    std::size_t lo = 0, hi = text.size();
    while (lo < hi && blank(text[lo])) { ++lo; }
    while (hi > lo && blank(text[hi - 1])) { --hi; }
    if (lo == hi || hi - lo > 9) { return false; }
    int v = 0;
    for (std::size_t i = lo; i < hi; ++i) {
        if (text[i] < '0' || text[i] > '9') { return false; }
        v = v * 10 + (text[i] - '0');
    }
    out = v;
    return true;
}
int clampPort(int v) { return std::clamp(v, cascade::net::kMinPort, cascade::net::kMaxPort); }

bool equalsAscii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) { return false; }
    }
    return true;
}

std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> out;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t eol = text.find('\n', at);
        if (eol == std::string::npos) { eol = text.size(); }
        if (eol > at) { out.push_back(text.substr(at, eol - at)); }
        at = eol + 1;
    }
    return out;
}

// The next TAB-separated field of `rest`, and `rest` moved past it.
std::string_view nextField(std::string_view& rest) {
    const std::size_t tab = rest.find('\t');
    const std::string_view field = rest.substr(0, tab);
    rest = tab == std::string_view::npos ? std::string_view() : rest.substr(tab + 1);
    return field;
}
long toLong(std::string_view s) {
    long v = 0;
    for (const char ch : s) {
        if (ch < '0' || ch > '9') { break; }
        v = v * 10 + (ch - '0');
    }
    return v;
}

const char* const kTransferReason = "a transfer is already in progress";

Tone toneOf(bool amber) { return amber ? Tone::Warn : Tone::Neutral; }

}  // namespace

// =================================================================================================================================
// WEB ACCESS AND CAT CONTROL: the reads

namespace {

struct ServerEntry {
    bool got = false, running = false, fault = false;
    int port = 0, clients = 0, audio = 0;
    unsigned long long commands = 0;
    std::string detail;
};

// serverPanel + readServerLive: the two entries (web first), read at the call.
ServerEntry readServer(TableReader& t, std::uint32_t index) {
    ServerEntry s;
    FoxListItem it{};
    it.structSize = sizeof(it);
    if (t.readList(FOXAPI_LIST_SERVERS, index, &it) <= static_cast<std::int32_t>(index)) { return s; }
    s.got = true;
    s.running = (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
    s.fault = (it.flags & FOXAPI_ITEM_FAULT) != 0u;
    s.port = static_cast<int>(it.value[0]);
    s.clients = static_cast<int>(it.value[1]);
    s.audio = static_cast<int>(it.value[2]);
    s.commands = it.value[3] > 0.0 ? static_cast<unsigned long long>(it.value[3]) : 0ull;
    s.detail = it.detail;
    return s;
}

// serverRefusalText: the entry's detail is the sentence cut at 159 bytes; the policy fact carries it whole.
std::string refusalText(const std::string& listed, const std::string& policy) {
    if (listed.empty()) { return listed; }
    if (policy.size() > listed.size() && policy.compare(0, listed.size(), listed) == 0) { return policy; }
    return listed;
}

int choiceOf(const std::string& bind) {
    return cascade::net::isLoopbackAddress(bind) ? 0 : cascade::net::isWildcardAddress(bind) ? 1 : 2;
}

}  // namespace

WebModel readWeb(TableReader& t, const ReadOptions& o) {
    WebModel m;
    const ServerEntry sv = readServer(t, 0u);
    const bool factEnabled = flagOf(t, "fact.web.enabled");
    std::string factBind = factOf(t, "fact.web.bindAddress");
    if (factBind.empty()) { factBind = "127.0.0.1"; }   // an empty address resets to the default (the config's own rule)
    const int factPort = numberOf(t, "fact.web.port");
    const std::string factUser = factOf(t, "fact.web.username");
    m.has_password = flagOf(t, "fact.web.hasPassword");
    m.reachable = flagOf(t, "fact.web.reachable");
    m.auth_required = flagOf(t, "fact.web.authRequired");
    const std::vector<std::string> addresses = linesOf(factOf(t, "fact.web.addresses"));

    // THE STAGED COPY is what shows while an edit waits for Apply, and while an answer is on its way (syncServerMirrors takes the facts only when neither).
    const bool useDraft = o.web.have && (o.web.dirty || o.server_asks_pending > 0);
    WebDraft d;
    d.have = o.web.have;
    d.dirty = o.web.have && o.web.dirty;
    d.enabled = useDraft ? o.web.enabled : factEnabled;
    d.bind = useDraft ? o.web.bind : factBind;
    d.port_text = useDraft ? o.web.port_text : std::to_string(factPort);
    d.user = useDraft ? o.web.user : factUser;
    d.password = o.web.password;
    d.confirm = o.web.confirm;
    d.error = o.web.error;

    m.running = sv.running;
    m.fault = sv.fault;
    m.listening_port = sv.port;
    m.configured_port = factPort;
    m.clients = sv.clients;
    m.audio_listeners = sv.audio;
    m.commands = sv.commands;

    m.serve = d.enabled;
    m.bind_address = d.bind;
    const int choice = choiceOf(d.bind);
    m.bind_choice = choice;
    for (int i = 0; i < (choice == 2 ? 3 : 2); ++i) {
        SelectItem s;
        s.label = i == 0 ? T("This machine only") : i == 1 ? T("Every network interface") : T("A specific address");
        s.id = i == 0 ? "127.0.0.1" : i == 1 ? "0.0.0.0" : d.bind;
        m.reach.push_back(std::move(s));
    }
    m.reach_index = choice;
    if (choice == 2) { m.address = cc::formatText(tr("address: %s"), d.bind.c_str()); }
    m.port = d.port_text;
    m.user = d.user;
    m.password_status = m.has_password ? T("a password is set") : T("no password set");
    m.password_draft = d.password;
    m.confirm_draft = d.confirm;
    m.set_label = T("Set password");
    m.clear_label = m.has_password ? T("Clear password") : std::string();
    m.clear_enabled = m.has_password;

    // Apply: the window's dirty flag (webDirty_): any edit since the last Apply - an edit typed back to what it was is still one - and nothing else.
    m.apply_enabled = d.dirty;
    m.apply_label = m.apply_enabled ? T("Apply") : T("Applied");

    // OUTCOME: the red line (this window's complaint, else the refusal a start was given), or what a running server says, or "not serving".
    const std::string refusal = d.error.empty() && sv.fault ? refusalText(sv.detail, factOf(t, "fact.web.policy")) : std::string();
    if (!d.error.empty() || !refusal.empty()) {
        m.error = d.error.empty() ? refusal : d.error;
    } else if (sv.running) {
        m.serving_note = m.reachable ? cc::formatText(tr("serving on port %d to every machine on your network"), sv.port)
                                     : cc::formatText(tr("serving at http://127.0.0.1:%d"), sv.port);
        if (m.reachable) {
            m.network_warning = T("reachable from your network");
            for (const std::string& a : addresses) { m.addresses.push_back(cc::formatText("http://%s:%d", a.c_str(), sv.port)); }
            m.http_note = T("This link is plain HTTP: the password is sent unencrypted, so use it on a network you trust. To reach it from the internet, put it behind something "
                            "that terminates TLS rather than forwarding this port.");
        }
        if (m.auth_required) { m.sessions = cc::formatText(tr("%d browser session(s)"), sv.clients); }
        if (sv.audio > 0) { m.listeners = cc::formatText(tr("%d listening to audio"), sv.audio); }
    } else if (d.enabled) {
        m.serving_note = T("not serving");
    }

    // THE RAIL ROW'S CHIP: ON, FAIL (the rust lamp) or OFF.
    m.chip = sv.running ? T("ON") : (sv.fault ? T("FAIL") : T("OFF"));
    m.chip_tone = sv.fault && !sv.running ? Tone::Err : Tone::Good;
    m.lit = sv.running || sv.fault;
    m.draft = d;
    return m;
}

CatModel readCat(TableReader& t, const ReadOptions& o) {
    CatModel m;
    const ServerEntry sv = readServer(t, 1u);
    const bool factEnabled = flagOf(t, "fact.cat.enabled");
    const bool factBindAll = flagOf(t, "fact.cat.bindAll");
    const int factPort = numberOf(t, "fact.cat.port");
    const bool useDraft = o.cat.have && o.server_asks_pending > 0;
    CatDraft d;
    d.have = o.cat.have;
    d.enabled = useDraft ? o.cat.enabled : factEnabled;
    d.bind_all = useDraft ? o.cat.bind_all : factBindAll;
    d.port_text = (useDraft && !o.cat.port_text.empty()) ? o.cat.port_text : std::to_string(factPort);

    m.running = sv.running;
    m.fault = sv.fault;
    m.clients = sv.clients;
    m.commands = sv.commands;
    m.listening_port = sv.port;
    m.intro = T("Lets logging and digital-mode software drive this receiver over the protocol Hamlib's rigctld speaks. Point the client at this machine's address on the port below "
                "and choose a Hamlib \"NET rigctl\" radio.");
    m.accept = d.enabled;
    m.port = d.port_text;
    m.reachable = d.bind_all;
    if (d.bind_all) {
        // Said plainly, because it is a blunter exposure than the web server's: there is no password to add.
        m.warning = T("This protocol has no password. Anything that can reach the port can retune the receiver.");
    }
    if (sv.fault) {
        m.status = sv.detail;
        m.status_tone = Tone::Err;
    } else if (sv.running) {
        int shown = factPort;
        if (!parsePort(d.port_text, shown)) { shown = factPort; }
        m.status = cc::formatText(tr("Listening on %s:%d \xe2\x80\x94 %d client(s), %llu command(s)"), d.bind_all ? "0.0.0.0" : "127.0.0.1", shown, sv.clients, sv.commands);
    } else {
        m.status = T("Not listening.");
    }
    // THE RAIL ROW'S CHIP: FAIL, OFF, "N CLI" with a client on the port, else ON.
    if (sv.fault) {
        m.chip = T("FAIL");
        m.chip_tone = Tone::Err;
    } else if (!sv.running) {
        m.chip = T("OFF");
    } else if (sv.clients > 0) {
        m.chip = cc::formatText(tr("%d CLI"), sv.clients);
    } else {
        m.chip = T("ON");
    }
    m.lit = sv.fault || sv.running;
    m.draft = d;
    return m;
}

// =================================================================================================================================
// THE STORE: the rules (mirrored from gui/plugin_store_view.cpp)

std::string lowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return out;
}

Category categoryFor(const std::string& id) {
    if (id == "aircraft") { return Category::Aircraft; }
    if (id == "marine") { return Category::Marine; }
    if (id == "satellites-weather") { return Category::SatellitesWeather; }
    if (id == "meters-paging") { return Category::MetersPaging; }
    if (id == "broadcast") { return Category::VoiceData; }
    if (id == "maps-tools") { return Category::MapsTools; }
    return Category::Other;
}

std::string categoryHeading(Category c) {
    switch (c) {
        case Category::Aircraft: return T("AIRCRAFT");
        case Category::Marine: return T("MARINE");
        case Category::SatellitesWeather: return T("SATELLITES AND WEATHER");
        case Category::MetersPaging: return T("METERS AND PAGING");
        case Category::VoiceData: return T("VOICE AND DATA");
        case Category::MapsTools: return T("MAPS AND TOOLS");
        case Category::Other: return T("OTHER");
    }
    return T("OTHER");
}

const char* categoryCensusName(Category c) {
    switch (c) {
        case Category::Aircraft: return "aircraft";
        case Category::Marine: return "marine";
        case Category::SatellitesWeather: return "satellites";
        case Category::MetersPaging: return "meters";
        case Category::VoiceData: return "voice";
        case Category::MapsTools: return "maps";
        case Category::Other: return "other";
    }
    return "other";
}

EntryKey keyFor(const StoreEntry& e, const KeyIn& in) {
    EntryKey k;
    // This plugin's own transfer: FITTING..., not a key to press.
    if (!in.busy_id.empty() && in.busy_id == e.id) {
        k.kind = kKeyFitting;
        return k;
    }
    switch (e.install) {
        case InstallKind::Installed:
        case InstallKind::NewerInstalled:
            k.kind = kKeyInstalled;
            return k;
        case InstallKind::UpdateAvailable:
            k.kind = kKeyUpdate;
            if (e.update_to_version.empty()) {
                // Newer by version and not plannable here: no build for this host, or another ABI. The key says so rather than doing nothing.
                k.reason = e.update_blocked_reason;
            } else if (in.busy_any) {
                k.reason = kTransferReason;
            }
            k.enabled = k.reason.empty() && !e.update_to_version.empty();
            return k;
        case InstallKind::NotInstalled:
            break;
    }
    k.kind = kKeyGet;
    // The gate asked as if the notice were acknowledged: whatever it says is a reason a tick cannot cure.
    std::string reason = e.blocked_reason_if_acknowledged;
    if (reason.empty() && !e.legal_notice.empty()) {
        if (in.on_page) {
            if (!in.notice_ticked) { reason = cascade::gui::kLegalNoticeFirstReason; }
        } else {
            // On a card, GET on a plugin with a notice OPENS ITS PAGE: the notice cannot be skipped, and the key is not greyed for it.
            k.opens_page = true;
        }
    }
    k.reason = reason;
    k.enabled = reason.empty();
    return k;
}

const char* keyLabel(int kind) {
    switch (kind) {
        case kKeyGet: return tr("GET");
        case kKeyUpdate: return tr("UPDATE");
        case kKeyFitting: return tr("FITTING...");
        case kKeyInstalled: return tr("INSTALLED");
        default: break;
    }
    return "";
}

bool matchesQuery(const StoreEntry& e, const std::string& lowerQuery) {
    if (lowerQuery.empty()) { return true; }
    const std::string hay = lowerAscii(e.name + " " + e.summary + " " + e.blurb + " " + categoryHeading(categoryFor(e.category)));
    return hay.find(lowerQuery) != std::string::npos;
}

std::vector<int> updateRows(const std::vector<StoreEntry>& entries, const std::string& lowerQuery) {
    std::vector<int> rows;
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        const StoreEntry& e = entries[static_cast<std::size_t>(i)];
        if (e.update_to_version.empty() || !matchesQuery(e, lowerQuery)) { continue; }
        rows.push_back(i);
    }
    std::sort(rows.begin(), rows.end(), [&](int a, int b) {
        return lowerAscii(entries[static_cast<std::size_t>(a)].name) < lowerAscii(entries[static_cast<std::size_t>(b)].name);
    });
    return rows;
}

std::string fromTo(const std::string& from, const std::string& to) { return cc::formatText(tr("%s to %s"), from.c_str(), to.c_str()); }

std::string bytesText(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024ull) { return cc::formatText("%.2f MB", static_cast<double>(bytes) / 1.0e6); }
    if (bytes >= 1000ull) { return cc::formatText("%.0f kB", static_cast<double>(bytes) / 1.0e3); }
    // EXACT UNDER A KILOBYTE, because rounding gets to "0 kB" - the one figure that must never be printed for something that is there.
    return cc::formatText("%llu bytes", static_cast<unsigned long long>(bytes));
}

std::string headerMeta(const StoreEntry& e, const std::string& hostPlatform) {
    std::string out = e.maker.empty() ? T("maker not stated") : e.maker;
    const auto add = [&](const std::string& piece) { out += "  \xc2\xb7  " + piece; };
    add(cc::formatText(tr("version %s"), e.version.empty() ? "?" : e.version.c_str()));
    if (e.have_build_here) {
        if (e.have_size) { add(bytesText(e.size_bytes)); }
        add(hostPlatform.empty() ? e.platforms : hostPlatform);
    } else {
        add(T("no build for this system"));
    }
    // WHERE IT COMES FROM, last (0.99.74): the one fact that is not about the plugin.
    if (e.third_party) { add(cc::formatText(tr("from %s"), e.site_host.empty() ? "?" : e.site_host.c_str())); }
    return out;
}

std::string whatsNewText(const StoreEntry& e) {
    if (!e.whats_new.empty()) { return e.whats_new; }
    return cc::formatText(tr("%s: first release."), e.version.empty() ? "?" : e.version.c_str());
}

namespace {

std::vector<std::string> reachKeys(const StoreEntry& e) {
    std::vector<std::string> r;
    if (!e.have_capabilities) { return r; }
    const std::uint32_t c = e.capabilities;
    if ((c & CASCADE_CAP_DECODER) != 0u) { r.push_back(T("Audio decoder")); }
    if ((c & CASCADE_CAP_IQ_DECODER) != 0u) { r.push_back(T("I/Q decoder")); }
    if ((c & CASCADE_CAP_IMAGE_DECODER) != 0u) { r.push_back(T("Image decoder")); }
    if ((c & CASCADE_CAP_AUDIO_OUT) != 0u) { r.push_back(T("Plays sound through FoxSDR")); }
    if ((c & CASCADE_CAP_AUDIO_PROCESSOR) != 0u) { r.push_back(T("Audio processor")); }
    if ((c & CASCADE_CAP_TX_AUDIO) != 0u) { r.push_back(T("Can transmit through FoxSDR")); }
    if ((c & CASCADE_CAP_TRACK_SOURCE) != 0u) { r.push_back(T("Map targets")); }
    if ((c & CASCADE_CAP_PANEL) != 0u) { r.push_back(T("A window of its own")); }
    if ((c & CASCADE_CAP_INSTRUMENT) != 0u) { r.push_back(T("An instrument of its own")); }
    if ((c & CASCADE_CAP_PRESET) != 0u) { r.push_back(T("Presets")); }
    if ((c & CASCADE_CAP_HOST_CLIENT) != 0u) { r.push_back(T("Can ask to move the receiver")); }
    if ((c & CASCADE_CAP_RECEIVER_LOCATOR) != 0u) { r.push_back(T("Can read your receiver's locator")); }
    if ((c & CASCADE_CAP_BASEMAP) != 0u) { r.push_back(T("Map imagery")); }
    if ((c & CASCADE_CAP_TRACK_INFO) != 0u) { r.push_back(T("Target look-up")); }
    if (r.empty()) { r.push_back(c == 0u ? T("Declares nothing") : T("Declares a capability this build does not know")); }
    return r;
}

enum class PlateState { NotFitted, Refused, Stopped, NoSignal, Started };
PlateState plateState(const StoreEntry& e) {
    constexpr std::uint32_t kSignalCaps = CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER | CASCADE_CAP_IMAGE_DECODER;
    if (!e.fitted) { return PlateState::NotFitted; }
    if (!e.loaded) { return PlateState::Refused; }
    if (!e.running) { return PlateState::Stopped; }
    if (e.have_capabilities && (e.capabilities & kSignalCaps) == 0u) { return PlateState::NoSignal; }
    return PlateState::Started;
}

std::string machineText(const StoreEntry& e) {
    switch (plateState(e)) {
        case PlateState::NotFitted: return T("not fitted");
        case PlateState::Refused: return T("fitted, refused");
        case PlateState::Stopped: return T("fitted, stopped");
        case PlateState::NoSignal: return T("fitted, takes no signal");
        case PlateState::Started: return T("fitted and started");
    }
    return T("not fitted");
}

PageFact hatch(const char* key, const std::string& why) {
    PageFact x;
    x.key = key;
    x.value = why;
    x.hatched = true;
    return x;
}
PageFact fact(const std::string& key, const std::string& value, bool copyable = false, Tone tone = Tone::Neutral) {
    PageFact x;
    x.key = key;
    x.value = value;
    x.copyable = copyable;
    x.tone = tone;
    return x;
}

}  // namespace

std::string reachesLine(const StoreEntry& e) {
    if (!e.have_capabilities) { return (e.fitted && !e.loaded) ? T("not known: the host did not accept this file") : T("not declared until it is fitted"); }
    std::string out;
    for (const std::string& k : reachKeys(e)) {
        if (!out.empty()) { out += ", "; }
        out += k;
    }
    return out;
}

std::vector<PageFact> pageFacts(const StoreEntry& e) {
    std::vector<PageFact> f;
    const bool read = e.have_descriptor;
    {
        PageFact x = fact(T("MAKER"), e.maker);
        if (!read) { x = hatch(tr("MAKER"), T("not read")); } else if (e.maker.empty()) { x = hatch(tr("MAKER"), T("not stated")); }
        f.push_back(x);
    }
    {
        // "No licence" is a decision, not a blank - but only where one was looked for.
        PageFact x = fact(T("LICENCE"), e.licence);
        if (!read) {
            x = hatch(tr("LICENCE"), T("not read"));
        } else if (e.licence.empty()) {
            x = hatch(tr("LICENCE"), T("none declared"));
            x.tone = Tone::Warn;
        }
        f.push_back(x);
    }
    {
        PageFact x = fact(T("VERSION"), e.version, false, Tone::Warn);
        if (!read || e.version.empty()) { x = hatch(tr("VERSION"), read ? T("not stated") : T("not read")); }
        f.push_back(x);
    }
    {
        PageFact x = fact(T("PLUGIN ABI"), std::string());
        if (!e.have_abi) {
            // abiVersion 0 means "not recorded": unknown, never a mismatch.
            x = hatch(tr("PLUGIN ABI"), T("not recorded"));
        } else if (e.abi_version == e.host_abi_version) {
            x.value = cc::formatText(tr("%u, matches this build"), e.abi_version);
            x.tone = Tone::Good;
        } else {
            x.value = cc::formatText(tr("%u, this build needs %u"), e.abi_version, e.host_abi_version);
            x.tone = Tone::Warn;
        }
        f.push_back(x);
    }
    {
        PageFact x = fact(T("DOWNLOAD"), std::string(), false, Tone::Warn);
        if (e.have_size) { x.value = bytesText(e.size_bytes); } else { x = hatch(tr("DOWNLOAD"), T("not stated")); }
        f.push_back(x);
    }
    {
        PageFact x = fact(T("BUILDS FOR"), e.platforms);
        if (e.platforms.empty()) { x = hatch(tr("BUILDS FOR"), T("not stated")); }
        f.push_back(x);
    }
    {
        PageFact x = fact(T("REACHES"), reachesLine(e));
        if (!e.have_capabilities) { x.hatched = true; }
        f.push_back(x);
    }
    {
        PageFact x = fact(T("HOMEPAGE"), e.homepage, true);
        if (e.homepage.empty()) { x = hatch(tr("HOMEPAGE"), T("not stated")); }
        f.push_back(x);
    }
    {
        PageFact x = fact(T("SHA-256"), e.sha256, true, Tone::Warn);
        if (e.sha256.empty()) { x = hatch(tr("SHA-256"), T("not stated")); }
        f.push_back(x);
    }
    {
        PageFact x = fact(T("PUBLISHED"), e.published);
        if (e.published.empty()) { x = hatch(tr("PUBLISHED"), T("not stated")); }
        f.push_back(x);
    }
    if (e.fitted) {
        PageFact x = fact(T("ON THIS MACHINE"), machineText(e));
        x.tone = plateState(e) == PlateState::Refused ? Tone::Err : Tone::Neutral;
        f.push_back(x);
        if (!e.file_name.empty()) { f.push_back(fact(T("FILE"), e.file_name, true)); }
    }
    if (!e.floor_version.empty()) { f.push_back(fact(T("RETIRED BELOW"), e.floor_version, false, Tone::Warn)); }
    return f;
}

std::string cleanupKeyLabel(std::size_t count) { return cc::formatText(tr("CLEAN UP OLD VERSIONS (%zu)"), count); }

std::string oldCopyLine(const std::string& name, const std::string& version, const std::string& file, const std::string& keptVersion) {
    return cc::formatText(tr("%s %s - %s (%s stays)"), name.c_str(), version.c_str(), file.c_str(), keptVersion.c_str());
}

std::string cleanupConfirmLabel(std::size_t count) {
    return count == 1 ? T("Remove 1 file") : cc::formatText(tr("Remove %zu files"), count);
}

std::string busyLabel(int kind, const std::string& name) {
    if (kind == 2) { return cc::formatText(tr("downloading %s"), name.c_str()); }   // one format string, so a translation can put the name where it goes
    return kind == 1 ? T("fetching the catalogue") : std::string();
}

std::string addAllLine(std::uint64_t position, std::uint64_t total, const std::string& name) {
    // "installing 4 of 23: GOES Weather Satellites (HRIT / LRIT)" - the position AND the name, because a bar with no name cannot tell a user which module is the slow one.
    return cc::formatText(tr("installing %zu of %zu: %s"), static_cast<std::size_t>(position), static_cast<std::size_t>(total), name.empty() ? tr("starting") : name.c_str());
}

std::string siteConsentText() {
    return T("Plugins from this site are not reviewed by FoxSDR. They run with the same access as this application. Only add a site you trust.");
}

std::string sitesLeftOutNote() {
    return T("Plugins from sites you added are not included here. Fit each one from its own page, after reading its notice, and update each with its own UPDATE key.");
}

std::string checkSiteAdd(const std::string& raw, const std::vector<std::string>& siteUrls, std::size_t maxSites, const std::string& officialUrl, std::string& normalised) {
    normalised.clear();
    std::string url, why;
    if (!cc::normalisePluginSiteUrl(raw, url, why)) {
        return T("That is not a site address. It must start with https:// (or be the path of an index.json on this computer).");
    }
    // The official catalogue is already listed, by definition: a second copy of it as a "site" would list every plugin twice, all hidden as the same id.
    bool already = url == officialUrl || url == cc::defaultPluginIndexUrl();
    for (const std::string& s : siteUrls) {
        if (s == url) { already = true; }
    }
    if (already) { return T("That site is already in the list."); }
    if (siteUrls.size() >= maxSites) { return T("The list is full. Remove a site before adding another."); }
    normalised = url;
    return {};
}

namespace {

std::string dateText(std::int64_t t) {
    std::tm tm{};
    const std::time_t tt = static_cast<std::time_t>(t);
#if defined(_WIN32)
    const bool ok = t > 0 && localtime_s(&tm, &tt) == 0;
#else
    const bool ok = t > 0 && localtime_r(&tt, &tm) != nullptr;
#endif
    if (!ok) { return T("an unknown date"); }
    return cc::formatText("%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

// "HH:MM" for a read made today, "YYYY-MM-DD HH:MM" for an earlier day.
std::string readTimeText(std::int64_t t, std::int64_t now) {
    std::tm tm{}, nowTm{};
    const std::time_t tt = static_cast<std::time_t>(t), nn = static_cast<std::time_t>(now);
#if defined(_WIN32)
    const bool ok = t > 0 && localtime_s(&tm, &tt) == 0;
    const bool haveNow = localtime_s(&nowTm, &nn) == 0;
#else
    const bool ok = t > 0 && localtime_r(&tt, &tm) != nullptr;
    const bool haveNow = localtime_r(&nn, &nowTm) != nullptr;
#endif
    if (!ok) { return T("an unknown date"); }
    const bool today = haveNow && nowTm.tm_year == tm.tm_year && nowTm.tm_yday == tm.tm_yday;
    if (today) { return cc::formatText("%02d:%02d", tm.tm_hour, tm.tm_min); }
    return cc::formatText("%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

}  // namespace

StoreLine catalogueLine(bool haveCatalogue, bool fromCache, bool refreshFailed, const std::string& sourceError, std::int64_t readTime, std::int64_t nowUnix) {
    (void)fromCache;
    StoreLine l;
    if (!haveCatalogue) {
        l.text = T("CATALOGUE NOT READ");
        return l;
    }
    if (refreshFailed && !sourceError.empty()) {
        // THE COPY ON SCREEN IS NOT THE LATEST, and the reason is the user's evidence, verbatim: a kept copy from an earlier session, or an earlier read of this one.
        l.text = cc::formatText(tr("Catalogue from %s; could not refresh: %s"), dateText(readTime).c_str(), sourceError.c_str());
        l.tone = Tone::Warn;
        return l;
    }
    l.text = cc::formatText(tr("Catalogue read %s"), readTimeText(readTime, nowUnix).c_str());
    return l;
}

// =================================================================================================================================
// THE STORE: the read

namespace {

struct SiteRow {
    std::string url, label, display, error;
    int state = 0;   // 0 not read, 1 kept, 2 read, 3 failed
    int plugins = 0, hiddenSameId = 0, hiddenOffHost = 0;
};

std::vector<SiteRow> parseSites(const std::string& text) {
    std::vector<SiteRow> out;
    for (const std::string& lineText : linesOf(text)) {
        std::string_view line(lineText);
        SiteRow s;
        const std::string_view url = nextField(line);
        s.url.assign(url.data(), url.size());
        s.label = cc::pluginSiteLabel(s.url);
        s.display = cc::pluginSiteDisplay(s.url, 90);
        s.state = static_cast<int>(toLong(nextField(line)));
        s.plugins = static_cast<int>(toLong(nextField(line)));
        s.hiddenSameId = static_cast<int>(toLong(nextField(line)));
        s.hiddenOffHost = static_cast<int>(toLong(nextField(line)));
        const std::string_view err = nextField(line);
        s.error.assign(err.data(), err.size());
        out.push_back(std::move(s));
    }
    return out;
}

// storeSiteLines: one status line per site (two or three when the merge hid something).
std::vector<StoreLine> siteLines(const SiteRow& s) {
    std::vector<StoreLine> out;
    const std::string who = s.label.empty() ? s.url : s.label;
    switch (s.state) {
        case 1:
            out.push_back({cc::formatText(s.plugins == 1 ? tr("%s: %d plugin (kept copy from an earlier session)") : tr("%s: %d plugins (kept copy from an earlier session)"),
                                          who.c_str(), s.plugins),
                           Tone::Neutral});
            break;
        case 2:
            out.push_back({cc::formatText(s.plugins == 1 ? tr("%s: %d plugin") : tr("%s: %d plugins"), who.c_str(), s.plugins), Tone::Neutral});
            break;
        case 3:
            // THE REASON IS THE USER'S EVIDENCE, verbatim, as the catalogue's is.
            out.push_back({cc::formatText(tr("%s: could not be read: %s"), who.c_str(), s.error.c_str()), Tone::Warn});
            break;
        default:
            out.push_back({cc::formatText(tr("%s: not read yet"), who.c_str()), Tone::Neutral});
            break;
    }
    if (s.hiddenSameId > 0) {
        out.push_back({cc::formatText(tr("%s: %d hidden: same id as a plugin already listed"), who.c_str(), s.hiddenSameId), Tone::Warn});
    }
    if (s.hiddenOffHost > 0) {
        out.push_back({cc::formatText(tr("%s: %d hidden: files not on the site's own host"), who.c_str(), s.hiddenOffHost), Tone::Warn});
    }
    return out;
}

struct OldCopy {
    std::string file, name, version, keptVersion;
};
std::vector<OldCopy> parseOldCopies(const std::string& text) {
    std::vector<OldCopy> out;
    for (const std::string& lineText : linesOf(text)) {
        std::string_view line(lineText);
        OldCopy o;
        const std::string_view file = nextField(line), name = nextField(line), version = nextField(line);
        (void)nextField(line);   // the kept FILE: the card says only which version stays
        const std::string_view kept = nextField(line);
        o.file.assign(file.data(), file.size());
        o.name.assign(name.data(), name.size());
        o.version.assign(version.data(), version.size());
        o.keptVersion.assign(kept.data(), kept.size());
        out.push_back(std::move(o));
    }
    return out;
}

struct PictureNow {
    std::string sha256, path, reason;
    int state = kPicNone;
};

struct HostRecord {
    std::string file;
    std::uint32_t caps = 0;
    bool loaded = false, stopped = false, tune = false;
};

// One CATALOGUE entry as the table gives it, before it is a StoreEntry (StoreRow).
struct CatRow {
    std::string id, name;
    std::uint32_t abi = 0;
    std::uint64_t sizeBytes = 0;
    bool installed = false, compatible = false, buildHere = false;
    std::string version, author, licence, description, summary, homepage, legal, floor, platforms, file, blocked, updateReason;
    std::string category, whatsNew, published, sha256, site, installState, installedVersion, installedFile;
    bool experimental = false, regional = false, haveCapabilities = false;
    std::uint32_t capabilities = 0;
    std::vector<PictureFrame> pictures;
    bool hasPlan = false;
    bool haveHost = false;
    std::string hostFile, hostError;
    bool hostLoaded = false, hostStopped = false, hostTune = false;
    std::uint32_t hostCaps = 0;
};

std::uint64_t foldBytes(std::uint64_t h, const void* data, std::size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) { h = (h ^ b[i]) * 1099511628211ull; }
    return h;
}
std::uint64_t foldText(std::uint64_t h, const std::string& s) {
    h = foldBytes(h, s.data(), s.size());
    return (h ^ 0xffu) * 1099511628211ull;   // (a separator: "ab","c" is not "a","bc")
}
std::uint64_t foldRow(std::uint64_t h, const CatRow& r) {
    h = foldText(h, r.id);
    h = foldText(h, r.name);
    h = foldText(h, r.version);
    h = foldText(h, r.author);
    h = foldText(h, r.licence);
    h = foldText(h, r.description);
    h = foldText(h, r.summary);
    h = foldText(h, r.homepage);
    h = foldText(h, r.legal);
    h = foldText(h, r.floor);
    h = foldText(h, r.platforms);
    h = foldBytes(h, &r.abi, sizeof(r.abi));
    h = foldBytes(h, &r.sizeBytes, sizeof(r.sizeBytes));
    return h;
}

// fillStoreModule: a StoreEntry from a row.
StoreEntry entryOf(const CatRow& r) {
    StoreEntry e;
    e.id = r.id;
    e.third_party = !r.site.empty();
    e.site_host = e.third_party ? cc::pluginSiteLabel(r.site) : std::string();
    e.have_descriptor = true;
    e.name = r.name;
    e.version = r.version;
    e.maker = r.author;
    e.licence = r.licence;
    e.blurb = r.description.empty() ? r.summary : r.description;
    e.summary = r.summary;
    e.homepage = r.homepage;
    // A plugin from a site the user added ALWAYS carries a notice (0.99.74): that it comes from somewhere else and has not been reviewed, then the maker's own legal notice if there
    // is one, verbatim.
    if (e.third_party) {
        e.legal_notice = cc::formatText(tr("This plugin comes from %s, not from the FoxSDR catalogue. FoxSDR has not reviewed it."), e.site_host.empty() ? "?" : e.site_host.c_str());
        if (!r.legal.empty()) { e.legal_notice += "\n\n" + r.legal; }
    } else {
        e.legal_notice = r.legal;
    }
    // WHERE IT COMES FROM, one muted line under the page's header.
    if (r.regional) {
        e.origin_note = T("Offered by foxsdr.com in some countries only.");
    } else if (e.third_party) {
        e.origin_note = cc::formatText(tr("From a site you added: %s"), cc::pluginSiteDisplay(r.site, 90).c_str());
    }
    e.category = r.category;
    e.experimental = r.experimental;
    e.whats_new = r.whatsNew;
    e.published = r.published;
    e.have_abi = true;
    e.abi_version = r.abi;
    e.host_abi_version = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
    e.floor_version = r.floor;
    e.platforms = r.platforms;
    {
        std::string_view rest(r.platforms);
        while (!rest.empty()) {
            const std::size_t comma = rest.find(", ");
            const std::string_view one = rest.substr(0, comma);
            const std::string os = lowerAscii(std::string(one.substr(0, one.find('/'))));
            if (os == "windows") { e.builds_windows = true; }
            if (os == "linux") { e.builds_linux = true; }
            if (comma == std::string_view::npos) { break; }
            rest.remove_prefix(comma + 2);
        }
    }
    e.have_build_here = r.buildHere;
    e.sha256 = r.sha256;
    e.have_size = r.sizeBytes > 0u;   // ADVISORY, AND ONLY WHEN STATED: 0 is "the index did not say", never an empty file
    e.size_bytes = r.sizeBytes;
    e.installable_here = r.compatible && r.buildHere;
    e.have_capabilities = r.haveCapabilities;
    e.capabilities = r.haveCapabilities ? r.capabilities : 0u;
    if (r.installState == "installed") {
        e.install = InstallKind::Installed;
    } else if (r.installState == "update") {
        e.install = InstallKind::UpdateAvailable;
    } else if (r.installState == "newer") {
        e.install = InstallKind::NewerInstalled;
    }
    e.installed_version = r.installedVersion;
    e.fitted = r.installed;
    if (r.haveHost) {
        e.file_name = r.hostFile;
        e.loaded = r.hostLoaded;
        e.have_capabilities = r.hostLoaded;
        e.capabilities = r.hostCaps;
        e.running = r.hostLoaded && !r.hostStopped;
        if (r.hostLoaded && (r.hostCaps & CASCADE_CAP_HOST_CLIENT) != 0u) {
            e.have_tune_grant = true;
            e.tune_granted = r.hostTune;
        }
    }
    e.blocked_reason_if_acknowledged = r.blocked;
    if (r.hasPlan) {
        e.update_to_version = r.version;
        e.update_reason = r.updateReason;
    }
    // An update by version that the planner has no plan for (no build for this host, another ABI): the UPDATE key is greyed and says why.
    if (e.install == InstallKind::UpdateAvailable && e.update_to_version.empty()) {
        if (!r.compatible) {
            e.update_blocked_reason = cascade::gui::pluginAbiMismatchReason(static_cast<unsigned>(r.abi), static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION));
        } else if (!r.buildHere) {
            e.update_blocked_reason = cascade::gui::pluginNoBuildReason(std::string(cc::pluginHostOs()) + "/" + cc::pluginHostArch());
        }
    }
    e.pictures = r.pictures;
    return e;
}

// setStoreBlockedReason
void setBlockedReason(StoreEntry& e, bool acknowledged) {
    if (acknowledged || !e.blocked_reason_if_acknowledged.empty() || e.legal_notice.empty()) {
        e.blocked_reason = e.blocked_reason_if_acknowledged;
    } else {
        e.blocked_reason = cascade::gui::kLegalNoticeFirstReason;
    }
}

// planAddAll: the engine's own rule (core/add_all_plan.hpp) handed the entries' rows.
cascade::engine::AddAllRule planAddAll(const std::vector<StoreEntry>& entries, bool haveCatalogue, bool listedNothing, bool lastCheckFailed, bool busy, bool acknowledged) {
    std::vector<cascade::engine::AddAllRow> rows;
    rows.reserve(entries.size());
    for (const StoreEntry& e : entries) {
        cascade::engine::AddAllRow r;
        r.name = e.name;
        r.fitted = e.fitted;
        r.hasUpdate = !e.update_to_version.empty();
        r.hasNotice = !e.legal_notice.empty();
        r.thirdParty = e.third_party;
        r.onDisk = e.install != InstallKind::NotInstalled;
        r.blockedReason = e.blocked_reason;
        r.blockedReasonIfAcknowledged = e.blocked_reason_if_acknowledged;
        rows.push_back(std::move(r));
    }
    cascade::engine::AddAllCatalogue cat;
    cat.haveCatalogue = haveCatalogue;
    cat.listedNothing = listedNothing;
    cat.lastCheckFailed = lastCheckFailed;
    cat.busy = busy;
    return cascade::engine::planAddAllRows(rows, cat, acknowledged);
}

std::string trReason(const std::string& english) { return cascade::gui::trStoredReason(english); }

StoreBadge badge(const std::string& text, Tone tone) {
    StoreBadge b;
    b.text = text;
    b.tone = tone;
    return b;
}

std::vector<StoreBadge> badgesOf(const StoreEntry& e) {
    std::vector<StoreBadge> out;
    if (e.third_party) { out.push_back(badge(T("THIRD PARTY"), Tone::Warn)); }
    if (e.experimental) { out.push_back(badge(T("EXPERIMENTAL"), Tone::Warn)); }
    // A BUILD badge (WINDOWS ONLY or LINUX ONLY) appears only when this host has NO build for the entry and the entry's builds are all one operating system.
    if (!e.have_build_here) {
        if (e.builds_windows && !e.builds_linux) { out.push_back(badge(T("WINDOWS ONLY"), Tone::Neutral)); }
        if (e.builds_linux && !e.builds_windows) { out.push_back(badge(T("LINUX ONLY"), Tone::Neutral)); }
    }
    return out;
}

int pictureStateOf(double v) { return v == 1.0 ? kPicPending : v == 2.0 ? kPicReady : v == 3.0 ? kPicFailed : kPicNone; }

}  // namespace

StoreRailChip readStoreChip(TableReader& t) {
    StoreRailChip c;
    bool transfer = false;
    eachItem(t, FOXAPI_LIST_STORE_ACTIVITY, [&](int, const FoxListItem& it) {
        if (it.id == 1u) { transfer = true; }
    });
    const bool readThisSession = flagOf(t, "fact.store.readThisSession");
    const std::size_t pending = readThisSession ? static_cast<std::size_t>(listSize(t, FOXAPI_LIST_STORE_UPDATES)) : 0u;
    if (transfer) {
        c.word = T("BUSY");   // A TRANSFER outranks the update count: something is moving over the network on the user's behalf
        c.lit = true;
    } else if (!readThisSession) {
        c.word = T("IDLE");
    } else if (pending > 0u) {
        c.word = cc::formatText(tr("%zu UPD"), pending);
        c.tone = Tone::Warn;
        c.lit = true;
    } else {
        c.word = T("OK");
    }
    return c;
}

StoreModel readStore(TableReader& t, const ReadOptions& o) {
    StoreModel m;
    StoreDraft d = o.store;
    m.search_placeholder = T("Search plugins");
    m.browse_label = T("BROWSE");
    m.sites_label = T("SITES");
    m.site_placeholder = T("index.json URL");
    m.no_sites_line = T("No sites added.");
    m.add_site_label = T("ADD");
    m.consent_sentence = siteConsentText();
    m.cancel_label = T("CANCEL");
    m.cleanup_title = T("Remove old plugin versions");
    m.cleanup_text = T("Each of these is an older copy of a plugin whose newer version is installed and running. Only these files are deleted.");
    m.cleanup_no = T("Keep them");
    m.plugin.back_label = T("< BROWSE");
    m.plugin.screenshots_heading = T("SCREENSHOTS");
    m.plugin.what_heading = T("WHAT IT DOES");
    m.plugin.new_heading = T("WHAT'S NEW");
    m.plugin.details_heading = T("DETAILS");
    m.plugin.notice_heading = T("BEFORE YOU FIT IT");
    m.plugin.notice_tick_label = T("I have read the notice above and accept responsibility");
    m.plugin.reach_lead = T("Declared by the maker, not enforced. A fitted module is loaded into this application's own process and runs with every privilege the application has: "
                            "there is no sandbox and no permission model. This list is what the module says it PROVIDES, not a limit on what it can take.");
    m.search = d.search;
    m.tab = d.tab;
    m.sites_open = d.sites_open;
    m.site_draft = d.site_draft;
    m.consent = d.consent;
    m.site_error = d.site_error;
    m.bulk_tick = d.bulk_tick;
    m.updates_label = cc::formatText(tr("UPDATES (%zu)"), static_cast<std::size_t>(0));
    if (!o.read_store) {
        m.draft = d;
        return m;
    }

    // ---- the rail tier: what the store is doing, the updates planned, the host's records ----
    m.have_catalogue = listSize(t, FOXAPI_LIST_CATALOGUE) > 0;
    m.read_this_session = flagOf(t, "fact.store.readThisSession");
    std::vector<std::string> planIds;
    eachItem(t, FOXAPI_LIST_STORE_UPDATES, [&](int, const FoxListItem& it) { planIds.push_back(it.name); });
    bool transfer = false;
    int transferKind = 0;
    std::string transferName;
    bool addAll = false;
    std::uint64_t addAllPos = 0, addAllTotal = 0;
    std::string addAllName;
    eachItem(t, FOXAPI_LIST_STORE_ACTIVITY, [&](int, const FoxListItem& it) {
        if (it.id == 1u) {
            transfer = true;
            transferKind = static_cast<int>(it.value[1]);
            transferName = it.name;
        } else if (it.id == 2u) {
            addAll = true;
            addAllPos = static_cast<std::uint64_t>(it.value[0] > 0.0 ? it.value[0] : 0.0);
            addAllTotal = static_cast<std::uint64_t>(it.value[1] > 0.0 ? it.value[1] : 0.0);
            addAllName = it.name;
        }
    });
    m.busy = transfer;
    m.busy_kind = transferKind;
    m.busy_label = transfer ? busyLabel(transferKind, transferName) : std::string();
    m.add_all_running = addAll;
    const std::string addAllProgress = addAll ? addAllLine(addAllPos, addAllTotal, addAllName) : std::string();
    std::vector<HostRecord> hosts;
    eachItem(t, FOXAPI_LIST_PLUGINS, [&](int, const FoxListItem& it) {
        HostRecord h;
        h.file = it.detail;
        h.caps = static_cast<std::uint32_t>(it.value[0] > 0.0 ? it.value[0] : 0.0);
        h.loaded = (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
        h.stopped = (it.flags & FOXAPI_ITEM_STOPPED) != 0u;
        h.tune = (it.flags & FOXAPI_ITEM_TUNE_ALLOWED) != 0u;
        hosts.push_back(std::move(h));
    });

    // ---- the full tier ----
    m.source_url = factOf(t, "fact.store.url");
    const std::string hostPlatform = std::string(cc::pluginHostOs()) + "/" + cc::pluginHostArch();
    const bool fromCache = flagOf(t, "fact.store.fromCache");
    const bool refreshFailed = flagOf(t, "fact.store.refreshFailed");
    const std::int64_t readTime = std::strtoll(factOf(t, "fact.store.cacheTime").c_str(), nullptr, 10);
    std::string busyId = factOf(t, "fact.store.busyKey");
    if (!(transfer && transferKind == 2)) { busyId.clear(); }
    const std::string resultId = factOf(t, "fact.store.resultKey");
    const std::vector<SiteRow> sites = parseSites(factOf(t, "fact.store.sites"));
    const std::vector<OldCopy> oldCopies = parseOldCopies(factOf(t, "fact.store.oldCopies"));
    const std::string cleanupReport = factOf(t, "fact.store.cleanupReport");

    std::vector<PictureNow> picturesNow;
    bool pictureBusy = false;
    eachItem(t, FOXAPI_LIST_STORE_PICTURES, [&](int, const FoxListItem& it) {
        PictureNow pn;
        pn.sha256 = it.name;
        pn.state = pictureStateOf(it.value[0]);
        if (pn.state == kPicPending) { pictureBusy = true; }
        const bool cutShort = std::strlen(it.detail) >= sizeof(it.detail) - 1u;
        if (pn.state == kPicReady || pn.state == kPicFailed) {
            std::string& into = pn.state == kPicReady ? pn.path : pn.reason;
            into = cutShort ? factOf(t, "fact.store.picture." + pn.sha256 + (pn.state == kPicReady ? ".path" : ".reason")) : std::string(it.detail);
        }
        picturesNow.push_back(std::move(pn));
    });
    m.picture_busy = pictureBusy;

    std::vector<StoreEntry> entries;
    std::uint64_t hash = 1469598103934665603ull;
    eachItem(t, FOXAPI_LIST_CATALOGUE, [&](int, const FoxListItem& it) {
        CatRow r;
        r.id = it.detail;
        r.name = it.name;
        r.abi = static_cast<std::uint32_t>(it.value[0] > 0.0 ? it.value[0] : 0.0);
        r.sizeBytes = static_cast<std::uint64_t>(it.value[1] > 0.0 ? it.value[1] : 0.0);
        r.installed = (it.flags & FOXAPI_ITEM_INSTALLED) != 0u;
        r.compatible = (it.flags & FOXAPI_ITEM_COMPATIBLE) != 0u;
        r.buildHere = (it.flags & FOXAPI_ITEM_BUILD_HERE) != 0u;
        const auto rowFact = [&](const char* leaf) { return factOf(t, "fact.store." + r.id + "." + leaf); };
        r.version = rowFact("version");
        r.author = rowFact("author");
        r.licence = rowFact("licence");
        r.description = rowFact("description");
        r.summary = rowFact("summary");
        r.homepage = rowFact("homepage");
        r.legal = rowFact("legal");
        r.floor = rowFact("floor");
        r.platforms = rowFact("platforms");
        r.file = rowFact("file");
        r.blocked = rowFact("blocked");
        r.updateReason = rowFact("updateReason");
        r.category = rowFact("category");
        r.whatsNew = rowFact("whatsNew");
        r.published = rowFact("published");
        r.sha256 = rowFact("sha256");
        r.site = rowFact("site");
        r.installState = rowFact("installState");
        r.installedVersion = rowFact("installedVersion");
        r.installedFile = rowFact("installedFile");
        r.experimental = rowFact("experimental") == "1";
        r.regional = rowFact("regional") == "1";
        const std::string caps = rowFact("capabilities");
        r.haveCapabilities = !caps.empty();
        r.capabilities = r.haveCapabilities ? static_cast<std::uint32_t>(std::strtoul(caps.c_str(), nullptr, 10)) : 0u;
        for (const std::string& pl : linesOf(rowFact("pictures"))) {
            std::string_view line(pl);
            PictureFrame p;
            const std::string_view sha = nextField(line);
            const std::string_view caption = nextField(line);
            p.sha256.assign(sha.data(), sha.size());
            p.caption.assign(caption.data(), caption.size());
            for (const PictureNow& pn : picturesNow) {
                if (pn.sha256 != p.sha256) { continue; }
                p.state = pn.state;
                p.path = pn.path;
                p.word = pn.reason;
                break;
            }
            r.pictures.push_back(std::move(p));
        }
        r.hasPlan = !r.id.empty() && std::find(planIds.begin(), planIds.end(), r.id) != planIds.end();
        // THE JOIN to the host's record: the module file of the newest installed copy, else the file this machine's build installs under (ASCII case ignored).
        const std::string& joinFile = !r.installedFile.empty() ? r.installedFile : r.file;
        if (!joinFile.empty()) {
            for (const HostRecord& h : hosts) {
                if (!equalsAscii(h.file, joinFile)) { continue; }
                r.haveHost = true;
                r.hostFile = h.file;
                r.hostLoaded = h.loaded;
                r.hostStopped = h.stopped;
                r.hostTune = h.tune;
                r.hostCaps = h.caps;
                r.hostError = factOf(t, "fact.plugin." + h.file + ".error");
                break;
            }
        }
        hash = foldRow(hash, r);
        entries.push_back(entryOf(r));
    });
    m.catalogue_hash = hash;
    m.hash_valid = true;

    const std::string sourceStatus = factOf(t, "fact.status.catalogStatus");
    const std::string sourceError = factOf(t, "fact.status.catalogError");
    const std::string resultReport = factOf(t, "fact.status.installReport");
    const std::string resultError = factOf(t, "fact.status.installError");
    m.add_all_summary = factOf(t, "fact.status.addAllSummary");
    if (!m.add_all_summary.empty()) {
        eachItem(t, FOXAPI_LIST_STATUS_LINES, [&](int, const FoxListItem& it) {
            if (std::strcmp(it.name, "addAllSummary") == 0) { m.add_all_failed = it.value[0] >= 3.0; }
        });
    }
    // THE PROGRESS, LIVE: the transfer's own figure, which moves with no list moving. 0 while the server has not sent a length.
    float progress = 0.0f;
    if (transfer) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (t.readList(FOXAPI_LIST_STORE_ACTIVITY, 0u, &it) > 0 && it.id == 1u) { progress = static_cast<float>(it.value[0]); }
    }

    // THE TICK BELONGS TO ONE MODULE, the one whose page is open.
    for (StoreEntry& e : entries) { setBlockedReason(e, d.notice_tick && e.id == d.page_id); }
    for (const StoreEntry& e : entries) { m.ids.push_back(e.id); }

    // ---- the page bookkeeping: an id, so a refresh cannot swap the plugin under it ----
    int pageIdx = -1;
    if (!d.page_id.empty()) {
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            if (entries[static_cast<std::size_t>(i)].id == d.page_id) { pageIdx = i; break; }
        }
        // A plugin the catalogue no longer lists has no page to show. Only once a catalogue is on screen: an empty one is "not read", not "gone".
        if (pageIdx < 0 && m.have_catalogue) {
            d.page_id.clear();
            d.notice_tick = false;
            d.details_open = false;
            m.draft_changed = true;
        }
    }

    const std::string query = lowerAscii(d.search);
    int updateCount = 0;
    for (const StoreEntry& e : entries) { if (!e.update_to_version.empty()) { ++updateCount; } }
    m.update_count = updateCount;
    m.updates_label = cc::formatText(tr("UPDATES (%zu)"), static_cast<std::size_t>(updateCount));

    // the top bar's catalogue line and its key
    const StoreLine line = catalogueLine(m.have_catalogue, fromCache, refreshFailed, sourceError, readTime, o.now_unix);
    m.catalogue_line = line.text;
    m.catalogue_tone = line.tone;
    const bool asked = m.have_catalogue || !sourceStatus.empty() || !sourceError.empty();
    m.check_label = asked ? T("CHECK AGAIN") : T("CHECK NOW");
    m.check_enabled = !transfer && !pictureBusy && !m.source_url.empty();

    // the sites panel, and a status line for each site (never on a page)
    std::vector<std::string> siteUrls;
    for (const SiteRow& s : sites) {
        StoreSite ss;
        ss.address = s.display;
        ss.remove_label = T("REMOVE");
        ss.url = s.url;
        ss.remove_enabled = !transfer;   // a transfer is in flight: the list does not change under it
        ss.remove_tooltip = transfer ? T(kTransferReason)
                                     : T("Forgets this site. What you installed from it stays installed, and gets no more updates.");
        m.sites.push_back(std::move(ss));
        siteUrls.push_back(s.url);
        for (StoreLine& l : siteLines(s)) { m.site_lines.push_back(std::move(l)); }
    }
    m.site_urls = siteUrls;
    // WHY ADD IS GREYED, in the order the user can cure it.
    if (transfer) {
        m.add_site_reason = T(kTransferReason);
    } else if (d.site_draft.empty()) {
        m.add_site_reason = T("Type the address of the site's index.json first.");
    } else if (!d.consent) {
        m.add_site_reason = T("Tick the box below first.");
    }
    m.add_site_enabled = m.add_site_reason.empty();

    // the picture of the empty catalogue (catalogueState: NeverAsked / Failed / ReadEmpty / Read)
    const auto emptyState = [&]() {
        if (m.have_catalogue) {
            m.empty_line = T("No plugin matches that search.");
        } else if (!sourceStatus.empty()) {
            m.empty_line = T("The catalogue was read and it lists no plugins.");
        } else if (!sourceError.empty()) {
            m.empty_line = T("The catalogue could not be read:") + " " + sourceError;
            m.empty_tone = Tone::Err;
        } else {
            m.empty_line = T("No catalogue has been read yet. Press CHECK NOW above and this application asks the source once.");
        }
    };

    const bool onPage = pageIdx >= 0;
    const auto nameOf = [&](const std::string& id) {
        for (const StoreEntry& e : entries) {
            if (e.id == id) { return e.name; }
        }
        return std::string();
    };
    if (!onPage) {
        // THE INSTALL ERROR, wherever the user is: a GET on a card that fails must not be silent. A CATALOGUE THAT LOADED WITH A WARNING is a real list with a real reason.
        if (m.have_catalogue && !refreshFailed && !sourceError.empty()) { m.top_warning = sourceError; }
        if (!resultError.empty()) {
            const std::string who = nameOf(resultId);
            m.top_error = (who.empty() ? std::string() : who + ": ") + trReason(resultError);
        }
    }

    // the cleanup foot (the UPDATES tab's) is read for both: the model is cheap and the publisher shows it by tab
    m.cleanup_enabled = !transfer;
    m.cleanup_report = cleanupReport;
    for (std::size_t i = 0; i < oldCopies.size(); ++i) {
        const OldCopy& c = oldCopies[i];
        const std::string text = oldCopyLine(c.name, c.version, c.file, c.keptVersion);
        m.cleanup_all.push_back(text);
        if (i < kCleanupListed) { m.cleanup_lines.push_back(text); }
    }
    if (oldCopies.size() > kCleanupListed) { m.cleanup_lines.push_back(cc::formatText(tr("and %zu more"), oldCopies.size() - kCleanupListed)); }
    if (!oldCopies.empty()) { m.cleanup_label = cleanupKeyLabel(oldCopies.size()); }
    m.cleanup_yes = cleanupConfirmLabel(oldCopies.size());
    // An empty list closes the confirmation (the popup closes itself), and the draft says so: the confirmation must not come back when a later update leaves a copy again.
    if (d.cleanup_open && oldCopies.empty()) {
        d.cleanup_open = false;
        m.draft_changed = true;
    }
    m.cleanup_open = d.cleanup_open;

    KeyIn kin;
    kin.busy_id = busyId;
    kin.busy_any = transfer;

    // ---- THE PLUGIN PAGE ----
    if (onPage) {
        const StoreEntry& e = entries[static_cast<std::size_t>(pageIdx)];
        StorePluginPage& p = m.plugin;
        KeyIn pin = kin;
        pin.on_page = true;
        pin.notice_ticked = d.notice_tick;
        const EntryKey key = keyFor(e, pin);
        p.shown = true;
        p.id = e.id;
        p.glyph = e.category;
        p.name = e.name;
        p.meta = headerMeta(e, hostPlatform);
        p.version = p.meta;
        p.badges = badgesOf(e);
        p.origin = e.origin_note;
        p.key_kind = key.kind;
        p.key_label = keyLabel(key.kind);
        p.key_enabled = key.enabled && key.kind != kKeyInstalled && key.kind != kKeyFitting;
        p.key_reason = (!key.enabled && !key.reason.empty()) ? trReason(key.reason) : std::string();
        if (key.kind == kKeyUpdate) { p.version_note = fromTo(e.installed_version, e.update_to_version.empty() ? e.version : e.update_to_version); }
        if (key.kind == kKeyFitting) {
            p.progress = m.busy_label;
            p.progress_norm = progress;
        }
        // THE INSTALL REPORT AND ERROR, directly UNDER the header of the plugin they concern and of no other: PluginRepo's own words, verbatim.
        if (resultId == e.id) {
            const std::string& txt = resultError.empty() ? resultReport : resultError;
            if (!txt.empty()) {
                p.report = resultError.empty() ? txt : trReason(txt);
                p.report_tone = resultError.empty() ? Tone::Good : Tone::Err;
            }
        }
        if (e.pictures.empty()) {
            PictureFrame f;
            f.word = T("No pictures published yet");
            p.screenshots.push_back(std::move(f));
        } else {
            for (PictureFrame f : e.pictures) {
                if (f.state == kPicReady) {
                    f.word.clear();
                } else if (f.state == kPicFailed) {
                    f.word = trReason(f.word);
                } else {
                    f.word = T("loading...");
                }
                for (char& c : f.caption) {
                    if (c == '\n' || c == '\r' || c == '\t') { c = ' '; }
                }
                p.screenshots.push_back(std::move(f));
            }
        }
        const std::string& blurb = e.blurb.empty() ? e.summary : e.blurb;
        p.what_it_does = blurb.empty() ? T("not stated") : blurb;
        p.whats_new = whatsNewText(e);
        p.facts = pageFacts(e);
        for (const PageFact& f : p.facts) { p.details += (p.details.empty() ? "" : "\n") + f.key + ": " + f.value; }
        p.details_open = d.details_open;
        p.details_label = d.details_open ? T("HIDE DETAILS") : T("SHOW DETAILS");
        p.notice_shown = !e.legal_notice.empty() && e.install == InstallKind::NotInstalled;
        if (p.notice_shown) { p.notice = e.legal_notice; }
        p.notice_tick = d.notice_tick;
        m.draft = d;
        return m;
    }

    // ---- THE LISTS ----
    // (the cards of BROWSE and the rows of UPDATES are built for the tab shown)
    if (d.tab == 0) {
        std::array<std::vector<int>, kCategoryCount> by;
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            const StoreEntry& e = entries[static_cast<std::size_t>(i)];
            if (!matchesQuery(e, query)) { continue; }
            by[static_cast<std::size_t>(categoryFor(e.category))].push_back(i);
        }
        for (int c = 0; c < kCategoryCount; ++c) {
            std::vector<int>& v = by[static_cast<std::size_t>(c)];
            if (v.empty()) { continue; }
            std::sort(v.begin(), v.end(), [&](int a, int b) {
                const StoreEntry& x = entries[static_cast<std::size_t>(a)];
                const StoreEntry& y = entries[static_cast<std::size_t>(b)];
                const std::string la = lowerAscii(x.name), lb = lowerAscii(y.name);
                if (la != lb) { return la < lb; }
                return x.id < y.id;
            });
            StoreCategory sec;
            sec.heading = categoryHeading(static_cast<Category>(c));
            sec.census = categoryCensusName(static_cast<Category>(c));
            for (const int i : v) {
                const StoreEntry& e = entries[static_cast<std::size_t>(i)];
                const EntryKey key = keyFor(e, kin);
                StoreCard card;
                card.id = e.id;
                card.glyph = e.category;
                card.name = e.name.empty() ? T("(unnamed module)") : e.name;
                card.summary = e.summary.empty() ? e.blurb : e.summary;
                card.badges = badgesOf(e);
                card.key_kind = key.kind;
                card.key_label = keyLabel(key.kind);
                card.key_enabled = key.enabled;
                card.opens_page = key.opens_page;
                card.key_reason = (!key.enabled && !key.reason.empty()) ? trReason(key.reason) : std::string();
                card.category_word = sec.heading;
                if (key.kind == kKeyUpdate) { card.version_note = fromTo(e.installed_version, e.update_to_version.empty() ? e.version : e.update_to_version); }
                if (key.kind == kKeyFitting) {
                    card.progress = m.busy_label;
                    card.progress_norm = progress;
                }
                sec.cards.push_back(std::move(card));
            }
            m.categories.push_back(std::move(sec));
        }
        if (m.categories.empty()) { emptyState(); }

        // ---- GET EVERYTHING, at the foot ----
        if (!entries.empty() && query.empty()) {
            const cascade::engine::AddAllRule plan = planAddAll(entries, m.have_catalogue, !sourceStatus.empty(), !sourceError.empty(), transfer, d.bulk_tick);
            int noticeModules = 0;
            std::string noticeNames;
            bool anyThirdParty = false;
            for (const StoreEntry& e : entries) {
                // A site's plugin is not in this run, so its notice is not one this tick covers.
                if (e.third_party) { anyThirdParty = true; continue; }
                if (e.install != InstallKind::NotInstalled || e.fitted || e.legal_notice.empty()) { continue; }
                if (!e.blocked_reason_if_acknowledged.empty()) { continue; }
                ++noticeModules;
                if (!noticeNames.empty()) { noticeNames += ", "; }
                noticeNames += e.name.empty() ? T("(unnamed module)") : e.name;
            }
            m.get_all_label = T("GET EVERYTHING");
            m.get_all_enabled = plan.blockedReason.empty() && !addAll;
            m.get_all_reason = m.get_all_enabled ? std::string() : trReason(plan.blockedReason);
            if (addAll) {
                m.foot_note = addAllProgress.empty() ? T("Working through the catalogue, one module at a time.") : addAllProgress;
            } else if (!plan.blockedReason.empty()) {
                m.foot_note = cc::formatText(tr("Cannot add all: %s"), trReason(plan.blockedReason).c_str());
            } else {
                m.foot_note = cc::formatText(tr("%d to fetch and %d to update, one after another. Each is fetched over https and refused unless its bytes hash to the sha256 the "
                                                "catalogue published - the same gate a single GET goes through. A module that fails does not stop the rest."),
                                             static_cast<int>(plan.install.size()), static_cast<int>(plan.update.size()));
            }
            if (!addAll && noticeModules > 0) {
                m.notice_note = cc::formatText(tr("%d of these carry a legal notice from their maker: %s. Each notice is on that plugin's page."), noticeModules, noticeNames.c_str());
                // THE COMBINED TICK: one consent covering every notice in the run.
                m.bulk_sentence = cc::formatText(noticeModules == 1 ? tr("I accept the %d legal notice on its plugin's page") : tr("I accept the %d legal notices on their plugins' pages"),
                                                 noticeModules);
                m.bulk_shown = true;
            }
            if (anyThirdParty) { m.left_out_note = sitesLeftOutNote(); }   // THE PLUGINS OF THE SITES THE USER ADDED ARE LEFT OUT, and the key says so (0.99.74)
        }
    } else {
        const std::vector<int> rows = updateRows(entries, query);
        const bool haveRows = updateCount > 0;
        m.updates_heading = haveRows ? T("UPDATES AVAILABLE") : T("NO UPDATES");
        if (haveRows) {
            const cascade::engine::AddAllRule plan = planAddAll(entries, m.have_catalogue, !sourceStatus.empty(), !sourceError.empty(), transfer, false);
            m.update_all_label = T("UPDATE ALL");
            m.update_all_enabled = !transfer && !addAll && !plan.update.empty() && m.have_catalogue;
        }
        if (!m.have_catalogue) { emptyState(); }
        bool anySiteRow = false;
        for (const int i : rows) {
            const StoreEntry& e = entries[static_cast<std::size_t>(i)];
            const EntryKey key = keyFor(e, kin);
            UpdateRow r;
            r.id = e.id;
            r.glyph = e.category;
            r.name = e.name;
            r.version_note = fromTo(e.installed_version, e.update_to_version);
            if (e.third_party) {
                r.badge = T("THIRD PARTY");
                anySiteRow = true;
            }
            r.whats_new = !e.whats_new.empty() ? e.whats_new : e.update_reason;
            r.key_kind = key.kind == kKeyFitting ? kKeyFitting : kKeyUpdate;
            r.key_label = key.kind == kKeyFitting ? std::string(keyLabel(kKeyFitting)) : std::string(keyLabel(kKeyUpdate));
            r.key_enabled = key.kind != kKeyFitting && key.enabled;
            r.key_reason = (!key.enabled && !key.reason.empty()) ? trReason(key.reason) : std::string();
            if (key.kind == kKeyFitting) {
                r.progress = m.busy_label;
                r.progress_norm = progress;
            }
            m.updates.push_back(std::move(r));
        }
        if (haveRows) {
            m.footnotes.push_back(T("Updates are fetched when you press CHECK AGAIN. Nothing updates itself."));
            if (anySiteRow) { m.footnotes.push_back(sitesLeftOutNote()); }   // UPDATE ALL does not take a site's plugins either (0.99.74): say so where the key is
        }
    }
    m.draft = d;
    return m;
}

ExtendModels readExtend(TableReader& t, const FoxReceiverState& state, const ReadOptions& o) {
    (void)state;
    ExtendModels e;
    e.web = readWeb(t, o);
    e.cat = readCat(t, o);
    e.store = readStore(t, o);
    e.chips[kRowWeb] = RowChip{e.web.chip, e.web.chip_tone, e.web.lit};
    e.chips[kRowCat] = RowChip{e.cat.chip, e.cat.chip_tone, e.cat.lit};
    return e;
}

// =================================================================================================================================
// THE CONSENTS

ConsentEdge watchConsents(ConsentWatch& w, const StoreModel& m) {
    ConsentEdge edge;
    // THE RUN'S RESULT LINE ARRIVING is the run having ended, however briefly the run itself was on the table.
    if (m.add_all_summary != w.last_add_all_summary) {
        w.last_add_all_summary = m.add_all_summary;
        if (!w.last_add_all_summary.empty()) { edge.clear_bulk_tick = true; }
    }
    // A catalogue fetch began: whatever was ticked was for the catalogue that is going. A run ended: the tick was for that run, which took the notices it covered.
    if (m.busy && m.busy_kind == 1) {
        edge.clear_notice_tick = true;
        edge.clear_bulk_tick = true;
    }
    if (w.add_all_was_running && !m.add_all_running) { edge.clear_bulk_tick = true; }
    w.add_all_was_running = m.add_all_running;
    // A DIFFERENT CATALOGUE from the one last read: the ticks were consent for the old one's notices.
    if (m.hash_valid) {
        if (w.have_hash && m.catalogue_hash != w.hash) {
            edge.clear_notice_tick = true;
            edge.clear_bulk_tick = true;
        }
        w.have_hash = true;
        w.hash = m.catalogue_hash;
    }
    return edge;
}

// =================================================================================================================================
// THE EFFECTS: Web access and CAT control

std::string jsonQuote(const std::string& s) {
    std::string out = "\"";
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    out += cc::formatText("\\u%04x", static_cast<unsigned>(c));
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
    return out;
}

std::string webConfigJson(const WebDraft& s, const std::string& extra, int fallbackPort) {
    int port = fallbackPort;
    if (!parsePort(s.port_text, port)) { port = fallbackPort; }
    std::string json = "{\"web\":{\"enabled\":";
    json += s.enabled ? "true" : "false";
    json += ",\"bindAddress\":" + jsonQuote(s.bind);
    json += ",\"port\":" + std::to_string(clampPort(port));
    json += ",\"username\":" + jsonQuote(s.user);
    if (!extra.empty()) { json += "," + extra; }
    json += "}}";
    return json;
}

namespace {

Effect webLocal(WebDraft d) {
    d.have = true;
    Effect e;
    e.local = Local::Web;
    e.web = std::move(d);
    return e;
}
Effect catLocal(CatDraft d) {
    d.have = true;
    Effect e;
    e.local = Local::Cat;
    e.cat = std::move(d);
    return e;
}
Effect sendConfig(const std::string& json) {
    Effect e = commandEffect(make(cmd::makeText(FOXAPI_OP_SERVER_CONFIG, json)));
    e.ask.kind = AskKind::ServerConfig;
    return e;
}

// applyWebSettings: the staged copy goes out; the error and the dirty flag are cleared. `extra` is a member of the "web" object (the password's).
Effect applyWebSettings(const WebModel& m, const std::string& extra, WebDraft draft) {
    Effect e = sendConfig(webConfigJson(draft, extra, m.configured_port));
    draft.dirty = false;
    draft.error.clear();
    e.local = Local::Web;
    draft.have = true;
    e.web = std::move(draft);
    return e;
}

}  // namespace

Effect applyWebServe(const WebModel& m, bool on) {
    WebDraft d = m.draft;
    d.enabled = on;
    d.dirty = true;
    return webLocal(std::move(d));
}

Effect applyWebReach(const WebModel& m, int index) {
    const int count = m.bind_choice == 2 ? 3 : 2;   // "A specific address" is offered only while the config already holds one
    if (index < 0 || index >= count) { return {}; }
    WebDraft d = m.draft;
    if (index == 0) { d.bind = "127.0.0.1"; }
    if (index == 1) { d.bind = "0.0.0.0"; }
    d.dirty = true;
    return webLocal(std::move(d));
}

Effect applyWebPort(const WebModel& m, const std::string& text) {
    WebDraft d = m.draft;
    d.port_text = cut(text, 11);
    d.dirty = true;
    return webLocal(std::move(d));
}

Effect applyWebUser(const WebModel& m, const std::string& text) {
    WebDraft d = m.draft;
    d.user = cut(text, kUserBytes);
    d.dirty = true;
    return webLocal(std::move(d));
}

Effect applyWebPassword(const WebModel& m, const std::string& text) {
    WebDraft d = m.draft;
    d.password = cut(text, kPasswordBytes);
    return webLocal(std::move(d));
}

Effect applyWebConfirm(const WebModel& m, const std::string& text) {
    WebDraft d = m.draft;
    d.confirm = cut(text, kPasswordBytes);
    return webLocal(std::move(d));
}

Effect applyWebSetPassword(const WebModel& m) {
    WebDraft d = m.draft;
    const std::string a = d.password;
    const bool same = a == d.confirm;
    // Do not leave the plaintext sitting in a box once it has been sent (or rejected).
    d.password.clear();
    d.confirm.clear();
    if (!same) {
        d.error = T("the two passwords do not match");   // webError_ is drawn and nothing else
        return webLocal(std::move(d));
    }
    // Clearing is allowed here too (both boxes empty): the POLICY decides whether the resulting configuration may still listen. The password is hashed by the ENGINE, on a worker.
    std::string member = a.empty() ? std::string("\"clearPassword\":true") : "\"password\":" + jsonQuote(a);
    Effect e = applyWebSettings(m, member, std::move(d));
    std::fill(member.begin(), member.end(), '\0');
    return e;
}

Effect applyWebClearPassword(const WebModel& m) {
    if (!m.has_password) { return {}; }   // the key is drawn only while a password is set
    WebDraft d = m.draft;
    return applyWebSettings(m, "\"clearPassword\":true", std::move(d));
}

Effect applyWebApply(const WebModel& m) {
    if (!m.apply_enabled) { return {}; }
    WebDraft d = m.draft;
    return applyWebSettings(m, std::string(), std::move(d));
}

Effect applyCatAccept(const CatModel& m, bool on) {
    Effect e = sendConfig(std::string("{\"cat\":{\"enabled\":") + (on ? "true" : "false") + "}}");
    CatDraft d = m.draft;
    d.enabled = on;
    d.bind_all = m.reachable;
    d.port_text = m.port;
    d.have = true;
    e.local = Local::Cat;
    e.cat = std::move(d);
    return e;
}

Effect applyCatPort(const CatModel& m, const std::string& text) {
    int v = 0;
    if (!parsePort(text, v)) { return {}; }
    const int port = clampPort(v);
    Effect e = sendConfig("{\"cat\":{\"port\":" + std::to_string(port) + "}}");
    CatDraft d = m.draft;
    d.enabled = m.accept;
    d.bind_all = m.reachable;
    d.port_text = std::to_string(port);
    d.have = true;
    e.local = Local::Cat;
    e.cat = std::move(d);
    return e;
}

Effect applyCatReachable(const CatModel& m, bool on) {
    Effect e = sendConfig(std::string("{\"cat\":{\"bindAll\":") + (on ? "true" : "false") + "}}");
    CatDraft d = m.draft;
    d.enabled = m.accept;
    d.bind_all = on;
    d.port_text = m.port;
    d.have = true;
    e.local = Local::Cat;
    e.cat = std::move(d);
    return e;
}

void applyServerAnswer(WebDraft& draft, int status, const std::string& message) {
    if (status != FOXAPI_OK && status != FOXAPI_NO_CHANGE) { draft.error = message; }
}

void scrubEffect(Effect& e) {
    for (Command& c : e.commands) {
        std::memset(c.q.c.text, 0, sizeof(c.q.c.text));
        std::fill(c.q.longText.begin(), c.q.longText.end(), '\0');
        c.q.longText.clear();
    }
    std::fill(e.web.password.begin(), e.web.password.end(), '\0');
    std::fill(e.web.confirm.begin(), e.web.confirm.end(), '\0');
    e.web.password.clear();
    e.web.confirm.clear();
}

// =================================================================================================================================
// THE EFFECTS: the Plugin store page

namespace {

Effect storeLocal(StoreDraft d) {
    Effect e;
    e.local = Local::Store;
    e.store = std::move(d);
    return e;
}

const StoreCard* cardOf(const StoreModel& m, const std::string& id) {
    for (const StoreCategory& c : m.categories) {
        for (const StoreCard& k : c.cards) {
            if (k.id == id) { return &k; }
        }
    }
    return nullptr;
}
const UpdateRow* rowOf(const StoreModel& m, const std::string& id) {
    for (const UpdateRow& r : m.updates) {
        if (r.id == id) { return &r; }
    }
    return nullptr;
}
bool listed(const StoreModel& m, const std::string& id) { return std::find(m.ids.begin(), m.ids.end(), id) != m.ids.end(); }

Effect install(const StoreModel& m, const std::string& id) {
    // The acknowledgement belongs to the plugin whose page is open and to no other, so a fit asked for from any other card is asked for unacknowledged.
    const bool ack = id == m.draft.page_id && m.draft.notice_tick;
    return commandEffect(make(cmd::makeText(FOXAPI_OP_STORE_INSTALL, id, ack ? 1 : 0)));
}
Effect update(const std::string& id) { return commandEffect(make(cmd::makeText(FOXAPP_OP_STORE_UPDATE, id))); }
Effect cancel() { return commandEffect(make(cmd::make(FOXAPI_OP_STORE_CANCEL))); }

}  // namespace

Effect applyStoreSearch(const StoreModel& m, const std::string& text) {
    StoreDraft d = m.draft;
    d.search = cut(text, kSearchBytes);
    // Typing is looking for something: that is on the list, not on a page.
    d.page_id.clear();
    d.notice_tick = false;
    d.details_open = false;
    return storeLocal(std::move(d));
}

Effect applyStoreTab(const StoreModel& m, int tab) {
    if (tab != 0 && tab != 1) { return {}; }
    StoreDraft d = m.draft;
    d.tab = tab;
    d.page_id.clear();
    d.notice_tick = false;
    d.details_open = false;
    return storeLocal(std::move(d));
}

Effect applyStoreCheck(const StoreModel& m) {
    if (!m.check_enabled) { return {}; }
    return commandEffect(make(cmd::make(FOXAPI_OP_STORE_FETCH)));
}

Effect applyStoreSitesToggle(const StoreModel& m) {
    StoreDraft d = m.draft;
    d.sites_open = !d.sites_open;
    return storeLocal(std::move(d));
}

Effect applyStoreSiteDraft(const StoreModel& m, const std::string& text) {
    StoreDraft d = m.draft;
    d.site_draft = cut(text, kSiteUrlBytes);
    d.site_error.clear();
    return storeLocal(std::move(d));
}

Effect applyStoreConsent(const StoreModel& m, bool on) {
    StoreDraft d = m.draft;
    d.consent = on;
    return storeLocal(std::move(d));
}

Effect applyStoreSiteAdd(const StoreModel& m) {
    if (!m.add_site_enabled) { return {}; }   // the key is dead: busy, no address, or the box unticked
    StoreDraft d = m.draft;
    std::string checked;
    const std::string refusal = checkSiteAdd(m.draft.site_draft, m.site_urls, kMaxSites, m.source_url, checked);
    if (!refusal.empty()) {
        d.site_error = refusal;
        return storeLocal(std::move(d));
    }
    Effect e = commandEffect(make(cmd::makeText(FOXAPP_OP_STORE_SITE, checked, 0, 1)));
    // EACH SITE IS A FRESH DECISION: the field and the tick go with the press.
    d.site_draft.clear();
    d.consent = false;
    d.site_error.clear();
    e.local = Local::Store;
    e.store = std::move(d);
    return e;
}

Effect applyStoreSiteRemove(const StoreModel& m, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m.sites.size() || !m.sites[static_cast<std::size_t>(index)].remove_enabled) { return {}; }
    return commandEffect(make(cmd::makeText(FOXAPP_OP_STORE_SITE, m.sites[static_cast<std::size_t>(index)].url, 1, 0)));
}

Effect applyStoreCardOpen(const StoreModel& m, const std::string& id) {
    if (id.empty() || !listed(m, id) || id == m.draft.page_id) { return {}; }
    StoreDraft d = m.draft;
    d.page_id = id;
    d.tab = 0;
    // A NEW PAGE: the tick belongs to the page that had it and the details start folded.
    d.notice_tick = false;
    d.details_open = false;
    // THE EDGE OF OPENING A PAGE asks for its pictures, a failed one again.
    Effect e = commandEffect(make(cmd::makeText(FOXAPP_OP_STORE_PICTURES_WANT, id, 1)));
    e.pictures_asked = true;
    e.local = Local::Store;
    e.store = std::move(d);
    return e;
}

Effect applyStoreCardKey(const StoreModel& m, const std::string& id) {
    const StoreCard* c = cardOf(m, id);
    if (c == nullptr) { return {}; }
    switch (c->key_kind) {
        case kKeyGet:
            if (c->opens_page) { return applyStoreCardOpen(m, id); }   // GET on a plugin with a notice OPENS ITS PAGE
            if (!c->key_enabled) { return {}; }
            return install(m, id);
        case kKeyUpdate:
            if (!c->key_enabled) { return {}; }
            return update(id);
        case kKeyFitting:
            return cancel();
        default: break;
    }
    return {};
}

Effect applyStoreGetAll(const StoreModel& m) {
    if (m.get_all_label.empty() || !m.get_all_enabled) { return {}; }
    // The ENGINE plans the run; this says only what it cannot know - the ADD ALL tick, and which one module's own notice tick is on (the module whose page is open).
    const std::string acknowledged = (m.draft.notice_tick && !m.draft.page_id.empty() && listed(m, m.draft.page_id)) ? m.draft.page_id : std::string();
    return commandEffect(make(cmd::makeText(FOXAPP_OP_STORE_ADD_ALL, acknowledged, m.draft.bulk_tick ? 1 : 0)));
}

Effect applyStoreBulkTick(const StoreModel& m, bool on) {
    StoreDraft d = m.draft;
    d.bulk_tick = on;
    return storeLocal(std::move(d));
}

Effect applyStoreUpdateAll(const StoreModel& m) {
    if (m.update_all_label.empty() || !m.update_all_enabled) { return {}; }
    // UPDATE ALL is the same queue restricted to updates (ival[1] 1), which need no acknowledgement: a plugin that is fitted has had its notice shown.
    return commandEffect(make(cmd::makeText(FOXAPP_OP_STORE_ADD_ALL, std::string(), 0, 1)));
}

Effect applyStoreUpdate(const StoreModel& m, const std::string& id) {
    if (const UpdateRow* r = rowOf(m, id)) {
        if (r->key_kind == kKeyFitting) { return cancel(); }
        if (!r->key_enabled) { return {}; }
        return update(id);
    }
    if (const StoreCard* c = cardOf(m, id)) {
        if (c->key_kind == kKeyFitting) { return cancel(); }
        if (c->key_kind == kKeyUpdate && c->key_enabled) { return update(id); }
    }
    return {};
}

Effect applyStoreCleanup(const StoreModel& m) {
    if (m.cleanup_label.empty() || !m.cleanup_enabled) { return {}; }
    StoreDraft d = m.draft;
    d.cleanup_open = true;
    return storeLocal(std::move(d));
}

Effect applyStoreCleanupConfirm(const StoreModel& m, bool yes) {
    StoreDraft d = m.draft;
    d.cleanup_open = false;
    Effect e;
    // What goes is what the engine's last scan calls superseded NOW, not the frame's list.
    if (yes && m.cleanup_open && !m.cleanup_label.empty() && m.cleanup_enabled) { e.commands.push_back(make(cmd::makeInt(FOXAPP_OP_STORE_CLEAN_OLD, 0))); }
    e.local = Local::Store;
    e.store = std::move(d);
    return e;
}

Effect applyStorePluginBack(const StoreModel& m) {
    StoreDraft d = m.draft;
    d.page_id.clear();
    d.tab = 0;
    d.notice_tick = false;
    d.details_open = false;
    return storeLocal(std::move(d));
}

Effect applyStorePluginKey(const StoreModel& m) {
    const StorePluginPage& p = m.plugin;
    if (!p.shown) { return {}; }
    switch (p.key_kind) {
        case kKeyGet:
            if (!p.key_enabled) { return {}; }
            return install(m, p.id);
        case kKeyUpdate:
            if (!p.key_enabled) { return {}; }
            return update(p.id);
        case kKeyFitting:
            return cancel();
        default: break;
    }
    return {};
}

Effect applyStorePluginDetails(const StoreModel& m, bool on) {
    StoreDraft d = m.draft;
    d.details_open = on;
    return storeLocal(std::move(d));
}

Effect applyStorePicture(const StoreModel& m, int index) {
    (void)m;
    (void)index;
    return {};
}

Effect applyStoreNoticeTick(const StoreModel& m, bool on) {
    if (m.draft.page_id.empty()) { return {}; }
    StoreDraft d = m.draft;
    d.notice_tick = on;
    return storeLocal(std::move(d));
}

Effect applyStoreCancel(const StoreModel& m) {
    if (!m.busy) { return {}; }
    return cancel();
}

Effect applyStoreOpened(const StoreModel& m, StoreFirstOpenState& first, bool readThisSession) {
    cascade::gui::StoreFirstOpen s;
    s.settled = first.settled;
    const bool ask = cascade::gui::storeShouldCheckOnOpen(s, true, readThisSession, m.busy || m.picture_busy);
    first.settled = s.settled;
    if (!ask) { return {}; }
    return commandEffect(make(cmd::make(FOXAPI_OP_STORE_FETCH)));
}

Effect applyStorePicturesTop(const StoreModel& m) {
    if (!m.plugin.shown || m.plugin.id.empty()) { return {}; }
    for (const PictureFrame& f : m.plugin.screenshots) {
        if (f.sha256.empty() || f.state != kPicNone) { continue; }
        Effect e = commandEffect(make(cmd::makeText(FOXAPP_OP_STORE_PICTURES_WANT, m.plugin.id, 0)));
        e.pictures_asked = true;
        return e;
    }
    return {};
}

}  // namespace foxsdr::slintextend
