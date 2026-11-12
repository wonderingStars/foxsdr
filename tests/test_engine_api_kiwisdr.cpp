// test_engine_api_kiwisdr.cpp - A KIWISDR RECEIVER AS A RADIO OF THE ENGINE (engine wiring lane 1, group A; docs/multi-radio-records/engine-wiring-1.md).
//
// Held against the real engine through the C table and its own passes, with the scripted stand-in receiver of tests/kiwisdr_fake_server.hpp on 127.0.0.1 (no real KiwiSDR is
// contacted; nothing here can reach outside this machine).
//
//   A  ADDRESS RULES: KiwiSdrSource::parseArgs and serverModeFor, pure;
//   B  A KIWISDR AS RADIO 2: it opens through the worker, the receiver's audio is in radio 2's spectrum (the stand-in sends a 1 kHz tone), the commands the receiver was sent are the
//      opening ones then the tune the engine made, a retune and a mode change reach the receiver, a VFO offset is refused (0 kept), the facts say what it is doing, the same receiver
//      twice is refused naming the holder, a radio removed lets go of it;
//   C  REFUSALS ARE PUBLISHED: a busy receiver, a receiver that wants a password, one that never answers, one that is not there, a bad address and a remote session's attempt;
//   D  THE KIND IS SAVED AND COMES BACK: the settings carry sourceKind "kiwisdr" and the address and NO password; a restart puts the radio back on the receiver; a receiver that is not
//      there leaves the generator, named in fact.radio.restore, the next save still naming the receiver;
//   E  RADIO 1 TOO: SELECT_SOURCE "open-kiwisdr:..." with no target opens it on the first radio, whose saved kind is the plain sourceKind key.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "kiwisdr_fake_server.hpp"
#include "source/kiwisdr_source.hpp"
#include "wire1_rig.hpp"

using cascade::source::KiwiSdrSource;
using kiwifake::Command;
using kiwifake::Conn;
using kiwifake::FakeKiwi;

namespace {

constexpr double kTone = 1000.0;

// What the stand-in saw: every text frame the client sent, in order.
struct Seen {
    std::mutex m;
    std::vector<std::string> texts;
    std::atomic<int> conns{0};
    void add(const Command& c) {
        std::lock_guard<std::mutex> lk(m);
        texts.push_back(c.text);
    }
    std::vector<std::string> all() {
        std::lock_guard<std::mutex> lk(m);
        return texts;
    }
    bool has(const std::string& part) {
        for (const std::string& t : all()) {
            if (t.find(part) != std::string::npos) { return true; }
        }
        return false;
    }
    // The latest text containing `part`.
    std::string last(const std::string& part) {
        std::string out;
        for (const std::string& t : all()) {
            if (t.find(part) != std::string::npos) { out = t; }
        }
        return out;
    }
};

std::string pcmTone(std::uint32_t& phase, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        const double v = 8000.0 * std::sin(2.0 * 3.14159265358979323846 * kTone * static_cast<double>(phase++) / 12000.0);
        const int q = static_cast<int>(std::lround(v));
        s.push_back(static_cast<char>((q >> 8) & 0xFF));
        s.push_back(static_cast<char>(q & 0xFF));
    }
    return s;
}

// A receiver that plays: 240 samples of the tone every 20 ms (12 kS/s), flags 0 (16-bit PCM, big-endian: the client's assumption), until the client goes.
FakeKiwi::Script playing(Seen& seen) {
    return [&seen](Conn& c, int) {
        c.onCommand = [&seen](const Command& x) { seen.add(x); };
        if (!c.upgrade()) { return; }
        ++seen.conns;
        std::uint32_t seq = 0, phase = 0;
        while (c.sleep(20)) { c.sendSnd(0x00, seq++, 0x0190 + static_cast<std::uint16_t>(seq % 7), pcmTone(phase, 240)); }
    };
}

// A receiver that answers with one MSG word and then says nothing.
FakeKiwi::Script saying(Seen& seen, const std::string& msg) {
    return [&seen, msg](Conn& c, int) {
        c.onCommand = [&seen](const Command& x) { seen.add(x); };
        if (!c.upgrade()) { return; }
        ++seen.conns;
        c.sendMsg(msg);
        while (c.sleep(20)) {}
    };
}

