// soundcard_source.cpp - see soundcard_source.hpp for what this is, why the
// device is found by name and host API, and how a dead card is noticed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soundcard_source.hpp"

// PortAudio has no Android host API and the Android build does not compile
// it (CMakeLists.txt, CASCADE_ANDROID): there the backend is an honest
// refusal, the same shape sink/audio_in.cpp takes - an empty list and a
// refused open - and everything above the backend is shared.
#if !defined(CASCADE_ANDROID)
#include <portaudio.h>
#ifdef _WIN32
#include <pa_win_wasapi.h>
#endif
#include "sink/pa_init.hpp"
#include "source/soundcard_portaudio.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "core/diag_log.hpp"

namespace cascade::source {

// --- settings, args, device list ------------------------------------------------

double soundCardIqRateHz(const SoundCardSettings& s) {
    return s.format == SoundCardFormat::RealMono ? dsp::RealToIq::outputRateHz(s.cardRateHz)
                                                 : s.cardRateHz;
}

double soundCardCentreHz(const SoundCardSettings& s) {
    return s.format == SoundCardFormat::RealMono ? dsp::RealToIq::centreHz(s.cardRateHz)
                                                 : s.iqCentreHz;
}

namespace {

// The characters that would break the args grammar (',' between fields, '='
// inside one, '|' between a patch key's driver and its args) and '%' itself.
// Everything else - spaces, brackets, non-ASCII bytes of a UTF-8 name - is
// passed through unchanged, so a saved key stays readable.
std::string encodeField(const std::string& v) {
    std::string out;
    for (const char c : v) {
        if (c == ',' || c == '=' || c == '|' || c == '%') {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            out += buf;
        } else {
            out += c;
        }
    }
    return out;
}

bool decodeField(const std::string& v, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '%') {
            out += v[i];
            continue;
        }
        if (i + 2 >= v.size()) { return false; }
        const auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') { return c - '0'; }
            if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
            if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
            return -1;
        };
        const int hi = hex(v[i + 1]);
        const int lo = hex(v[i + 2]);
        if (hi < 0 || lo < 0) { return false; }
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
    }
    return true;
}

bool parseNumber(const std::string& v, double& out) {
    if (v.empty()) { return false; }
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0' || !std::isfinite(d)) { return false; }
    out = d;
    return true;
}

// Every rate asked of a card, ascending. The standard audio rates from 8 kHz
// to 384 kHz; a card lists the subset its host API accepts.
constexpr double kCandidateRatesHz[] = {8000.0,   11025.0,  16000.0,  22050.0,  24000.0,
                                        32000.0,  44100.0,  48000.0,  88200.0,  96000.0,
                                        176400.0, 192000.0, 352800.0, 384000.0};

}  // namespace

std::string soundCardArgs(const SoundCardSettings& s) {
    char rate[32];
    std::snprintf(rate, sizeof(rate), "%.0f", s.cardRateHz);
    char centre[40];
    std::snprintf(centre, sizeof(centre), "%.3f", s.iqCentreHz);
    std::string out = "device=" + encodeField(s.device) + ",api=" + encodeField(s.hostApi) +
                      ",rate=" + rate +
                      ",format=" + (s.format == SoundCardFormat::IqStereo ? "iq" : "real") +
                      ",channel=" + (s.channel == 1 ? "right" : "left") +
                      ",swap=" + (s.swapIq ? "1" : "0") + ",centre=" + centre;
    return out;
}

std::string soundCardDeviceArgs(const std::string& device, const std::string& hostApi) {
    return "device=" + encodeField(device) + ",api=" + encodeField(hostApi);
}

bool parseSoundCardArgs(const std::string& args, SoundCardSettings& out) {
    SoundCardSettings s = out;
    std::string v;
    if (!decodeField(argValue(args, "device"), v)) { return false; }
    if (!argValue(args, "device").empty()) { s.device = v; }
    if (!decodeField(argValue(args, "api"), v)) { return false; }
    if (!argValue(args, "api").empty()) { s.hostApi = v; }
    const std::string rate = argValue(args, "rate");
    if (!rate.empty()) {
        double d = 0.0;
        if (!parseNumber(rate, d) || !(d > 0.0)) { return false; }
        s.cardRateHz = d;
    }
    const std::string format = argValue(args, "format");
    if (format == "iq") {
        s.format = SoundCardFormat::IqStereo;
    } else if (format == "real") {
        s.format = SoundCardFormat::RealMono;
    } else if (!format.empty()) {
        return false;
    }
    const std::string channel = argValue(args, "channel");
    if (channel == "right") {
        s.channel = 1;
    } else if (channel == "left") {
        s.channel = 0;
    } else if (!channel.empty()) {
        return false;
    }
    const std::string swap = argValue(args, "swap");
    if (swap == "1") {
        s.swapIq = true;
    } else if (swap == "0") {
        s.swapIq = false;
    } else if (!swap.empty()) {
        return false;
    }
    const std::string centre = argValue(args, "centre");
    if (!centre.empty()) {
        double d = 0.0;
        if (!parseNumber(centre, d)) { return false; }
        s.iqCentreHz = d;
    }
    out = s;
    return true;
}

std::string soundCardDeviceLabel(const SoundCardDevice& d) {
    if (d.hostApi.empty()) { return d.name; }
    return d.name + " (" + d.hostApi + ")";
}

