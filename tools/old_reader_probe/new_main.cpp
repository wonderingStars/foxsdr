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
    std::printf("new writer: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
