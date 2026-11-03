// The EXTEND bank's pure logic (src/slint/extend_logic.hpp) against a STUB TABLE: what Web access, CAT control and the Plugin store page show, and the exact effect each control
// has - the op with its arguments and grant, the answer it waits for, the draft the publisher keeps - which are the ImGui window's own. No Slint, no engine, no window.
//
// It also READS the ImGui window's sources for every English sentence the logic words (they must be the drawers' own, so a catalogue key can never be one nobody draws), for the
// JSON the SERVER_CONFIG ops carry, for the ops each store key sends, and for the constants (the buffers, the port range, the site limit).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "extend_logic.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/i18n.hpp"
#include "core/plugin_repo_data.hpp"
#include "core/utf8_text.hpp"

using namespace foxsdr::slintextend;
using cascade::i18n::tr;
using Json = nlohmann::json;
using Names = std::vector<std::string>;

namespace {

// ---- a table of lists and facts a test fills in (the other banks' tests') ----
class StubTable : public TableReader {
public:
    std::map<std::uint32_t, std::vector<FoxListItem>> lists;
    std::map<std::string, std::string> facts;

    std::int32_t readList(std::uint32_t list, std::uint32_t index, FoxListItem* out) override {
        const auto it = lists.find(list);
        const std::int32_t n = it == lists.end() ? 0 : static_cast<std::int32_t>(it->second.size());
        if (out != nullptr && static_cast<std::int32_t>(index) < n) { *out = it->second[index]; }
        return n;
    }
    bool fact(const std::string& key, std::string& out) override {
        const auto it = facts.find(key);
        if (it == facts.end()) { out.clear(); return false; }
        out = it->second;
        return true;
    }
    void add(std::uint32_t list, std::uint64_t id, const char* name, const char* detail, std::uint32_t flags = 0, double v0 = 0, double v1 = 0, double v2 = 0, double v3 = 0) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        it.id = id;
        std::snprintf(it.name, sizeof(it.name), "%s", name);
        std::snprintf(it.detail, sizeof(it.detail), "%s", detail);
        it.flags = flags;
        it.value[0] = v0; it.value[1] = v1; it.value[2] = v2; it.value[3] = v3;
        lists[list].push_back(it);
    }
};

bool localTm(std::time_t tt, std::tm& out) {
#if defined(_WIN32)
    return localtime_s(&out, &tt) == 0;
#else
    return localtime_r(&tt, &out) != nullptr;
#endif
}

// A vector read that cannot go out of range: a missing element is a default one and the CHECK that wanted it fails by name (never an out-of-bounds read).
template <typename T>
const T& at(const std::vector<T>& v, std::size_t i) {
    static const T none{};
    return i < v.size() ? v[i] : none;
}

bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string commandText(const Command& c) { return foxsdr::slintsignal::textOfCommand(c); }
// A command by index that cannot read past the end: a missing one is an invalid, empty Command and the CHECK that wanted it fails (never a crash).
const Command& cmdOf(const Effect& e, std::size_t i) {
    static const Command none{};
    return i < e.commands.size() ? e.commands[i] : none;
}
const FoxCommand& fcOf(const Effect& e, std::size_t i) { return cmdOf(e, i).q.c; }