namespace {

// A run of decimal digits at `at`, as an int; false for none (or absurdly many).
bool parseDigits(const std::string& s, std::size_t& at, int& out) {
    const std::size_t from = at;
    int v = 0;
    while (at < s.size() && s[at] >= '0' && s[at] <= '9') {
        if (at - from >= 6) { return false; }
        v = v * 10 + (s[at] - '0');
        ++at;
    }
    if (at == from) { return false; }
    out = v;
    return true;
}

// The host API PortAudio's ALSA backend reports (pa_linux_alsa.c,
// PaAlsa_Initialize: "ALSA").
constexpr const char* kAlsaHostApi = "ALSA";

}  // namespace

bool parseAlsaHwName(const std::string& name, AlsaHwName& out) {
    // "<stripped> (hw:N,M)" or "<stripped> (plughw:N,M)", and nothing after.
    if (name.empty() || name.back() != ')') { return false; }
    const std::size_t open = name.rfind(" (");
    if (open == std::string::npos || open == 0) { return false; }
    std::size_t at = open + 2;
    for (const char* prefix : {"plughw:", "hw:"}) {
        const std::size_t len = std::char_traits<char>::length(prefix);
        if (name.compare(at, len, prefix) == 0) {
            at += len;
            int card = -1;
            int device = -1;
            if (!parseDigits(name, at, card)) { return false; }
            if (at >= name.size() || name[at] != ',') { return false; }
            ++at;
            if (!parseDigits(name, at, device)) { return false; }
            if (at + 1 != name.size()) { return false; }  // only the ')' may follow
            out.stripped = name.substr(0, open);
            out.card = card;
            out.device = device;
            return true;
        }
    }
    return false;
}

SoundCardMatch matchSoundCard(const std::vector<SoundCardDevice>& list, const std::string& name,
                              const std::string& hostApi, bool exact) {
    SoundCardMatch m;
    if (name.empty()) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].isDefault) {
                m.at = static_cast<int>(i);
                return m;
            }
        }
        return m;
    }
    // AN ALSA HARDWARE NAME READ BACK FROM A FILE is matched by identity -
    // the name without the card number, and the PCM device - because the
    // card number is only the order ALSA found the cards in at this boot.
    AlsaHwName saved;
    if (!exact && hostApi == kAlsaHostApi && parseAlsaHwName(name, saved)) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            AlsaHwName here;
            if (list[i].hostApi == hostApi && parseAlsaHwName(list[i].name, here) &&
                here.stripped == saved.stripped && here.device == saved.device) {
                m.candidates.push_back(static_cast<int>(i));
            }
        }
        if (m.candidates.size() == 1) {
            m.at = m.candidates.front();
            m.candidates.clear();
        }
        // Two or more: identical cards, and nothing says which one this was.
        return m;
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].name == name && list[i].hostApi == hostApi) {
            m.at = static_cast<int>(i);
            return m;
        }
    }
    return m;
}

int findSoundCard(const std::vector<SoundCardDevice>& list, const std::string& name,
                  const std::string& hostApi) {
    return matchSoundCard(list, name, hostApi, false).at;
}

std::vector<SoundCardRate> soundCardRatesFor(const SoundCardDevice& d, SoundCardFormat f) {
    std::vector<SoundCardRate> out;
    if (f == SoundCardFormat::IqStereo && d.maxInputChannels < 2) { return out; }
    for (const SoundCardRate& r : d.rates) {
        if (f == SoundCardFormat::RealMono) {
            // Half the card rate must be a whole number the pipeline accepts.
            const double half = r.hz / 2.0;
            if (r.hz < 16000.0 || half != std::floor(half)) { continue; }
        } else if (r.hz < 8000.0) {
            continue;
        }
        out.push_back(r);
    }
    return out;
}

// --- the PortAudio backend -------------------------------------------------------

#if defined(CASCADE_ANDROID)

namespace {

// No PortAudio on Android (see the includes): no inputs, and an open that says
// why. A tablet's own microphone is a later slice, like the transmitter's.
class NoSoundCardBackend final : public SoundCardBackend {
public:
    std::vector<SoundCardDevice> listDevices() override { return {}; }
    bool open(const SoundCardDevice& /*dev*/, int /*channels*/, double /*rateHz*/, bool /*exclusive*/,
              PushFn /*push*/, void* /*user*/, std::string& error) override {
        error = "sound card input is not available in this build";
        return false;
    }
    void close() override {}
    bool alive() override { return false; }
};

}  // namespace

std::shared_ptr<SoundCardBackend> makePortAudioSoundCardBackend() {
    return std::make_shared<NoSoundCardBackend>();
}

#else  // !CASCADE_ANDROID

