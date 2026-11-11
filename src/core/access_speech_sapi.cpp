// access_speech_sapi.cpp - see access_speech_sapi.hpp. Windows: SAPI 5.
// Every other platform gets an unavailable speaker.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/access_speech_sapi.hpp"

#include <cstdio>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <sapi.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "sapi.lib")
#endif

namespace cascade::core::access {

#ifdef _WIN32
namespace {

template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** out() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::wstring widen(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) { return std::wstring(); }
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const wchar_t* w) {
    if (!w) { return std::string(); }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) { return std::string(); }
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

struct SapiMemorySpeaker::Impl {
    bool comInit = false;
    Com<ISpVoice> voice;
    Com<IStream> mem;          // the HGLOBAL stream the audio lands in
    Com<ISpStream> spStream;   // SAPI's wrapper that gives it a format
    bool memoryOutput = false;
    PcmFormat fmt;
    std::string voiceDesc;
    std::string error;
    std::uint32_t timeout = 20000;
    std::vector<std::uint8_t> captured;   // bytes copied out of `mem` after each speak
    ULONGLONG copied = 0;

    void fail(const char* what, HRESULT hr) {
        char b[96];
        std::snprintf(b, sizeof b, "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
        error = b;
    }

    void init() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comInit = SUCCEEDED(co);           // S_OK or S_FALSE: balanced by CoUninitialize
        if (FAILED(co) && co != RPC_E_CHANGED_MODE) { fail("CoInitializeEx", co); return; }
        HRESULT hr = CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, __uuidof(ISpVoice),
                                      reinterpret_cast<void**>(voice.out()));
        if (FAILED(hr)) { fail("CoCreateInstance(SpVoice)", hr); return; }
        hr = CreateStreamOnHGlobal(nullptr, TRUE, mem.out());
        if (FAILED(hr)) { fail("CreateStreamOnHGlobal", hr); return; }
        hr = CoCreateInstance(__uuidof(SpStream), nullptr, CLSCTX_ALL, __uuidof(ISpStream),
                              reinterpret_cast<void**>(spStream.out()));
        if (FAILED(hr)) { fail("CoCreateInstance(SpStream)", hr); return; }
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 1;
        wf.nSamplesPerSec = 22050;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = static_cast<WORD>(wf.nChannels * wf.wBitsPerSample / 8);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
        hr = spStream->SetBaseStream(mem.p, SPDFID_WaveFormatEx, &wf);
        if (FAILED(hr)) { fail("ISpStream::SetBaseStream", hr); return; }
        // OUTPUT: the memory stream. fAllowFormatChanges = TRUE lets the voice
        // keep its native format if it differs; nothing plays either way.
        hr = voice->SetOutput(spStream.p, TRUE);
        if (FAILED(hr)) { fail("ISpVoice::SetOutput", hr); return; }
        // CONFIRM: read the output back and compare it with what was attached.
        Com<IUnknown> got;
        hr = voice->GetOutputStream(reinterpret_cast<ISpStreamFormat**>(got.out()));
        if (FAILED(hr) || !got) { fail("ISpVoice::GetOutputStream", FAILED(hr) ? hr : E_POINTER); return; }
        Com<IUnknown> a, b;
        got->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(a.out()));
        spStream->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(b.out()));
        if (!a || !b || a.p != b.p) { error = "voice output is not the memory stream"; return; }
        memoryOutput = true;
        fmt.sampleRate = wf.nSamplesPerSec; fmt.channels = wf.nChannels; fmt.bitsPerSample = wf.wBitsPerSample;
        // The voice's own description, for the record.
        Com<ISpObjectToken> tok;
        if (SUCCEEDED(voice->GetVoice(tok.out())) && tok) {
            LPWSTR desc = nullptr;
            if (SUCCEEDED(tok->GetStringValue(nullptr, &desc)) && desc) { voiceDesc = narrow(desc); CoTaskMemFree(desc); }
        }
    }

    // Copy any new bytes the engine wrote into `mem` since the last copy.
    void harvest() {
        HGLOBAL h = nullptr;
        if (FAILED(GetHGlobalFromStream(mem.p, &h)) || !h) { return; }
        STATSTG st = {};
        if (FAILED(mem->Stat(&st, STATFLAG_NONAME))) { return; }
        const ULONGLONG size = st.cbSize.QuadPart;
        if (size <= copied) { return; }
        const void* data = GlobalLock(h);
        if (!data) { return; }
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
        captured.insert(captured.end(), bytes + copied, bytes + size);
        copied = size;
        GlobalUnlock(h);
    }
};