// The JSON a SERVER_CONFIG carries, parsed; a text that is no JSON is a null value (and the CHECK that wanted a member fails, never throws).
Json jsonOf(const Effect& e) {
    Json j = Json::parse(commandText(cmdOf(e, 0)), nullptr, false);
    return j.is_discarded() ? Json(nullptr) : j;   // (a discarded value throws on operator[]; a null one does not)
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
// <repo>/src/<rel>, from this file's own path (.../src/slint/tests/test_slint_host_extend.cpp).
std::string srcPath(const char* rel) {
    std::string p = __FILE__;
    for (int i = 0; i < 3; ++i) {
        const std::size_t cut = p.find_last_of("/\\");
        if (cut == std::string::npos) { break; }
        p.resize(cut);
    }
    return p + "/" + rel;
}

// ---- the fixtures: Web access and CAT ----
void fillServers(StubTable& t, std::uint32_t webFlags, double webPort, double sessions, double audio, double commands, std::uint32_t catFlags, double catClients, double catCommands,
                 const char* webDetail = "", const char* catDetail = "") {
    t.lists[FOXAPI_LIST_SERVERS].clear();
    t.add(FOXAPI_LIST_SERVERS, 1, "web", webDetail, webFlags, webPort, sessions, audio, commands);
    t.add(FOXAPI_LIST_SERVERS, 2, "cat", catDetail, catFlags, 4532, catClients, 0, catCommands);
}
constexpr std::uint32_t kActive = FOXAPI_ITEM_ACTIVE, kFault = FOXAPI_ITEM_FAULT;

// A web server on loopback with a password, three browsers, one listening to audio; the CAT listener off.
StubTable webRunning() {
    StubTable t;
    t.facts["fact.web.enabled"] = "1";
    t.facts["fact.web.bindAddress"] = "127.0.0.1";
    t.facts["fact.web.port"] = "8073";
    t.facts["fact.web.username"] = "admin";
    t.facts["fact.web.hasPassword"] = "1";
    t.facts["fact.web.reachable"] = "0";
    t.facts["fact.web.authRequired"] = "1";
    t.facts["fact.web.addresses"] = "";
    t.facts["fact.web.policy"] = "";
    t.facts["fact.cat.enabled"] = "0";
    t.facts["fact.cat.bindAll"] = "0";
    t.facts["fact.cat.port"] = "4532";
    fillServers(t, kActive, 8073, 3, 1, 42, 0, 0, 0);
    return t;
}

WebModel webModel(StubTable& t, const ReadOptions& o = ReadOptions{}) { return readWeb(t, o); }
CatModel catModel(StubTable& t, const ReadOptions& o = ReadOptions{}) { return readCat(t, o); }

// ================================================================================================================================
// the sentences are the ImGui window's own
// ================================================================================================================================

// The ImGui sources with every string literal that is split over lines ("a" newline "b") joined, so a sentence can be searched for as one.
std::string joinedSource(const char* rel) {
    std::string s = slurp(srcPath(rel));
    static const std::regex join(R"re("[ \t]*\r?\n[ \t]*")re");
    return std::regex_replace(s, join, "");
}
// A sentence as the source spells it: quotes and breaks escaped.
std::string asSource(const std::string& english) {
    std::string out;
    for (const char c : english) {
        if (c == '"') { out += "\\\""; }
        else if (c == '\n') { out += "\\n"; }
        else { out += c; }
    }
    return out;
}

void sourcesStateWhatWeSend() {
    const std::string app = joinedSource("gui/app_window.cpp");
    const std::string store = joinedSource("gui/app_window_store.cpp");
    const std::string view = joinedSource("gui/plugin_store_view.cpp");
    CHECK(!app.empty() && !store.empty() && !view.empty());
    // THE SERVER_CONFIG TEXT: the members, in the order the ImGui window builds them.
    CHECK(has(app, "std::string json = \"{\\\"web\\\":{\\\"enabled\\\":\";"));
    CHECK(has(app, "json += \",\\\"bindAddress\\\":\" + serverJsonQuote(webStageBind_);"));
    CHECK(has(app, "json += \",\\\"port\\\":\" + std::to_string(webPortMirror_);"));
    CHECK(has(app, "json += \",\\\"username\\\":\" + serverJsonQuote(webUserBuf_);"));
    CHECK(has(app, "if (!extraWebMembers.empty()) { json += \",\" + extraWebMembers; }"));
    CHECK(has(app, "password.empty() ? std::string(\"\\\"clearPassword\\\":true\") : \"\\\"password\\\":\" + serverJsonQuote(password)"));
    CHECK(has(app, "std::string(\"{\\\"cat\\\":{\\\"enabled\\\":\") + (catEnabled_ ? \"true\" : \"false\") + \"}}\""));
    CHECK(has(app, "\"{\\\"cat\\\":{\\\"port\\\":\" + std::to_string(catPortMirror_) + \"}}\""));
    CHECK(has(app, "std::string(\"{\\\"cat\\\":{\\\"bindAll\\\":\") + (catBindAll_ ? \"true\" : \"false\") + \"}}\""));
    CHECK(has(app, "uiSubmitTicket(cascade::core::cmd::makeText(FOXAPI_OP_SERVER_CONFIG, json))"));
    CHECK(has(app, "if (r.status != FOXAPI_OK && r.status != FOXAPI_NO_CHANGE) { webError_ = r.message; }"));
    CHECK(has(app, "webPortMirror_ = std::clamp(webPortMirror_, cascade::net::kMinPort,"));
    CHECK(has(app, "catPortMirror_ = std::max(1024, std::min(catPortMirror_, 65535));"));
    CHECK(has(app, "webError_ = tr(\"the two passwords do not match\");"));
    CHECK(has(app, "std::memset(webPassBuf_, 0, sizeof(webPassBuf_));"));
    CHECK(has(app, "kWhere[] = {tr(\"This machine only\"), tr(\"Every network interface\"),"));
    CHECK(has(app, "const int whereCount = (webBindChoice_ == 2) ? 3 : 2;"));
    CHECK(has(app, "webStageBind_ = \"127.0.0.1\";") && has(app, "webStageBind_ = \"0.0.0.0\";"));
    CHECK(has(app, "ImGui::BeginDisabled(!webDirty_);"));
    CHECK(has(app, "if (hasPassword) {"));
    // THE BUFFERS and the limits
    const std::string hpp = slurp(srcPath("gui/app_window.hpp"));
    CHECK(has(hpp, "char webUserBuf_[64]") && has(hpp, "char webPassBuf_[128]") && has(hpp, "char webPassConfirmBuf_[128]"));
    CHECK(kUserBytes == 63 && kPasswordBytes == 127);
    const std::string vhpp = slurp(srcPath("gui/plugin_store_view.hpp"));
    CHECK(has(vhpp, "char search[128]") && has(vhpp, "char siteUrl[512]"));
    CHECK(kSearchBytes == 127 && kSiteUrlBytes == 511);
    const std::string ports = slurp(srcPath("core/server_ports.hpp"));
    CHECK(has(ports, "kMinPort = 1024") && has(ports, "kMaxPort = 65535") && kMinPort == 1024 && kMaxPort == 65535);
    CHECK(has(slurp(srcPath("core/config.hpp")), "kMaxPluginSites = 8") && kMaxSites == 8);
    CHECK(kCleanupListed == 3 && has(view, "k < oldCount && k < 3"));
    // THE OPS EACH STORE KEY SENDS, as drawPluginStoreWindow sends them (the engine re-tests every one)
    CHECK(has(store, "if (pluginStoreView_->checkNowRequested()) { uiSubmit(cmd::make(FOXAPI_OP_STORE_FETCH)); }"));
    CHECK(has(store, "if (pluginStoreView_->cancelRequested()) { uiSubmit(cmd::make(FOXAPI_OP_STORE_CANCEL)); }"));
    CHECK(has(store, "uiSubmit(cmd::makeInt(FOXAPP_OP_STORE_CLEAN_OLD, 0))"));
    CHECK(has(store, "uiSubmit(cmd::makeText(FOXAPP_OP_STORE_ADD_ALL, acknowledged, deck.addAllAck ? 1 : 0));"));
    CHECK(has(store, "uiSubmit(cmd::makeText(FOXAPP_OP_STORE_ADD_ALL, std::string(), 0, 1))"));
    CHECK(has(store, "uiSubmit(cmd::makeText(FOXAPI_OP_STORE_INSTALL, feKey, ack ? 1 : 0));"));
    CHECK(has(store, "const bool ack = feKey == deck.pageId && deck.legalAck;"));
    CHECK(has(store, "uiSubmit(cmd::makeText(FOXAPP_OP_STORE_UPDATE, model.modules[static_cast<std::size_t>(updIdx)].id));"));
    CHECK(has(store, "uiSubmit(cmd::makeText(FOXAPP_OP_STORE_SITE, pluginStoreView_->addSiteRequested(), 0, 1));"));
    CHECK(has(store, "uiSubmit(cmd::makeText(FOXAPP_OP_STORE_SITE, pluginStoreView_->removeSiteRequested(), 1, 0));"));
    CHECK(has(store, "uiSubmit(cmd::makeText(FOXAPP_OP_STORE_PICTURES_WANT, pluginStoreView_->pageOpened(), 1));"));
    CHECK(has(store, "uiSubmit(cmd::makeText(FOXAPP_OP_STORE_PICTURES_WANT, pluginStoreView_->pageId(), 0));"));
    // the op numbers
    CHECK(FOXAPI_OP_SERVER_CONFIG == 0x1001u && FOXAPI_OP_STORE_FETCH == 0x0801u && FOXAPI_OP_STORE_INSTALL == 0x0802u && FOXAPI_OP_STORE_CANCEL == 0x0804u);
    CHECK(FOXAPP_OP_STORE_UPDATE == 0x8705u && FOXAPP_OP_STORE_SITE == 0x8709u && FOXAPP_OP_STORE_PICTURES_WANT == 0x870Au && FOXAPP_OP_STORE_CLEAN_OLD == 0x870Bu &&
          FOXAPP_OP_STORE_ADD_ALL == 0x841Eu);
    // THE ENGINE'S TWO BULK OPS: 0x0805 is the whole ADD ALL run; the UPDATE ALL key is 0x841E with ival[1] 1 (why this file does not send 0x0805)
    const std::string eng = joinedSource("engine/engine.cpp");
    CHECK(has(eng, "case FOXAPI_OP_STORE_UPDATE_ALL:") && has(eng, "startAddAll(on);"));
    CHECK(has(eng, "startAddAll(on, text, c.ival[1] != 0);"));
    // the first-open rule, the first-open fetch and the pictures' top-up
    CHECK(has(store, "storeShouldCheckOnOpen(storeFirstOpen_, true, store.readThisSession, store.transfer || store.pictureBusy)"));
    CHECK(has(store, "storePicturesWantSeq_ != uiState_.listSeq"));
    // the store's rail chip and key rules
    CHECK(has(app, "storeChip = tr(\"BUSY\");") && has(app, "storeChip = tr(\"IDLE\");") && has(app, "tr(\"%zu UPD\")") && has(app, "storeChip = tr(\"OK\");"));
}

// A literal's \xNN escapes as the bytes they stand for (the logic's sources are ASCII; the drawers' own literals hold the characters).
std::string unescapeHex(const std::string& s) {
    const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x' && hex(s[i + 2]) >= 0 && hex(s[i + 3]) >= 0) {
            out += static_cast<char>(hex(s[i + 2]) * 16 + hex(s[i + 3]));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

// EVERY tr("...") / T("...") LITERAL OF extend_logic.cpp is a sentence one of the drawers says: the logic adds no key. (A literal split over lines is joined first, as the drawers' own
// are; a catalogue key is what stands before a widget label's "##suffix".)
void everySentenceIsADrawers() {
    const std::string mine = joinedSource("slint/extend_logic.cpp");
    CHECK(!mine.empty());
    const std::string drawers = joinedSource("gui/app_window.cpp") + joinedSource("gui/plugin_store_view.cpp") + joinedSource("gui/app_window_store.cpp") +
                                joinedSource("core/plugin_store_reasons.cpp") + joinedSource("core/add_all_plan.hpp");
    static const std::regex literal(R"re(\b(?:tr|T)\(\s*("(?:[^"\\]|\\.)*")\s*\))re");
    int seen = 0, missing = 0;
    for (std::sregex_iterator it(mine.begin(), mine.end(), literal), end; it != end; ++it) {
        const std::string quoted = unescapeHex((*it)[1]);   // the literal as the source spells it, quotes included (this file spells the UTF-8 dash as \xNN; the drawers' own is raw)
        const std::string body = quoted.substr(0, quoted.size() - 1);
        ++seen;
        if (drawers.find(quoted) == std::string::npos && drawers.find(body + "##") == std::string::npos) {
            std::printf("  a sentence of extend_logic.cpp that no drawer says: %s\n", quoted.c_str());
            ++missing;
        }
    }
    CHECK(seen >= 150);   // the whole of them were found (the pattern did not quietly match nothing)
    CHECK(missing == 0);
}

// Every sentence of the logic has a translation: the German catalogue carries it (a key nobody translated reads as English in a German window).
void sentencesAreTranslated() {
    const std::string was = cascade::i18n::current();
    cascade::i18n::setLanguage("de");
    int untranslated = 0;
    const char* const keys[] = {"Set password", "Clear password", "Apply", "Applied", "a password is set", "no password set", "This machine only", "Every network interface",
                                "Not listening.", "CHECK AGAIN", "CHECK NOW", "BROWSE", "GET EVERYTHING", "UPDATE ALL", "CLEAN UP OLD VERSIONS (%zu)", "Keep them", "REMOVE",
                                "Search plugins", "NO UPDATES", "UPDATES AVAILABLE", "No sites added.", "SHOW DETAILS", "HIDE DETAILS", "WHAT IT DOES", "< BROWSE",
                                "CATALOGUE NOT READ", "reachable from your network", "serving at http://127.0.0.1:%d"};
    for (const char* k : keys) {
        if (std::string(tr(k)) == k) {
            std::printf("  not translated into German: %s\n", k);
            ++untranslated;
        }
    }
    CHECK(untranslated == 0);
    // ...and the model says it in the language in force.
    StubTable t = webRunning();
    const WebModel m = readWeb(t, ReadOptions{});
    CHECK(m.set_label == tr("Set password") && m.set_label != "Set password");
    CHECK(m.password_status == tr("a password is set") && m.password_status != "a password is set");
    cascade::i18n::setLanguage(was);
    CHECK(readWeb(t, ReadOptions{}).set_label == "Set password");
}

// ================================================================================================================================
// Web access
// ================================================================================================================================

void webReads() {
    // RUNNING on this machine only, a password, a login required, three browsers, one listening to audio
    StubTable t = webRunning();
    WebModel m = webModel(t);
    CHECK(m.serve && m.running && !m.fault && !m.reachable && m.auth_required && m.has_password);
    CHECK(m.reach.size() == 2 && at(m.reach, 0).label == "This machine only" && at(m.reach, 1).label == "Every network interface" && m.reach_index == 0);
    CHECK(m.address.empty() && m.port == "8073" && m.user == "admin" && m.bind_address == "127.0.0.1");
    CHECK(m.password_status == "a password is set" && m.set_label == "Set password" && m.clear_label == "Clear password" && m.clear_enabled);
    CHECK(m.apply_label == "Applied" && !m.apply_enabled && m.error.empty());
    CHECK(m.serving_note == "serving at http://127.0.0.1:8073" && m.network_warning.empty() && m.addresses.empty() && m.http_note.empty());
    CHECK(m.sessions == "3 browser session(s)" && m.listeners == "1 listening to audio");
    CHECK(m.chip == "ON" && m.chip_tone == Tone::Good && m.lit && m.listening_port == 8073 && m.clients == 3 && m.audio_listeners == 1 && m.commands == 42u);
    CHECK(m.password_draft.empty() && m.confirm_draft.empty());

    // no login required: the session count is not said; nobody listening: the audio line is not said
    t.facts["fact.web.authRequired"] = "0";
    fillServers(t, kActive, 8073, 0, 0, 7, 0, 0, 0);
    m = webModel(t);
    CHECK(m.sessions.empty() && m.listeners.empty() && m.serving_note == "serving at http://127.0.0.1:8073");

    // REACHABLE from the network: the port the server listens on, the red line, one address a line, the plain-HTTP note
    t = webRunning();
    t.facts["fact.web.bindAddress"] = "0.0.0.0";
    t.facts["fact.web.reachable"] = "1";
    t.facts["fact.web.addresses"] = "192.168.1.5\n10.0.0.2\n";
    fillServers(t, kActive, 9100, 0, 0, 0, 0, 0, 0);
    m = webModel(t);
    CHECK(m.reach_index == 1 && m.reach.size() == 2 && m.address.empty());
    CHECK(m.serving_note == "serving on port 9100 to every machine on your network");
    CHECK(m.network_warning == "reachable from your network");
    CHECK(m.addresses == Names({"http://192.168.1.5:9100", "http://10.0.0.2:9100"}));
    CHECK(has(m.http_note, "This link is plain HTTP: the password is sent unencrypted") && has(m.http_note, "rather than forwarding this port."));

    // A SPECIFIC ADDRESS: offered because the config holds one, and read out
    t = webRunning();
    t.facts["fact.web.bindAddress"] = "192.168.1.5";
    m = webModel(t);
    CHECK(m.reach.size() == 3 && at(m.reach, 2).label == "A specific address" && m.reach_index == 2 && m.address == "address: 192.168.1.5" && m.bind_address == "192.168.1.5");
    t.facts["fact.web.bindAddress"] = "::";   // the wildcard classes: "every interface"
    CHECK(webModel(t).reach_index == 1);
    t.facts["fact.web.bindAddress"] = "127.0.0.2";   // all of 127/8 is this machine
    CHECK(webModel(t).reach_index == 0);
    t.facts["fact.web.bindAddress"] = "";   // an empty address is the default (loopback)
    m = webModel(t);
    CHECK(m.reach_index == 0 && m.bind_address == "127.0.0.1");

    // REFUSED TO START: the setting stays on, the lamp is rust and lit, the refusal is the red line - whole from the policy fact when the entry cut it
    t = webRunning();
    fillServers(t, kFault, 8073, 0, 0, 0, 0, 0, 0, "the port 8073 is already in use");
    m = webModel(t);
    CHECK(m.fault && !m.running && m.serve && m.chip == "FAIL" && m.chip_tone == Tone::Err && m.lit);
    CHECK(m.error == "the port 8073 is already in use" && m.serving_note.empty() && m.sessions.empty());
    t.facts["fact.web.policy"] = "the port 8073 is already in use by another program on this machine; choose another";
    CHECK(webModel(t).error == "the port 8073 is already in use by another program on this machine; choose another");
    t.facts["fact.web.policy"] = "an unrelated sentence that is longer than the listed one";   // not the listed one's beginning: the listed one stands
    CHECK(webModel(t).error == "the port 8073 is already in use");
    // a complaint of this window's own takes the refusal's place, and hides what a running server says
    ReadOptions o;
    o.web.error = "the two passwords do not match";
    CHECK(webModel(t, o).error == "the two passwords do not match");
    t = webRunning();
    CHECK(webModel(t, o).error == "the two passwords do not match" && webModel(t, o).serving_note.empty() && webModel(t, o).sessions.empty());

    // OFF: no password, nothing to clear, "not serving" only when it is switched on and is not running
    t = webRunning();
    t.facts["fact.web.enabled"] = "0";
    t.facts["fact.web.hasPassword"] = "0";
    fillServers(t, 0, 8073, 0, 0, 0, 0, 0, 0);
    m = webModel(t);
    CHECK(!m.serve && !m.running && !m.fault && m.chip == "OFF" && m.chip_tone == Tone::Good && !m.lit);
    CHECK(m.password_status == "no password set" && m.clear_label.empty() && !m.clear_enabled && m.serving_note.empty() && m.error.empty());
    t.facts["fact.web.enabled"] = "1";
    CHECK(webModel(t).serving_note == "not serving");
    // before the first publish: nothing answers, nothing is claimed
    StubTable none;
    m = webModel(none);
    CHECK(!m.running && !m.fault && m.chip == "OFF" && m.serving_note.empty() && m.reach_index == 0 && m.port == "0");
}

void webStagedCopy() {
    StubTable t = webRunning();
    ReadOptions o;
    o.web.have = true;
    o.web.dirty = true;
    o.web.enabled = false;
    o.web.bind = "0.0.0.0";
    o.web.port_text = "9000";
    o.web.user = "bob";
    o.web.password = "hunter2hunter2";
    o.web.confirm = "hunter2";
    WebModel m = webModel(t, o);
    // the staged copy shows (webDirty_), and Apply says there is something to apply
    CHECK(!m.serve && m.reach_index == 1 && m.port == "9000" && m.user == "bob" && m.apply_enabled && m.apply_label == "Apply");
    CHECK(m.password_draft == "hunter2hunter2" && m.confirm_draft == "hunter2");
    CHECK(m.draft.dirty && m.draft.have && m.draft.port_text == "9000");
    // not dirty and nothing in flight: the facts show again (syncServerMirrors)
    o.web.dirty = false;
    m = webModel(t, o);
    CHECK(m.serve && m.reach_index == 0 && m.port == "8073" && m.user == "admin");
    // ...but an answer owed keeps the staged copy on screen: it is what was sent
    o.server_asks_pending = 1;
    m = webModel(t, o);
    CHECK(!m.serve && m.port == "9000" && m.user == "bob");
    o.server_asks_pending = 0;
    // Apply is the window's dirty flag and nothing else: a staged copy that is not dirty (it was applied, an answer is owed) says "Applied"; an edit typed back is still an edit
    o.server_asks_pending = 1;
    o.web.dirty = false;
    CHECK(!webModel(t, o).apply_enabled && webModel(t, o).apply_label == "Applied");
    o.server_asks_pending = 0;
    o.web.enabled = true;
    o.web.bind = "127.0.0.1";
    o.web.port_text = "8073";
    o.web.user = "admin";
    o.web.dirty = true;
    CHECK(webModel(t, o).apply_enabled && webModel(t, o).apply_label == "Apply");
}

void catReads() {
    StubTable t = webRunning();
    // LISTENING with nobody connected
    fillServers(t, kActive, 8073, 0, 0, 0, kActive, 0, 12);
    CatModel m = catModel(t);
    CHECK(!m.accept && m.port == "4532" && !m.reachable && m.warning.empty());
    CHECK(has(m.intro, "Lets logging and digital-mode software drive this receiver") && has(m.intro, "\"NET rigctl\" radio."));
    CHECK(m.status == "Listening on 127.0.0.1:4532 \xe2\x80\x94 0 client(s), 12 command(s)" && m.status_tone == Tone::Neutral);
    CHECK(m.chip == "ON" && m.chip_tone == Tone::Good && m.lit && m.running && !m.fault);
    // ONE CLIENT: the chip says how many, because a client on the port can retune the receiver
    fillServers(t, kActive, 8073, 0, 0, 0, kActive, 1, 12);
    m = catModel(t);
    CHECK(m.chip == "1 CLI" && m.status == "Listening on 127.0.0.1:4532 \xe2\x80\x94 1 client(s), 12 command(s)");
    // REACHABLE: bound on every interface, and said plainly
    t.facts["fact.cat.bindAll"] = "1";
    t.facts["fact.cat.enabled"] = "1";
    t.facts["fact.cat.port"] = "4600";
    m = catModel(t);
    CHECK(m.accept && m.reachable && m.port == "4600" && m.warning == "This protocol has no password. Anything that can reach the port can retune the receiver.");
    CHECK(m.status == "Listening on 0.0.0.0:4600 \xe2\x80\x94 1 client(s), 12 command(s)");
    // FAILED: the refusal is the line, the lamp is rust
    fillServers(t, kFault, 8073, 0, 0, 0, kFault, 0, 0, "", "the port 4600 is already in use");
    m = catModel(t);
    CHECK(m.fault && !m.running && m.status == "the port 4600 is already in use" && m.status_tone == Tone::Err && m.chip == "FAIL" && m.chip_tone == Tone::Err && m.lit);
    // OFF
    t.facts["fact.cat.enabled"] = "0";
    t.facts["fact.cat.bindAll"] = "0";
    fillServers(t, 0, 8073, 0, 0, 0, 0, 0, 0);
    m = catModel(t);
    CHECK(m.status == "Not listening." && m.status_tone == Tone::Neutral && m.chip == "OFF" && !m.lit && !m.accept && m.warning.empty());
    // A STAGED COPY while an answer is owed: what was sent shows, the facts do not (syncServerMirrors)
    ReadOptions o;
    o.cat.have = true;
    o.cat.enabled = true;
    o.cat.bind_all = true;
    o.cat.port_text = "4700";
    m = catModel(t, o);
    CHECK(!m.accept && !m.reachable && m.port == "4600");   // nothing owed: the facts (the port the engine holds)
    o.server_asks_pending = 2;
    m = catModel(t, o);
    CHECK(m.accept && m.reachable && m.port == "4700" && m.warning.size() > 10);
    // a listener that is up shows the port staged, the address staged
    fillServers(t, kActive, 8073, 0, 0, 0, kActive, 0, 3);
    CHECK(catModel(t, o).status == "Listening on 0.0.0.0:4700 \xe2\x80\x94 0 client(s), 3 command(s)");
    // the two rows' chips through readExtend
    FoxReceiverState s{};
    s.structSize = sizeof(s);
    const ExtendModels all = readExtend(t, s, ReadOptions{});
    CHECK(all.chips[kRowWeb].word == "ON" && all.chips[kRowWeb].lit && all.chips[kRowCat].word == "ON" && all.chips[kRowCat].lit);
    CHECK(all.web.chip == all.chips[kRowWeb].word && all.cat.chip == all.chips[kRowCat].word);
}

void webEffects() {
    StubTable t = webRunning();
    WebModel m = webModel(t);
    Effect e = applyWebServe(m, false);
    CHECK(e.commands.empty() && e.local == Local::Web && e.web.have && e.web.dirty && !e.web.enabled && e.web.bind == "127.0.0.1" && e.web.port_text == "8073" && e.web.user == "admin");
    // reach: this machine only / every interface / the specific address kept while the config holds one
    e = applyWebReach(m, 1);
    CHECK(e.commands.empty() && e.local == Local::Web && e.web.bind == "0.0.0.0" && e.web.dirty);
    CHECK(applyWebReach(m, 0).web.bind == "127.0.0.1");
    CHECK(applyWebReach(m, 2).empty() && applyWebReach(m, 3).empty() && applyWebReach(m, -1).empty());   // "a specific address" is not offered with no address held
    t.facts["fact.web.bindAddress"] = "192.168.1.5";
    WebModel specific = webModel(t);
    CHECK(applyWebReach(specific, 2).web.bind == "192.168.1.5" && applyWebReach(specific, 1).web.bind == "0.0.0.0" && applyWebReach(specific, 3).empty());
    t.facts["fact.web.bindAddress"] = "127.0.0.1";
    // port and user: staged, dirty; the user cut at the buffer's 63 bytes through no character
    e = applyWebPort(m, "9000");
    CHECK(e.web.port_text == "9000" && e.web.dirty && e.commands.empty());
    e = applyWebUser(m, std::string(70, 'u'));
    CHECK(e.web.user.size() == 63 && e.web.dirty);
    std::string wide;
    for (int i = 0; i < 40; ++i) { wide += "\xc3\xa9"; }   // 80 bytes of two-byte characters: the cut is at 62, not 63
    e = applyWebUser(m, wide);
    CHECK(e.web.user.size() == 62 && e.web.user == wide.substr(0, 62));
    // the password boxes: kept, cut at 127, never "dirty" (as the ImGui boxes), never in a command
    e = applyWebPassword(m, "hunter2hunter2");
    CHECK(e.local == Local::Web && e.web.password == "hunter2hunter2" && !e.web.dirty && e.commands.empty());
    CHECK(applyWebPassword(m, std::string(200, 'p')).web.password.size() == 127);
    CHECK(applyWebConfirm(m, "abc").web.confirm == "abc" && applyWebConfirm(m, std::string(200, 'p')).web.confirm.size() == 127);
    // the edits chain through the model's own draft: what was typed in the other boxes is kept
    ReadOptions o;
    o.web.have = true;
    o.web.dirty = true;
    o.web.enabled = false;
    o.web.bind = "0.0.0.0";
    o.web.port_text = "9000";
    o.web.user = "bob";
    WebModel staged = webModel(t, o);
    e = applyWebPort(staged, "9001");
    CHECK(!e.web.enabled && e.web.bind == "0.0.0.0" && e.web.user == "bob" && e.web.port_text == "9001");

    // SET PASSWORD: the two boxes differ - the line, both boxes wiped, nothing sent
    o = ReadOptions{};
    o.web.have = true;
    o.web.password = "hunter2hunter2";
    o.web.confirm = "hunter2hunter3";
    WebModel typed = webModel(t, o);
    e = applyWebSetPassword(typed);
    CHECK(e.commands.empty() && e.local == Local::Web && e.web.error == "the two passwords do not match" && e.web.password.empty() && e.web.confirm.empty());
    // ...equal: SERVER_CONFIG with the staged copy and the password, hashed by the engine; the draft is applied and the boxes are wiped
    o.web.confirm = "hunter2hunter2";
    typed = webModel(t, o);
    e = applyWebSetPassword(typed);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_SERVER_CONFIG && cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN && e.ask.kind == AskKind::ServerConfig);
    Json j = jsonOf(e);
    CHECK(j.is_object() && j.size() == 1 && j.contains("web"));
    CHECK(j["web"].is_object() && j["web"].size() == 5 && j["web"]["enabled"] == true && j["web"]["bindAddress"] == "127.0.0.1" && j["web"]["port"] == 8073 &&
          j["web"]["username"] == "admin" && j["web"]["password"] == "hunter2hunter2" && !j["web"].contains("clearPassword"));
    CHECK(e.local == Local::Web && !e.web.dirty && e.web.error.empty() && e.web.password.empty() && e.web.confirm.empty() && e.web.have);
    // THE MEMBER ORDER is the ImGui window's (a test reads the string, not the parse)
    CHECK(commandText(cmdOf(e, 0)) == "{\"web\":{\"enabled\":true,\"bindAddress\":\"127.0.0.1\",\"port\":8073,\"username\":\"admin\",\"password\":\"hunter2hunter2\"}}");
    // THE PASSWORD IS NOWHERE BUT THE COMMAND'S TEXT: not in the Effect's draft, not in the model read after, and scrubbed from the command once sent
    CHECK(!has(e.web.error, "hunter2") && e.web.password.find("hunter2") == std::string::npos);
    scrubEffect(e);
    CHECK(commandText(cmdOf(e, 0)).empty() && !has(commandText(cmdOf(e, 0)), "hunter2") && cmdOf(e, 0).q.longText.empty());
    // both boxes empty and equal: the ImGui key CLEARS the password (setWebPassword("") is clearPassword)
    o.web.password.clear();
    o.web.confirm.clear();
    e = applyWebSetPassword(webModel(t, o));
    CHECK(commandText(cmdOf(e, 0)) == "{\"web\":{\"enabled\":true,\"bindAddress\":\"127.0.0.1\",\"port\":8073,\"username\":\"admin\",\"clearPassword\":true}}");
    // the staged edits go with it (as applying the settings after the hash always did)
    o.web.have = true;
    o.web.dirty = true;
    o.web.enabled = false;
    o.web.bind = "0.0.0.0";
    o.web.port_text = "9000";
    o.web.user = "bob";
    o.web.password = "longenoughpassword";
    o.web.confirm = "longenoughpassword";
    e = applyWebSetPassword(webModel(t, o));
    j = jsonOf(e);
    CHECK(j["web"]["enabled"] == false && j["web"]["bindAddress"] == "0.0.0.0" && j["web"]["port"] == 9000 && j["web"]["username"] == "bob" && j["web"]["password"] == "longenoughpassword");
    CHECK(!e.web.dirty);
    // a password with characters that must be escaped round-trips through the JSON
    o.web.password = "a\"b\\c\td\n\x01z";
    o.web.confirm = o.web.password;
    e = applyWebSetPassword(webModel(t, o));
    j = jsonOf(e);
    CHECK(j.is_object() && j["web"]["password"] == "a\"b\\c\td\n\x01z");
    // a user name with quotes
    o.web.password.clear();
    o.web.confirm.clear();
    o.web.user = "o\"ne\\two";
    j = jsonOf(applyWebApply(webModel(t, o)));
    CHECK(j.is_object() && j["web"]["username"] == "o\"ne\\two");

    // CLEAR PASSWORD: only while one is set
    e = applyWebClearPassword(m);
    CHECK(e.commands.size() == 1 && e.ask.kind == AskKind::ServerConfig && !e.web.dirty);
    CHECK(commandText(cmdOf(e, 0)) == "{\"web\":{\"enabled\":true,\"bindAddress\":\"127.0.0.1\",\"port\":8073,\"username\":\"admin\",\"clearPassword\":true}}");
    t.facts["fact.web.hasPassword"] = "0";
    CHECK(applyWebClearPassword(webModel(t)).empty());
    t.facts["fact.web.hasPassword"] = "1";

    // APPLY: nothing unless something waits; the staged copy, no password member; the draft is applied
    CHECK(applyWebApply(m).empty());
    e = applyWebApply(staged);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_SERVER_CONFIG && cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN && e.ask.kind == AskKind::ServerConfig);
    CHECK(commandText(cmdOf(e, 0)) == "{\"web\":{\"enabled\":false,\"bindAddress\":\"0.0.0.0\",\"port\":9000,\"username\":\"bob\"}}");
    CHECK(e.local == Local::Web && !e.web.dirty && e.web.error.empty() && e.web.have && !e.web.enabled && e.web.bind == "0.0.0.0");
    // the port is clamped as the box clamps it, and an unreadable one is the engine's own
    o = ReadOptions{};
    o.web.have = true;
    o.web.dirty = true;
    o.web.enabled = true;
    o.web.bind = "127.0.0.1";
    o.web.user = "admin";
    o.web.port_text = "80";
    CHECK(jsonOf(applyWebApply(webModel(t, o)))["web"]["port"] == 1024);
    o.web.port_text = "70000";
    CHECK(jsonOf(applyWebApply(webModel(t, o)))["web"]["port"] == 65535);
    o.web.port_text = " 9100 ";
    CHECK(jsonOf(applyWebApply(webModel(t, o)))["web"]["port"] == 9100);
    o.web.port_text = "abc";
    CHECK(jsonOf(applyWebApply(webModel(t, o)))["web"]["port"] == 8073);
    o.web.port_text = "";
    CHECK(jsonOf(applyWebApply(webModel(t, o)))["web"]["port"] == 8073);
    o.web.port_text = "-5";
    CHECK(jsonOf(applyWebApply(webModel(t, o)))["web"]["port"] == 8073);

    // THE ENGINE'S ANSWER: a refusal is the red line, "ok" and "no change" are not
    WebDraft d;
    applyServerAnswer(d, FOXAPI_BAD_ARGUMENT, "the password must be at least 8 characters");
    CHECK(d.error == "the password must be at least 8 characters");
    WebDraft fine;
    applyServerAnswer(fine, FOXAPI_OK, "the password is being hashed and its record will follow");
    applyServerAnswer(fine, FOXAPI_NO_CHANGE, "already so");
    CHECK(fine.error.empty());
    // a new apply clears the line (applyWebSettings: webError_.clear())
    o = ReadOptions{};
    o.web.have = true;
    o.web.dirty = true;
    o.web.enabled = true;
    o.web.error = "the password must be at least 8 characters";
    e = applyWebApply(webModel(t, o));
    CHECK(e.commands.size() == 1 && e.web.error.empty());
}