namespace {

// THE LIST-AND-OPEN LOCK. An enumeration (every rate of every input asked of
// the host API) and an open can each come from a different thread - the
// Source section's worker, a patch radio's worker - and PortAudio is not
// documented as safe for concurrent calls into one host API, so the sound card
// takes them one at a time. NOTHING ELSE EVER TAKES IT: not a close or an
// abort (which may never return on a card that has gone - see "CLOSING" in
// soundcard_source.hpp), not alive(), not a constructor or a destructor. So
// the most a hung close can hold up is its own closer thread. Allocated once
// and never destroyed, for the reason sink/pa_init.cpp gives.
std::mutex& listOpenMutex() {
    static std::mutex* const m = new std::mutex;
    return *m;
}

bool alsaHardwareName(const char* name) {
    AlsaHwName hw;
    return name != nullptr && parseAlsaHwName(name, hw);
}

PaError realInitialize() { return sink::paInitializeShared() ? paNoError : paNotInitialized; }

PaError realTerminate() {
    sink::paTerminateShared();
    return paNoError;
}

const SoundCardPaApi kRealPaApi = {
    &realInitialize,     &realTerminate,  &Pa_GetDeviceCount, &Pa_GetDefaultInputDevice,
    &Pa_GetDeviceInfo,   &Pa_GetHostApiInfo, &Pa_IsFormatSupported, &Pa_OpenStream,
    &Pa_StartStream,     &Pa_AbortStream, &Pa_CloseStream,    &Pa_IsStreamActive,
    &Pa_GetErrorText,
};

class PortAudioSoundCardBackend final : public SoundCardBackend {
public:
    // Calls nothing: PortAudio is initialised by the first list or open, on
    // the worker that asks (see makePortAudioSoundCardBackend).
    explicit PortAudioSoundCardBackend(const SoundCardPaApi& api) : api_(api) {}

    ~PortAudioSoundCardBackend() override {
        close();
        // Paired with OUR successful Pa_Initialize only; PortAudio refcounts.
        // A backend whose close never returned never gets here, so its
        // initialisation keeps PortAudio from being torn down under it.
        if (paOk_) { api_.terminate(); }
    }

    std::vector<SoundCardDevice> listDevices() override {
        std::vector<SoundCardDevice> out;
        if (!initialised()) { return out; }
        std::lock_guard<std::mutex> lk(listOpenMutex());
        const PaDeviceIndex count = api_.getDeviceCount();
        const PaDeviceIndex def = api_.getDefaultInputDevice();
        std::vector<std::size_t> apiDefaults;  // rows that are their host API's default input
        for (PaDeviceIndex i = 0; i < count; ++i) {
            const PaDeviceInfo* info = api_.getDeviceInfo(i);
            if (info == nullptr || info->maxInputChannels < 1) { continue; }
            const PaHostApiInfo* api = api_.getHostApiInfo(info->hostApi);
            // Only host APIs whose inputs keep their identity - never MME's
            // renumbered waveIn IDs, never a mapper that follows the Windows
            // default (see the header).
            if (api == nullptr || !soundCardHostApiListed(api->type)) { continue; }
            // ...and on ALSA only the HARDWARE inputs: "default", "pulse",
            // "sysdefault", "dsnoop" and the rest of ALSA's configured PCMs
            // follow the system default or share another device, which is
            // "another input opened in its place" by another name (see
            // AlsaHwName in the header).
            if (api->type == paALSA && !alsaHardwareName(info->name)) { continue; }
            SoundCardDevice d;
            d.index = static_cast<int>(i);
            d.name = info->name != nullptr ? info->name : "";
            d.hostApi = (api != nullptr && api->name != nullptr) ? api->name : "";
            d.maxInputChannels = info->maxInputChannels;
            d.defaultRateHz = info->defaultSampleRate;
            d.isDefault = (i == def);
            if (api->defaultInputDevice == i) { apiDefaults.push_back(out.size()); }
            PaStreamParameters p{};
            p.device = i;
            p.channelCount = std::min(2, info->maxInputChannels);
            p.sampleFormat = paFloat32;
            p.suggestedLatency = info->defaultHighInputLatency;
            p.hostApiSpecificStreamInfo = nullptr;
#ifdef _WIN32
            const bool wasapi = (api != nullptr && api->type == paWASAPI);
            PaWasapiStreamInfo excl{};
            excl.size = sizeof(PaWasapiStreamInfo);
            excl.hostApiType = paWASAPI;
            excl.version = 1;
            excl.flags = paWinWasapiExclusive;
#endif
            for (const double hz : kCandidateRatesHz) {
                p.hostApiSpecificStreamInfo = nullptr;
                if (api_.isFormatSupported(&p, nullptr, hz) == paFormatIsSupported) {
                    d.rates.push_back({hz, false});
                    continue;
                }
#ifdef _WIN32
                // WASAPI SHARED mode takes only the rate the Windows mixer is
                // set to; EXCLUSIVE mode takes what the hardware itself runs
                // at. A VLF card set to 48 kHz in the Sound control panel can
                // still be opened at 192 kHz this way, without anybody
                // changing a Windows setting.
                if (wasapi) {
                    p.hostApiSpecificStreamInfo = &excl;
                    if (api_.isFormatSupported(&p, nullptr, hz) == paFormatIsSupported) {
                        d.rates.push_back({hz, true});
                    }
                }
#endif
            }
            out.push_back(std::move(d));
        }
        // THE DEFAULT INPUT when PortAudio's own is not offered. On Windows
        // Pa_GetDefaultInputDevice() is MME's - the Sound Mapper - which is
        // hidden above; the default row is then the first listed host API's
        // own default input (WASAPI's is the Windows default capture device).
        const bool haveDefault =
            std::any_of(out.begin(), out.end(), [](const SoundCardDevice& x) { return x.isDefault; });
        if (!haveDefault && !apiDefaults.empty()) { out[apiDefaults.front()].isDefault = true; }
        return out;
    }

