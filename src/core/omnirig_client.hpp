// omnirig_client.hpp - a client model for OmniRig, the freeware COM component for transceiver CAT control by Alex Shovkoplyas VE3NEA (Afreet Software):
// READ operations first (frequency, mode, status, the transmit STATE), and two writes that cannot key anything (set frequency, set mode).
//
// WHAT THIS IS AND IS NOT: the COM interface is reached through an abstract port (OmniRigRigPort) that mirrors the properties of OmniRig's IRigX.
// This file never calls COM and never starts a COM server; a Windows COM bridge that implements the port (CoCreateInstance of "OmniRig.OmniRigX",
// IRigX property get/put) is a HAND-OFF written by the wiring lane. Tests run against a stand-in that implements the port in memory.
//
// SOURCE (opened 2026-11-11): the OmniRig type library source https://github.com/VE3NEA/OmniRig/blob/master/OmniRig.ridl (repository licence: MIT,
// "Copyright (c) 2024 Alex Shovkoplyas", read from https://raw.githubusercontent.com/VE3NEA/OmniRig/master/LICENSE) and the maker's page
// https://www.dxatlas.com/OmniRig/ (OmniRig 1.20, "A COM component for transceiver/receiver CAT control", FREEWARE, (c) 1998-2020 Afreet Software). The RigParamX
// enumeration values below are the ones the .ridl declares; IRigX has Freq, FreqA, FreqB (long, read/write), Vfo, Split, Rit, Xit, Tx, Mode (RigParamX,
// read/write), Status (RigStatusX, read), StatusStr, RigType, and the methods SendCustomCommand, SetSimplexMode, SetSplitMode, ClearRit, GetRxFrequency,
// GetTxFrequency; IPortBits has Rts, Dtr (read/write) and Lock/Unlock.
//
// NO CALL THAT KEYS A TRANSMITTER, BY CONSTRUCTION: the port has NO setter for the Tx property (writing PM_TX/PM_RX is how a client keys the rig), no
// SendCustomCommand (raw CAT bytes can key it), no PortBits access (RTS/DTR are the usual PTT lines), no split/simplex/RIT/XIT writes. The Tx property is
// exposed READ-ONLY as a state. tests/test_omnirig_client.cpp holds all of that by a compile-time concept check, a source scan and a randomised run
// against a stand-in that records every call.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>

namespace cascade::core::rig {

// RigParamX, values as declared in OmniRig.ridl.
enum RigParam : std::uint32_t {
    PM_UNKNOWN = 1, PM_FREQ = 2, PM_FREQA = 4, PM_FREQB = 8, PM_PITCH = 16, PM_RITOFFSET = 32, PM_RIT0 = 64, PM_VFOAA = 128, PM_VFOAB = 256,
    PM_VFOBA = 512, PM_VFOBB = 1024, PM_VFOA = 2048, PM_VFOB = 4096, PM_VFOEQUAL = 8192, PM_VFOSWAP = 16384, PM_SPLITON = 32768, PM_SPLITOFF = 65536,
    PM_RITON = 131072, PM_RITOFF = 262144, PM_XITON = 524288, PM_XITOFF = 1048576, PM_RX = 2097152, PM_TX = 4194304, PM_CW_U = 8388608,
    PM_CW_L = 16777216, PM_SSB_U = 33554432, PM_SSB_L = 67108864, PM_DIG_U = 134217728, PM_DIG_L = 268435456, PM_AM = 536870912, PM_FM = 1073741824
};

// RigStatusX
enum RigStatus : std::int32_t { ST_NOTCONFIGURED = 0, ST_DISABLED = 1, ST_PORTBUSY = 2, ST_NOTRESPONDING = 3, ST_ONLINE = 4 };

// The rig as OmniRig shows it. Reads return OmniRig's raw values; the writes are the ONLY two a client has.
class OmniRigRigPort {
public:
    virtual ~OmniRigRigPort() = default;
    // reads
    virtual std::int32_t status() = 0;          // RigStatusX
    virtual std::int32_t freq() = 0;            // Hz (OmniRig's Freq is a long)
    virtual std::int32_t freqA() = 0;
    virtual std::int32_t freqB() = 0;
    virtual std::uint32_t mode() = 0;           // one RigParamX bit
    virtual std::uint32_t vfo() = 0;            // PM_VFOAA ... PM_VFOB
    virtual std::uint32_t split() = 0;          // PM_SPLITON / PM_SPLITOFF
    virtual std::uint32_t txState() = 0;        // READ-ONLY: PM_TX while the rig transmits, PM_RX otherwise
    // the only writes
    virtual bool putFreq(std::int32_t hz) = 0;
    virtual bool putMode(std::uint32_t param) = 0;
};

// The mode bits a client may write: the eight mode values and nothing else (no PM_TX, no PM_RX, no VFO/split/RIT/XIT value).
inline bool isWritableMode(std::uint32_t p) {
    return p == PM_CW_U || p == PM_CW_L || p == PM_SSB_U || p == PM_SSB_L || p == PM_DIG_U || p == PM_DIG_L || p == PM_AM || p == PM_FM;
}

inline const char* modeName(std::uint32_t p) {
    switch (p) {
        case PM_CW_U: return "CW";
        case PM_CW_L: return "CW-R";
        case PM_SSB_U: return "USB";
        case PM_SSB_L: return "LSB";
        case PM_DIG_U: return "DIG-U";
        case PM_DIG_L: return "DIG-L";
        case PM_AM: return "AM";
        case PM_FM: return "FM";
        default: return "";
    }
}
inline std::uint32_t modeFromName(const std::string& n) {
    for (std::uint32_t p : {PM_CW_U, PM_CW_L, PM_SSB_U, PM_SSB_L, PM_DIG_U, PM_DIG_L, PM_AM, PM_FM}) {
        if (n == modeName(p)) { return p; }
    }
    return 0;
}

struct RigSnapshot {
    bool online = false;
    std::int32_t status = ST_NOTCONFIGURED;
    double freqHz = 0, freqAHz = 0, freqBHz = 0;
    std::uint32_t mode = 0;        // RigParamX bit, 0 when unknown
    std::string modeText;          // modeName(mode), empty when unknown
    bool splitOn = false;
    bool transmitting = false;     // the READ-ONLY Tx state
    bool vfoA = false;             // true when VFO A is the receive VFO (PM_VFOAA / PM_VFOAB / PM_VFOA)
};

class OmniRigClient {
public:
    explicit OmniRigClient(OmniRigRigPort* port) : port_(port) {}