void catEffects() {
    StubTable t = webRunning();
    fillServers(t, kActive, 8073, 0, 0, 0, kActive, 0, 0);
    t.facts["fact.cat.port"] = "4532";
    CatModel m = catModel(t);
    Effect e = applyCatAccept(m, true);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_SERVER_CONFIG && cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN && e.ask.kind == AskKind::ServerConfig);
    CHECK(commandText(cmdOf(e, 0)) == "{\"cat\":{\"enabled\":true}}");
    CHECK(e.local == Local::Cat && e.cat.have && e.cat.enabled && !e.cat.bind_all && e.cat.port_text == "4532");
    CHECK(commandText(cmdOf(applyCatAccept(m, false), 0)) == "{\"cat\":{\"enabled\":false}}");
    // the port: a COMMIT, clamped as the box clamps it; text that is no number sends nothing
    e = applyCatPort(m, "4533");
    CHECK(e.commands.size() == 1 && commandText(cmdOf(e, 0)) == "{\"cat\":{\"port\":4533}}" && e.ask.kind == AskKind::ServerConfig && e.cat.port_text == "4533" && e.local == Local::Cat);
    CHECK(commandText(cmdOf(applyCatPort(m, "80"), 0)) == "{\"cat\":{\"port\":1024}}" && applyCatPort(m, "80").cat.port_text == "1024");
    CHECK(commandText(cmdOf(applyCatPort(m, "70000"), 0)) == "{\"cat\":{\"port\":65535}}");
    CHECK(commandText(cmdOf(applyCatPort(m, " 4600 "), 0)) == "{\"cat\":{\"port\":4600}}");
    CHECK(applyCatPort(m, "").empty() && applyCatPort(m, "abc").empty() && applyCatPort(m, "45x").empty() && applyCatPort(m, "-1").empty() && applyCatPort(m, "4.5").empty());
    // reachable from other machines
    e = applyCatReachable(m, true);
    CHECK(commandText(cmdOf(e, 0)) == "{\"cat\":{\"bindAll\":true}}" && e.cat.bind_all && e.ask.kind == AskKind::ServerConfig);
    CHECK(commandText(cmdOf(applyCatReachable(m, false), 0)) == "{\"cat\":{\"bindAll\":false}}");
    // the staged copy chains: a port just sent is kept when the box is ticked next
    ReadOptions o;
    o.cat.have = true;
    o.cat.enabled = true;
    o.cat.bind_all = false;
    o.cat.port_text = "4700";
    o.server_asks_pending = 1;
    const CatModel staged = catModel(t, o);
    e = applyCatReachable(staged, true);
    CHECK(e.cat.enabled && e.cat.bind_all && e.cat.port_text == "4700");
}

