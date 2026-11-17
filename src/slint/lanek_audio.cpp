// lanek_audio.cpp - see lanek_audio.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanek_audio.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__linux__)
#include <alsa/asoundlib.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <sapi.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace foxsdr::slintlanek {

namespace acc = cascade::core::access;

#if defined(_WIN32)
struct ToneOut::Impl {
    static constexpr int kRate = 22050;
    static constexpr int kBlockFrames = kRate / 20;   // 50 ms
    static constexpr int kBlocks = 4;
    HWAVEOUT dev = nullptr;
    std::array<WAVEHDR, kBlocks> hdr{};
    std::array<std::vector<short>, kBlocks> pcm;
    acc::ToneRenderer renderer{static_cast<float>(kRate)};
    std::vector<float> scratch;
    std::string error;
    Failure failure;
    bool open = false;
};

ToneOut::ToneOut() : p_(std::make_unique<Impl>()) {}
ToneOut::~ToneOut() { stop(); }

bool ToneOut::start() {
    Impl& I = *p_;
    if (I.open) { return true; }
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = Impl::kRate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = static_cast<WORD>(f.nChannels * f.wBitsPerSample / 8);
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    if (const MMRESULT opened = waveOutOpen(&I.dev, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL); opened != MMSYSERR_NOERROR) {
        I.error = "no sound output could be opened";
        I.dev = nullptr;
        I.failure = Failure{Api::Mme, opened == MMSYSERR_ALLOCATED ? Reason::Busy
                                      : (opened == WAVERR_BADFORMAT ? Reason::Format
                                         : (opened == MMSYSERR_NODRIVER || opened == MMSYSERR_BADDEVICEID ? Reason::NoDevice : Reason::Other))};
        return false;
    }
    I.scratch.assign(static_cast<std::size_t>(Impl::kBlockFrames) * 2, 0.0f);
    for (int i = 0; i < Impl::kBlocks; ++i) {
        I.pcm[static_cast<std::size_t>(i)].assign(static_cast<std::size_t>(Impl::kBlockFrames) * 2, 0);
        WAVEHDR& h = I.hdr[static_cast<std::size_t>(i)];
        h = WAVEHDR{};
        h.lpData = reinterpret_cast<LPSTR>(I.pcm[static_cast<std::size_t>(i)].data());
        h.dwBufferLength = static_cast<DWORD>(I.pcm[static_cast<std::size_t>(i)].size() * sizeof(short));
        waveOutPrepareHeader(I.dev, &h, sizeof(h));
        h.dwFlags |= WHDR_DONE;   // free: the first feed() fills it
    }
    I.open = true;
    return true;
}

void ToneOut::stop() {
    Impl& I = *p_;
    if (!I.open) { return; }
    waveOutReset(I.dev);
    for (auto& h : I.hdr) { waveOutUnprepareHeader(I.dev, &h, sizeof(h)); }
    waveOutClose(I.dev);
    I.dev = nullptr;
    I.open = false;
}

bool ToneOut::running() const { return p_->open; }
const std::string& ToneOut::lastError() const { return p_->error; }
const ToneOut::Failure& ToneOut::lastFailure() const { return p_->failure; }

void ToneOut::feed(const acc::ToneParams& params) {
    Impl& I = *p_;
    if (!I.open) { return; }
    for (auto& h : I.hdr) {
        if ((h.dwFlags & WHDR_DONE) == 0) { continue; }
        I.renderer.render(params, static_cast<std::size_t>(Impl::kBlockFrames), I.scratch.data());
        short* out = reinterpret_cast<short*>(h.lpData);
        for (std::size_t i = 0; i < I.scratch.size(); ++i) {
            float v = I.scratch[i];
            v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
            out[i] = static_cast<short>(v * 32000.0f);
        }
        h.dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
        waveOutWrite(I.dev, &h, sizeof(h));
    }
}

namespace {
class SapiDeviceSpeaker : public acc::Speaker {
public:
    SapiDeviceSpeaker() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        inited_ = SUCCEEDED(hr);   // S_FALSE: already done on this thread; RPC_E_CHANGED_MODE: another mode, still usable
        if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&voice_)))) { voice_ = nullptr; }
    }
    ~SapiDeviceSpeaker() override {
        if (voice_ != nullptr) { voice_->Release(); }
        if (inited_) { CoUninitialize(); }
    }
    bool ok() const { return voice_ != nullptr; }
    bool speak(const std::string& utf8, bool interrupt) override {
        if (voice_ == nullptr || utf8.empty()) { return false; }
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (n <= 0) { return false; }
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
        const DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML | (interrupt ? SPF_PURGEBEFORESPEAK : 0);
        return SUCCEEDED(voice_->Speak(w.c_str(), flags, nullptr));
    }
    void stop() override { if (voice_ != nullptr) { voice_->Speak(L"", SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr); } }
    std::string name() const override { return "sapi-default-output"; }

private:
    ISpVoice* voice_ = nullptr;
    bool inited_ = false;
};
}  // namespace

