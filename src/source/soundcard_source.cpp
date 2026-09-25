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
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

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

int findSoundCard(const std::vector<SoundCardDevice>& list, const std::string& name,
                  const std::string& hostApi) {
    if (name.empty()) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].isDefault) { return static_cast<int>(i); }
        }
        return -1;
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].name == name && list[i].hostApi == hostApi) { return static_cast<int>(i); }
    }
    return -1;
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

// ONE LOCK FOR EVERY PORTAUDIO CALL THE SOUND CARD MAKES. The list, the probes
// behind it, the opens and the closes can each come from a different thread -
// the Source section's worker, a patch node's worker, the pipeline's source
// thread asking alive(), a thread closing a dead card - and PortAudio is not
// documented as safe for concurrent calls into one host API. alive() only
// ever TRIES it, so the source thread never waits behind a slow probe.
std::mutex& paMutex() {
    static std::mutex m;
    return m;
}

class PortAudioSoundCardBackend final : public SoundCardBackend {
public:
    PortAudioSoundCardBackend() {
        std::lock_guard<std::mutex> lk(paMutex());
        paOk_ = (Pa_Initialize() == paNoError);
    }

    ~PortAudioSoundCardBackend() override {
        close();
        std::lock_guard<std::mutex> lk(paMutex());
        // Paired with OUR successful Pa_Initialize only; PortAudio refcounts.
        if (paOk_) { Pa_Terminate(); }
    }

    std::vector<SoundCardDevice> listDevices() override {
        std::vector<SoundCardDevice> out;
        std::lock_guard<std::mutex> lk(paMutex());
        if (!paOk_) { return out; }
        const PaDeviceIndex count = Pa_GetDeviceCount();
        const PaDeviceIndex def = Pa_GetDefaultInputDevice();
        for (PaDeviceIndex i = 0; i < count; ++i) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
            if (info == nullptr || info->maxInputChannels < 1) { continue; }
            const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
            SoundCardDevice d;
            d.index = static_cast<int>(i);
            d.name = info->name != nullptr ? info->name : "";
            d.hostApi = (api != nullptr && api->name != nullptr) ? api->name : "";
            d.maxInputChannels = info->maxInputChannels;
            d.defaultRateHz = info->defaultSampleRate;
            d.isDefault = (i == def);
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
                if (Pa_IsFormatSupported(&p, nullptr, hz) == paFormatIsSupported) {
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
                    if (Pa_IsFormatSupported(&p, nullptr, hz) == paFormatIsSupported) {
                        d.rates.push_back({hz, true});
                    }
                }
#endif
            }
            out.push_back(std::move(d));
        }
        return out;
    }

    bool open(const SoundCardDevice& dev, int channels, double rateHz, bool exclusive,
              PushFn push, void* user, std::string& error) override {
        close();
        std::lock_guard<std::mutex> lk(paMutex());
        if (!paOk_) {
            error = "the audio system (PortAudio) did not start";
            return false;
        }
        const PaDeviceInfo* info =
            (dev.index >= 0 && dev.index < Pa_GetDeviceCount()) ? Pa_GetDeviceInfo(dev.index) : nullptr;
        // The index came from THIS process's list, which PortAudio does not
        // renumber while it is initialised; the name check is the belt to
        // that brace, so a stale index can never open a different card.
        if (info == nullptr || info->name == nullptr || dev.name != info->name ||
            info->maxInputChannels < channels) {
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
        const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
        if (exclusive && api != nullptr && api->type == paWASAPI) {
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
        PaError e = Pa_OpenStream(&s, &p, nullptr, rateHz, paFramesPerBufferUnspecified, paNoFlag,
                                  &PortAudioSoundCardBackend::callback, &cb_);
        if (e != paNoError) {
            error = std::string("the card refused to open: ") + Pa_GetErrorText(e);
            return false;
        }
        e = Pa_StartStream(s);
        if (e != paNoError) {
            Pa_CloseStream(s);
            error = std::string("the card would not start: ") + Pa_GetErrorText(e);
            return false;
        }
        stream_ = s;
        return true;
    }

    void close() override {
        std::lock_guard<std::mutex> lk(paMutex());
        if (stream_ == nullptr) { return; }
        // Abort, not Stop: nothing in an input queue is worth waiting for.
        Pa_AbortStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }

    bool alive() override {
        std::unique_lock<std::mutex> lk(paMutex(), std::try_to_lock);
        if (!lk.owns_lock()) { return true; }  // busy is not dead - see the header
        if (stream_ == nullptr) { return false; }
        // 1 = the callback is being called. 0 or an error code is a stream
        // that has stopped, which is what a vanished device produces.
        return Pa_IsStreamActive(stream_) == 1;
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

    bool paOk_ = false;
    PaStream* stream_ = nullptr;
    Cb cb_;
};

}  // namespace

std::shared_ptr<SoundCardBackend> makePortAudioSoundCardBackend() {
    return std::make_shared<PortAudioSoundCardBackend>();
}

#endif  // CASCADE_ANDROID

// --- the source ------------------------------------------------------------------

SoundCardSource::SoundCardSource(BackendFactory factory)
    : factory_(factory ? std::move(factory) : BackendFactory(&makePortAudioSoundCardBackend)),
      backend_(factory_()) {}

SoundCardSource::~SoundCardSource() { closeDevice(); }

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
    const std::vector<SoundCardDevice> list = backend_->listDevices();
    return openLocked(s, list);
}