// ================================================================================================================================
// the Plugin store
// ================================================================================================================================

constexpr std::uint32_t kInstalled = FOXAPI_ITEM_INSTALLED, kCompatible = FOXAPI_ITEM_COMPATIBLE, kBuild = FOXAPI_ITEM_BUILD_HERE;
const std::string kShaA(64, 'a'), kShaB(64, 'b'), kShaC(64, 'c'), kShaD(64, 'd');

void setEntry(StubTable& t, const char* key, const char* name, std::uint32_t flags, const char* version, const char* category, const char* summary) {
    t.add(FOXAPI_LIST_CATALOGUE, 0, name, key, flags, 3, 412000);
    const std::string p = std::string("fact.store.") + key + ".";
    t.facts[p + "version"] = version;
    t.facts[p + "author"] = "FoxSDR";
    t.facts[p + "licence"] = "MIT";
    t.facts[p + "summary"] = summary;
    t.facts[p + "description"] = std::string(summary) + " in full.";
    t.facts[p + "category"] = category;
    t.facts[p + "platforms"] = "windows/x64, linux/x64";
    t.facts[p + "installState"] = "none";
}

// Six entries: ADS-B (aircraft, free to get, three pictures), AIS (marine, installed), WEFAX (satellites, an update planned), Pager (a legal notice), Linux Thing (no build for this
// machine), Maps Thing (from a site the user added, experimental).
StubTable storeTable() {
    StubTable t;
    t.facts["fact.store.url"] = "https://foxsdr.com/plugins/index.json";
    t.facts["fact.store.readThisSession"] = "1";
    t.facts["fact.store.fromCache"] = "0";
    t.facts["fact.store.refreshFailed"] = "0";
    t.facts["fact.store.cacheTime"] = "1762167600";
    t.facts["fact.store.busyKey"] = "";
    t.facts["fact.store.resultKey"] = "";
    t.facts["fact.store.sites"] = "";
    t.facts["fact.store.oldCopies"] = "";
    t.facts["fact.store.cleanupReport"] = "";
    t.facts["fact.status.catalogStatus"] = "";
    t.facts["fact.status.catalogError"] = "";
    t.facts["fact.status.installReport"] = "";
    t.facts["fact.status.installError"] = "";
    t.facts["fact.status.addAllSummary"] = "";

    setEntry(t, "adsb", "ADS-B Decoder", kCompatible | kBuild, "1.2.0", "aircraft", "Aircraft positions");
    t.facts["fact.store.adsb.whatsNew"] = "Faster decoding.";
    t.facts["fact.store.adsb.published"] = "2026-10-01";
    t.facts["fact.store.adsb.sha256"] = kShaD;
    t.facts["fact.store.adsb.homepage"] = "https://foxsdr.com/adsb";
    t.facts["fact.store.adsb.capabilities"] = "9";   // an audio decoder that publishes map targets
    t.facts["fact.store.adsb.pictures"] = kShaA + "\tThe map view\n" + kShaB + "\tThe settings\n" + kShaC + "\tAn old one\n";

    setEntry(t, "ais", "AIS Decoder", kInstalled | kCompatible | kBuild, "1.0.0", "marine", "Ship positions");
    t.facts["fact.store.ais.installState"] = "installed";
    t.facts["fact.store.ais.installedVersion"] = "1.0.0";
    t.facts["fact.store.ais.blocked"] = "already installed";

    setEntry(t, "wefax", "WEFAX", kInstalled | kCompatible | kBuild, "1.1.0", "satellites-weather", "Weather charts");
    t.facts["fact.store.wefax.installState"] = "update";
    t.facts["fact.store.wefax.installedVersion"] = "1.0.0";
    t.facts["fact.store.wefax.installedFile"] = "wefax-1.0.0.dll";
    t.facts["fact.store.wefax.updateReason"] = "a newer build is published";
    t.facts["fact.store.wefax.whatsNew"] = "Hand start and slant.";
    t.facts["fact.store.wefax.blocked"] = "already installed";
    t.add(FOXAPI_LIST_STORE_UPDATES, 1, "wefax", "1.0.0 -> 1.1.0");

    setEntry(t, "pager", "Pager", kCompatible | kBuild, "2.0.0", "meters-paging", "Pager messages");
    t.facts["fact.store.pager.legal"] = "Receiving pager traffic is an offence in some places. You are responsible.";

    setEntry(t, "linuxonly", "Linux Thing", kCompatible, "0.5.0", "", "Only for Linux");
    t.facts["fact.store.linuxonly.platforms"] = "linux/x64";
    t.facts["fact.store.linuxonly.blocked"] = "no build for windows/x64";

    setEntry(t, "maps@example.com", "Maps Thing", kCompatible | kBuild, "0.1.0", "maps-tools", "Maps from a site");
    t.facts["fact.store.maps@example.com.site"] = "https://example.com/index.json";
    t.facts["fact.store.maps@example.com.experimental"] = "1";
    return t;
}

StoreModel storeModel(StubTable& t, const ReadOptions& o = ReadOptions{}) { return readStore(t, o); }
StoreModel storeModel(StubTable&& t, const ReadOptions& o) { return readStore(t, o); }

const StoreCard* cardById(const StoreModel& m, const std::string& id) {
    for (const StoreCategory& c : m.categories) {
        for (const StoreCard& k : c.cards) {
            if (k.id == id) { return &k; }
        }
    }
    return nullptr;
}
const StoreCard& card(const StoreModel& m, const std::string& id) {
    static const StoreCard none{};
    const StoreCard* c = cardById(m, id);
    return c != nullptr ? *c : none;
}
Names headings(const StoreModel& m) {
    Names h;
    for (const StoreCategory& c : m.categories) { h.push_back(c.heading); }
    return h;
}
Names cardIds(const StoreModel& m) {
    Names ids;
    for (const StoreCategory& c : m.categories) {
        for (const StoreCard& k : c.cards) { ids.push_back(k.id); }
    }
    return ids;
}
Names badgeTexts(const std::vector<StoreBadge>& b) {
    Names out;
    for (const StoreBadge& x : b) { out.push_back(x.text); }
    return out;
}