// A receiver that accepts the connection and never sends a word.
FakeKiwi::Script silent(Seen& seen) {
    return [&seen](Conn& c, int) {
        c.onCommand = [&seen](const Command& x) { seen.add(x); };
        if (!c.upgrade()) { return; }
        ++seen.conns;
        while (c.sleep(20)) {}
    };
}

std::string addr(const FakeKiwi& s) { return "127.0.0.1:" + std::to_string(s.port()); }

// A port that nothing listens on: bound, read, closed.
std::string closedAddress() {
    FakeKiwi probe([](Conn&, int) {});
    const std::string a = addr(probe);
    return a;   // (the stand-in is destroyed on return: the port is closed again)
}

bool hasMode(Seen& s, const std::string& mod) { return s.last("SET mod=" + mod + " ").size() > 0; }

// The spectrum has its peak a tone's distance from zero on either side (the audio is a real signal: mirrored about the centre).
bool toneInSpectrum(const Rig::Spectrum& sp) {
    if (sp.status != FOXAPI_OK || sp.bins.empty()) { return false; }
    return (peakAt(sp, kTone, 4) || peakAt(sp, -kTone, 4));
}

// ---- A -----------------------------------------------------------------------------------------------------------------------------------------

void testAAddressRules() {
    std::printf("A: the address and mode rules\n");
    KiwiSdrSource::Address a;
    std::string why;
    CHECK(KiwiSdrSource::parseArgs("kiwisdr=rx.example.org:8074", a, why));
    CHECK_EQ(a.host, std::string("rx.example.org"));
    CHECK_EQ(static_cast<int>(a.port), 8074);
    CHECK(KiwiSdrSource::parseArgs("127.0.0.1", a, why));
    CHECK_EQ(a.host, std::string("127.0.0.1"));
    CHECK_EQ(static_cast<int>(a.port), 8073);
    CHECK(KiwiSdrSource::parseArgs("  kiwisdr=my-kiwi.local  ", a, why));
    CHECK_EQ(a.host, std::string("my-kiwi.local"));
    for (const char* bad : {"", "kiwisdr=", ":8073", "host:", "host:0", "host:65536", "host:80a", "host:123456", "http://host", "host/path", "user@host", "host name", "host?x=1",
                            "host#f", "host%20x", "ho\nst", "host:8073:9"}) {
        std::string w;
        KiwiSdrSource::Address x;
        const bool ok = KiwiSdrSource::parseArgs(bad, x, w);
        if (ok) { std::printf("  accepted but should not be: \"%s\"\n", bad); }
        CHECK(!ok);
        CHECK(!w.empty());
    }
    CHECK(!KiwiSdrSource::parseArgs(std::string(300, 'a'), a, why));
    // the mode words: the engine's demodulator order is NFM WFM AM DSB USB LSB CW RAW
    CHECK_EQ(std::string(KiwiSdrSource::serverModeFor(0).word), std::string("nbfm"));
    CHECK_EQ(std::string(KiwiSdrSource::serverModeFor(2).word), std::string("am"));
    CHECK_EQ(std::string(KiwiSdrSource::serverModeFor(4).word), std::string("usb"));
    CHECK_EQ(std::string(KiwiSdrSource::serverModeFor(5).word), std::string("lsb"));
    CHECK_EQ(std::string(KiwiSdrSource::serverModeFor(6).word), std::string("cw"));
    for (int none : {1, 3, 7, -1, 8}) { CHECK_EQ(std::string(KiwiSdrSource::serverModeFor(none).word), std::string("")); }
    CHECK(KiwiSdrSource::serverModeFor(5).lowCutHz < 0 && KiwiSdrSource::serverModeFor(5).highCutHz < 0);
    CHECK_EQ(KiwiSdrSource::serverModeFor(2).lowCutHz, -4900);   // the one figure the live test confirmed
}