std::unique_ptr<acc::Speaker> makeDeviceSpeaker() {
    auto s = std::make_unique<SapiDeviceSpeaker>();
    if (!s->ok()) { return nullptr; }
    return s;
}
#elif defined(__linux__)
// ALSA, as the engine's own sound output reaches it: the PCM named by FOXSDR_ALSA_DEVICE (a test names a file-writing one), else "default" (PulseAudio, PipeWire and dmix all answer to it).
// Non-blocking, no thread of its own, as the Windows path: each tick writes the blocks the device has room for.
struct ToneOut::Impl {
    static constexpr unsigned kRate = 22050;
    static constexpr snd_pcm_uframes_t kBlockFrames = kRate / 20;   // 50 ms
    static constexpr int kBlocks = 4;
    snd_pcm_t* pcm = nullptr;
    acc::ToneRenderer renderer{static_cast<float>(kRate)};
    std::vector<float> scratch;
    std::vector<short> pcmOut;
    std::string error;
    Failure failure;
    bool open = false;
};

ToneOut::ToneOut() : p_(std::make_unique<Impl>()) {}
ToneOut::~ToneOut() { stop(); }

bool ToneOut::start() {
    Impl& I = *p_;
    if (I.open) { return true; }
    const char* env = std::getenv("FOXSDR_ALSA_DEVICE");
    const std::string device = (env != nullptr && env[0] != '\0') ? env : "default";
    int rc = snd_pcm_open(&I.pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (rc < 0) {
        I.error = std::string("no sound output could be opened: ") + snd_strerror(rc);
        I.pcm = nullptr;
        I.failure = Failure{Api::Alsa, rc == -EBUSY ? Reason::Busy : ((rc == -ENOENT || rc == -ENODEV) ? Reason::NoDevice : Reason::Other)};
        return false;
    }
    rc = snd_pcm_set_params(I.pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, Impl::kRate, 1 /* let ALSA resample */, 200000 /* us of buffer */);
    if (rc < 0) {
        I.error = std::string("the sound output refused 22.05 kHz stereo: ") + snd_strerror(rc);
        I.failure = Failure{Api::Alsa, Reason::Format};
        snd_pcm_close(I.pcm);
        I.pcm = nullptr;
        return false;
    }
    I.scratch.assign(static_cast<std::size_t>(Impl::kBlockFrames) * 2, 0.0f);
    I.pcmOut.assign(static_cast<std::size_t>(Impl::kBlockFrames) * 2, 0);
    I.open = true;
    return true;
}

void ToneOut::stop() {
    Impl& I = *p_;
    if (!I.open) { return; }
    snd_pcm_drop(I.pcm);
    snd_pcm_close(I.pcm);
    I.pcm = nullptr;
    I.open = false;
}

bool ToneOut::running() const { return p_->open; }
const std::string& ToneOut::lastError() const { return p_->error; }
const ToneOut::Failure& ToneOut::lastFailure() const { return p_->failure; }

void ToneOut::feed(const acc::ToneParams& params) {
    Impl& I = *p_;
    if (!I.open) { return; }
    for (int block = 0; block < Impl::kBlocks; ++block) {
        const snd_pcm_sframes_t room = snd_pcm_avail_update(I.pcm);
        if (room == -EPIPE) { snd_pcm_prepare(I.pcm); continue; }   // an underrun: the tick was late; start the stream again
        if (room < static_cast<snd_pcm_sframes_t>(Impl::kBlockFrames)) { return; }   // the device is full (or the call failed): the next tick
        I.renderer.render(params, static_cast<std::size_t>(Impl::kBlockFrames), I.scratch.data());
        for (std::size_t i = 0; i < I.scratch.size(); ++i) {
            float v = I.scratch[i];
            v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
            I.pcmOut[i] = static_cast<short>(v * 32000.0f);
        }
        const snd_pcm_sframes_t wrote = snd_pcm_writei(I.pcm, I.pcmOut.data(), Impl::kBlockFrames);
        if (wrote == -EPIPE || wrote == -ESTRPIPE) { snd_pcm_prepare(I.pcm); }
        else if (wrote < 0) {
            I.error = std::string("the sound output failed: ") + snd_strerror(static_cast<int>(wrote));
            I.failure = Failure{Api::Alsa, Reason::Host};   // a stream that died while it played
            stop();   // the device is closed, so running() is false and the window tells the person (lanek_publish.cpp tick)
            return;
        }
    }
}

std::unique_ptr<acc::Speaker> makeDeviceSpeaker() { return nullptr; }   // no speech engine is part of this build; the tone above is the sound path
#else
struct ToneOut::Impl { std::string error = "no sound output on this platform"; Failure failure; };
ToneOut::ToneOut() : p_(std::make_unique<Impl>()) {}
ToneOut::~ToneOut() = default;
bool ToneOut::start() { return false; }
void ToneOut::stop() {}
bool ToneOut::running() const { return false; }
void ToneOut::feed(const acc::ToneParams&) {}
const std::string& ToneOut::lastError() const { return p_->error; }
const ToneOut::Failure& ToneOut::lastFailure() const { return p_->failure; }
std::unique_ptr<acc::Speaker> makeDeviceSpeaker() { return nullptr; }
#endif

}  // namespace foxsdr::slintlanek
