// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifdef _WIN32

#include "midi_win.hpp"

#include <chrono>
#include <future>
#include <map>
#include <thread>

namespace cascade::input {

namespace {
std::string toUtf8(const wchar_t* w) {
    if (!w || !*w) { return {}; }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) { return {}; }
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string mmErrorText(MMRESULT r) {
    wchar_t buf[MAXERRORLENGTH] = {};
    if (midiInGetErrorTextW(r, buf, MAXERRORLENGTH) != MMSYSERR_NOERROR) { return "winmm error " + std::to_string(r); }
    return toUtf8(buf);
}
}  // namespace

std::vector<MidiPortInfo> enumerateMidiInputs() {
    std::vector<MidiPortInfo> out;
    std::map<std::string, int> seen;
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) { continue; }
        MidiPortInfo p;
        p.index = i;
        p.name = toUtf8(caps.szPname);
        p.manufacturer = caps.wMid;
        p.product = caps.wPid;
        const int k = seen[p.name]++;
        p.stableId = "midi:" + p.name + (k > 0 ? "#" + std::to_string(k + 1) : std::string());
        out.push_back(std::move(p));
    }
    return out;
}

bool midiPortPresent(const std::string& stableId) {
    for (const MidiPortInfo& p : enumerateMidiInputs()) {
        if (p.stableId == stableId) { return true; }
    }
    return false;
}

struct MidiInputPort::Impl {
    HMIDIIN h = nullptr;
    EventQueue* q = nullptr;
    MidiDecoder* dec = nullptr;
    std::uint32_t device = 0;
    std::atomic<bool> closing{false};
    std::atomic<std::uint64_t> messages{0}, rejected{0};
};

namespace {
void CALLBACK midiCb(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR) {
    auto* im = reinterpret_cast<MidiInputPort::Impl*>(inst);
    if (!im) { return; }
    switch (msg) {
        case MIM_DATA: {
            MidiMessage m;
            if (!unpackShortMessage(static_cast<std::uint32_t>(p1), m)) { ++im->rejected; return; }
            ++im->messages;
            InputEvent ev[2];
            const std::size_t n = im->dec->handle(m, nowMicros(), ev);
            for (std::size_t i = 0; i < n; ++i) { im->q->push(ev[i]); }
            return;
        }
        case MIM_CLOSE:
            if (!im->closing.load()) {  // closed under us: the device went away
                InputEvent e;
                e.device = im->device;
                e.kind = EventKind::Disconnected;
                e.timeUs = nowMicros();
                im->q->push(e);
            }
            return;
        case MIM_ERROR:
        case MIM_LONGERROR: ++im->rejected; return;
        default: return;  // MIM_OPEN, MIM_LONGDATA (never asked for), MIM_MOREDATA (not enabled)
    }
}
}  // namespace

bool MidiInputPort::open(const MidiPortInfo& info, EventQueue& q, std::string& error) { return open(info.index, info.stableId, q, error); }

bool MidiInputPort::open(unsigned id, const std::string& stableId, EventQueue& q, std::string& error) {
    close();
    auto* im = new Impl();
    im->q = &q;
    im->device = deviceIdFor(stableId);
    auto dec = std::make_unique<MidiDecoder>(im->device);
    im->dec = dec.get();
    HMIDIIN h = nullptr;
    const MMRESULT r = midiInOpen(&h, id, reinterpret_cast<DWORD_PTR>(&midiCb), reinterpret_cast<DWORD_PTR>(im), CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
        error = "cannot open MIDI input " + std::to_string(id) + ": " + mmErrorText(r);
        delete im;
        return false;
    }
    im->h = h;
    const MMRESULT s = midiInStart(h);
    if (s != MMSYSERR_NOERROR) {
        error = "cannot start MIDI input: " + mmErrorText(s);
        im->closing = true;
        midiInClose(h);
        delete im;
        return false;
    }
    impl_ = im;
    dec_ = std::move(dec);
    return true;
}

bool MidiInputPort::close(unsigned timeoutMs) {
    if (!impl_) { return true; }
    Impl* im = impl_;
    impl_ = nullptr;
    im->closing = true;
    auto work = std::make_shared<std::promise<void>>();
    std::future<void> fut = work->get_future();
    HMIDIIN h = im->h;
    std::thread([work, h] {
        midiInStop(h);
        midiInReset(h);
        midiInClose(h);
        work->set_value();
    }).detach();
    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
        // The driver did not return: the callback may still run, so the Impl and the decoder it points at are leaked.
        (void)dec_.release();
        return false;
    }
    delete im;  // no callback can run after midiInClose has returned
    dec_.reset();
    return true;
}

std::uint64_t MidiInputPort::messages() const { return impl_ ? impl_->messages.load() : 0; }
std::uint64_t MidiInputPort::rejected() const { return impl_ ? impl_->rejected.load() : 0; }

}  // namespace cascade::input

#endif  // _WIN32