    bool open(const SoundCardDevice& dev, int channels, double rateHz, bool exclusive,
              PushFn push, void* user, std::string& error) override {
        {
            // ONE STREAM PER BACKEND, and a backend is never reopened after a
            // close (the source makes a new one): a close runs on a thread of
            // its own and may still be using this object.
            std::lock_guard<std::mutex> s(streamMutex_);
            if (stream_ != nullptr) {
                error = "this input already has a stream open";
                return false;
            }
        }
        if (!initialised()) {
            error = "the audio system (PortAudio) did not start";
            return false;
        }
        std::lock_guard<std::mutex> lk(listOpenMutex());
        const PaDeviceInfo* info = (dev.index >= 0 && dev.index < api_.getDeviceCount())
                                       ? api_.getDeviceInfo(dev.index)
                                       : nullptr;
        const PaHostApiInfo* hostApi = info != nullptr ? api_.getHostApiInfo(info->hostApi) : nullptr;
        // The index came from THIS process's list, which PortAudio does not
        // renumber while it is initialised; the name and host API checks are
        // the belt to that brace, so a stale index - or an entry from a host
        // API this source does not offer - can never open a different card.
        if (info == nullptr || info->name == nullptr || dev.name != info->name || hostApi == nullptr ||
            hostApi->name == nullptr || dev.hostApi != hostApi->name ||
            !soundCardHostApiListed(hostApi->type) ||
            (hostApi->type == paALSA && !alsaHardwareName(info->name)) || info->maxInputChannels < channels) {
            error = "\"" + dev.name + "\" is no longer in the list of inputs";
            return false;
        }
        PaStreamParameters p{};
        p.device = dev.index;
        p.channelCount = channels;
        p.sampleFormat = paFloat32;
        // A receiver cares about gaps, not about latency: the high default
        // gives the host API the most room before it drops a buffer.
        p.suggestedLatency = info->defaultHighInputLatency;
        p.hostApiSpecificStreamInfo = nullptr;
#ifdef _WIN32
        PaWasapiStreamInfo excl{};
        if (exclusive && hostApi->type == paWASAPI) {
            excl.size = sizeof(PaWasapiStreamInfo);
            excl.hostApiType = paWASAPI;
            excl.version = 1;
            excl.flags = paWinWasapiExclusive;
            p.hostApiSpecificStreamInfo = &excl;
        }
#else
        (void)exclusive;
#endif
        cb_.push = push;
        cb_.user = user;
        PaStream* s = nullptr;
        PaError e = paNoError;
        {
            // PortAudio's list of open streams has no lock of its own (see
            // sink/pa_init.hpp, "THE STREAM-LIST LOCK").
            sink::PaStreamListGuard g;
            e = api_.openStream(&s, &p, nullptr, rateHz, paFramesPerBufferUnspecified, paNoFlag,
                                &PortAudioSoundCardBackend::callback, &cb_);
        }
        if (e != paNoError) {
            error = std::string("the card refused to open: ") + api_.getErrorText(e);
            return false;
        }
        e = api_.startStream(s);
        if (e != paNoError) {
            {
                sink::PaStreamListGuard g;
                api_.closeStream(s);
            }
            error = std::string("the card would not start: ") + api_.getErrorText(e);
            return false;
        }
        std::lock_guard<std::mutex> sl(streamMutex_);
        stream_ = s;
        return true;
    }

    void close() override {
        // THE HANDLE IS TAKEN OUT UNDER THIS BACKEND'S OWN LOCK, AND CLOSED
        // OUTSIDE ANY LOCK. From here the caller - the source's closer thread
        // - owns the stream exclusively, so a close that never returns holds
        // nothing an open, a list, alive() or another backend needs.
        PaStream* s = nullptr;
        {
            std::lock_guard<std::mutex> lk(streamMutex_);
            s = stream_;
            stream_ = nullptr;
        }
        if (s == nullptr) { return; }
        // Abort, not Stop: nothing in an input queue is worth waiting for.
        // The abort touches no list, so it takes no lock.
        api_.abortStream(s);
        // Pa_CloseStream takes the stream off PortAudio's unlocked list as
        // its first act. If the host API's close then hangs, this thread
        // keeps the guard - and everybody else stops waiting for it after
        // kStreamListWaitMs, by when this close is past the list
        // (sink/pa_init.hpp).
        sink::PaStreamListGuard g;
        api_.closeStream(s);
    }

    bool alive() override {
        std::unique_lock<std::mutex> lk(streamMutex_, std::try_to_lock);
        if (!lk.owns_lock()) { return true; }  // busy is not dead - see the header
        if (stream_ == nullptr) { return false; }
        // 1 = the callback is being called. 0 or an error code is a stream
        // that has stopped, which is what a vanished device produces.
        return api_.isStreamActive(stream_) == 1;
    }

private:
    struct Cb {
        PushFn push = nullptr;
        void* user = nullptr;
    };

    static int callback(const void* input, void* /*output*/, unsigned long frames,
                        const PaStreamCallbackTimeInfo* /*time*/, PaStreamCallbackFlags /*flags*/,
                        void* userData) {
        const Cb* cb = static_cast<const Cb*>(userData);
        // A null input is a period the device produced nothing for; there is
        // nothing to push, and it is not an error.
        if (cb != nullptr && cb->push != nullptr && input != nullptr) {
            cb->push(cb->user, static_cast<const float*>(input), static_cast<std::size_t>(frames));
        }
        return paContinue;
    }