// ---- B -----------------------------------------------------------------------------------------------------------------------------------------

void testBAsRadioTwo() {
    std::printf("B: a KiwiSDR receiver as radio 2\n");
    Seen seen;
    FakeKiwi srv(playing(seen));
    CHECK(srv.ok());
    Rig r;
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0.0, 1)).status(), FOXAPI_OK);
    r.pump(20);
    const Rig::Spectrum sp1a = r.spectrum(1);
    const std::size_t peak1a = peakBin(sp1a);

    CHECK_EQ(r.add(2, "open-kiwisdr:" + addr(srv)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "kiwisdr"; }, 20000));
    CHECK_EQ(r.fact("radio.2.source"), std::string("kiwisdr"));
    CHECK_EQ(r.fact("radio.2.refusal"), std::string("<absent>"));
    CHECK_EQ(r.fact("radio.2.device"), std::string("kiwisdr|kiwisdr=" + addr(srv)));
    CHECK_EQ(r.fact("radio.2.range"), std::string("<absent>"));   // receivers differ: no range is claimed
    // the receiver's audio is in radio 2's spectrum, at 12 kS/s
    CHECK(r.until([&] { return r.radio(2).sampleRateHz == 12000.0 && toneInSpectrum(r.spectrum(2)); }, 20000));
    CHECK_EQ(r.radio(2).sampleRateHz, 12000.0);
    CHECK(toneInSpectrum(r.spectrum(2)));
    CHECK_EQ(r.fact("radio.2.kiwi.state"), std::string("playing"));
    CHECK_EQ(r.fact("radio.2.kiwi.mode"), std::string("am"));
    CHECK(r.fact("radio.2.kiwi.rssi") != "<absent>");
    CHECK(r.fact("radio.2.kiwi.detail").find("receiving") != std::string::npos);
    CHECK_EQ(r.fact("radio.1.kiwi.state"), std::string("<absent>"));   // only a KiwiSDR radio carries the facts
    // the commands the receiver was sent: the opening ones, then the tune, with the receiver's own mode word
    CHECK(seen.has("SET auth t=kiwi"));
    CHECK(seen.has("SET ident_user=FoxSDR"));
    CHECK(seen.has("SET compression="));
    CHECK(hasMode(seen, "am"));
    CHECK(seen.has("freq="));
    // radio 1 does not move
    CHECK_EQ(peakBin(r.spectrum(1)), peak1a);
    CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
    // the second connection from the same receiver is refused, naming the holder, in either spelling
    {
        const testing::Outcome again = r.add(3, "open-kiwisdr:" + addr(srv));
        CHECK_EQ(again.status(), FOXAPI_BUSY);
        CHECK(std::string(again.result.message).find("Radio 2") != std::string::npos);
        const testing::Outcome again2 = r.add(3, "open-kiwisdr:kiwisdr=" + addr(srv));
        CHECK_EQ(again2.status(), FOXAPI_BUSY);
        CHECK_EQ(r.fact("radio.count"), std::string("2"));
        CHECK_EQ(srv.accepted(), 1);
    }
    // a retune reaches the receiver in its own units (kHz) and moves the radio's centre; the offset stays 0
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 7100000.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return seen.has("freq=7100.000"); }, 10000));
    CHECK(r.until([&] { return r.radio(2).centreHz == 7100000.0; }, 5000));
    CHECK_EQ(r.radio(2).vfoOffsetHz, 0.0);
    // a mode the receiver has is sent; the radio's own demodulator is not what changes (the audio is already demodulated)
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, 2, 0.0, 5)).status(), FOXAPI_OK);   // USB (the keys' 1-based order)
    CHECK(r.until([&] { return seen.has("SET mod=usb "); }, 10000));
    CHECK_EQ(r.radio(2).demodMode, 5u);
    CHECK(r.until([&] { return r.fact("radio.2.kiwi.mode") == "usb"; }, 5000));
    {
        const std::string t = seen.last("SET mod=usb ");
        CHECK(t.find("low_cut=300") != std::string::npos);
        CHECK(t.find("high_cut=2700") != std::string::npos);
        CHECK(t.find("freq=7100.000") != std::string::npos);   // the mode change carries the radio's frequency
    }
    // a mode the receiver has no word for is kept as the user's pick and sends nothing new
    const std::size_t before = seen.all().size();
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, 2, 0.0, 2)).status(), FOXAPI_OK);   // WFM
    r.pump(10);
    CHECK_EQ(r.radio(2).demodMode, 2u);
    CHECK_EQ(r.fact("radio.2.kiwi.mode"), std::string("usb"));
    {
        bool tuned = false;
        const std::vector<std::string> now = seen.all();
        for (std::size_t i = before; i < now.size(); ++i) { tuned = tuned || now[i].find("SET mod=") != std::string::npos; }
        CHECK(!tuned);
    }
    // a VFO offset is refused (0 kept): the receiver tunes itself. (A narrow bandwidth first, so that the span clamp alone would have let 2 kHz stand.)
    {
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_BANDWIDTH, 2, 3000.0)).status(), FOXAPI_OK);
        const testing::Outcome o = r.send(r.aimed(FOXAPI_OP_SET_VFO_OFFSET, 2, 2000.0));
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK((o.result.flags & FOXAPI_RESULT_CLAMPED) != 0u);
        r.pump(10);
        CHECK_EQ(r.radio(2).vfoOffsetHz, 0.0);
        const testing::Outcome free = r.send(r.aimed(FOXAPP_OP_SET_VFO_OFFSET_FREE, 2, 2000.0));
        CHECK_EQ(free.status(), FOXAPI_OK);
        r.pump(10);
        CHECK_EQ(r.radio(2).vfoOffsetHz, 0.0);
        // a click on the display moves the receiver's frequency, not an offset
        CHECK_EQ(r.send(r.aimed(FOXAPP_OP_VFO_TO_ABSOLUTE, 2, 7110000.0)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.radio(2).centreHz == 7110000.0; }, 5000));
        CHECK_EQ(r.radio(2).vfoOffsetHz, 0.0);
        CHECK(r.until([&] { return seen.has("freq=7110.000"); }, 10000));
    }
    // the audio still plays after all that
    CHECK(r.until([&] { return toneInSpectrum(r.spectrum(2)); }, 10000));
    // a removed radio lets go of the receiver: the connection ends and it can be opened again
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 1, 0.0, 2)).status(), FOXAPI_OK);
    CHECK_EQ(r.add(2, "open-kiwisdr:" + addr(srv)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "kiwisdr" && r.fact("radio.2.kiwi.state") == "playing"; }, 20000));
    CHECK_EQ(srv.accepted(), 2);
}

