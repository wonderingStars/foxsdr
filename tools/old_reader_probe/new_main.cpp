#include <cstdio>
#include <string>
#include "core/config.hpp"
using namespace cascade::core;
int main(int argc, char** argv) {
    if (argc < 3) { return 2; }
    AppConfig c;
    c.sourceKind = "rtlsdr";
    c.nativeArgs = "serial=00000001";
    c.centerHz = 98.5e6;
    c.mode = "WFM";
    c.volume = 0.35f;
    c.agcMode = 2;
    c.nbEnabled = true;
    std::string err;
    bool ok = ConfigStore::save(argv[1], c, err);   // ONE radio
    for (int n = 2; n <= 8; ++n) {
        SavedRadio r;
        r.number = n;
        r.sourceKind = "siggen";
        r.toneHz = 1000.0 * n;
        r.centerHz = 100e6 + n;
        c.radios.push_back(r);
    }
    c.pluginRadio["pocsag-decoder"] = 3;
    ok = ConfigStore::save(argv[2], c, err) && ok;   // EIGHT radios
    if (argc > 3) {
        // STAGE 7: radios on a file, an rtl_tcp server and a Pluto, saved with their real kind
        c.radios.clear();
        const struct { int n; const char* kind; const char* args; } rows[] = {{2, "file", "D:/rec/sample.wav"}, {3, "rtltcp", "rtltcp=192.168.1.20:1234"}, {4, "pluto", "uri=ip:192.168.2.1"}};
        for (const auto& row : rows) {
            SavedRadio r;
            r.number = row.n;
            r.sourceKind = row.kind;
            r.args = row.args;
            r.centerHz = 100e6 + row.n;
            c.radios.push_back(r);
        }
        ok = ConfigStore::save(argv[3], c, err) && ok;   // FILE, RTL_TCP, PLUTO
    }
    if (argc > 4) {
        // ENGINE FEATURES LANE A: the TCI server, audio over the network and the timed recordings, all switched on, beside a radio 1 the old reader must still read exactly
        c.radios.clear();
        c.tciEnabled = true;
        c.tciBindAll = true;
        c.tciPort = 50123;
        c.udpAudioEnabled = true;
        c.udpAudioHost = "192.168.1.20";
        c.udpAudioPort = 7400;
        c.udpAudioRadio = 3;
        c.udpAudioPacketFrames = 960;
        sched::Entry e;
        e.id = 1;
        e.name = "weather pass";
        e.radio = 2;
        e.startUnix = 1790000100;
        e.lengthS = 600;
        e.repeat = sched::Repeat::Daily;
        e.tune = true;
        e.freqHz = 137.9e6;
        e.mode = 1;
        e.recordIq = true;
        c.schedule.push_back(e);
        ok = ConfigStore::save(argv[4], c, err) && ok;   // TCI, UDP AUDIO, SCHEDULE
    }
    if (argc > 5) {
        // ENGINE WIRING LANE 1: radio 1 on a KiwiSDR receiver with a tone squelch, radios 2 and 3 with tone squelch settings of their own and one of them on a KiwiSDR receiver
        c.radios.clear();
        c.tciEnabled = false;
        c.udpAudioEnabled = false;
        c.schedule.clear();
        c.sourceKind = "kiwisdr";
        c.nativeArgs = "kiwisdr=rx.example.org:8073";
        c.toneSquelchKind = 2;
        c.toneSquelchCode = 754;
        c.toneSquelchPolarity = 2;
        c.toneRemove = true;
        SavedRadio a;
        a.number = 2;
        a.sourceKind = "kiwisdr";
        a.args = "kiwisdr=rx.example.org:8074";
        a.centerHz = 7.1e6;
        a.toneSquelchKind = 1;
        a.toneSquelchHz = 123.0;
        c.radios.push_back(a);
        SavedRadio b;
        b.number = 3;
        b.sourceKind = "siggen";
        b.toneSquelchKind = 2;
        b.toneSquelchCode = 23;
        c.radios.push_back(b);
        ok = ConfigStore::save(argv[5], c, err) && ok;   // KIWISDR, TONE SQUELCH
    }
    std::printf("new writer: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