    // PortAudio's initialisation for this backend, taken once, by the first
    // list or open (a worker), never by the constructor.
    bool initialised() {
        std::call_once(initOnce_, [this] { paOk_ = (api_.initialize() == paNoError); });
        return paOk_;
    }

    const SoundCardPaApi& api_;
    std::once_flag initOnce_;
    bool paOk_ = false;
    // Guards stream_ only, and only for as long as it takes to read or swap
    // it (and for alive()'s one query) - never across an open or a close.
    std::mutex streamMutex_;
    PaStream* stream_ = nullptr;
    Cb cb_;
};

}  // namespace

bool soundCardHostApiListed(PaHostApiTypeId type) {
    switch (type) {
        case paMME:          // renumbered waveIn IDs, resampled rates, the mapper
        case paDirectSound:  // "Primary Sound Capture Driver" follows the default
        case paWDMKS:        // not in this build; not offered if it ever is
        case paASIO:         // not in this build; not offered if it ever is
            return false;
        default:
            return true;
    }
}

const SoundCardPaApi& realSoundCardPaApi() { return kRealPaApi; }

std::shared_ptr<SoundCardBackend> makePortAudioSoundCardBackend(const SoundCardPaApi& api) {
    return std::make_shared<PortAudioSoundCardBackend>(api);
}

std::shared_ptr<SoundCardBackend> makePortAudioSoundCardBackend() {
    return makePortAudioSoundCardBackend(kRealPaApi);
}

#endif  // CASCADE_ANDROID

// --- the source ------------------------------------------------------------------

namespace {

std::atomic<std::uint64_t> gAbandonedCloses{0};
std::atomic<bool> gCloseWaitEnabled{true};

// Every capture block that exists, by address (captureAlive). Touched only
// when a block is made or destroyed - an open or a close - never by the
// realtime callback. Allocated once and never destroyed: a closer thread can
// still be destroying its block while the process runs its static
// destructors.
std::mutex& captureRegistryMutex() {
    static std::mutex* const m = new std::mutex;
    return *m;
}
std::vector<const void*>& captureRegistry() {
    static std::vector<const void*>* const v = new std::vector<const void*>;
    return *v;
}

}  // namespace

// How a closer thread tells whoever is waiting that it has finished.
struct SoundCardCloseTicket {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::string label;  // "device (host API)", for the log line if it is left behind
};

SoundCardSource::Capture::Capture(std::size_t floats) : ring(floats) {
    std::lock_guard<std::mutex> lk(captureRegistryMutex());
    captureRegistry().push_back(this);
}

SoundCardSource::Capture::~Capture() {
    std::lock_guard<std::mutex> lk(captureRegistryMutex());
    auto& r = captureRegistry();
    r.erase(std::remove(r.begin(), r.end(), static_cast<const void*>(this)), r.end());
}

bool SoundCardSource::captureAlive(const void* user) {
    std::lock_guard<std::mutex> lk(captureRegistryMutex());
    const auto& r = captureRegistry();
    return std::find(r.begin(), r.end(), user) != r.end();
}

namespace {

// The batch collecting this thread's closes, if one is open (CloseBatch).
thread_local SoundCardSource::CloseBatch* tBatch = nullptr;

// Waits for every ticket against ONE deadline; a close not finished by then
// is left on its thread, counted and logged.
void waitForCloses(const std::vector<std::shared_ptr<SoundCardCloseTicket>>& tickets,
                   std::chrono::steady_clock::time_point deadline) {
    for (const auto& t : tickets) {
        std::unique_lock<std::mutex> lk(t->m);
        if (t->cv.wait_until(lk, deadline, [&t] { return t->done; })) { continue; }
        gAbandonedCloses.fetch_add(1, std::memory_order_relaxed);
        cascade::core::diagWarnf(
            "source: the sound card %s did not close within %lld ms; the close is left to finish on a "
            "thread of its own",
            t->label.c_str(), static_cast<long long>(SoundCardSource::kCloseWaitMs.count()));
    }
}

}  // namespace

SoundCardSource::CloseBatch::CloseBatch() : outer_(tBatch) { tBatch = this; }

SoundCardSource::CloseBatch::~CloseBatch() {
    tBatch = outer_;
    if (outer_ != nullptr) {
        // Nested: the outer batch waits for these along with its own.
        outer_->tickets_.insert(outer_->tickets_.end(), tickets_.begin(), tickets_.end());
        return;
    }
    // Nothing is waited for once the application's teardown has begun, even
    // for closes collected before it did.
    if (tickets_.empty() || !gCloseWaitEnabled.load(std::memory_order_relaxed)) { return; }
    waitForCloses(tickets_, std::chrono::steady_clock::now() + kCloseWaitMs);
}

// The constructor asks the factory for nothing: see BackendFactory.
SoundCardSource::SoundCardSource(BackendFactory factory)
    : factory_(factory ? std::move(factory)
                       : BackendFactory([] { return makePortAudioSoundCardBackend(); })) {}

SoundCardSource::~SoundCardSource() { closeDevice(); }

std::uint64_t SoundCardSource::abandonedCloses() {
    return gAbandonedCloses.load(std::memory_order_relaxed);
}

void SoundCardSource::setCloseWaitEnabled(bool on) {
    gCloseWaitEnabled.store(on, std::memory_order_relaxed);
}

SoundCardBackend& SoundCardSource::backend() {
    if (!backend_) { backend_ = factory_(); }
    return *backend_;
}