// ---- C -----------------------------------------------------------------------------------------------------------------------------------------

void testCRefusalsArePublished() {
    std::printf("C: a refused or unreachable receiver is a refusal fact, the radio stays on the generator\n");
    Rig r;
    // busy
    {
        Seen seen;
        FakeKiwi srv(saying(seen, "too_busy=1"));
        CHECK_EQ(r.add(2, "open-kiwisdr:" + addr(srv)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.2.refusal") != "<absent>"; }, 30000));
        CHECK_EQ(r.fact("radio.2.source"), std::string("siggen"));
        CHECK(r.fact("radio.2.refusal").find("busy") != std::string::npos);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 1, 0.0, 2)).status(), FOXAPI_OK);
    }
    // a password
    {
        Seen seen;
        FakeKiwi srv(saying(seen, "badp=1"));
        CHECK_EQ(r.add(2, "open-kiwisdr:" + addr(srv)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.2.refusal") != "<absent>"; }, 30000));
        CHECK(r.fact("radio.2.refusal").find("password") != std::string::npos);
        CHECK_EQ(r.fact("radio.2.source"), std::string("siggen"));
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 1, 0.0, 2)).status(), FOXAPI_OK);
    }
    // down
    {
        Seen seen;
        FakeKiwi srv(saying(seen, "down=1"));
        CHECK_EQ(r.add(2, "open-kiwisdr:" + addr(srv)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.2.refusal") != "<absent>"; }, 30000));
        CHECK(r.fact("radio.2.refusal").find("down") != std::string::npos);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 1, 0.0, 2)).status(), FOXAPI_OK);
    }
    // not there at all
    {
        CHECK_EQ(r.add(2, "open-kiwisdr:" + closedAddress()).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.2.refusal") != "<absent>"; }, 30000));
        CHECK_EQ(r.fact("radio.2.source"), std::string("siggen"));
        CHECK(r.fact("radio.2.refusal").find("connect") != std::string::npos);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 1, 0.0, 2)).status(), FOXAPI_OK);
    }
    // a bad address is refused at the add, nothing is made
    {
        const testing::Outcome o = r.add(2, "open-kiwisdr:http://rx.example.org/");
        CHECK_EQ(o.status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.fact("radio.count"), std::string("1"));
    }
    // a remote session may not make the engine connect anywhere
    {
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        FoxCommand c = testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0.0, 0, "open-kiwisdr:127.0.0.1:8073");
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(remote.raw(), &c, 1, &sr);
        std::int32_t status = n < 0 ? n : sr.status;
        for (int pass = 0; pass < 40 && status == FOXAPI_OK; ++pass) {
            r.pump(1);
            FoxCommandResult res[8];
            for (auto& x : res) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(remote.raw(), res, 8);
            for (int32_t i = 0; i < got; ++i) {
                if (res[i].ticket == sr.ticket) {
                    status = res[i].status;
                    pass = 99;
                }
            }
        }
        CHECK_EQ(status, FOXAPI_DENIED);
    }
    // a receiver that accepts and says nothing is given up on, in words (the client's first-word wait is shortened for the test by the source's Timing in the unit below)
    {
        Seen seen;
        FakeKiwi srv(silent(seen));
        KiwiSdrSource::Timing t;
        t.firstAudioWait = std::chrono::milliseconds(300);
        KiwiSdrSource src(t);
        CHECK(!src.open(addr(srv)));
        CHECK(std::string(src.lastError()).find("no audio") != std::string::npos);
        CHECK(!src.isOpen());
        CHECK(!src.faulted());
    }
}

