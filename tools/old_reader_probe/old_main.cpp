#include <cstdio>
#include <string>
#include "core/config.hpp"
using cascade::core::AppConfig;
using cascade::core::ConfigStore;
int main(int argc, char** argv) {
    if (argc < 3) { return 2; }
    AppConfig cfg;
    std::string err;
    const bool ok = ConfigStore::load(argv[1], cfg, err);
    std::printf("old reader: load %s (%s)\n", ok ? "ok" : "FAILED", err.c_str());
    std::printf("old reader: sourceKind=%s centerHz=%.0f mode=%s volume=%.2f  nativeArgs=%s schemaVersion=%d\n", cfg.sourceKind.c_str(), cfg.centerHz, cfg.mode.c_str(), cfg.volume, cfg.nativeArgs.c_str(), cfg.schemaVersion);
    const bool saved = ConfigStore::save(argv[2], cfg, err);
    std::printf("old reader: write-back %s (%s)\n", saved ? "ok" : "FAILED", err.c_str());
    return ok && saved ? 0 : 1;
}
