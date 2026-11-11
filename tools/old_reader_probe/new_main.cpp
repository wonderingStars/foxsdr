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
    std::printf("new writer: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
