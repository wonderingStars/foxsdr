// mp3_writer.cpp - see mp3_writer.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/mp3_writer.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace cascade::core {

namespace {

// A tiny owner for COM pointers, so every early return releases what it took.
template <typename T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p != nullptr) {
            p->Release();
            p = nullptr;
        }
    }
    T** put() {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
};

std::string hr(const std::string& what, HRESULT h) {
    char buf[96];
    std::snprintf(buf, sizeof buf, " failed (0x%08lX)", static_cast<unsigned long>(h));
    return what + buf;
}

// The encoder's own list of output types, and the one closest to what was
// asked for at this rate and channel count. Asking the encoder rather than
// building a type by hand is what keeps this working across Windows builds:
// the MP3 encoder accepts only the bit rates it lists, and the list is its own.
HRESULT pickOutputType(unsigned rate, unsigned channels, unsigned bitrateKbps,
                       IMFMediaType** out) {
    *out = nullptr;
    Com<IMFCollection> types;
    HRESULT h = MFTranscodeGetAudioOutputAvailableTypes(MFAudioFormat_MP3, MFT_ENUM_FLAG_ALL,
                                                        nullptr, types.put());
    if (FAILED(h)) { return h; }
    DWORD count = 0;
    types->GetElementCount(&count);
    const UINT32 wantBytes = bitrateKbps * 1000u / 8u;
    IMFMediaType* best = nullptr;
    UINT32 bestErr = 0xFFFFFFFFu;
    for (DWORD i = 0; i < count; ++i) {
        Com<IUnknown> unk;
        if (FAILED(types->GetElement(i, unk.put()))) { continue; }
        Com<IMFMediaType> t;
        if (FAILED(unk->QueryInterface(IID_PPV_ARGS(t.put())))) { continue; }
        UINT32 r = 0, c = 0, b = 0;
        t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
        t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c);
        t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &b);
        if (r != rate || c != channels) { continue; }
        const UINT32 err = b > wantBytes ? b - wantBytes : wantBytes - b;
        if (err < bestErr) {
            if (best != nullptr) { best->Release(); }
            best = t.p;
            best->AddRef();
            bestErr = err;
        }
    }
    if (best == nullptr) { return MF_E_INVALIDMEDIATYPE; }
    *out = best;
    return S_OK;
}

}  // namespace

struct Mp3Writer::Impl {
    IMFSinkWriter* writer = nullptr;
    DWORD stream = 0;
    unsigned rate = 0;
    unsigned channels = 0;
    bool comInit = false;
    bool mfStarted = false;
    bool failed = false;
};

bool Mp3Writer::available() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

Mp3Writer::~Mp3Writer() { close(); }

bool Mp3Writer::open(const std::string& path, unsigned sampleRateHz, unsigned channels,
                     unsigned bitrateKbps, std::string& error) {
    error.clear();
    if (impl_ != nullptr) {
        error = "mp3: already open";
        return false;
    }
    auto* im = new Impl();
    im->rate = sampleRateHz;
    im->channels = channels;
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    im->comInit = SUCCEEDED(ci);   // S_FALSE (already joined) also needs its CoUninitialize
    HRESULT h = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(h)) {
        error = hr("mp3: MFStartup", h);
        if (im->comInit) { CoUninitialize(); }
        delete im;
        return false;
    }
    im->mfStarted = true;

    const auto fail = [&](const std::string& why) {
        error = why;
        if (im->writer != nullptr) { im->writer->Release(); }
        MFShutdown();
        if (im->comInit) { CoUninitialize(); }
        delete im;
        return false;
    };

    Com<IMFMediaType> outType;
    h = pickOutputType(sampleRateHz, channels, bitrateKbps, outType.put());
    if (FAILED(h)) {
        return fail("mp3: Windows' MP3 encoder offers no output at " +
                    std::to_string(sampleRateHz) + " Hz, " + std::to_string(channels) +
                    " channel(s) (" + hr("MFTranscodeGetAudioOutputAvailableTypes", h) + ")");
    }

    const std::wstring wpath = std::filesystem::path(path).wstring();
    h = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, nullptr, &im->writer);
    if (FAILED(h)) { return fail(hr("mp3: cannot create \"" + path + "\" - MFCreateSinkWriterFromURL", h)); }
    h = im->writer->AddStream(outType.p, &im->stream);
    if (FAILED(h)) { return fail(hr("mp3: AddStream", h)); }

    Com<IMFMediaType> inType;
    h = MFCreateMediaType(inType.put());
    if (FAILED(h)) { return fail(hr("mp3: MFCreateMediaType", h)); }
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRateHz);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2u * channels);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 2u * channels * sampleRateHz);
    inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    h = im->writer->SetInputMediaType(im->stream, inType.p, nullptr);
    if (FAILED(h)) { return fail(hr("mp3: SetInputMediaType", h)); }
    h = im->writer->BeginWriting();
    if (FAILED(h)) { return fail(hr("mp3: BeginWriting", h)); }

    impl_ = im;
    frames_ = 0;
    return true;
}

bool Mp3Writer::write(const std::int16_t* interleaved, std::size_t frames) {
    if (impl_ == nullptr || impl_->failed) { return false; }
    if (frames == 0) { return true; }
    const DWORD bytes = static_cast<DWORD>(frames * impl_->channels * sizeof(std::int16_t));
    Com<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(bytes, buf.put()))) {
        impl_->failed = true;
        return false;
    }
    BYTE* dst = nullptr;
    if (FAILED(buf->Lock(&dst, nullptr, nullptr))) {
        impl_->failed = true;
        return false;
    }
    std::memcpy(dst, interleaved, bytes);
    buf->Unlock();
    buf->SetCurrentLength(bytes);
    Com<IMFSample> sample;
    if (FAILED(MFCreateSample(sample.put()))) {
        impl_->failed = true;
        return false;
    }
    sample->AddBuffer(buf.p);
    // Times from the running FRAME count, so per-block rounding cannot
    // accumulate into drift over an hour-long recording.
    const auto at = [this](std::uint64_t f) {
        return static_cast<LONGLONG>(f * 10000000ULL / impl_->rate);
    };
    sample->SetSampleTime(at(frames_));
    sample->SetSampleDuration(at(frames_ + frames) - at(frames_));
    if (FAILED(impl_->writer->WriteSample(impl_->stream, sample.p))) {
        impl_->failed = true;
        return false;
    }
    frames_ += frames;
    return true;
}

void Mp3Writer::close() {
    if (impl_ == nullptr) { return; }
    if (impl_->writer != nullptr) {
        impl_->writer->Finalize();
        impl_->writer->Release();
    }
    if (impl_->mfStarted) { MFShutdown(); }
    if (impl_->comInit) { CoUninitialize(); }
    delete impl_;
    impl_ = nullptr;
}

}  // namespace cascade::core

#else  // !_WIN32

namespace cascade::core {

struct Mp3Writer::Impl {};

bool Mp3Writer::available() { return false; }

Mp3Writer::~Mp3Writer() { close(); }

bool Mp3Writer::open(const std::string&, unsigned, unsigned, unsigned, std::string& error) {
    error = "MP3 needs Windows' own encoder; this build writes WAV instead";
    return false;
}

bool Mp3Writer::write(const std::int16_t*, std::size_t) { return false; }

void Mp3Writer::close() {}

}  // namespace cascade::core

#endif