void storeBrowseRead() {
    StubTable t = storeTable();
    ReadOptions o;
    o.now_unix = 1762167600 + 3 * 86400;
    StoreModel m = storeModel(t, o);
    CHECK(m.have_catalogue && m.read_this_session && m.update_count == 1 && m.updates_label == "UPDATES (1)" && m.browse_label == "BROWSE" && m.search_placeholder == "Search plugins");
    CHECK(m.tab == 0 && m.check_label == "CHECK AGAIN" && m.check_enabled && m.sites_label == "SITES" && m.source_url == "https://foxsdr.com/plugins/index.json");
    // THE CATEGORIES in the ImGui order, a card under the heading of its category; one with no category (or one this build does not know) is OTHER
    CHECK(headings(m) == Names({"AIRCRAFT", "MARINE", "SATELLITES AND WEATHER", "METERS AND PAGING", "MAPS AND TOOLS", "OTHER"}));
    CHECK(cardIds(m) == Names({"adsb", "ais", "wefax", "pager", "maps@example.com", "linuxonly"}));
    CHECK(at(m.categories, 0).census == "aircraft" && at(m.categories, 2).census == "satellites" && at(m.categories, 3).census == "meters" && at(m.categories, 4).census == "maps" && at(m.categories, 5).census == "other");
    // THE CARDS' KEYS: GET, INSTALLED, UPDATE with the two versions, GET that opens a page (a legal notice), GET dead with a reason, a site's plugin
    const StoreCard& adsb = card(m, "adsb");
    CHECK(adsb.name == "ADS-B Decoder" && adsb.summary == "Aircraft positions" && adsb.glyph == "aircraft" && adsb.category_word == "AIRCRAFT");
    CHECK(adsb.key_kind == kKeyGet && adsb.key_label == "GET" && adsb.key_enabled && !adsb.opens_page && adsb.key_reason.empty() && adsb.badges.empty());
    const StoreCard& ais = card(m, "ais");
    CHECK(ais.key_kind == kKeyInstalled && ais.key_label == "INSTALLED" && !ais.key_enabled);
    const StoreCard& wefax = card(m, "wefax");
    CHECK(wefax.key_kind == kKeyUpdate && wefax.key_label == "UPDATE" && wefax.key_enabled && wefax.version_note == "1.0.0 to 1.1.0");
    const StoreCard& pager = card(m, "pager");
    CHECK(pager.key_kind == kKeyGet && pager.key_enabled && pager.opens_page);
    const StoreCard& linux = card(m, "linuxonly");
    CHECK(linux.key_kind == kKeyGet && !linux.key_enabled && linux.key_reason == "no build for windows/x64" && badgeTexts(linux.badges) == Names({"LINUX ONLY"}));
    CHECK(at(linux.badges, 0).tone == Tone::Neutral);
    const StoreCard& maps = card(m, "maps@example.com");
    CHECK(maps.key_kind == kKeyGet && maps.opens_page && badgeTexts(maps.badges) == Names({"THIRD PARTY", "EXPERIMENTAL"}) && at(maps.badges, 0).tone == Tone::Warn && at(maps.badges, 1).tone == Tone::Warn);
    // WITHIN A CATEGORY the cards are by name, ignoring case, and by key when two names are the same
    {
        StubTable order = storeTable();
        setEntry(order, "zeta", "Zeta Air", kCompatible | kBuild, "1.0.0", "aircraft", "Z");
        setEntry(order, "alpha", "alpha air", kCompatible | kBuild, "1.0.0", "aircraft", "A");
        setEntry(order, "alpha2", "alpha air", kCompatible | kBuild, "1.0.0", "aircraft", "A2");
        setEntry(order, "alpha1", "Alpha Air", kCompatible | kBuild, "1.0.0", "aircraft", "A1");
        const Names ids = cardIds(storeModel(order, o));
        CHECK(Names(ids.begin(), ids.begin() + std::min<std::size_t>(5, ids.size())) == Names({"adsb", "alpha", "alpha1", "alpha2", "zeta"}));
    }
    // a card with no summary says the description's words
    t.facts["fact.store.adsb.summary"] = "";
    CHECK(card(storeModel(t, o), "adsb").summary == "Aircraft positions in full.");
    t.facts["fact.store.adsb.summary"] = "Aircraft positions";

    // THE CATALOGUE LINE: read today at HH:MM; the date when it was not today; a kept copy that could not refresh, amber; none read
    {
        const std::time_t tt = 1762167600;
        std::tm tm{};
        localTm(tt, tm);
        char today[16], earlier[32];
        std::snprintf(today, sizeof(today), "%02d:%02d", tm.tm_hour, tm.tm_min);
        std::snprintf(earlier, sizeof(earlier), "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        ReadOptions same;
        same.now_unix = 1762167600 + 60;
        CHECK(storeModel(t, same).catalogue_line == std::string("Catalogue read ") + today && storeModel(t, same).catalogue_tone == Tone::Neutral);
        CHECK(m.catalogue_line == std::string("Catalogue read ") + earlier);
        char date[16];
        std::snprintf(date, sizeof(date), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        t.facts["fact.store.refreshFailed"] = "1";
        t.facts["fact.status.catalogError"] = "the connection timed out";
        m = storeModel(t, o);
        CHECK(m.catalogue_line == std::string("Catalogue from ") + date + "; could not refresh: the connection timed out" && m.catalogue_tone == Tone::Warn);
        // no time recorded: never drawn as 1970
        t.facts["fact.store.cacheTime"] = "0";
        CHECK(storeModel(t, o).catalogue_line == "Catalogue from an unknown date; could not refresh: the connection timed out");
        t.facts["fact.store.cacheTime"] = "1762167600";
        t.facts["fact.store.refreshFailed"] = "0";
        // a catalogue that loaded WITH a warning: the list is real, the reason is above it
        m = storeModel(t, o);
        CHECK(m.top_warning == "the connection timed out" && has(m.catalogue_line, "Catalogue read"));
        t.facts["fact.status.catalogError"] = "";
        CHECK(storeModel(t, o).top_warning.empty());
    }

    // GET EVERYTHING: one to fetch (ADS-B), one to update (WEFAX); the notice that holds the pager back is said, with the tick that covers it; the site's plugin is left out and the key says so
    m = storeModel(t, o);
    CHECK(m.get_all_label == "GET EVERYTHING" && m.get_all_enabled && m.get_all_reason.empty());
    CHECK(has(m.foot_note, "1 to fetch and 1 to update, one after another. Each is fetched over https and refused unless its bytes hash to the sha256 the catalogue published") &&
          has(m.foot_note, "A module that fails does not stop the rest."));
    CHECK(m.bulk_shown && m.bulk_sentence == "I accept the 1 legal notice on its plugin's page" && !m.bulk_tick);
    CHECK(m.notice_note == "1 of these carry a legal notice from their maker: Pager. Each notice is on that plugin's page.");
    CHECK(has(m.left_out_note, "Plugins from sites you added are not included here."));
    // the tick covers the notice: the pager joins the run (the count says so)
    o.store.bulk_tick = true;
    m = storeModel(t, o);
    CHECK(m.bulk_tick && has(m.foot_note, "2 to fetch and 1 to update"));
    o.store.bulk_tick = false;
    // a search hides the key (and the cards that do not match)
    o.store.search = "AIRCRAFT";
    m = storeModel(t, o);
    CHECK(m.search == "AIRCRAFT" && cardIds(m) == Names({"adsb"}) && m.get_all_label.empty() && !m.get_all_enabled && !m.bulk_shown);
    o.store.search = "weather charts";   // the summary's words
    CHECK(cardIds(storeModel(t, o)) == Names({"wefax"}));
    o.store.search = "zzz";
    m = storeModel(t, o);
    CHECK(m.categories.empty() && m.empty_line == "No plugin matches that search." && m.empty_tone == Tone::Neutral);
    o.store.search.clear();
    // the key's own reason when it cannot be pressed
    StubTable all = storeTable();
    for (const char* k : {"adsb", "pager", "linuxonly", "maps@example.com"}) { all.facts[std::string("fact.store.") + k + ".installState"] = "installed"; }
    m = storeModel(all, o);
    CHECK(m.get_all_enabled);   // WEFAX still has its planned update
    all.facts["fact.store.wefax.installState"] = "installed";
    all.lists[FOXAPI_LIST_STORE_UPDATES].clear();
    m = storeModel(all, o);
    CHECK(m.get_all_label == "GET EVERYTHING" && !m.get_all_enabled && m.get_all_reason == "every module in the catalogue is already fitted, and none has a newer build");
    CHECK(m.foot_note == "Cannot add all: every module in the catalogue is already fitted, and none has a newer build");
}

void storeStates() {
    ReadOptions o;
    // NO CATALOGUE: never asked, asked and failed, asked and empty
    StubTable t;
    t.facts["fact.store.url"] = "https://foxsdr.com/plugins/index.json";
    StoreModel m = storeModel(t, o);
    CHECK(!m.have_catalogue && m.catalogue_line == "CATALOGUE NOT READ" && m.check_label == "CHECK NOW" && m.check_enabled);
    CHECK(m.empty_line == "No catalogue has been read yet. Press CHECK NOW above and this application asks the source once." && m.empty_tone == Tone::Neutral && m.get_all_label.empty());
    t.facts["fact.status.catalogError"] = "could not resolve the host";
    m = storeModel(t, o);
    CHECK(m.empty_line == "The catalogue could not be read: could not resolve the host" && m.empty_tone == Tone::Err && m.check_label == "CHECK AGAIN" && m.top_warning.empty());
    t.facts["fact.status.catalogError"] = "";
    t.facts["fact.status.catalogStatus"] = "read, no entries";
    m = storeModel(t, o);
    CHECK(m.empty_line == "The catalogue was read and it lists no plugins." && m.check_label == "CHECK AGAIN");
    // no catalogue url: CHECK is dead
    t.facts["fact.store.url"] = "";
    CHECK(!storeModel(t, o).check_enabled);

    // A TRANSFER IN FLIGHT: a catalogue read (kind 1) or a download (kind 2, with this card's key FITTING...)
    StubTable b = storeTable();
    b.add(FOXAPI_LIST_STORE_ACTIVITY, 1, "WEFAX", "", kActive, 0.4, 2);
    b.facts["fact.store.busyKey"] = "wefax";
    m = storeModel(b, o);
    CHECK(m.busy && m.busy_kind == 2 && m.busy_label == "downloading WEFAX" && m.cancel_label == "CANCEL" && !m.check_enabled);
    const StoreCard& fitting = card(m, "wefax");
    CHECK(fitting.key_kind == kKeyFitting && fitting.key_label == "FITTING..." && !fitting.key_enabled && fitting.progress == "downloading WEFAX" && fitting.version_note.empty());
    CHECK(fitting.progress_norm > 0.39f && fitting.progress_norm < 0.41f);
    // the other cards' keys do not carry the progress line, and an update that cannot start while a transfer runs says why
    CHECK(card(m, "adsb").progress.empty());
    b.facts["fact.store.busyKey"] = "adsb";   // the one downloading is ADS-B: WEFAX's UPDATE waits behind it
    m = storeModel(b, o);
    CHECK(card(m, "adsb").key_kind == kKeyFitting && card(m, "wefax").key_kind == kKeyUpdate && !card(m, "wefax").key_enabled && card(m, "wefax").key_reason == "a transfer is already in progress");
    // a catalogue read: no card is FITTING (a read has no plugin)
    StubTable r = storeTable();
    r.add(FOXAPI_LIST_STORE_ACTIVITY, 1, "", "", kActive, 0.0, 1);
    r.facts["fact.store.busyKey"] = "adsb";   // (a stale key: it is dropped unless a download is in flight)
    m = storeModel(r, o);
    CHECK(m.busy && m.busy_kind == 1 && m.busy_label == "fetching the catalogue" && card(m, "adsb").key_kind == kKeyGet);
    // the ADD ALL run: the line says where it has got to, the key is dead
    StubTable run = storeTable();
    run.add(FOXAPI_LIST_STORE_ACTIVITY, 2, "AIS Decoder", "", kActive, 2, 5);
    m = storeModel(run, o);
    CHECK(m.add_all_running && !m.get_all_enabled && m.foot_note == "installing 2 of 5: AIS Decoder" && !m.bulk_shown && m.notice_note.empty());
    run.lists[FOXAPI_LIST_STORE_ACTIVITY].clear();
    run.add(FOXAPI_LIST_STORE_ACTIVITY, 2, "", "", kActive, 0, 5);
    CHECK(storeModel(run, o).foot_note == "installing 0 of 5: starting");
    // ...and what the last run did, with its level
    run.lists[FOXAPI_LIST_STORE_ACTIVITY].clear();
    run.facts["fact.status.addAllSummary"] = "5 fitted, 1 failed";
    run.add(FOXAPI_LIST_STATUS_LINES, 1, "addAllSummary", "5 fitted, 1 failed", 0, 3);
    m = storeModel(run, o);
    CHECK(m.add_all_summary == "5 fitted, 1 failed" && m.add_all_failed);
    run.lists[FOXAPI_LIST_STATUS_LINES][0].value[0] = 1;
    CHECK(!storeModel(run, o).add_all_failed);
    // the install error, wherever the user is, with the plugin's name; the page of that plugin carries it under its key
    StubTable er = storeTable();
    er.facts["fact.store.resultKey"] = "adsb";
    er.facts["fact.status.installError"] = "the file's sha256 does not match";
    m = storeModel(er, o);
    CHECK(m.top_error == "ADS-B Decoder: the file's sha256 does not match" && m.top_error_tone == Tone::Err);
    er.facts["fact.store.resultKey"] = "gone";
    CHECK(storeModel(er, o).top_error == "the file's sha256 does not match");
    er.facts["fact.store.resultKey"] = "adsb";
    o.store.page_id = "adsb";
    m = storeModel(er, o);
    CHECK(m.top_error.empty() && m.plugin.report == "the file's sha256 does not match" && m.plugin.report_tone == Tone::Err);
    er.facts["fact.status.installError"] = "";
    er.facts["fact.status.installReport"] = "ADS-B Decoder 1.2.0 fitted";
    m = storeModel(er, o);
    CHECK(m.plugin.report == "ADS-B Decoder 1.2.0 fitted" && m.plugin.report_tone == Tone::Good);
    er.facts["fact.store.resultKey"] = "ais";   // the outcome is about another plugin: this page says nothing of it
    CHECK(storeModel(er, o).plugin.report.empty());
}

void storeUpdatesRead() {
    StubTable t = storeTable();
    ReadOptions o;
    o.store.tab = 1;
    StoreModel m = storeModel(t, o);
    CHECK(m.tab == 1 && m.categories.empty() && m.get_all_label.empty());
    CHECK(m.updates_heading == "UPDATES AVAILABLE" && m.update_all_label == "UPDATE ALL" && m.update_all_enabled);
    CHECK(m.updates.size() == 1 && at(m.updates, 0).id == "wefax" && at(m.updates, 0).name == "WEFAX" && at(m.updates, 0).version_note == "1.0.0 to 1.1.0" && at(m.updates, 0).whats_new == "Hand start and slant." &&
          at(m.updates, 0).key_label == "UPDATE" && at(m.updates, 0).key_enabled && at(m.updates, 0).badge.empty() && at(m.updates, 0).glyph == "satellites-weather");
    CHECK(m.footnotes == Names({"Updates are fetched when you press CHECK AGAIN. Nothing updates itself."}));
    // the update's own reason stands in for a paragraph the catalogue does not carry
    t.facts["fact.store.wefax.whatsNew"] = "";
    CHECK(at(storeModel(t, o).updates, 0).whats_new == "a newer build is published");
    // a transfer: UPDATE ALL is dead, the row's key waits
    StubTable busy = storeTable();
    busy.add(FOXAPI_LIST_STORE_ACTIVITY, 1, "ADS-B Decoder", "", kActive, 0.1, 2);
    busy.facts["fact.store.busyKey"] = "adsb";
    m = storeModel(busy, o);
    CHECK(!m.update_all_enabled && !at(m.updates, 0).key_enabled && at(m.updates, 0).key_kind == kKeyUpdate && at(m.updates, 0).key_reason == "a transfer is already in progress");
    // no update: the heading, and no key
    StubTable none = storeTable();
    none.lists[FOXAPI_LIST_STORE_UPDATES].clear();
    m = storeModel(none, o);
    CHECK(m.updates_heading == "NO UPDATES" && m.update_all_label.empty() && !m.update_all_enabled && m.updates.empty() && m.footnotes.empty() && m.updates_label == "UPDATES (0)");
    // no catalogue at all: the reason it is empty
    StubTable empty;
    m = storeModel(empty, o);
    CHECK(m.updates_heading == "NO UPDATES" && m.empty_line == "No catalogue has been read yet. Press CHECK NOW above and this application asks the source once.");
    // an update for a site's plugin: the badge, the left-out note under the key
    StubTable site = storeTable();
    site.facts["fact.store.maps@example.com.installState"] = "update";
    site.facts["fact.store.maps@example.com.installedVersion"] = "0.0.9";
    site.add(FOXAPI_LIST_STORE_UPDATES, 2, "maps@example.com", "0.0.9 -> 0.1.0");
    m = storeModel(site, o);
    CHECK(m.updates.size() == 2 && at(m.updates, 0).id == "maps@example.com" && at(m.updates, 1).id == "wefax");   // by name: "Maps Thing" before "WEFAX"
    CHECK(at(m.updates, 0).badge == "THIRD PARTY" && m.footnotes.size() == 2 && has(at(m.footnotes, 1), "Plugins from sites you added are not included here."));
    // a search narrows the rows, not the count on the tab
    o.store.search = "weather";
    m = storeModel(site, o);
    CHECK(m.updates.size() == 1 && at(m.updates, 0).id == "wefax" && m.updates_label == "UPDATES (2)");
    // THE ROWS ARE BY NAME, not in the catalogue's order (the catalogue lists WEFAX, then AIS, then the site's plugin: by name it is AIS, Maps Thing, WEFAX)
    {
        StubTable three = site;
        three.facts["fact.store.ais.installState"] = "update";
        three.facts["fact.store.ais.installedVersion"] = "0.9.0";
        three.add(FOXAPI_LIST_STORE_UPDATES, 3, "ais", "0.9.0 -> 1.0.0");
        o.store.search.clear();
        const StoreModel tm = storeModel(three, o);
        CHECK(tm.updates.size() == 3 && at(tm.updates, 0).id == "ais" && at(tm.updates, 1).id == "maps@example.com" && at(tm.updates, 2).id == "wefax");
    }
    // an ADD ALL run is going: UPDATE ALL waits for it (the key is drawn, dead)
    {
        StubTable run = storeTable();
        run.add(FOXAPI_LIST_STORE_ACTIVITY, 2, "AIS Decoder", "", kActive, 1, 5);
        o.store.search.clear();
        const StoreModel rm = storeModel(run, o);
        CHECK(rm.add_all_running && rm.update_all_label == "UPDATE ALL" && !rm.update_all_enabled);
    }
    // an installed entry that the catalogue marks "newer": a planned update by id and still no key to press
    StubTable newer = storeTable();
    newer.facts["fact.store.wefax.installState"] = "newer";
    o.store.search.clear();
    m = storeModel(newer, o);
    CHECK(m.updates.size() == 1 && at(m.updates, 0).key_label == "UPDATE" && !at(m.updates, 0).key_enabled);
}

void storeSitesAndCleanup() {
    StubTable t = storeTable();
    t.facts["fact.store.sites"] = "https://example.com/index.json\t2\t1\t0\t0\t\nhttps://broken.example.org/index.json\t3\t0\t2\t1\tHTTP 404\nhttps://kept.example.net/index.json\t1\t4\t0\t0\t\n"
                                  "https://new.example.net/index.json\t0\t0\t0\t0\t\n";
    ReadOptions o;
    o.store.sites_open = true;
    StoreModel m = storeModel(t, o);
    CHECK(m.sites_open && m.sites.size() == 4 && at(m.sites, 0).url == "https://example.com/index.json" && at(m.sites, 0).remove_label == "REMOVE" && at(m.sites, 0).remove_enabled);
    CHECK(has(at(m.sites, 0).address, "example.com") && m.no_sites_line == "No sites added." && m.add_site_label == "ADD" && has(m.consent_sentence, "Plugins from this site are not reviewed by FoxSDR."));
    CHECK(has(at(m.sites, 0).remove_tooltip, "Forgets this site."));
    // one status line a site, more when the merge hid something - the failed one's reason verbatim, amber
    Names texts;
    for (const StoreLine& l : m.site_lines) { texts.push_back(l.text); }
    CHECK(texts == Names({"example.com: 1 plugin", "broken.example.org: could not be read: HTTP 404", "broken.example.org: 2 hidden: same id as a plugin already listed",
                          "broken.example.org: 1 hidden: files not on the site's own host", "kept.example.net: 4 plugins (kept copy from an earlier session)",
                          "new.example.net: not read yet"}));
    CHECK(at(m.site_lines, 0).tone == Tone::Neutral && at(m.site_lines, 1).tone == Tone::Warn && at(m.site_lines, 2).tone == Tone::Warn && at(m.site_lines, 3).tone == Tone::Warn && at(m.site_lines, 4).tone == Tone::Neutral);
    CHECK(m.site_urls.size() == 4);
    // ADD's reason, in the order the user can cure it
    CHECK(!m.add_site_enabled && m.add_site_reason == "Type the address of the site's index.json first.");
    o.store.site_draft = "https://sites.example.org/index.json";
    m = storeModel(t, o);
    CHECK(!m.add_site_enabled && m.add_site_reason == "Tick the box below first." && m.site_draft == "https://sites.example.org/index.json");
    o.store.consent = true;
    m = storeModel(t, o);
    CHECK(m.add_site_enabled && m.add_site_reason.empty() && m.consent);
    t.add(FOXAPI_LIST_STORE_ACTIVITY, 1, "", "", kActive, 0.0, 1);
    m = storeModel(t, o);
    CHECK(!m.add_site_enabled && m.add_site_reason == "a transfer is already in progress" && !at(m.sites, 0).remove_enabled && at(m.sites, 0).remove_tooltip == "a transfer is already in progress");
    t.lists[FOXAPI_LIST_STORE_ACTIVITY].clear();

    // THE OLD COPIES: the key's count, three lines and "and N more", the confirmation with every one, its keys
    t.facts["fact.store.oldCopies"] = "ais-1.0.0.dll\tAIS Decoder\t1.0.0\tais-1.1.0.dll\t1.1.0\nwefax-0.9.dll\tWEFAX\t0.9.0\twefax-1.0.0.dll\t1.0.0\n"
                                      "adsb-1.dll\tADS-B Decoder\t1.0.0\tadsb-1.2.dll\t1.2.0\npager-1.dll\tPager\t1.0.0\tpager-2.dll\t2.0.0\n"
                                      "maps-0.dll\tMaps Thing\t0.0.1\tmaps-0.1.dll\t0.1.0\n";
    t.facts["fact.store.cleanupReport"] = "Removed 2 old files.";
    o.store.tab = 1;
    m = storeModel(t, o);
    CHECK(m.cleanup_label == "CLEAN UP OLD VERSIONS (5)" && m.cleanup_enabled && m.cleanup_report == "Removed 2 old files.");
    CHECK(m.cleanup_lines == Names({"AIS Decoder 1.0.0 - ais-1.0.0.dll (1.1.0 stays)", "WEFAX 0.9.0 - wefax-0.9.dll (1.0.0 stays)", "ADS-B Decoder 1.0.0 - adsb-1.dll (1.2.0 stays)", "and 2 more"}));
    CHECK(m.cleanup_all.size() == 5 && at(m.cleanup_all, 4) == "Maps Thing 0.0.1 - maps-0.dll (0.1.0 stays)");
    CHECK(m.cleanup_title == "Remove old plugin versions" && has(m.cleanup_text, "Each of these is an older copy of a plugin whose newer version is installed and running. Only these files are deleted."));
    CHECK(m.cleanup_yes == "Remove 5 files" && m.cleanup_no == "Keep them" && !m.cleanup_open);
    o.store.cleanup_open = true;
    CHECK(storeModel(t, o).cleanup_open);
    // one old copy: the singular; none: the key is gone and an open confirmation closes itself; the report stays where the key was
    t.facts["fact.store.oldCopies"] = "ais-1.0.0.dll\tAIS Decoder\t1.0.0\tais-1.1.0.dll\t1.1.0\n";
    m = storeModel(t, o);
    CHECK(m.cleanup_label == "CLEAN UP OLD VERSIONS (1)" && m.cleanup_yes == "Remove 1 file" && m.cleanup_lines.size() == 1);
    t.facts["fact.store.oldCopies"] = "";
    m = storeModel(t, o);
    CHECK(m.cleanup_label.empty() && !m.cleanup_open && m.cleanup_lines.empty() && m.cleanup_report == "Removed 2 old files.");
    CHECK(m.draft_changed && !m.draft.cleanup_open);   // the draft is told, so the confirmation does not come back when a later update leaves a copy again
    // a transfer: the key is dead
    t.facts["fact.store.oldCopies"] = "ais-1.0.0.dll\tAIS Decoder\t1.0.0\tais-1.1.0.dll\t1.1.0\n";
    t.add(FOXAPI_LIST_STORE_ACTIVITY, 1, "", "", kActive, 0.0, 1);
    CHECK(!storeModel(t, o).cleanup_enabled);
}

void storePluginPage() {
    StubTable t = storeTable();
    t.add(FOXAPI_LIST_STORE_PICTURES, 1, kShaA.c_str(), "C:/cache/store-cache/aaaa.png", kActive, 2);
    t.add(FOXAPI_LIST_STORE_PICTURES, 2, kShaB.c_str(), "HTTP 404 for the picture", 0, 3);
    t.add(FOXAPI_LIST_STORE_PICTURES, 3, kShaC.c_str(), "", 0, 1);
    ReadOptions o;
    o.store.page_id = "adsb";
    StoreModel m = storeModel(t, o);
    const StorePluginPage& p = m.plugin;
    CHECK(p.shown && p.id == "adsb" && p.back_label == "< BROWSE" && p.name == "ADS-B Decoder" && p.glyph == "aircraft" && m.categories.empty() && m.updates.empty());
    // THE HEADER: maker, version, the size and build of THIS machine's build
    const std::string hostPlatform = std::string(cascade::core::pluginHostOs()) + "/" + cascade::core::pluginHostArch();
    CHECK(p.version == "FoxSDR  \xc2\xb7  version 1.2.0  \xc2\xb7  412 kB  \xc2\xb7  " + hostPlatform && p.meta == p.version);
    CHECK(p.key_kind == kKeyGet && p.key_label == "GET" && p.key_enabled && p.key_reason.empty() && p.origin.empty() && p.badges.empty());
    CHECK(p.what_it_does == "Aircraft positions in full." && p.whats_new == "Faster decoding." && !p.notice_shown && p.details_label == "SHOW DETAILS" && !p.details_open);
    CHECK(p.screenshots_heading == "SCREENSHOTS" && p.what_heading == "WHAT IT DOES" && p.new_heading == "WHAT'S NEW" && p.details_heading == "DETAILS" && p.notice_heading == "BEFORE YOU FIT IT");
    // THE SCREENSHOTS: ready (the file the publisher loads), failed (the reason), pending (the wait), and one nobody asked for yet (the wait)
    t.facts["fact.store.adsb.pictures"] = kShaA + "\tThe map view\n" + kShaB + "\tThe settings\n" + kShaC + "\tAn old one\n" + kShaD + "\tNever asked\n";
    m = storeModel(t, o);
    CHECK(m.plugin.screenshots.size() == 4);
    CHECK(at(m.plugin.screenshots, 0).state == kPicReady && at(m.plugin.screenshots, 0).path == "C:/cache/store-cache/aaaa.png" && at(m.plugin.screenshots, 0).word.empty() && at(m.plugin.screenshots, 0).caption == "The map view" &&
          at(m.plugin.screenshots, 0).sha256 == kShaA);
    CHECK(at(m.plugin.screenshots, 1).state == kPicFailed && at(m.plugin.screenshots, 1).word == "HTTP 404 for the picture" && at(m.plugin.screenshots, 1).path.empty());
    CHECK(at(m.plugin.screenshots, 2).state == kPicPending && at(m.plugin.screenshots, 2).word == "loading...");
    CHECK(at(m.plugin.screenshots, 3).state == kPicNone && at(m.plugin.screenshots, 3).word == "loading..." && at(m.plugin.screenshots, 3).caption == "Never asked");
    // a path the item cut is the whole fact
    {
        StubTable cutT = storeTable();
        const std::string longPath = "C:/" + std::string(220, 'p') + "/aaaa.png";
        cutT.add(FOXAPI_LIST_STORE_PICTURES, 1, kShaA.c_str(), longPath.substr(0, 159).c_str(), kActive, 2);
        cutT.facts["fact.store.picture." + kShaA + ".path"] = longPath;
        const StoreModel cm = storeModel(cutT, o);
        CHECK(!cm.plugin.screenshots.empty() && at(cm.plugin.screenshots, 0).path == longPath);
    }
    // a picture is queued: the one transfer slot is held
    CHECK(m.picture_busy && !m.check_enabled);
    // no picture published: one frame that says so
    t.facts["fact.store.adsb.pictures"] = "";
    m = storeModel(t, o);
    CHECK(m.plugin.screenshots.size() == 1 && at(m.plugin.screenshots, 0).word == "No pictures published yet" && at(m.plugin.screenshots, 0).sha256.empty());
    // THE DETAILS: the facts grid, in order, each with a source or the hatched reason
    const std::vector<PageFact>& f = m.plugin.facts;
    Names keys;
    for (const PageFact& x : f) { keys.push_back(x.key); }
    CHECK(keys == Names({"MAKER", "LICENCE", "VERSION", "PLUGIN ABI", "DOWNLOAD", "BUILDS FOR", "REACHES", "HOMEPAGE", "SHA-256", "PUBLISHED"}));
    CHECK(at(f, 0).value == "FoxSDR" && !at(f, 0).hatched && at(f, 1).value == "MIT" && at(f, 2).value == "1.2.0" && at(f, 3).value == "3, matches this build" && at(f, 3).tone == Tone::Good);
    CHECK(at(f, 4).value == "412 kB" && at(f, 5).value == "windows/x64, linux/x64" && at(f, 6).value == "Audio decoder, Map targets" && at(f, 7).value == "https://foxsdr.com/adsb" && at(f, 7).copyable);
    CHECK(at(f, 8).value == kShaD && at(f, 8).copyable && at(f, 9).value == "2026-10-01");
    CHECK(has(m.plugin.details, "MAKER: FoxSDR\nLICENCE: MIT\nVERSION: 1.2.0\nPLUGIN ABI: 3, matches this build\n") && has(m.plugin.reach_lead, "Declared by the maker, not enforced."));
    // ...and the hatched ones say why they are blank
    t.facts["fact.store.adsb.licence"] = "";
    t.facts["fact.store.adsb.author"] = "";
    t.facts["fact.store.adsb.homepage"] = "";
    t.facts["fact.store.adsb.sha256"] = "";
    t.facts["fact.store.adsb.published"] = "";
    t.facts["fact.store.adsb.capabilities"] = "";
    t.facts["fact.store.adsb.platforms"] = "";
    t.facts["fact.store.adsb.floor"] = "1.0.0";
    t.lists[FOXAPI_LIST_CATALOGUE][0].value[1] = 0;
    m = storeModel(t, o);
    const std::vector<PageFact>& g = m.plugin.facts;
    CHECK(g.size() == 11 && at(g, 0).hatched && at(g, 0).value == "not stated" && at(g, 1).hatched && at(g, 1).value == "none declared" && at(g, 1).tone == Tone::Warn && at(g, 4).hatched && at(g, 4).value == "not stated" &&
          at(g, 5).hatched && at(g, 6).hatched && at(g, 6).value == "not declared until it is fitted" && at(g, 7).hatched && at(g, 8).hatched && at(g, 9).hatched);
    CHECK(at(g, 10).key == "RETIRED BELOW" && at(g, 10).value == "1.0.0" && at(g, 10).tone == Tone::Warn);
    CHECK(has(m.plugin.version, "maker not stated") && has(m.plugin.version, "version 1.2.0") && !has(m.plugin.version, "412"));
    // the page of a plugin with no build here: said in the header and as the badge
    o.store.page_id = "linuxonly";
    m = storeModel(t, o);
    CHECK(has(m.plugin.version, "no build for this system") && badgeTexts(m.plugin.badges) == Names({"LINUX ONLY"}) && m.plugin.key_kind == kKeyGet && !m.plugin.key_enabled && m.plugin.key_reason == "no build for windows/x64");
    // DETAILS open and folded
    o.store.page_id = "adsb";
    o.store.details_open = true;
    m = storeModel(t, o);
    CHECK(m.plugin.details_open && m.plugin.details_label == "HIDE DETAILS");
    // an installed plugin's page: ON THIS MACHINE and FILE, from the host's record, the loaded module's own capabilities
    StubTable fitted = storeTable();
    fitted.add(FOXAPI_LIST_PLUGINS, 1, "AIS Decoder", "ais-1.0.0.dll", kActive, CASCADE_CAP_DECODER | CASCADE_CAP_PANEL);
    fitted.facts["fact.store.ais.installedFile"] = "AIS-1.0.0.DLL";   // ASCII case is ignored (the engine's own rule)
    o.store.page_id = "ais";
    m = storeModel(fitted, o);
    CHECK(m.plugin.key_kind == kKeyInstalled && m.plugin.key_label == "INSTALLED" && !m.plugin.key_enabled);
    Names fk;
    for (const PageFact& x : m.plugin.facts) { fk.push_back(x.key); }
    CHECK(fk.size() >= 12 && at(fk, 10) == "ON THIS MACHINE" && at(fk, 11) == "FILE");
    CHECK(at(m.plugin.facts, 6).value == "Audio decoder, A window of its own" && at(m.plugin.facts, 10).value == "fitted and started" && at(m.plugin.facts, 11).value == "ais-1.0.0.dll");
    // the update page: both versions, the key waits for a transfer
    o.store.page_id = "wefax";
    m = storeModel(fitted, o);
    CHECK(m.plugin.key_kind == kKeyUpdate && m.plugin.key_label == "UPDATE" && m.plugin.key_enabled && m.plugin.version_note == "1.0.0 to 1.1.0");
    // FITTING...: this plugin's own transfer, the progress line
    fitted.add(FOXAPI_LIST_STORE_ACTIVITY, 1, "WEFAX", "", kActive, 0.6, 2);
    fitted.facts["fact.store.busyKey"] = "wefax";
    m = storeModel(fitted, o);
    CHECK(m.plugin.key_kind == kKeyFitting && m.plugin.key_label == "FITTING..." && !m.plugin.key_enabled && m.plugin.progress == "downloading WEFAX" && m.plugin.progress_norm > 0.59f);
    // a page the catalogue no longer lists goes (once a catalogue is on screen), and the model says the draft changed
    o.store.page_id = "gone";
    o.store.notice_tick = true;
    o.store.details_open = true;
    m = storeModel(storeTable(), o);
    CHECK(!m.plugin.shown && m.draft.page_id.empty() && !m.draft.notice_tick && !m.draft.details_open && m.draft_changed && !m.categories.empty());
    // ...but an EMPTY catalogue is "not read", not "gone"
    StubTable none;
    m = storeModel(none, o);
    CHECK(m.draft.page_id == "gone" && !m.draft_changed);
}

void storeNotices() {
    StubTable t = storeTable();
    ReadOptions o;
    o.store.page_id = "pager";
    StoreModel m = storeModel(t, o);
    // THE NOTICE BOX: the maker's words verbatim, a tick under it; GET waits for the tick
    CHECK(m.plugin.notice_shown && m.plugin.notice == "Receiving pager traffic is an offence in some places. You are responsible." && !m.plugin.notice_tick);
    CHECK(m.plugin.notice_tick_label == "I have read the notice above and accept responsibility");
    CHECK(m.plugin.key_kind == kKeyGet && !m.plugin.key_enabled && m.plugin.key_reason == "the legal notice must be acknowledged first");
    o.store.notice_tick = true;
    m = storeModel(t, o);
    CHECK(m.plugin.notice_tick && m.plugin.key_enabled && m.plugin.key_reason.empty());
    // a site's plugin ALWAYS carries a notice, ahead of the maker's own
    o.store.page_id = "maps@example.com";
    o.store.notice_tick = false;
    t.facts["fact.store.maps@example.com.legal"] = "Use at your own risk.";
    m = storeModel(t, o);
    CHECK(m.plugin.notice == "This plugin comes from example.com, not from the FoxSDR catalogue. FoxSDR has not reviewed it.\n\nUse at your own risk.");
    CHECK(has(m.plugin.version, "from example.com") && has(m.plugin.origin, "From a site you added: ") && has(m.plugin.origin, "example.com") && badgeTexts(m.plugin.badges) == Names({"THIRD PARTY", "EXPERIMENTAL"}));
    CHECK(!m.plugin.key_enabled);
    // the regional list's sentence
    t.facts["fact.store.adsb.regional"] = "1";
    o.store.page_id = "adsb";
    CHECK(storeModel(t, o).plugin.origin == "Offered by foxsdr.com in some countries only.");
    // an installed plugin shows no box
    StubTable inst = storeTable();
    inst.facts["fact.store.pager.installState"] = "installed";
    o.store.page_id = "pager";
    CHECK(!storeModel(inst, o).plugin.notice_shown);
}

void railChip() {
    StubTable t;
    CHECK(readStoreChip(t).word == "IDLE" && !readStoreChip(t).lit);
    t.facts["fact.store.readThisSession"] = "1";
    CHECK(readStoreChip(t).word == "OK" && readStoreChip(t).tone == Tone::Good && !readStoreChip(t).lit);
    t.add(FOXAPI_LIST_STORE_UPDATES, 1, "wefax", "1.0.0 -> 1.1.0");
    t.add(FOXAPI_LIST_STORE_UPDATES, 2, "ais", "1.0.0 -> 1.1.0");
    StoreRailChip c = readStoreChip(t);
    CHECK(c.word == "2 UPD" && c.tone == Tone::Warn && c.lit);
    // a kept copy of an earlier day is no read: "looked" means this session
    t.facts["fact.store.readThisSession"] = "0";
    CHECK(readStoreChip(t).word == "IDLE");
    t.facts["fact.store.readThisSession"] = "1";
    // a transfer outranks the count
    t.add(FOXAPI_LIST_STORE_ACTIVITY, 1, "", "", kActive, 0.0, 1);
    c = readStoreChip(t);
    CHECK(c.word == "BUSY" && c.lit && c.tone == Tone::Good);
    // an ADD ALL run alone (entry 2) is not "a transfer in flight" to the rail
    StubTable run;
    run.facts["fact.store.readThisSession"] = "1";
    run.add(FOXAPI_LIST_STORE_ACTIVITY, 2, "x", "", kActive, 1, 3);
    CHECK(readStoreChip(run).word == "OK");
}

void storeEffects() {
    StubTable t = storeTable();
    ReadOptions o;
    StoreModel m = storeModel(t, o);
    // ---- local changes ----
    Effect e = applyStoreSearch(m, "adsb");
    CHECK(e.commands.empty() && e.local == Local::Store && e.store.search == "adsb" && e.store.page_id.empty());
    CHECK(applyStoreSearch(m, std::string(300, 's')).store.search.size() == 127);
    ReadOptions onPage;
    onPage.store.page_id = "adsb";
    onPage.store.notice_tick = true;
    onPage.store.details_open = true;
    onPage.store.tab = 1;
    const StoreModel pm = storeModel(t, onPage);
    e = applyStoreSearch(pm, "x");   // typing is looking for something: that is on the list, not on a page
    CHECK(e.store.page_id.empty() && !e.store.notice_tick && !e.store.details_open && e.store.search == "x" && e.store.tab == 1);
    e = applyStoreTab(pm, 0);
    CHECK(e.local == Local::Store && e.store.tab == 0 && e.store.page_id.empty() && !e.store.notice_tick);
    CHECK(applyStoreTab(m, 1).store.tab == 1 && applyStoreTab(m, 2).empty() && applyStoreTab(m, -1).empty());
    CHECK(applyStoreSitesToggle(m).store.sites_open && applyStoreSitesToggle(m).commands.empty());
    ReadOptions open;
    open.store.sites_open = true;
    CHECK(!applyStoreSitesToggle(storeModel(t, open)).store.sites_open);
    e = applyStoreSiteDraft(m, "https://x.example/index.json");
    CHECK(e.store.site_draft == "https://x.example/index.json" && e.store.site_error.empty());
    CHECK(applyStoreSiteDraft(m, std::string(600, 'u')).store.site_draft.size() == 511);
    ReadOptions withErr;
    withErr.store.site_error = "That site is already in the list.";
    CHECK(storeModel(t, withErr).site_error == "That site is already in the list." && applyStoreSiteDraft(storeModel(t, withErr), "h").store.site_error.empty());
    CHECK(applyStoreConsent(m, true).store.consent && !applyStoreConsent(m, false).store.consent);
    CHECK(applyStoreBulkTick(m, true).store.bulk_tick && applyStoreBulkTick(m, true).commands.empty());
    CHECK(applyStorePluginDetails(pm, false).store.details_open == false && applyStorePluginDetails(m, true).store.details_open);
    e = applyStorePluginBack(pm);
    CHECK(e.store.page_id.empty() && e.store.tab == 0 && !e.store.notice_tick && !e.store.details_open && e.commands.empty());
    CHECK(applyStorePicture(pm, 0).empty());
    // the notice tick: the open page's only
    CHECK(applyStoreNoticeTick(m, true).empty());
    ReadOptions pgOnly;
    pgOnly.store.page_id = "pager";
    const StoreModel pagerPage = storeModel(t, pgOnly);
    e = applyStoreNoticeTick(pagerPage, true);
    CHECK(e.local == Local::Store && e.store.notice_tick && e.store.page_id == "pager" && e.commands.empty());

    // ---- CHECK AGAIN / CANCEL ----
    e = applyStoreCheck(m);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_STORE_FETCH && cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN);
    StubTable busy = storeTable();
    busy.add(FOXAPI_LIST_STORE_ACTIVITY, 1, "WEFAX", "", kActive, 0.4, 2);
    busy.facts["fact.store.busyKey"] = "wefax";
    const StoreModel bm = storeModel(busy, o);
    CHECK(applyStoreCheck(bm).empty());
    e = applyStoreCancel(bm);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_STORE_CANCEL && cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN);
    CHECK(applyStoreCancel(m).empty());
    // a catalogue url nobody set: CHECK is dead
    StubTable nourl = storeTable();
    nourl.facts["fact.store.url"] = "";
    CHECK(applyStoreCheck(storeModel(nourl, o)).empty());
    // ...and a picture in flight holds the slot
    StubTable pic = storeTable();
    pic.add(FOXAPI_LIST_STORE_PICTURES, 1, kShaA.c_str(), "", 0, 1);
    CHECK(applyStoreCheck(storeModel(pic, o)).empty());

    // ---- the cards ----
    // GET: enabled with no notice = INSTALL, the notice bit unset (the card is not the page that holds a tick)
    e = applyStoreCardKey(m, "adsb");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_STORE_INSTALL && commandText(cmdOf(e, 0)) == "adsb" && fcOf(e, 0).ival[0] == 0 && cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN && e.local == Local::None);
    // GET on a plugin with a notice OPENS THE PAGE (and asks for its pictures, a failed one again)
    e = applyStoreCardKey(m, "pager");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_STORE_PICTURES_WANT && commandText(cmdOf(e, 0)) == "pager" && fcOf(e, 0).ival[0] == 1 && e.pictures_asked);
    CHECK(e.local == Local::Store && e.store.page_id == "pager" && e.store.tab == 0 && !e.store.notice_tick && !e.store.details_open);
    // a dead GET does nothing; INSTALLED does nothing; an unknown card does nothing
    CHECK(applyStoreCardKey(m, "linuxonly").empty() && applyStoreCardKey(m, "ais").empty() && applyStoreCardKey(m, "nothing").empty() && applyStoreCardKey(m, "").empty());
    // UPDATE
    e = applyStoreCardKey(m, "wefax");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_STORE_UPDATE && commandText(cmdOf(e, 0)) == "wefax" && cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN);
    CHECK(fcOf(applyStoreCardKey(bm, "wefax"), 0).op == FOXAPI_OP_STORE_CANCEL && applyStoreCardKey(bm, "wefax").commands.size() == 1);   // wefax is the one FITTING...
    busy.facts["fact.store.busyKey"] = "adsb";
    const StoreModel bm2 = storeModel(busy, o);
    CHECK(applyStoreCardKey(bm2, "wefax").empty());   // its UPDATE waits behind ADS-B's download
    CHECK(fcOf(applyStoreCardKey(bm2, "adsb"), 0).op == FOXAPI_OP_STORE_CANCEL);

    // open a page by name or glyph: the edge asks for the pictures; opening the page already open does nothing; a key the catalogue does not list does nothing
    e = applyStoreCardOpen(m, "adsb");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_STORE_PICTURES_WANT && fcOf(e, 0).ival[0] == 1 && e.store.page_id == "adsb" && e.pictures_asked);
    CHECK(applyStoreCardOpen(m, "nothing").empty() && applyStoreCardOpen(m, "").empty());
    CHECK(applyStoreCardOpen(pm, "adsb").empty());
    // opening from the UPDATES tab goes to BROWSE's page; the notice tick and the details start again
    ReadOptions up;
    up.store.tab = 1;
    up.store.notice_tick = true;
    up.store.details_open = true;
    e = applyStoreCardOpen(storeModel(t, up), "wefax");
    CHECK(e.store.tab == 0 && e.store.page_id == "wefax" && !e.store.notice_tick && !e.store.details_open);

    // ---- the page's key: the notice tick unblocks GET, and rides the install as the ack bit ----
    ReadOptions pager;
    pager.store.page_id = "pager";
    StoreModel pg = storeModel(t, pager);
    CHECK(applyStorePluginKey(pg).empty());   // dead until the notice is ticked
    pager.store.notice_tick = true;
    pg = storeModel(t, pager);
    e = applyStorePluginKey(pg);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_STORE_INSTALL && commandText(cmdOf(e, 0)) == "pager" && fcOf(e, 0).ival[0] == 1);
    ReadOptions plain;
    plain.store.page_id = "adsb";
    e = applyStorePluginKey(storeModel(t, plain));
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_STORE_INSTALL && fcOf(e, 0).ival[0] == 0);
    // a tick given to one plugin is not carried to another card (the bit is the open page's alone)
    pager.store.page_id = "";
    CHECK(fcOf(applyStoreCardKey(storeModel(t, pager), "adsb"), 0).ival[0] == 0);
    ReadOptions wf;
    wf.store.page_id = "wefax";
    e = applyStorePluginKey(storeModel(t, wf));
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_STORE_UPDATE && commandText(cmdOf(e, 0)) == "wefax");
    wf.store.page_id = "ais";
    CHECK(applyStorePluginKey(storeModel(t, wf)).empty());   // INSTALLED
    CHECK(applyStorePluginKey(m).empty());                   // no page
    CHECK(fcOf(applyStorePluginKey(storeModel(busy, [&] { ReadOptions q; q.store.page_id = "adsb"; return q; }())), 0).op == FOXAPI_OP_STORE_CANCEL);

    // ---- the UPDATES tab ----
    const StoreModel um = storeModel(t, up);
    e = applyStoreUpdate(um, "wefax");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_STORE_UPDATE && commandText(cmdOf(e, 0)) == "wefax");
    CHECK(applyStoreUpdate(um, "adsb").empty() && applyStoreUpdate(um, "").empty());
    e = applyStoreUpdateAll(um);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_STORE_ADD_ALL && commandText(cmdOf(e, 0)).empty() && fcOf(e, 0).ival[0] == 0 && fcOf(e, 0).ival[1] == 1 && cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN);
    CHECK(applyStoreUpdateAll(m).empty());   // the BROWSE tab has no such key
    up.store.tab = 1;
    const StoreModel ubusy = storeModel(busy, up);
    CHECK(applyStoreUpdateAll(ubusy).empty() && applyStoreUpdate(ubusy, "wefax").empty());   // ADS-B is downloading: UPDATE ALL is dead and the row's UPDATE waits
    busy.facts["fact.store.busyKey"] = "wefax";
    CHECK(fcOf(applyStoreUpdate(storeModel(busy, up), "wefax"), 0).op == FOXAPI_OP_STORE_CANCEL);   // a row FITTING... is the one CANCEL
    busy.facts["fact.store.busyKey"] = "adsb";
    // GET EVERYTHING
    e = applyStoreGetAll(m);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_STORE_ADD_ALL && commandText(cmdOf(e, 0)).empty() && fcOf(e, 0).ival[0] == 0 && fcOf(e, 0).ival[1] == 0 && cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN);
    ReadOptions ticked;
    ticked.store.bulk_tick = true;
    e = applyStoreGetAll(storeModel(t, ticked));
    CHECK(e.commands.size() == 1 && fcOf(e, 0).ival[0] == 1 && fcOf(e, 0).ival[1] == 0);
    ReadOptions searching;
    searching.store.search = "a";
    CHECK(applyStoreGetAll(storeModel(t, searching)).empty() && applyStoreGetAll(pm).empty());
    StubTable run = storeTable();
    run.add(FOXAPI_LIST_STORE_ACTIVITY, 2, "AIS Decoder", "", kActive, 1, 5);
    CHECK(applyStoreGetAll(storeModel(run, o)).empty());
    StubTable none;
    CHECK(applyStoreGetAll(storeModel(none, o)).empty());

    // ---- the sites ----
    ReadOptions s;
    s.store.sites_open = true;
    s.store.site_draft = "  https://new.example.org/plugins/index.json  ";
    s.store.consent = true;
    StubTable st = storeTable();
    st.facts["fact.store.sites"] = "https://example.com/index.json\t2\t1\t0\t0\t\n";
    StoreModel sm = storeModel(st, s);
    e = applyStoreSiteAdd(sm);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_STORE_SITE && commandText(cmdOf(e, 0)) == "https://new.example.org/plugins/index.json" && fcOf(e, 0).ival[0] == 0 && fcOf(e, 0).ival[1] == 1 &&
          cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN);
    CHECK(e.local == Local::Store && e.store.site_draft.empty() && !e.store.consent && e.store.site_error.empty() && e.store.sites_open);
    // not an address: the view's own sentence, nothing sent, the draft kept (a text with no "://" is the path of a local index.json: the validator takes it)
    s.store.site_draft = "ftp://files.example.org/index.json";
    e = applyStoreSiteAdd(storeModel(st, s));
    CHECK(e.commands.empty() && e.local == Local::Store && e.store.site_error == "That is not a site address. It must start with https:// (or be the path of an index.json on this computer)." &&
          e.store.site_draft == "ftp://files.example.org/index.json" && e.store.consent);
    s.store.site_draft = "C:/plugins/index.json";
    e = applyStoreSiteAdd(storeModel(st, s));
    CHECK(e.commands.size() == 1 && commandText(cmdOf(e, 0)) == "C:/plugins/index.json");
    s.store.site_draft = "https:///index.json";   // no host
    CHECK(applyStoreSiteAdd(storeModel(st, s)).commands.empty());
    s.store.site_draft = "http://insecure.example.org/index.json";   // https only
    CHECK(applyStoreSiteAdd(storeModel(st, s)).store.site_error.rfind("That is not a site address.", 0) == 0);
    // already listed, the official address, a full list
    s.store.site_draft = "https://example.com/index.json";
    e = applyStoreSiteAdd(storeModel(st, s));
    CHECK(e.commands.empty() && e.store.site_error == "That site is already in the list.");
    s.store.site_draft = cascade::core::defaultPluginIndexUrl();
    CHECK(applyStoreSiteAdd(storeModel(st, s)).store.site_error == "That site is already in the list.");
    s.store.site_draft = "https://foxsdr.com/plugins/index.json";   // the url the catalogue is read from
    CHECK(applyStoreSiteAdd(storeModel(st, s)).store.site_error == "That site is already in the list.");
    std::string many;
    for (int i = 0; i < 8; ++i) { many += "https://site" + std::to_string(i) + ".example.org/index.json\t2\t1\t0\t0\t\n"; }
    st.facts["fact.store.sites"] = many;
    s.store.site_draft = "https://ninth.example.org/index.json";
    e = applyStoreSiteAdd(storeModel(st, s));
    CHECK(e.commands.empty() && e.store.site_error == "The list is full. Remove a site before adding another.");
    st.facts["fact.store.sites"] = "https://example.com/index.json\t2\t1\t0\t0\t\n";
    // the key is dead: no address, no consent, or a transfer
    ReadOptions dead = s;
    dead.store.site_draft = "https://ok.example.org/index.json";
    dead.store.consent = false;
    CHECK(applyStoreSiteAdd(storeModel(st, dead)).empty());
    dead.store.consent = true;
    dead.store.site_draft.clear();
    CHECK(applyStoreSiteAdd(storeModel(st, dead)).empty());
    dead.store.site_draft = "https://ok.example.org/index.json";
    StubTable stb = st;
    stb.add(FOXAPI_LIST_STORE_ACTIVITY, 1, "", "", kActive, 0.0, 1);
    CHECK(applyStoreSiteAdd(storeModel(stb, dead)).empty());
    // REMOVE: the address as listed; nothing while a transfer runs or for a row that is not there
    sm = storeModel(st, s);
    e = applyStoreSiteRemove(sm, 0);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_STORE_SITE && commandText(cmdOf(e, 0)) == "https://example.com/index.json" && fcOf(e, 0).ival[0] == 1 && fcOf(e, 0).ival[1] == 0 &&
          cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN);
    CHECK(applyStoreSiteRemove(sm, 1).empty() && applyStoreSiteRemove(sm, -1).empty() && applyStoreSiteRemove(storeModel(stb, s), 0).empty());

    // ---- the clean-up: one confirmation for all of them ----
    StubTable cl = storeTable();
    cl.facts["fact.store.oldCopies"] = "ais-1.0.0.dll\tAIS Decoder\t1.0.0\tais-1.1.0.dll\t1.1.0\n";
    ReadOptions c1;
    c1.store.tab = 1;
    StoreModel cm = storeModel(cl, c1);
    e = applyStoreCleanup(cm);
    CHECK(e.commands.empty() && e.local == Local::Store && e.store.cleanup_open);
    c1.store.cleanup_open = true;
    cm = storeModel(cl, c1);
    e = applyStoreCleanupConfirm(cm, true);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_STORE_CLEAN_OLD && fcOf(e, 0).ival[0] == 0 && commandText(cmdOf(e, 0)).empty() && cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN);
    CHECK(e.local == Local::Store && !e.store.cleanup_open);
    e = applyStoreCleanupConfirm(cm, false);
    CHECK(e.commands.empty() && e.local == Local::Store && !e.store.cleanup_open);
    // a confirmation that was never opened (a stray callback) sends nothing
    c1.store.cleanup_open = false;
    CHECK(applyStoreCleanupConfirm(storeModel(cl, c1), true).commands.empty());
    // no old copy, or a transfer: the key does nothing
    CHECK(applyStoreCleanup(storeModel(storeTable(), c1)).empty());
    cl.add(FOXAPI_LIST_STORE_ACTIVITY, 1, "", "", kActive, 0.0, 1);
    CHECK(applyStoreCleanup(storeModel(cl, c1)).empty());
    c1.store.cleanup_open = true;
    CHECK(applyStoreCleanupConfirm(storeModel(cl, c1), true).commands.empty());

    // ---- the first open reads the catalogue once; the pictures' top-up ----
    StubTable fresh;
    fresh.facts["fact.store.url"] = "https://foxsdr.com/plugins/index.json";
    const StoreModel fm = storeModel(fresh, o);
    StoreFirstOpenState first;
    e = applyStoreOpened(fm, first, false);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_STORE_FETCH && first.settled);
    CHECK(applyStoreOpened(fm, first, false).empty());   // once per session
    StoreFirstOpenState second;
    CHECK(applyStoreOpened(m, second, true).empty() && second.settled);   // a catalogue already read this session settles it without asking
    StoreFirstOpenState third;
    CHECK(applyStoreOpened(bm, third, false).empty() && third.settled);   // a transfer in flight, too
    // the top-up: the open page lists a picture nobody asked for (state None), once
    StubTable tp = storeTable();
    tp.add(FOXAPI_LIST_STORE_PICTURES, 1, kShaA.c_str(), "C:/x.png", kActive, 2);
    ReadOptions page;
    page.store.page_id = "adsb";
    e = applyStorePicturesTop(storeModel(tp, page));
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_STORE_PICTURES_WANT && commandText(cmdOf(e, 0)) == "adsb" && fcOf(e, 0).ival[0] == 0 && e.pictures_asked);   // B and C are not in STORE_PICTURES
    tp.add(FOXAPI_LIST_STORE_PICTURES, 2, kShaB.c_str(), "no", 0, 3);
    tp.add(FOXAPI_LIST_STORE_PICTURES, 3, kShaC.c_str(), "", 0, 1);
    CHECK(applyStorePicturesTop(storeModel(tp, page)).empty());   // every one is known now
    CHECK(applyStorePicturesTop(m).empty());                       // no page
}

