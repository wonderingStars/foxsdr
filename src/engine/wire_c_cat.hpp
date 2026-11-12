// wire_c_cat.hpp - CAT EMULATION for logging programs (wiring lane C row L7; the model is net/cat_emulation.hpp): FoxSDR answers like a Kenwood TS-480 or a Yaesu FT-891 on a TCP port,
// so a logger or a digital-mode program that wants a rig can read FoxSDR's frequency and mode and can tune FoxSDR - a SoftRock-style receiver on the other end of a CAT cable.
//
// RECEIVE ONLY, by the model's construction and checked here again: every transmit command (TX, PTT, the keyer, power, memory write) is answered E; or ?; and nothing is keyed or sent; there
// is no way to key a transmitter from this port. OFF by default; 127.0.0.1 only unless the person switches "all addresses" on; the port must be given (0 is "not configured") and must not
// be 4532, the port of FoxSDR's own rigctl server (two servers cannot share it and a logger set up for one would talk to the other). The CAT commands that retune FoxSDR use the engine's
// own CAT session (the same closed list of operations the rigctl server has), so a logger can do no more than a rigctl client.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "net/cat_emulation.hpp"

struct FoxEngineApi;
struct FoxSession;

namespace cascade::engine::wirec {

class CatEmulation {
public:
    // How the emulator reaches the engine: the table and ONE internal CAT session (opened by the caller, closed by this object).
    struct Opener {
        const FoxEngineApi* table = nullptr;
        std::function<FoxSession*()> open;
    };
    CatEmulation() = default;
    ~CatEmulation() { stop(); }
    CatEmulation(const CatEmulation&) = delete;
    CatEmulation& operator=(const CatEmulation&) = delete;

    // Applies the settings: closes what runs and opens anew when asked. `error` ("" when fine or off) is a sentence. Returns true when the running state changed.
    bool configure(bool enabled, const std::string& dialect, int port, bool bindAll, const Opener& opener);
    void stop();
    bool running() const;
    int boundPort() const;
    int clients() const;
    std::uint64_t commands() const;
    const std::string& error() const { return error_; }

private:
    struct Bridge;
    std::shared_ptr<Bridge> bridge_;
    std::unique_ptr<cascade::net::CatTcpService> service_;
    std::string error_;
};

}  // namespace cascade::engine::wirec
