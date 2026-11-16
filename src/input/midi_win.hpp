// midi_win.hpp - MIDI input ports through the Windows multimedia API (winmm midiIn*).
//
// WHY winmm. It is in every Windows, needs no SDK package, no window and no message loop (CALLBACK_FUNCTION runs on a driver thread), and Windows
// MIDI Services re-routes it on current Windows 11 (the design record's facts report). Its limits, stated: MIDI 1.0 only; the port name is truncated by
// the API to 31 characters; a short message arrives packed in one DWORD with running status already expanded (Microsoft, MIM_DATA); system exclusive
// arrives only if buffers are added with midiInAddBuffer, which this layer never does, so sysex is not delivered at all; whether a system real-time byte (clock,
// active sensing) reaches a MIM_DATA callback at all is NOT stated by the cited page and is UNVERIFIED (the decoder ignores them either way); winmm gives
// no documented "device removed" notice, so a removed port is found by presence polling (midiPortPresent) and by MIM_CLOSE if the driver sends one.
//
// midiInClose is never called from the callback (that deadlocks); close() runs on the caller's thread, and is bounded: it is done on a helper thread that
// close() waits for at most `timeoutMs`, so a driver that never returns from midiInReset cannot hang the caller (it returns false and the port is leaked).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#ifdef _WIN32

#include <windows.h>

#include <mmsystem.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "input_event.hpp"
#include "midi_parse.hpp"

namespace cascade::input {

struct MidiPortInfo {
    unsigned index = 0;      // winmm device id (changes when ports come and go)
    std::string name;        // utf-8, as winmm reports it (max 31 characters)
    std::string stableId;    // midi:<name>, with #2, #3 for ports of the same name in enumeration order
    unsigned manufacturer = 0, product = 0;
};

std::vector<MidiPortInfo> enumerateMidiInputs();
bool midiPortPresent(const std::string& stableId);  // by re-enumerating

class MidiInputPort {
public:
    MidiInputPort() = default;
    ~MidiInputPort() { close(); }
    MidiInputPort(const MidiInputPort&) = delete;
    MidiInputPort& operator=(const MidiInputPort&) = delete;

    // Opens by winmm id (a bad id or a busy port fails with the winmm message in `error`) and starts delivering events to `q` (which must outlive the port).
    bool open(const MidiPortInfo& info, EventQueue& q, std::string& error);
    bool open(unsigned winmmId, const std::string& stableId, EventQueue& q, std::string& error);
    bool close(unsigned timeoutMs = 2000);
    bool isOpen() const { return impl_ != nullptr; }
    MidiDecoder* decoder() { return dec_.get(); }  // per-control encodings are set here
    std::uint64_t messages() const;
    std::uint64_t rejected() const;

    struct Impl;

private:
    Impl* impl_ = nullptr;
    std::unique_ptr<MidiDecoder> dec_;
};

}  // namespace cascade::input

#endif  // _WIN32
