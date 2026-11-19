// Tests for core/radio_message.{hpp,cpp} and the `radioMessages` list of the update manifest (0.99.110; docs/multi-radio-records/radio-message.md).
//
// The owner's rule for this feature: no new tracking. The application never sends its radios; the answer carries a keyed hash and the application hashes its OWN
// radios against it. So the tests that matter are: the hash is the one the site computes (tests/fixtures/radio-message-contract.json, the same file the Go tests read),
// a radio that is not the addressed one learns nothing, the text is sanitised and clamped on receipt, a hostile entry costs only itself, an old reader's fields
// are untouched by the new key, the popup is shown once per id and in the right order.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/radio_message.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "core/updater.hpp"
#include "test_check.hpp"

using cascade::core::KnownRadio;
using cascade::core::RadioMessage;
using cascade::core::UpdateInfo;
using nlohmann::json;
namespace rm = cascade::core::radiomessage;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const char* kSha = "393e5fd91b7b2292611c52af7e9f2db1e2c730d78f1639adefceaf0fb5cca0ea";

// A well-formed manifest at 0.99.110 for a copy running `running`, with `extra` spliced in as more members.
std::string manifest(const std::string& extra) {
    return std::string("{\"ok\":true,\"version\":\"0.99.110\",\"url\":\"https://foxsdr.com/download/foxsdr-setup-0.99.110.exe\",\"sha256\":\"") + kSha +
           "\",\"size\":3274221,\"newer\":true,\"critical\":false,\"notes\":[]" + (extra.empty() ? "" : "," + extra) + "}";
}

json entryFor(const std::string& id, const std::string& salt, const std::string& radioKey, const std::string& text) {
    return json{{"id", id}, {"date", "2026-11-19"}, {"text", text}, {"salt", salt}, {"hash", cascade::core::radioMessageHash(salt, radioKey)}};
}

UpdateInfo parse(const std::string& body) {
    UpdateInfo info;
    std::string err;
    const bool ok = cascade::core::parseUpdateManifest(body, "0.99.109", info, err);
    CHECK(ok);
    return info;
}

KnownRadio radio(const std::string& model, const std::string& serial) {
    return KnownRadio{cascade::core::radioreport::radioKey(model, serial), model, serial};
}

}  // namespace