bool SoundCardSource::openWith(const SoundCardSettings& s, const std::vector<SoundCardDevice>& list) {
    closeDevice();
    return openLocked(s, list);
}

bool SoundCardSource::openLocked(const SoundCardSettings& s, const std::vector<SoundCardDevice>& list) {
    error_.clear();
    const int at = findSoundCard(list, s.device, s.hostApi);
    if (at < 0) {
        if (s.device.empty()) {
            error_ = "no sound card input is present";
        } else {
            // NEVER ANOTHER CARD IN ITS PLACE - see the file header.
            error_ = "\"" + s.device + "\" (" + s.hostApi +
                     ") is not connected; no other input was opened in its place";
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
    if (!backend_->open(dev, channels, pick.hz, pick.exclusive, &SoundCardSource::pushFrames,
                        cap.get(), err)) {
        error_ = "\"" + dev.name + "\": " + err;
        return false;
    }
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
    if (faulted()) {
        // A CARD THAT DIED IS CLOSED ON A THREAD OF ITS OWN. A host API asked
        // to close a stream on a device that has gone is the one call here
        // nobody can promise returns promptly, and this runs on the GUI
        // thread (a source swap destroys the old source there). The thread
        // owns the backend and the capture block, so a last callback during
        // the close still has somewhere to write.
        std::thread([b = backend_, c = cap_]() { b->close(); }).detach();
        // ...and this source never touches that backend again: a reopen on it
        // would race the close, which could then shut the NEW stream.
        backend_ = factory_();
    } else {
        backend_->close();
    }
    cap_.reset();
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
        error_ = "no sound card is open";
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
    const bool wasRunning = running();
    closeDevice();
    if (!openLocked(s, list)) { return false; }
    if (wasRunning) { start(); }
    return true;
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
            latchFault("the sound card \"" + settings_.device + "\" (" + settings_.hostApi +
                       ") has delivered nothing for 2 s - unplugged, or taken by another "
                       "program? Plug it back in and press Open");
            return 0;
        }
        if (t - lastAlivePollAt_ >= kAlivePollMs) {
            lastAlivePollAt_ = t;
            if (!backend_->alive()) {
                latchFault("the input stream of \"" + settings_.device + "\" (" +
                           settings_.hostApi +
                           ") stopped - the card was unplugged or taken away. Plug it back in "
                           "and press Open");
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

}  // namespace cascade::source