std::uint64_t SoundCardSource::overruns() const {
    return cap_ ? cap_->overruns.load(std::memory_order_relaxed) : 0;
}

void SoundCardSource::pushFrames(void* user, const float* interleaved, std::size_t frames) {
    auto* c = static_cast<Capture*>(user);
    if (c == nullptr || interleaved == nullptr || frames == 0) { return; }
    const int ch = c->channels.load(std::memory_order_relaxed);
    if (ch < 1) { return; }
    const std::size_t fit = c->ring.freeSpace() / static_cast<std::size_t>(ch);
    const std::size_t take = std::min(frames, fit);
    // WHOLE FRAMES, in one write, so the consumer can never see half of one:
    // the ring publishes its write index once, after the copy.
    if (take != 0) { c->ring.write(interleaved, take * static_cast<std::size_t>(ch)); }
    if (take < frames) { c->overruns.fetch_add(1, std::memory_order_relaxed); }
}

bool SoundCardSource::openWith(const SoundCardSettings& s) {
    closeDevice();
    const std::vector<SoundCardDevice> list = backend().listDevices();
    return openLocked(s, list);
}

bool SoundCardSource::openWith(const SoundCardSettings& s, const std::vector<SoundCardDevice>& list) {
    closeDevice();
    return openLocked(s, list);
}

bool SoundCardSource::openLocked(const SoundCardSettings& s, const std::vector<SoundCardDevice>& list) {
    error_.clear();
    const SoundCardMatch match = matchSoundCard(list, s.device, s.hostApi, s.pickedFromList);
    const int at = match.at;
    if (at < 0 && !match.candidates.empty()) {
        // TWO IDENTICAL CARDS, and a saved name that cannot say which: ALSA
        // numbers cards in the order it finds them, which can change at every
        // boot. Neither is opened; the user picks one from the list, which
        // names it exactly for this session.
        std::string both;
        for (std::size_t i = 0; i < match.candidates.size(); ++i) {
            if (i != 0) { both += i + 1 == match.candidates.size() ? " and " : ", "; }
            both += "\"" + list[static_cast<std::size_t>(match.candidates[i])].name + "\"";
        }
        error_ = "\"" + s.device + "\" (" + s.hostApi + ") could be any of " + both +
                 ": identical cards, which ALSA may number differently at each start, so neither was "
                 "opened. Choose one in the list";
        return false;
    }
    if (at < 0) {
        if (s.device.empty()) {
            error_ = "no sound card input is present";
        } else {
            // NEVER ANOTHER CARD IN ITS PLACE - see the file header. A card
            // saved from an MME entry lands here too: MME is not listed.
            error_ = "\"" + s.device + "\" (" + s.hostApi +
                     ") is not connected; no other input was opened in its place. Sound cards are "
                     "listed when FoxSDR starts: plug it in and restart FoxSDR";
        }
        return false;
    }
    const SoundCardDevice& dev = list[static_cast<std::size_t>(at)];
    if (s.format == SoundCardFormat::IqStereo && dev.maxInputChannels < 2) {
        error_ = "\"" + dev.name + "\" has one input channel, and I/Q needs two";
        return false;
    }
    if (s.format == SoundCardFormat::RealMono && s.channel == 1 && dev.maxInputChannels < 2) {
        error_ = "\"" + dev.name + "\" has no right channel";
        return false;
    }
    const std::vector<SoundCardRate> rates = soundCardRatesFor(dev, s.format);
    if (rates.empty()) {
        error_ = "\"" + dev.name + "\" offers no sample rate this format can use";
        return false;
    }
    // The nearest rate the card offers; a tie goes to the higher one (more
    // bandwidth is never the surprising choice for a receiver).
    SoundCardRate pick = rates.front();
    for (const SoundCardRate& r : rates) {
        if (std::fabs(r.hz - s.cardRateHz) <= std::fabs(pick.hz - s.cardRateHz)) { pick = r; }
    }
    const int channels = std::min(2, dev.maxInputChannels);
    auto cap = std::make_shared<Capture>(kRingFloats);
    cap->channels.store(channels, std::memory_order_relaxed);
    std::string err;
    if (!backend().open(dev, channels, pick.hz, pick.exclusive, &SoundCardSource::pushFrames,
                        cap.get(), err)) {
        error_ = "\"" + dev.name + "\": " + err;
        return false;
    }
    closedBecause_.clear();
    cap_ = std::move(cap);
    device_ = dev;
    channels_ = channels;
    settings_ = s;
    settings_.device = dev.name;
    settings_.hostApi = dev.hostApi;
    settings_.cardRateHz = pick.hz;
    settings_.channel = (s.channel == 1) ? 1 : 0;
    label_ = soundCardDeviceLabel(dev);
    name_ = "Sound card: " + dev.name;
    realToIq_.reset();
    faulted_.store(false, std::memory_order_release);
    faultMsg_.clear();
    open_ = true;
    lastDataAt_ = std::chrono::steady_clock::now();
    lastAlivePollAt_ = lastDataAt_;
    if (pick.hz != s.cardRateHz) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "the card does not offer %.0f Hz; it is running at %.0f Hz",
                      s.cardRateHz, pick.hz);
        error_ = buf;
    }
    return true;
}

bool SoundCardSource::open(const std::string& args) {
    SoundCardSettings s;
    if (!parseSoundCardArgs(args, s)) {
        error_ = "cannot read the sound card settings \"" + args + "\"";
        return false;
    }
    return openWith(s);
}