int main() {
    const std::string root = CASCADE_SOURCE_ROOT;

    // ---- THE CONTRACT FIXTURE: the same JSON the Go side hashes --------------------------------------------------------------------------------------
    {
        const json fx = json::parse(slurp(root + "/tests/fixtures/radio-message-contract.json"), nullptr, false);
        CHECK(fx.is_object());
        int n = 0;
        for (const json& v : fx["vectors"]) {
            CHECK(cascade::core::radioMessageHash(v["salt"].get<std::string>(), v["radioKey"].get<std::string>()) == v["hash"].get<std::string>());
            ++n;
        }
        CHECK(n >= 4);
        // the message of the fixture, through the real manifest reader, is for exactly the radio the fixture says and for none of the near misses
        const std::string body = manifest("\"radioMessages\":[" + fx["message"].dump() + "]");
        const UpdateInfo info = parse(body);
        CHECK(info.radioMessages.size() == 1);
        if (info.radioMessages.size() == 1) {
            CHECK(cascade::core::radioMessageIsFor(info.radioMessages[0], fx["message_is_for"].get<std::string>()));
            for (const json& k : fx["message_is_not_for"]) { CHECK(!cascade::core::radioMessageIsFor(info.radioMessages[0], k.get<std::string>())); }
            // markup and a link arrive as the literal characters, un-interpreted
            CHECK(info.radioMessages[0].text.find("<b>x</b>") != std::string::npos);
            CHECK(info.radioMessages[0].text.find("https://example.test/") != std::string::npos);
        }
    }

    // ---- HASH: match and non-match -------------------------------------------------------------------------------------------------------------------
    {
        const KnownRadio r = radio("NESDR Smart v5", "00a1b2c3");
        CHECK(r.key == "nesdr-smart-v5:00A1B2C3");   // the key the reports use; the serial is upper-cased by radioKey
        RadioMessage m;
        m.id = "rmaaaa1111";
        m.salt = "0123456789abcdef";
        m.hash = cascade::core::radioMessageHash(m.salt, r.key);
        CHECK(cascade::core::radioMessageIsFor(m, r.key));
        CHECK(!cascade::core::radioMessageIsFor(m, radio("NESDR Smart v5", "00A1B2C4").key));   // another unit of the same model
        CHECK(!cascade::core::radioMessageIsFor(m, radio("RTL-SDR Blog V4", "00A1B2C3").key));  // the same serial on another model
        CHECK(!cascade::core::radioMessageIsFor(m, ""));
        // the salt is part of the lock: the same radio under another salt is another hash
        CHECK(cascade::core::radioMessageHash("fedcba9876543210", r.key) != m.hash);
        RadioMessage broken = m;
        broken.hash = "abc";
        CHECK(!cascade::core::radioMessageIsFor(broken, r.key));
        broken = m;
        broken.salt.clear();
        CHECK(!cascade::core::radioMessageIsFor(broken, r.key));
    }

    // ---- SANITISING ----------------------------------------------------------------------------------------------------------------------------------
    {
        using cascade::core::sanitiseRadioMessageText;
        CHECK(sanitiseRadioMessageText("plain text") == "plain text");
        // control characters dropped, a tab becomes a space, a carriage return goes, a new line stays
        CHECK(sanitiseRadioMessageText(std::string("a\x01" "b\x07" "c\x7f" "d\te\r\nf")) == "abcd e\nf");
        // the C1 controls, the zero-width and direction controls, the line separator, the override, the byte-order mark
        CHECK(sanitiseRadioMessageText("a\xC2\x85" "b\xE2\x80\xAE" "c\xE2\x80\x8B" "d\xE2\x80\xA8" "e\xEF\xBB\xBF" "f") == "abcdef");
        // invalid UTF-8 is dropped, not passed on: a lone continuation byte, an overlong NUL, a surrogate
        CHECK(sanitiseRadioMessageText("a\x80" "b\xC0\x80" "c\xED\xA0\x80" "d") == "abcd");
        // markup is NOT interpreted or removed: it is shown literally
        CHECK(sanitiseRadioMessageText("<script>alert(1)</script> <a href=\"http://x\">link</a> &amp; **bold**") == "<script>alert(1)</script> <a href=\"http://x\">link</a> &amp; **bold**");
        // a run of blank lines is one, and the ends are trimmed
        CHECK(sanitiseRadioMessageText("\n\n  one\n\n\n\n\ntwo \n\n") == "one\n\ntwo");
        // over-long: cut to 600 characters on a character boundary (a two-byte letter is one character)
        const std::string longText(1000, 'x');
        CHECK(sanitiseRadioMessageText(longText).size() == 600);
        std::string two;
        for (int i = 0; i < 700; ++i) { two += "\xC3\xA9"; }   // e-acute
        const std::string cut = sanitiseRadioMessageText(two);
        CHECK(cut.size() == 1200);
        CHECK(cascade::core::radioreport::utf8Length(cut) == 600);
        // nothing printable left is empty
        CHECK(sanitiseRadioMessageText(std::string("\x01\x02 \n\t")).empty());
    }

    // ---- THE READER: bounded, entry by entry, sanitised on receipt ------------------------------------------------------------------------------------
    {
        const std::string salt = "0123456789abcdef";
        json list = json::array();
        list.push_back(entryFor("rmgood0001", salt, "b200:EDR04ZDB2", "first"));
        list.push_back("not an object");
        list.push_back(entryFor("bad id!", salt, "b200:EDR04ZDB2", "dropped: id"));
        list.push_back(entryFor("rmshortsalt", "abcd", "b200:EDR04ZDB2", "dropped: salt too short"));
        json upper = entryFor("rmupper0001", "0123456789ABCDEF", "b200:EDR04ZDB2", "dropped: salt not lower-case hex");
        list.push_back(upper);
        json shortHash = entryFor("rmshorthash", salt, "b200:EDR04ZDB2", "dropped: hash");
        shortHash["hash"] = "1234";
        list.push_back(shortHash);
        list.push_back(entryFor("rmempty0001", salt, "b200:EDR04ZDB2", std::string("\x01\x02 \n")));
        json wrongType = entryFor("rmwrongtype", salt, "b200:EDR04ZDB2", "x");
        wrongType["text"] = 5;
        list.push_back(wrongType);
        list.push_back(entryFor("rmgood0002", salt, "b200:EDR04ZDB2", std::string(5000, 'y')));
        const UpdateInfo info = parse(manifest("\"radioMessages\":" + list.dump()));
        CHECK(info.radioMessages.size() == 2);
        if (info.radioMessages.size() == 2) {
            CHECK(info.radioMessages[0].id == "rmgood0001" && info.radioMessages[0].text == "first");
            CHECK(info.radioMessages[1].id == "rmgood0002" && info.radioMessages[1].text.size() == 600);   // clamped on receipt
        }
        // only the first kMaxRadioMessages usable entries are kept
        json many = json::array();
        for (int i = 0; i < 40; ++i) { many.push_back(entryFor("rm" + std::to_string(1000 + i), salt, "b200:EDR04ZDB2", "m")); }
        CHECK(parse(manifest("\"radioMessages\":" + many.dump())).radioMessages.size() == cascade::core::kMaxRadioMessages);
        // not an array, or no key at all: an empty list, and the manifest still parses
        CHECK(parse(manifest("\"radioMessages\":\"nope\"")).radioMessages.empty());
        CHECK(parse(manifest("\"radioMessages\":{\"id\":\"x\"}")).radioMessages.empty());
        CHECK(parse(manifest("")).radioMessages.empty());
    }

    // ---- AN OLD READER'S FIELDS ARE UNTOUCHED BY THE NEW KEY ------------------------------------------------------------------------------------------
    // (the 0.99.109 reader itself was run on such a manifest: docs/multi-radio-records/radio-message.md; here the same key beside the team messages and notes)
    {
        const std::string salt = "0123456789abcdef";
        const std::string plain = manifest("\"messages\":[{\"id\":\"team0001\",\"date\":\"2026-11-19\",\"lines\":[\"for everyone\"],\"important\":false}]");
        const std::string with = manifest("\"messages\":[{\"id\":\"team0001\",\"date\":\"2026-11-19\",\"lines\":[\"for everyone\"],\"important\":false}],\"radioMessages\":[" +
                                          entryFor("rmgood0001", salt, "b200:EDR04ZDB2", "for one radio").dump() + "]");
        const UpdateInfo a = parse(plain);
        const UpdateInfo b = parse(with);
        CHECK(a.messages.size() == 1 && b.messages.size() == 1);
        CHECK(b.messages[0].id == "team0001" && b.messages[0].lines == a.messages[0].lines);
        CHECK(a.version == b.version && a.newer == b.newer && a.url == b.url && a.sha256 == b.sha256 && a.notes.size() == b.notes.size());
        // the message for one radio is NOT among the messages for everyone
        for (const auto& m : b.messages) { CHECK(m.id != "rmgood0001"); }
    }

    // ---- THE KNOWN-RADIO REGISTRY, fed by the reporter's funnel ---------------------------------------------------------------------------------------
    {
        rm::resetForTest();
        CHECK(rm::knownRadios().empty() && rm::generation() == 0);
        rm::noteRadio("nesdr-smart-v5:00A1B2C3", "NESDR Smart v5", "00a1b2c3");
        rm::noteRadio("nesdr-smart-v5:00A1B2C3", "NESDR Smart v5", "00a1b2c3");   // the same radio again: nothing new
        rm::noteRadio("sound-card:-", "sound card", "");                          // no serial: cannot be addressed, not recorded
        rm::noteRadio("sound-card:-", "sound card", "-");
        CHECK(rm::knownRadios().size() == 1 && rm::generation() == 1);
        CHECK(rm::knownRadios()[0].serial == "00A1B2C3");
        // the real funnel: radio_found and radio_open note the radio, radio_settings does not
        cascade::core::radioreport::Event ev;
        ev.kind = "radio_found";
        ev.radio = cascade::core::radioreport::identify("rtlsdr", "serial=00000777", "RTL-SDR (serial 00000777)");
        CHECK(!ev.radio.key.empty());
        cascade::core::radioreport::global().event(ev);
        CHECK(rm::knownRadios().size() == 2 && rm::generation() == 2);
        cascade::core::radioreport::Event other;
        other.kind = "radio_settings";
        other.radio = cascade::core::radioreport::identify("rtlsdr", "serial=00000888", "RTL-SDR (serial 00000888)");
        cascade::core::radioreport::global().event(other);
        CHECK(rm::knownRadios().size() == 2);
        rm::resetForTest();
    }

    // ---- THE POPUP: shown once per id, oldest first, only for a radio found, naming the model and the last four characters -------------------------------
    {
        const std::string salt = "0123456789abcdef";
        const KnownRadio mine = radio("NESDR Smart v5", "00A1B2C3");
        const KnownRadio other = radio("Airspy HF+ Discovery", "3B52A5E9");
        auto mk = [&](const std::string& id, const KnownRadio& r, const std::string& text, const std::string& date) {
            RadioMessage m;
            m.id = id; m.date = date; m.text = text; m.salt = salt;
            m.hash = cascade::core::radioMessageHash(salt, r.key);
            return m;
        };
        // the answer lists newest first
        const std::vector<RadioMessage> msgs = {mk("rmnew00001", mine, "newer", "2026-11-19"), mk("rmforother", other, "for the Airspy", "2026-11-18"), mk("rmold00001", mine, "older", "2026-11-17")};

        // no radio found yet: nothing shown, and nothing learned
        CHECK(!cascade::core::radioMessagePopupFor(true, false, msgs, {}, {}).shown);
        // the radio is found: the OLDER message first, naming model and the last four characters of the serial only
        cascade::core::RadioMessagePopup p = cascade::core::radioMessagePopupFor(true, false, msgs, {mine}, {});
        CHECK(p.shown && p.id == "rmold00001" && p.text == "older" && p.more == 1);
        CHECK(p.model == "NESDR Smart v5" && p.serialTail == "B2C3");
        CHECK(p.serialTail.find("00A1") == std::string::npos);
        // Seen is once per id: the next one comes, then none
        p = cascade::core::radioMessagePopupFor(true, false, msgs, {mine}, {"rmold00001"});
        CHECK(p.shown && p.id == "rmnew00001" && p.more == 0);
        p = cascade::core::radioMessagePopupFor(true, false, msgs, {mine}, {"rmold00001", "rmnew00001"});
        CHECK(!p.shown);
        // a radio plugged in LATER: the same answer now matches
        CHECK(!cascade::core::radioMessagePopupFor(true, false, msgs, {mine}, {"rmold00001", "rmnew00001"}).shown);
        p = cascade::core::radioMessagePopupFor(true, false, msgs, {mine, other}, {"rmold00001", "rmnew00001"});
        CHECK(p.shown && p.id == "rmforother" && p.model == "Airspy HF+ Discovery" && p.serialTail == "A5E9");
        // a short serial is shown whole; never more than four characters of a long one
        const KnownRadio shortSerial = radio("Pluto", "AB");
        CHECK(cascade::core::radioMessagePopupFor(true, false, {mk("rmshort0001", shortSerial, "x", "d")}, {shortSerial}, {}).serialTail == "AB");
        // the Updates tick off (no check made) or a Store copy: nothing
        CHECK(!cascade::core::radioMessagePopupFor(false, false, msgs, {mine}, {}).shown);
        CHECK(!cascade::core::radioMessagePopupFor(true, true, msgs, {mine}, {}).shown);
    }

    return testSummary("test_radio_message");
}