void consentEdges() {
    ConsentWatch w;
    StubTable t = storeTable();
    ReadOptions o;
    StoreModel m = storeModel(t, o);
    // the first look records the catalogue and clears nothing
    ConsentEdge e = watchConsents(w, m);
    CHECK(!e.any() && w.have_hash);
    CHECK(!watchConsents(w, m).any());
    // a catalogue fetch beginning clears both ticks
    t.add(FOXAPI_LIST_STORE_ACTIVITY, 1, "", "", kActive, 0.0, 1);
    e = watchConsents(w, storeModel(t, o));
    CHECK(e.clear_notice_tick && e.clear_bulk_tick);
    // a download is not a fetch of the catalogue
    t.lists[FOXAPI_LIST_STORE_ACTIVITY].clear();
    CHECK(!watchConsents(w, storeModel(t, o)).any());
    t.add(FOXAPI_LIST_STORE_ACTIVITY, 1, "WEFAX", "", kActive, 0.5, 2);
    CHECK(!watchConsents(w, storeModel(t, o)).any());
    t.lists[FOXAPI_LIST_STORE_ACTIVITY].clear();
    // the ADD ALL run ending clears the bulk tick only
    t.add(FOXAPI_LIST_STORE_ACTIVITY, 2, "AIS", "", kActive, 1, 3);
    CHECK(!watchConsents(w, storeModel(t, o)).any() && w.add_all_was_running);
    t.lists[FOXAPI_LIST_STORE_ACTIVITY].clear();
    e = watchConsents(w, storeModel(t, o));
    CHECK(!e.clear_notice_tick && e.clear_bulk_tick && !w.add_all_was_running);
    // a new summary line clears it too, however briefly the run was on the table
    t.facts["fact.status.addAllSummary"] = "3 fitted";
    e = watchConsents(w, storeModel(t, o));
    CHECK(!e.clear_notice_tick && e.clear_bulk_tick);
    CHECK(!watchConsents(w, storeModel(t, o)).any());
    // a different catalogue clears both ticks (they were consent for the old one's notices)
    t.facts["fact.store.pager.legal"] = "A different notice.";
    e = watchConsents(w, storeModel(t, o));
    CHECK(e.clear_notice_tick && e.clear_bulk_tick);
    CHECK(!watchConsents(w, storeModel(t, o)).any());
    // a catalogue that changed only in what the hash leaves out (the install state) is the same catalogue
    t.facts["fact.store.adsb.installState"] = "installed";
    CHECK(!watchConsents(w, storeModel(t, o)).any());
    // a store read that was not made (the page is closed) says nothing
    ReadOptions off;
    off.read_store = false;
    CHECK(!watchConsents(w, storeModel(t, off)).any());
}