// ---- D -----------------------------------------------------------------------------------------------------------------------------------------

void testDSavedAndRestored() {
    std::printf("D: the kind is saved with the address and no password, and comes back\n");
    Seen seen;
    auto srv = std::make_unique<FakeKiwi>(playing(seen));
    const std::string a = addr(*srv);
    AppConfig file;
    {
        Rig x;
        CHECK_EQ(x.add(2, "open-kiwisdr:" + a).status(), FOXAPI_OK);
        CHECK_EQ(x.add(3, "").status(), FOXAPI_OK);
        CHECK(x.until([&] { return x.fact("radio.2.source") == "kiwisdr" && x.fact("radio.2.kiwi.state") == "playing"; }, 20000));
        CHECK_EQ(x.send(x.aimed(FOXAPI_OP_SET_CENTRE, 2, 5000000.0)).status(), FOXAPI_OK);
        CHECK_EQ(x.send(x.aimed(FOXAPI_OP_SET_MODE, 2, 0.0, 5)).status(), FOXAPI_OK);   // USB
        CHECK_EQ(x.send(x.aimed(FOXAPI_OP_SET_VOLUME, 2, 0.37)).status(), FOXAPI_OK);
        x.pump(20);
        file = throughTheFile(x.eng);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(2));
        CHECK_EQ(file.radios[0].number, 2);
        CHECK_EQ(file.radios[0].sourceKind, std::string("kiwisdr"));
        CHECK_EQ(file.radios[0].args, "kiwisdr=" + a);
        CHECK_EQ(file.radios[0].mode, std::string("USB"));
        CHECK_EQ(file.radios[1].sourceKind, std::string("siggen"));
        const std::string text = ConfigStore::serialize(Access::snapshot(x.eng));
        CHECK(text.find("password") == std::string::npos);
        const json j = json::parse(text);
        CHECK_EQ(j["radios"][0]["sourceKind"].get<std::string>(), std::string("kiwisdr"));
        CHECK_EQ(j["sourceKind"].get<std::string>(), std::string("siggen"));
    }
    // A RESTART from the file alone
    {
        Rig y(&file);
        CHECK(y.until([&] { return y.fact("radio.2.source") == "kiwisdr"; }, 20000));
        CHECK_EQ(y.fact("radio.restore"), std::string("<absent>"));
        CHECK(y.until([&] { return y.fact("radio.2.kiwi.state") == "playing"; }, 20000));
        CHECK(std::fabs(y.radio(2).volume - 0.37) < 1e-6);
        CHECK_EQ(y.radio(2).demodMode, 5u);
        CHECK_EQ(y.radio(2).centreHz, 5000000.0);
        CHECK_EQ(y.fact("radio.2.kiwi.mode"), std::string("usb"));
        CHECK_EQ(Access::kind(y.eng, 1), std::string("kiwisdr"));
        const AppConfig again = throughTheFile(y.eng);
        CHECK_EQ(again.radios.size(), file.radios.size());
        CHECK_EQ(again.radios[0].sourceKind, std::string("kiwisdr"));
        CHECK_EQ(again.radios[0].args, file.radios[0].args);
    }
    // THE RECEIVER IS NOT THERE: the generator stands in, the restore says so, the next save still names the receiver
    srv.reset();
    {
        Rig z(&file);
        CHECK(z.until([&] { return z.fact("radio.restore") != "<absent>"; }, 30000));
        CHECK(z.fact("radio.restore").find("Radio 2:") != std::string::npos);
        CHECK_EQ(z.fact("radio.2.source"), std::string("siggen"));
        CHECK(z.fact("radio.2.refusal") != "<absent>");
        const AppConfig again = throughTheFile(z.eng);
        CHECK_EQ(again.radios.size(), file.radios.size());
        CHECK_EQ(again.radios[0].sourceKind, std::string("kiwisdr"));
        CHECK_EQ(again.radios[0].args, file.radios[0].args);
    }
    // an older reader: the key it does not know is ignored, the rest loads (the file with the kind replaced by one that does not exist)
    {
        AppConfig junk = file;
        junk.radios[0].sourceKind = "nonesuch";
        AppConfig back;
        std::string error;
        CHECK(ConfigStore::parse(ConfigStore::serialize(junk), "test", back, error));
        CHECK_EQ(back.radios[0].sourceKind, std::string("siggen"));
    }
}