    // One read of every property. When the rig is not online only status/online are filled (OmniRig's other properties mean nothing then).
    RigSnapshot snapshot() {
        RigSnapshot s;
        if (port_ == nullptr) { return s; }
        s.status = port_->status();
        s.online = s.status == ST_ONLINE;
        if (!s.online) { return s; }
        s.freqHz = port_->freq();
        s.freqAHz = port_->freqA();
        s.freqBHz = port_->freqB();
        s.mode = port_->mode();
        s.modeText = modeName(s.mode);
        if (s.modeText.empty()) { s.mode = 0; }
        s.splitOn = port_->split() == PM_SPLITON;
        s.transmitting = port_->txState() == PM_TX;
        const std::uint32_t v = port_->vfo();
        s.vfoA = v == PM_VFOAA || v == PM_VFOAB || v == PM_VFOA;
        return s;
    }

    // Sets the current VFO's frequency. Refused (false, a sentence) when there is no port, the rig is not online, the value is outside 1 Hz..2147483647 Hz, or
    // the rig is transmitting (a frequency change under power is never sent from here).
    bool setFrequency(double hz, std::string& error) {
        if (port_ == nullptr) { error = "no OmniRig connection"; return false; }
        if (port_->status() != ST_ONLINE) { error = "the rig is not online in OmniRig"; return false; }
        if (!(hz >= 1.0 && hz <= 2147483647.0)) { error = "the frequency is outside what OmniRig can set"; return false; }
        if (port_->txState() == PM_TX) { error = "the rig is transmitting"; return false; }
        if (!port_->putFreq(static_cast<std::int32_t>(hz + 0.5))) { error = "OmniRig did not accept the frequency"; return false; }
        error.clear();
        return true;
    }

    // Sets the mode by name ("CW", "CW-R", "USB", "LSB", "DIG-U", "DIG-L", "AM", "FM"). Same refusals as setFrequency, plus an unknown name.
    bool setMode(const std::string& name, std::string& error) {
        if (port_ == nullptr) { error = "no OmniRig connection"; return false; }
        const std::uint32_t p = modeFromName(name);
        if (p == 0 || !isWritableMode(p)) { error = "that is not a mode OmniRig can be given"; return false; }
        if (port_->status() != ST_ONLINE) { error = "the rig is not online in OmniRig"; return false; }
        if (port_->txState() == PM_TX) { error = "the rig is transmitting"; return false; }
        if (!port_->putMode(p)) { error = "OmniRig did not accept the mode"; return false; }
        error.clear();
        return true;
    }

private:
    OmniRigRigPort* port_;
};

}  // namespace cascade::core::rig