void notReadWhileClosed() {
    StubTable t = storeTable();
    ReadOptions o;
    o.read_store = false;
    o.store.search = "x";
    o.store.tab = 1;
    const StoreModel m = readStore(t, o);
    CHECK(m.categories.empty() && m.updates.empty() && !m.plugin.shown && m.draft.search == "x" && m.search == "x" && m.tab == 1 && !m.hash_valid);
    CHECK(m.search_placeholder == "Search plugins");
    // the Web and CAT bodies are read whatever
    FoxReceiverState s{};
    s.structSize = sizeof(s);
    StubTable w = webRunning();
    const ExtendModels all = readExtend(w, s, o);
    CHECK(all.web.running && all.chips[kRowWeb].word == "ON" && all.store.categories.empty());
}

// The model that is shown is the model that was read: the same table read twice gives the same page (no hidden state in the read).
void readsAreStable() {
    StubTable t = storeTable();
    ReadOptions o;
    const StoreModel a = storeModel(t, o), b = storeModel(t, o);
    CHECK(cardIds(a) == cardIds(b) && a.catalogue_line == b.catalogue_line && a.foot_note == b.foot_note && a.catalogue_hash == b.catalogue_hash && a.catalogue_hash != 0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not eat the lines before it
    sourcesStateWhatWeSend();
    everySentenceIsADrawers();
    sentencesAreTranslated();
    webReads();
    webStagedCopy();
    catReads();
    webEffects();
    catEffects();
    storeBrowseRead();
    storeStates();
    storeUpdatesRead();
    storeSitesAndCleanup();
    storePluginPage();
    storeNotices();
    railChip();
    storeEffects();
    consentEdges();
    notReadWhileClosed();
    readsAreStable();
    return testSummary("test_slint_host_extend");
}
