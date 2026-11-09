// Tests for gui/track_photo_cache.hpp - the photo a track-info plugin offers
// for a target whose details are open (CASCADE_CAP_TRACK_PHOTO, 0.99.98) -
// driven through a scriptable fake of the C table and fake texture hooks, so
// no GL context is needed.
//
// What matters here is the opposite of TrackInfoCache's policy: a photo is
// kept ONLY while it is requested every frame, is let go (texture deleted,
// plugin told to forget) the first frame it is not, and is refused outright
// unless it carries a credit and a link the shell may safely be handed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "gui/track_info_cache.hpp"
#include "gui/track_photo_cache.hpp"
#include "test_check.hpp"

using cascade::gui::fitPhoto;
using cascade::gui::photoLinkIsSafe;
using cascade::gui::photoPixelsAreSane;
using cascade::gui::TrackInfoCache;
using cascade::gui::TrackPhotoCache;

namespace {

// ---- the fake plugin ---------------------------------------------------------

struct Scripted {
    std::int32_t result = CASCADE_PHOTO_PENDING;
    std::uint32_t width = 3, height = 2, stride = 12;  // stride padded past 9 bytes
    std::uint32_t format = CASCADE_IMAGE_RGB24;
    bool nullPixels = false;
    std::string credit = "Photo: A. Smith / Planespotters.net";
    std::string link = "https://www.planespotters.net/photo/1/x";
    bool nullCredit = false;
    bool nullLink = false;
};
std::map<std::string, Scripted> g_script;
std::vector<std::uint8_t> g_pixels;
std::vector<std::string> g_asked, g_released, g_forgotten, g_calls;
void* const kHandle = reinterpret_cast<void*>(0x5a);
bool g_wrongHandle = false;

std::int32_t fakeGetPhoto(void* h, const char* id, std::uint32_t, CascadeTrackPhoto* out) {
    if (h != kHandle) { g_wrongHandle = true; }
    g_asked.push_back(id);
    g_calls.push_back(std::string("get ") + id);
    const auto it = g_script.find(id);
    if (it == g_script.end()) { return CASCADE_PHOTO_MISSING; }
    const Scripted& s = it->second;
    if (s.result != CASCADE_PHOTO_READY) { return s.result; }
    // Row r, column c, channel k = 10*r + 3*c + k; padding bytes are 0xEE so a
    // packing fault shows up as a wrong byte in the upload.
    g_pixels.assign(static_cast<std::size_t>(s.stride) * s.height, 0xEE);
    for (std::uint32_t r = 0; r < s.height; ++r) {
        for (std::uint32_t c = 0; c < s.width * 3u && c < s.stride; ++c) {
            g_pixels[r * s.stride + c] = static_cast<std::uint8_t>(10u * r + c);
        }
    }
    out->width = s.width;
    out->height = s.height;
    out->format = s.format;
    out->stride = s.stride;
    out->pixels = s.nullPixels ? nullptr : g_pixels.data();
    out->credit = s.nullCredit ? nullptr : s.credit.c_str();
    out->link = s.nullLink ? nullptr : s.link.c_str();
    return CASCADE_PHOTO_READY;
}
void fakeReleasePhoto(void* h, const CascadeTrackPhoto*) {
    if (h != kHandle) { g_wrongHandle = true; }
    g_released.push_back("r");
}
void fakeForgetPhoto(void* h, const char* id) {
    if (h != kHandle) { g_wrongHandle = true; }
    g_forgotten.push_back(id);
    g_calls.push_back(std::string("forget ") + id);
}

const CascadeTrackPhotoApi kPhotoApi = {static_cast<std::uint32_t>(sizeof(CascadeTrackPhotoApi)),
                                        &fakeGetPhoto, &fakeReleasePhoto, &fakeForgetPhoto};

// ---- fake textures -------------------------------------------------------------

unsigned int g_nextTex = 100;
bool g_uploadFails = false;
std::vector<std::vector<std::uint8_t>> g_uploads;
std::vector<unsigned int> g_deleted;

void useFakeTextures(TrackPhotoCache& c) {
    c.setTextureHooks(
        [](const std::uint8_t* rgb, std::uint32_t w, std::uint32_t h) -> unsigned int {
            if (g_uploadFails) { return 0u; }
            g_uploads.emplace_back(rgb, rgb + static_cast<std::size_t>(w) * 3u * h);
            return g_nextTex++;
        },
        [](unsigned int tex) { g_deleted.push_back(tex); });
}

void resetFake() {
    g_script.clear();
    g_asked.clear();
    g_released.clear();
    g_forgotten.clear();
    g_calls.clear();
    g_uploads.clear();
    g_deleted.clear();
    g_uploadFails = false;
    g_wrongHandle = false;
    g_nextTex = 100;
}

// ---- the pure checks -------------------------------------------------------------

void testLinkCheck() {
    std::printf("  the link: https, printable, one argument, terminated inside the bound\n");
    CHECK(photoLinkIsSafe("https://www.planespotters.net/photo/1234/g-ezbx", 512));
    CHECK(photoLinkIsSafe("https://a", 512));
    CHECK(!photoLinkIsSafe(nullptr, 512));
    CHECK(!photoLinkIsSafe("", 512));
    CHECK(!photoLinkIsSafe("https://", 512));
    CHECK(!photoLinkIsSafe("https:///etc/passwd", 512));
    CHECK(!photoLinkIsSafe("http://www.planespotters.net/photo/1", 512));
    CHECK(!photoLinkIsSafe("HTTPS://www.planespotters.net/photo/1", 512));
    CHECK(!photoLinkIsSafe("file:///C:/Windows/System32/calc.exe", 512));
    CHECK(!photoLinkIsSafe("C:\\Windows\\System32\\calc.exe", 512));
    CHECK(!photoLinkIsSafe("https://x.net/a b", 512));
    CHECK(!photoLinkIsSafe("https://x.net/a\"b", 512));
    CHECK(!photoLinkIsSafe("https://x.net/a'b", 512));
    CHECK(!photoLinkIsSafe("https://x.net/a\\b", 512));
    CHECK(!photoLinkIsSafe("https://x.net/a`b", 512));
    CHECK(!photoLinkIsSafe("https://x.net/a<b>", 512));
    CHECK(!photoLinkIsSafe("https://x.net/a\nb", 512));
    CHECK(!photoLinkIsSafe("https://x.net/a\x7f", 512));
    CHECK(!photoLinkIsSafe("https://x.net/\xc3\xa9", 512));
    // The bound: a link of exactly cap-1 bytes fits, cap bytes does not.
    const std::string base = "https://x.net/";
    const std::string fits = base + std::string(511 - base.size(), 'a');
    const std::string over = base + std::string(512 - base.size(), 'a');
    CHECK(fits.size() == 511 && photoLinkIsSafe(fits.c_str(), 512));
    CHECK(over.size() == 512 && !photoLinkIsSafe(over.c_str(), 512));
}

void testPixelCheck() {
    std::printf("  the pixels: RGB24, in bounds, a stride that covers a row\n");
    const std::uint8_t px[12] = {};
    CascadeTrackPhoto p{};
    p.structSize = sizeof p;
    p.width = 2;
    p.height = 2;
    p.format = CASCADE_IMAGE_RGB24;
    p.stride = 6;
    p.pixels = px;
    CHECK(photoPixelsAreSane(p));
    CascadeTrackPhoto q = p;
    q.pixels = nullptr;
    CHECK(!photoPixelsAreSane(q));
    q = p;
    q.format = CASCADE_IMAGE_GRAY8;
    CHECK(!photoPixelsAreSane(q));
    q = p;
    q.width = 0;
    CHECK(!photoPixelsAreSane(q));
    q = p;
    q.height = 0;
    CHECK(!photoPixelsAreSane(q));
    q = p;
    q.stride = 5;
    CHECK(!photoPixelsAreSane(q));
    q = p;
    q.width = CASCADE_TRACK_PHOTO_MAX_DIM;
    q.stride = CASCADE_TRACK_PHOTO_MAX_DIM * 3u;
    CHECK(photoPixelsAreSane(q));
    q.width = CASCADE_TRACK_PHOTO_MAX_DIM + 1u;
    q.stride = q.width * 3u;
    CHECK(!photoPixelsAreSane(q));
    q = p;
    q.height = CASCADE_TRACK_PHOTO_MAX_DIM + 1u;
    CHECK(!photoPixelsAreSane(q));
}

void testFit() {
    std::printf("  the drawn size: inside the box, aspect kept, never enlarged\n");
    // Planespotters' small thumbnail in a wide panel: drawn at its own size.
    auto f = fitPhoto(200, 133, 400.0f, 280.0f);
    CHECK_NEAR(f.w, 200.0, 1e-4);
    CHECK_NEAR(f.h, 133.0, 1e-4);
    // Its large one in a narrow rail: width-limited, aspect kept.
    f = fitPhoto(420, 280, 210.0f, 280.0f);
    CHECK_NEAR(f.w, 210.0, 1e-4);
    CHECK_NEAR(f.h, 140.0, 1e-4);
    // A portrait one: height-limited.
    f = fitPhoto(180, 360, 400.0f, 180.0f);
    CHECK_NEAR(f.w, 90.0, 1e-4);
    CHECK_NEAR(f.h, 180.0, 1e-4);
    // Nothing to draw into, or nothing to draw.
    f = fitPhoto(200, 133, 0.0f, 280.0f);
    CHECK(f.w == 0.0f && f.h == 0.0f);
    f = fitPhoto(200, 133, -5.0f, 280.0f);
    CHECK(f.w == 0.0f && f.h == 0.0f);
    f = fitPhoto(200, 133, std::numeric_limits<float>::quiet_NaN(), 280.0f);
    CHECK(f.w == 0.0f && f.h == 0.0f);
    f = fitPhoto(0, 133, 400.0f, 280.0f);
    CHECK(f.w == 0.0f && f.h == 0.0f);
}

// ---- the cache ---------------------------------------------------------------------

void testInactiveAsksNothing() {
    std::printf("  no photo source: nothing asked, nothing shown\n");
    resetFake();
    TrackPhotoCache c;
    useFakeTextures(c);
    const TrackPhotoCache::Photo* ph = reinterpret_cast<const TrackPhotoCache::Photo*>(1);
    CHECK(c.request("4CA7B5", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Unavailable);
    CHECK(ph == nullptr);
    c.attach(nullptr, kHandle);
    CHECK(!c.active());
    c.attach(&kPhotoApi, nullptr);
    CHECK(!c.active());
    CHECK(c.request("4CA7B5", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Unavailable);
    c.endFrame();
    CHECK(g_asked.empty() && g_forgotten.empty());
    c.attach(&kPhotoApi, kHandle);
    CHECK(c.request("", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Unavailable);
    CHECK(g_asked.empty());
}

void testPendingThenReadyCopiesAndReleasesOnce() {
    std::printf("  PENDING is asked again; READY is copied, packed and released once\n");
    resetFake();
    TrackPhotoCache c;
    useFakeTextures(c);
    c.attach(&kPhotoApi, kHandle);
    const TrackPhotoCache::Photo* ph = nullptr;

    g_script["4CA7B5"].result = CASCADE_PHOTO_PENDING;
    CHECK(c.request("4CA7B5", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Pending);
    CHECK(ph == nullptr);
    c.endFrame();
    CHECK(c.request("4CA7B5", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Pending);
    CHECK(g_asked.size() == 2);
    c.endFrame();

    g_script["4CA7B5"].result = CASCADE_PHOTO_READY;
    CHECK(c.request("4CA7B5", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Ready);
    CHECK(ph != nullptr);
    CHECK(g_released.size() == 1);
    CHECK(g_uploads.size() == 1);
    if (ph != nullptr) {
        CHECK(ph->texture == 100u);
        CHECK(ph->width == 3u && ph->height == 2u);
        CHECK(ph->credit == "Photo: A. Smith / Planespotters.net");
        CHECK(ph->link == "https://www.planespotters.net/photo/1/x");
    }
    // Rows packed: the stride's padding (0xEE) is not uploaded.
    if (g_uploads.size() == 1) {
        const std::vector<std::uint8_t>& u = g_uploads[0];
        CHECK(u.size() == 18u);
        for (std::uint32_t r = 0; r < 2; ++r) {
            for (std::uint32_t k = 0; k < 9; ++k) {
                CHECK(u[r * 9u + k] == static_cast<std::uint8_t>(10u * r + k));
            }
        }
    }

    // Kept while it is asked for every frame - and the plugin is not asked again.
    for (int frame = 0; frame < 5; ++frame) {
        c.endFrame();
        CHECK(c.request("4CA7B5", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Ready);
    }
    CHECK(g_asked.size() == 3);
    CHECK(g_released.size() == 1);
    CHECK(g_deleted.empty() && g_forgotten.empty());
    CHECK(!g_wrongHandle);
}

void testNotShownIsLetGoAndAskedAgain() {
    std::printf("  a frame without the details: texture deleted, plugin told, asked afresh\n");
    resetFake();
    TrackPhotoCache c;
    useFakeTextures(c);
    c.attach(&kPhotoApi, kHandle);
    g_script["4CA7B5"].result = CASCADE_PHOTO_READY;
    const TrackPhotoCache::Photo* ph = nullptr;
    CHECK(c.request("4CA7B5", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Ready);
    c.endFrame();  // was requested: kept
    CHECK(g_deleted.empty() && c.shown() == 1);
    c.endFrame();  // was not: let go
    CHECK(g_deleted.size() == 1 && g_deleted[0] == 100u);
    CHECK(g_forgotten.size() == 1 && g_forgotten[0] == "4CA7B5");
    CHECK(c.shown() == 0);
    // Opened again: asked again, and a new texture.
    CHECK(c.request("4CA7B5", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Ready);
    CHECK(g_asked.size() == 2);
    CHECK(ph != nullptr && ph->texture == 101u);

    // A PENDING one that stops being shown is forgotten too, so the plugin can
    // drop a download nobody will see.
    g_script["400F01"].result = CASCADE_PHOTO_PENDING;
    CHECK(c.request("400F01", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Pending);
    c.endFrame();
    CHECK(c.request("4CA7B5", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Ready);
    c.endFrame();
    CHECK(g_forgotten.size() == 2 && g_forgotten[1] == "400F01");
    CHECK(c.shown() == 1);
}

void testMissingIsNotReasked() {
    std::printf("  MISSING is remembered while shown, asked again once reopened\n");
    resetFake();
    TrackPhotoCache c;
    useFakeTextures(c);
    c.attach(&kPhotoApi, kHandle);
    const TrackPhotoCache::Photo* ph = nullptr;
    for (int frame = 0; frame < 4; ++frame) {
        CHECK(c.request("ABCDEF", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Missing);
        CHECK(ph == nullptr);
        c.endFrame();
    }
    CHECK(g_asked.size() == 1);
    CHECK(g_released.empty() && g_uploads.empty());
    c.endFrame();
    CHECK(c.request("ABCDEF", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Missing);
    CHECK(g_asked.size() == 2);
    // An answer that is none of the three is no photo.
    g_script["ABCDEF"].result = 7;
    c.endFrame();
    c.endFrame();
    CHECK(c.request("ABCDEF", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Missing);
}

// One READY answer the host must refuse: shown as no photo, released all the same.
void expectRefused(const char* what, const Scripted& s) {
    resetFake();
    TrackPhotoCache c;
    useFakeTextures(c);
    c.attach(&kPhotoApi, kHandle);
    g_script["4CA7B5"] = s;
    g_script["4CA7B5"].result = CASCADE_PHOTO_READY;
    const TrackPhotoCache::Photo* ph = nullptr;
    const TrackPhotoCache::State st = c.request("4CA7B5", CASCADE_TRACK_AIRCRAFT, &ph);
    CHECK(st == TrackPhotoCache::State::Missing);
    CHECK(ph == nullptr);
    CHECK(g_released.size() == 1);
    if (st != TrackPhotoCache::State::Missing || g_released.size() != 1) {
        std::printf("FAIL refused answer: %s\n", what);
    }
    // Whatever was uploaded before the refusal (only a failed upload) is not
    // left behind: nothing to delete, and nothing shown.
    CHECK(g_deleted.empty());
}

void testRefusedAnswers() {
    std::printf("  READY without a credit, a safe link or usable pixels is no photo\n");
    Scripted s;
    s.nullCredit = true;
    expectRefused("null credit", s);
    s = Scripted{};
    s.credit = "";
    expectRefused("empty credit", s);
    s = Scripted{};
    s.credit = "   \t\n ";
    expectRefused("blank credit", s);
    s = Scripted{};
    s.nullLink = true;
    expectRefused("null link", s);
    s = Scripted{};
    s.link = "file:///C:/x.exe";
    expectRefused("file link", s);
    s = Scripted{};
    s.link = "https://x.net/a\" --flag";
    expectRefused("argument in the link", s);
    s = Scripted{};
    s.nullPixels = true;
    expectRefused("null pixels", s);
    s = Scripted{};
    s.format = CASCADE_IMAGE_GRAY8;
    expectRefused("grey pixels", s);
    s = Scripted{};
    s.stride = 8;
    expectRefused("short stride", s);
    s = Scripted{};
    s.width = CASCADE_TRACK_PHOTO_MAX_DIM + 1u;
    s.stride = s.width * 3u;
    s.height = 1;
    expectRefused("too wide", s);
    // And the texture that could not be made.
    resetFake();
    TrackPhotoCache c;
    useFakeTextures(c);
    c.attach(&kPhotoApi, kHandle);
    g_script["4CA7B5"].result = CASCADE_PHOTO_READY;
    g_uploadFails = true;
    CHECK(c.request("4CA7B5", CASCADE_TRACK_AIRCRAFT, nullptr) == TrackPhotoCache::State::Missing);
    CHECK(g_released.size() == 1);
}

void testCreditIsCleaned() {
    std::printf("  the credit: control bytes made spaces, bounded on a UTF-8 boundary\n");
    resetFake();
    TrackPhotoCache c;
    useFakeTextures(c);
    c.attach(&kPhotoApi, kHandle);
    const TrackPhotoCache::Photo* ph = nullptr;
    g_script["000001"].result = CASCADE_PHOTO_READY;
    g_script["000001"].credit = "Photo:\tJos\xc3\xa9\nM.";
    CHECK(c.request("000001", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Ready);
    CHECK(ph != nullptr && ph->credit == "Photo: Jos\xc3\xa9 M.");

    // 159 ASCII bytes then a two-byte e-acute: the bound (160) falls inside
    // the character, which is dropped whole rather than cut in half.
    g_script["000002"].result = CASCADE_PHOTO_READY;
    g_script["000002"].credit = std::string(159, 'a') + "\xc3\xa9" + "tail";
    CHECK(c.request("000002", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Ready);
    CHECK(ph != nullptr && ph->credit == std::string(159, 'a'));
    // 158 then the same character: it fits whole.
    g_script["000003"].result = CASCADE_PHOTO_READY;
    g_script["000003"].credit = std::string(158, 'a') + "\xc3\xa9" + "tail";
    CHECK(c.request("000003", CASCADE_TRACK_AIRCRAFT, &ph) == TrackPhotoCache::State::Ready);
    CHECK(ph != nullptr && ph->credit == std::string(158, 'a') + "\xc3\xa9");
}

void testShownIsCapped() {
    std::printf("  at most kMaxShown targets at once; the next is not even asked\n");
    resetFake();
    TrackPhotoCache c;
    useFakeTextures(c);
    c.attach(&kPhotoApi, kHandle);
    const char* ids[] = {"000001", "000002", "000003", "000004", "000005"};
    for (const char* id : ids) { g_script[id].result = CASCADE_PHOTO_PENDING; }
    for (std::size_t i = 0; i < TrackPhotoCache::kMaxShown; ++i) {
        CHECK(c.request(ids[i], CASCADE_TRACK_AIRCRAFT, nullptr) ==
              TrackPhotoCache::State::Pending);
    }
    CHECK(c.request(ids[4], CASCADE_TRACK_AIRCRAFT, nullptr) ==
          TrackPhotoCache::State::Unavailable);
    CHECK(g_asked.size() == TrackPhotoCache::kMaxShown);
    CHECK(c.shown() == TrackPhotoCache::kMaxShown);
    // Room again once one is let go.
    c.endFrame();
    for (std::size_t i = 1; i < TrackPhotoCache::kMaxShown; ++i) {
        c.request(ids[i], CASCADE_TRACK_AIRCRAFT, nullptr);
    }
    c.endFrame();
    CHECK(c.request(ids[4], CASCADE_TRACK_AIRCRAFT, nullptr) == TrackPhotoCache::State::Pending);
}

void testDetachLetsEverythingGo() {
    std::printf("  detach: every shown id forgotten, every texture deleted\n");
    resetFake();
    TrackPhotoCache c;
    useFakeTextures(c);
    c.attach(&kPhotoApi, kHandle);
    g_script["000001"].result = CASCADE_PHOTO_READY;
    g_script["000002"].result = CASCADE_PHOTO_PENDING;
    c.request("000001", CASCADE_TRACK_AIRCRAFT, nullptr);
    c.request("000002", CASCADE_TRACK_AIRCRAFT, nullptr);
    c.detach();
    CHECK(!c.active());
    CHECK(c.shown() == 0);
    CHECK(g_forgotten.size() == 2);
    CHECK(g_deleted.size() == 1 && g_deleted[0] == 100u);
    // Nothing more after detach.
    c.endFrame();
    CHECK(c.request("000001", CASCADE_TRACK_AIRCRAFT, nullptr) ==
          TrackPhotoCache::State::Unavailable);
    CHECK(g_forgotten.size() == 2);
}

// ---- through TrackInfoCache: the photos ride on ITS instance ------------------------

int g_infoCreates = 0;
int g_infoDestroys = 0;
void* infoCreate() {
    ++g_infoCreates;
    g_calls.push_back("create");
    return kHandle;
}
std::int32_t infoGet(void*, const char*, std::uint32_t, CascadeTrackInfo*) {
    return CASCADE_INFO_MISSING;
}
void infoRelease(void*, const CascadeTrackInfo*) {}
std::int32_t infoPoll(void*, char*, size_t) { return 0; }
void infoDestroy(void* h) {
    if (h != kHandle) { g_wrongHandle = true; }
    ++g_infoDestroys;
    g_calls.push_back("destroy");
}
const CascadeTrackInfoApi kInfoApi = {static_cast<std::uint32_t>(sizeof(CascadeTrackInfoApi)),
                                      &infoCreate, &infoGet, &infoRelease, &infoPoll,
                                      &infoDestroy};

void testRidesOnTheTrackInfoInstance() {
    std::printf("  through TrackInfoCache: same handle, forgotten before it is destroyed\n");
    resetFake();
    g_infoCreates = g_infoDestroys = 0;
    {
        TrackInfoCache info;
        info.attach(&kInfoApi);
        CHECK(info.active());
        CHECK(!info.photos().active());  // a plugin with no photo table
        info.detach();
        CHECK(g_infoDestroys == 1);

        info.attach(&kInfoApi, std::string(), &kPhotoApi);
        CHECK(info.photos().active());
        useFakeTextures(info.photos());
        g_script["4CA7B5"].result = CASCADE_PHOTO_READY;
        CHECK(info.photos().request("4CA7B5", CASCADE_TRACK_AIRCRAFT, nullptr) ==
              TrackPhotoCache::State::Ready);
        // The same table with the photo table added is a different attachment.
        g_calls.clear();
        info.attach(&kInfoApi, std::string(), nullptr);
        CHECK(!info.photos().active());
        CHECK(g_calls.size() == 3);
        if (g_calls.size() == 3) {
            CHECK(g_calls[0] == "forget 4CA7B5");  // before the instance goes
            CHECK(g_calls[1] == "destroy");
            CHECK(g_calls[2] == "create");
        }
        CHECK(g_deleted.size() == 1);

        info.attach(&kInfoApi, std::string(), &kPhotoApi);
        useFakeTextures(info.photos());
        info.photos().request("4CA7B5", CASCADE_TRACK_AIRCRAFT, nullptr);
        g_calls.clear();
    }  // destructor
    CHECK(g_calls.size() == 2);
    if (g_calls.size() == 2) {
        CHECK(g_calls[0] == "forget 4CA7B5");
        CHECK(g_calls[1] == "destroy");
    }
    CHECK(g_infoCreates == g_infoDestroys);
    CHECK(!g_wrongHandle);
}

}  // namespace

int main() {
    testLinkCheck();
    testPixelCheck();
    testFit();
    testInactiveAsksNothing();
    testPendingThenReadyCopiesAndReleasesOnce();
    testNotShownIsLetGoAndAskedAgain();
    testMissingIsNotReasked();
    testRefusedAnswers();
    testCreditIsCleaned();
    testShownIsCapped();
    testDetachLetsEverythingGo();
    testRidesOnTheTrackInfoInstance();
    return testSummary("test_track_photo_cache");
}