void SoundCardSource::closeDevice() {
    running_.store(false, std::memory_order_relaxed);
    stopRequested_.store(true, std::memory_order_relaxed);
    if (!open_) { return; }
    open_ = false;
    // A CARD THAT IS ALREADY GONE is not waited for: one whose failure read()
    // latched, and one the host API says has stopped - a card pulled out while
    // the receiver was stopped, which no read() was there to notice. alive()
    // never blocks (a backend that cannot answer at once says "alive", and is
    // waited for, as before).
    const bool dead = faulted() || (backend_ && !backend_->alive());
    // EVERY CLOSE RUNS ON A THREAD OF ITS OWN (see "CLOSING" in the header).
    // A host API asked to close a stream can take as long as it likes - for
    // ever, on a card that has gone - and this runs on the GUI thread (a
    // source swap, a patch radio, the exit). The thread takes the backend and
    // the capture block with it: the stream is then its alone, a last
    // callback during the close still has somewhere to write, and PortAudio's
    // termination for that backend happens there too, after the close.
    // THIS SOURCE NEVER TOUCHES THAT BACKEND AGAIN - the next open asks the
    // factory for a new one - so a close still running can never shut a
    // stream opened after it.
    auto done = std::make_shared<SoundCardCloseTicket>();
    done->label = settings_.device + " (" + settings_.hostApi + ")";
    std::thread([b = std::move(backend_), c = std::move(cap_), done]() mutable {
        b->close();
        b.reset();
        c.reset();
        {
            std::lock_guard<std::mutex> lk(done->m);
            done->done = true;
        }
        done->cv.notify_all();
    }).detach();
    backend_.reset();
    cap_.reset();
    // A card that has already failed is not waited for at all, and nothing
    // is once the application's teardown has begun (setCloseWaitEnabled). A
    // healthy one otherwise is, for a bounded time, so that reopening the
    // same card straight away (a new rate; WASAPI exclusive mode; the patch
    // page taking the receiver's card) finds it free.
    if (dead || !gCloseWaitEnabled.load(std::memory_order_relaxed)) { return; }
    // Under a CloseBatch the wait is the batch's, shared with every other
    // close it collects.
    if (tBatch != nullptr) {
        tBatch->tickets_.push_back(std::move(done));
        return;
    }
    waitForCloses({done}, std::chrono::steady_clock::now() + kCloseWaitMs);
}

bool SoundCardSource::setGainDb(const std::string& /*name*/, double /*db*/) {
    error_ = "a sound card has no gain here; set its level in the system's sound settings";
    return false;
}

std::vector<std::string> SoundCardSource::antennas() const { return {"Line in"}; }

bool SoundCardSource::setAntenna(const std::string& name) { return name == "Line in"; }

std::string SoundCardSource::antenna() const { return "Line in"; }

std::vector<double> SoundCardSource::supportedSampleRatesHz() const {
    std::vector<double> out;
    for (const SoundCardRate& r : soundCardRatesFor(device_, settings_.format)) {
        SoundCardSettings s = settings_;
        s.cardRateHz = r.hz;
        out.push_back(soundCardIqRateHz(s));
    }
    return out;
}

bool SoundCardSource::frequencyRangeHz(double& loHz, double& hiHz) const {
    const double half = soundCardIqRateHz(settings_) / 2.0;
    const double c = soundCardCentreHz(settings_);
    loHz = c - half;
    hiHz = c + half;
    return true;
}

std::string SoundCardSource::faultedWhile() const { return faulted() ? "streaming" : ""; }

bool SoundCardSource::start() {
    if (!open_) {
        // A rate change that could not put the card back says why it is shut.
        error_ = closedBecause_.empty() ? std::string("no sound card is open") : closedBecause_;
        return false;
    }
    if (faulted()) { return false; }
    // What the card captured while the pipeline was stopped is stale; the
    // receiver resumes from now. This is the ring's consumer side, and the
    // pipeline calls start() before the source thread exists.
    float scratch[1024];
    while (cap_->ring.read(scratch, sizeof(scratch) / sizeof(scratch[0])) != 0) {}
    realToIq_.reset();
    lastDataAt_ = std::chrono::steady_clock::now();
    lastAlivePollAt_ = lastDataAt_;
    stopRequested_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    return true;
}