// ---- E -----------------------------------------------------------------------------------------------------------------------------------------

void testERadioOne() {
    std::printf("E: the first radio on a KiwiSDR receiver\n");
    Seen seen;
    FakeKiwi srv(playing(seen));
    AppConfig file;
    {
        Rig r;
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SELECT_SOURCE, 0, 0.0, 0, ("open-kiwisdr:" + addr(srv)).c_str())).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.1.source") == "kiwisdr" && r.fact("radio.1.kiwi.state") == "playing"; }, 20000));
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0.0, 1)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return toneInSpectrum(r.spectrum(1)); }, 20000));
        CHECK_EQ(r.radio(1).sampleRateHz, 12000.0);
        file = throughTheFile(r.eng);
        CHECK_EQ(file.sourceKind, std::string("kiwisdr"));
        CHECK_EQ(file.nativeArgs, "kiwisdr=" + addr(srv));
    }
    {
        Rig y(&file);
        CHECK(y.until([&] { return y.fact("radio.1.source") == "kiwisdr"; }, 20000));
        CHECK_EQ(y.send(y.aimed(FOXAPI_OP_RUN, 1, 0.0, 1)).status(), FOXAPI_OK);
        CHECK(y.until([&] { return toneInSpectrum(y.spectrum(1)); }, 20000));
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_kiwisdr\n");
    engine_api_test::hermetic("kiwisdr");
    Access::installHooks();
    testAAddressRules();
    testBAsRadioTwo();
    testCRefusalsArePublished();
    testDSavedAndRestored();
    testERadioOne();
    return check::finish("test_engine_api_kiwisdr");
}