SapiMemorySpeaker::SapiMemorySpeaker() : impl_(new Impl) { impl_->init(); }

SapiMemorySpeaker::~SapiMemorySpeaker() {
    if (impl_) {
        impl_->spStream.reset();
        impl_->voice.reset();
        impl_->mem.reset();
        if (impl_->comInit) { CoUninitialize(); }
    }
}

bool SapiMemorySpeaker::available() const { return impl_ && impl_->memoryOutput; }
bool SapiMemorySpeaker::outputIsMemory() const { return impl_ && impl_->memoryOutput; }

bool SapiMemorySpeaker::speak(const std::string& utf8, bool interrupt) {
    if (!impl_ || !impl_->memoryOutput) { return false; }       // NEVER reach the engine without the memory output
    const std::wstring w = widen(utf8);
    if (w.empty()) { impl_->error = "empty text"; return false; }
    DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML;
    if (interrupt) { flags |= SPF_PURGEBEFORESPEAK; }
    HRESULT hr = impl_->voice->Speak(w.c_str(), flags, nullptr);
    if (FAILED(hr)) { impl_->fail("ISpVoice::Speak", hr); return false; }
    hr = impl_->voice->WaitUntilDone(impl_->timeout);
    if (hr != S_OK) {                                           // S_FALSE = timed out
        impl_->voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
        impl_->error = "speech did not finish in time";
        impl_->harvest();
        return false;
    }
    impl_->harvest();
    return true;
}

void SapiMemorySpeaker::stop() {
    if (impl_ && impl_->memoryOutput) { impl_->voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr); }
}

std::vector<std::uint8_t> SapiMemorySpeaker::pcm() const { return impl_ ? impl_->captured : std::vector<std::uint8_t>(); }
void SapiMemorySpeaker::clearPcm() { if (impl_) { impl_->captured.clear(); } }
PcmFormat SapiMemorySpeaker::format() const { return impl_ ? impl_->fmt : PcmFormat(); }
std::string SapiMemorySpeaker::voiceName() const { return impl_ ? impl_->voiceDesc : std::string(); }
std::string SapiMemorySpeaker::lastError() const { return impl_ ? impl_->error : std::string(); }
void SapiMemorySpeaker::setTimeoutMs(std::uint32_t ms) { if (impl_) { impl_->timeout = ms; } }
std::uint32_t SapiMemorySpeaker::timeoutMs() const { return impl_ ? impl_->timeout : 0; }

#else  // not Windows: an unavailable speaker

struct SapiMemorySpeaker::Impl {};
SapiMemorySpeaker::SapiMemorySpeaker() : impl_(new Impl) {}
SapiMemorySpeaker::~SapiMemorySpeaker() = default;
bool SapiMemorySpeaker::available() const { return false; }
bool SapiMemorySpeaker::outputIsMemory() const { return false; }
bool SapiMemorySpeaker::speak(const std::string&, bool) { return false; }
void SapiMemorySpeaker::stop() {}
std::vector<std::uint8_t> SapiMemorySpeaker::pcm() const { return {}; }
void SapiMemorySpeaker::clearPcm() {}
PcmFormat SapiMemorySpeaker::format() const { return {}; }
std::string SapiMemorySpeaker::voiceName() const { return {}; }
std::string SapiMemorySpeaker::lastError() const { return "SAPI is Windows only"; }
void SapiMemorySpeaker::setTimeoutMs(std::uint32_t) {}
std::uint32_t SapiMemorySpeaker::timeoutMs() const { return 0; }

#endif

}  // namespace cascade::core::access
