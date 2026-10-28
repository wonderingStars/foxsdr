// A REAL plugin module that is a TRACK-INFO source WITH PHOTOS (CASCADE_CAP_TRACK_PHOTO, 0.99.98),
// for tests/test_track_photo_app.cpp.
//
// It declares CASCADE_CAP_TRACK_INFO (display name "Photo Probe") and CASCADE_CAP_TRACK_PHOTO, and
// knows every aircraft: get_info answers READY with registration G-PHOTO, and get_photo answers
// PENDING on the first ask for an id and READY on every later one, with a 40 x 20 picture (red
// left half, blue right), the credit "Photo: Ada Test / Photo Probe" and the link
// https://example.test/photo/1. Nothing is fetched from anywhere.
//
// Every photo call is written, one line each, to the file PHOTO_PROBE_LOG names: "get <id>",
// "ready <id>", "release <id>", "forget <id>". That is how the test sees WHEN the host asks - only
// once the details are opened - and that it lets the photo go once they are closed.
//
// Built by tests/CMakeLists.txt as a MODULE and including ONLY plugin_abi.h, as every third-party
// plugin must.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "plugin_abi.h"

namespace {

constexpr uint32_t kW = 40;
constexpr uint32_t kH = 20;

struct Probe {
    uint8_t pixels[kW * kH * 3];
    char asked[64][CASCADE_TRACK_ID_CHARS];  // ids asked about once already
    int nAsked = 0;
};

// The OS copy of the environment, read when it is needed (see the aircraft-info plugin's
// envString for why a module's getenv is not enough on Windows).
void logLine(const char* what, const char* id) {
    char path[1024] = {0};
#if defined(_WIN32)
    const DWORD got = ::GetEnvironmentVariableA("PHOTO_PROBE_LOG", path, sizeof path);
    if (got == 0 || got >= sizeof path) { return; }
#else
    const char* v = std::getenv("PHOTO_PROBE_LOG");
    if (v == nullptr || v[0] == '\0') { return; }
    std::snprintf(path, sizeof path, "%s", v);
#endif
    FILE* f = std::fopen(path, "ab");
    if (f == nullptr) { return; }
    std::fprintf(f, "%s %s\n", what, id != nullptr ? id : "?");
    std::fclose(f);
}

void* create() {
    Probe* p = new (std::nothrow) Probe();
    if (p == nullptr) { return nullptr; }
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            uint8_t* px = p->pixels + (y * kW + x) * 3u;
            const bool left = x < kW / 2;
            px[0] = left ? 220 : 30;
            px[1] = 30;
            px[2] = left ? 30 : 220;
        }
    }
    return p;
}

int32_t getInfo(void*, const char*, uint32_t kind, CascadeTrackInfo* out) {
    if (kind != CASCADE_TRACK_AIRCRAFT) { return CASCADE_INFO_MISSING; }
    out->registration = "G-PHOTO";
    out->typeCode = "A320";
    out->typeName = "Airbus A320";
    out->manufacturer = "Airbus";
    out->operatorName = "Probe Air";
    out->country = "United Kingdom";
    return CASCADE_INFO_READY;
}

void releaseInfo(void*, const CascadeTrackInfo*) {}

int32_t pollText(void*, char*, size_t) { return 0; }

void destroy(void* h) { delete static_cast<Probe*>(h); }

int32_t getPhoto(void* h, const char* id, uint32_t kind, CascadeTrackPhoto* out) {
    Probe* p = static_cast<Probe*>(h);
    logLine("get", id);
    if (p == nullptr || id == nullptr || kind != CASCADE_TRACK_AIRCRAFT) {
        return CASCADE_PHOTO_MISSING;
    }
    bool seen = false;
    for (int i = 0; i < p->nAsked; ++i) {
        if (std::strcmp(p->asked[i], id) == 0) { seen = true; }
    }
    if (!seen) {
        if (p->nAsked < 64) {
            std::snprintf(p->asked[p->nAsked++], CASCADE_TRACK_ID_CHARS, "%s", id);
        }
        return CASCADE_PHOTO_PENDING;
    }
    out->width = kW;
    out->height = kH;
    out->format = CASCADE_IMAGE_RGB24;
    out->stride = kW * 3u;
    out->pixels = p->pixels;
    out->credit = "Photo: Ada Test / Photo Probe";
    out->link = "https://example.test/photo/1";
    logLine("ready", id);
    return CASCADE_PHOTO_READY;
}

void releasePhoto(void*, const CascadeTrackPhoto*) { logLine("release", "-"); }

void forgetPhoto(void* h, const char* id) {
    Probe* p = static_cast<Probe*>(h);
    logLine("forget", id);
    // Forgotten means asked afresh next time: PENDING first again.
    if (p == nullptr || id == nullptr) { return; }
    for (int i = 0; i < p->nAsked; ++i) {
        if (std::strcmp(p->asked[i], id) == 0) {
            p->asked[i][0] = '\0';
        }
    }
}

const CascadeTrackInfoApi kTrackInfo = {
    static_cast<uint32_t>(sizeof(CascadeTrackInfoApi)),
    &create, &getInfo, &releaseInfo, &pollText, &destroy,
};

const CascadeTrackPhotoApi kTrackPhoto = {
    static_cast<uint32_t>(sizeof(CascadeTrackPhotoApi)),
    &getPhoto, &releasePhoto, &forgetPhoto,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_TRACK_INFO, static_cast<uint32_t>(sizeof(CascadeTrackInfoApi)), &kTrackInfo},
    {CASCADE_CAP_TRACK_PHOTO, static_cast<uint32_t>(sizeof(CascadeTrackPhotoApi)), &kTrackPhoto},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Photo Probe",
    "1.0.0",
    "FoxSDR tests",
    "PolyForm-Noncommercial-1.0.0",
    CASCADE_CAP_TRACK_INFO | CASCADE_CAP_TRACK_PHOTO,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}