void SoundCardSource::stop() {
    stopRequested_.store(true, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
}

double SoundCardSource::sampleRateHz() const { return soundCardIqRateHz(settings_); }

bool SoundCardSource::setSampleRateHz(double hz) {
    if (!(hz > 0.0)) { return false; }
    SoundCardSettings s = settings_;
    s.cardRateHz = (s.format == SoundCardFormat::RealMono) ? hz * 2.0 : hz;
    if (!open_) {
        settings_ = s;
        return true;
    }
    if (s.cardRateHz == settings_.cardRateHz) { return true; }
    // A new rate is a new stream. The card is re-found in the list it was
    // opened from, so a reopen never enumerates.
    const std::vector<SoundCardDevice> list{device_};
    const SoundCardSettings before = settings_;
    const bool wasRunning = running();
    closeDevice();
    if (openLocked(s, list)) {
        if (wasRunning) { start(); }
        return true;
    }
    // THE CARD REFUSED THE NEW RATE. Put it back as it was - the rate it had,
    // running again if it was - so the caller's "the radio refused this rate
    // and runs at its own" is true.
    const std::string refused = error_;
    char buf[96];
    if (openLocked(before, list)) {
        if (wasRunning) { start(); }
        std::snprintf(buf, sizeof(buf), "; it is still running at %.0f Hz", settings_.cardRateHz);
        error_ = refused + buf;
        return false;
    }
    // ...and when even that fails the card is CLOSED, and says so: isOpen()
    // is false and start() refuses with both reasons.
    std::snprintf(buf, sizeof(buf), "; it could not be reopened at %.0f Hz either: ", before.cardRateHz);
    closedBecause_ = refused + buf + error_;
    error_ = closedBecause_;
    return false;
}

double SoundCardSource::centerFrequencyHz() const { return soundCardCentreHz(settings_); }

bool SoundCardSource::setCenterFrequencyHz(double hz) {
    if (!std::isfinite(hz)) { return false; }
    if (settings_.format == SoundCardFormat::IqStereo) {
        settings_.iqCentreHz = hz;
        return true;
    }
    const double fixed = soundCardCentreHz(settings_);
    if (std::fabs(hz - fixed) < 0.5) { return true; }
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "a sound card in real mode receives 0 - %.1f kHz and has no tuner to move",
                  settings_.cardRateHz / 2000.0);
    error_ = buf;
    return false;
}

void SoundCardSource::latchFault(const std::string& why) {
    // Written once, BEFORE the release store; see lastError().
    faultMsg_ = why;
    faulted_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_relaxed);
}

std::size_t SoundCardSource::read(std::complex<float>* dst, std::size_t n) {
    if (!open_ || dst == nullptr || n == 0 || faulted()) { return 0; }
    const std::size_t ch = static_cast<std::size_t>(channels_);
    auto& ring = cap_->ring;
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t availFrames = 0;
    for (;;) {
        if (stopRequested_.load(std::memory_order_relaxed)) { return 0; }
        availFrames = ring.size() / ch;
        if (availFrames != 0) { break; }
        const auto t = std::chrono::steady_clock::now();
        if (t - lastDataAt_ >= kStallMs) {
            // THE SAME ADVICE AS EVERY OTHER MESSAGE ABOUT A CARD: PortAudio's
            // list of inputs is fixed for the session (see the header).
            latchFault("the sound card \"" + settings_.device + "\" (" + settings_.hostApi +
                       ") has delivered nothing for 2 s - unplugged, or taken by another "
                       "program? Sound cards are listed when FoxSDR starts: after plugging a card "
                       "back in, restart FoxSDR");
            return 0;
        }
        if (t - lastAlivePollAt_ >= kAlivePollMs) {
            lastAlivePollAt_ = t;
            if (!backend_->alive()) {
                latchFault("the input stream of \"" + settings_.device + "\" (" +
                           settings_.hostApi +
                           ") stopped - the card was unplugged or taken away. Sound cards are "
                           "listed when FoxSDR starts: after plugging it back in, restart FoxSDR");
                return 0;
            }
        }
        if (t - t0 >= kReadWaitMs) { return 0; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    lastDataAt_ = std::chrono::steady_clock::now();

    const bool real = settings_.format == SoundCardFormat::RealMono;
    // REAL: two card samples per complex output, and 2n samples give exactly
    // n outputs (see RealToIq::process), so dst can never be overrun.
    const std::size_t want = real ? 2 * n : n;
    const std::size_t frames = std::min(availFrames, want);
    interleaved_.resize(frames * ch);
    const std::size_t got = ring.read(interleaved_.data(), frames * ch) / ch;
    if (real) {
        const std::size_t sel = std::min<std::size_t>(static_cast<std::size_t>(settings_.channel), ch - 1);
        mono_.resize(got);
        for (std::size_t i = 0; i < got; ++i) { mono_[i] = interleaved_[i * ch + sel]; }
        return realToIq_.process(mono_.data(), got, dst);
    }
    const std::size_t iAt = settings_.swapIq ? 1 : 0;
    const std::size_t qAt = settings_.swapIq ? 0 : 1;
    for (std::size_t i = 0; i < got; ++i) {
        dst[i] = {interleaved_[i * ch + iAt], interleaved_[i * ch + qAt]};
    }
    return got;
}

const char* SoundCardSource::lastError() const {
    if (faulted_.load(std::memory_order_acquire)) { return faultMsg_.c_str(); }
    return error_.c_str();
}

SoundCardOpenOutcome openSoundCardOrRestore(const SoundCardSettings& want,
                                            const std::vector<SoundCardDevice>& list,
                                            const SoundCardSettings* previous,
                                            const SoundCardSource::BackendFactory& factory) {
    SoundCardOpenOutcome out;
    auto src = std::make_unique<SoundCardSource>(factory);
    if (src->openWith(want, list)) {
        out.note = src->lastError();  // a coerced rate, or nothing
        out.src = std::move(src);
        return out;
    }
    out.refused = src->lastError();
    if (previous == nullptr) { return out; }
    // THE CARD AS IT WAS. It was running a moment ago and was released only
    // so that this open could have it; putting it back is the difference
    // between "the new settings were refused" and a receiver that silently
    // stopped.
    auto back = std::make_unique<SoundCardSource>(factory);
    if (back->openWith(*previous, list)) {
        out.restoredPrevious = true;
        out.src = std::move(back);
        return out;
    }
    out.previousRefused = back->lastError();
    return out;
}

}  // namespace cascade::source
