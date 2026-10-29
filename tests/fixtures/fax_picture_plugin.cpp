// A REAL PLUGIN MODULE FOR tests/test_fax_picture_app.cpp: a radiofax machine in miniature - an
// IMAGE DECODER that has already received a picture, a FAX INSTRUMENT (the recorder) with a memory
// beneath it, and an IMAGE INPUT that takes a click on the picture - against the plugin ABI and
// nothing else. It is the shape of the Weather Fax plugin, and the shape the host draws as ONE
// window: the recorder with the decoded chart printed inside it, the memory's rows beside it, and a
// click on the chart handed to the plugin in the chart's own pixels.
//
// The picture is 400 wide and 100 high - four to one, nowhere near square - so a test can tell a
// picture fitted with its aspect kept from one stretched to the window. It is offered, complete, on
// the first poll_image after the decoder is created, whatever audio is (or is not) being fed to it:
// the test is about where the host draws a picture and where it sends a click, not about decoding.
// Its pixels are a left-to-right ramp, so that the texture is not a flat colour.
//
// The recorder (title "Fax Picture Recorder", kind CASCADE_INSTRUMENT_FAX) has a two-column memory,
// "Time" and "Event":
//   Last chart            plain row   - a row with no key: text, never mentioned by the census
//   SET LEFT EDGE         KEY(3)      - registered with add_command, so it is pressable
// The key is drawn as a key only when get_state says the host draws them (CASCADE_STATE_ROW_KEYS),
// and the command is registered then, exactly as tests/fixtures/row_keys_plugin.cpp does it.
//
// WHAT IT WRITES: when the environment variable FAX_PICTURE_FIXTURE_LOG names a file, one line is
// appended for everything the host hands it -
//   "press <x> <y>"   CascadeImageInputApi::press, in native picture pixels (top left 0,0)
//   "key <id>"        a command taken from poll_command (a press of the memory's key)
// so the test reads what the plugin was actually given, not what the host says it sent.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "plugin_abi.h"

namespace {

constexpr uint32_t kPictureW = 400u;
constexpr uint32_t kPictureH = 100u;
constexpr uint32_t kKeyId = 3u;

const CascadeHostApi* g_host = nullptr;
bool g_registered = false;

void logLine(const char* line) {
    const char* path = std::getenv("FAX_PICTURE_FIXTURE_LOG");
    if (path == nullptr || path[0] == '\0') { return; }
    std::FILE* f = std::fopen(path, "ab");
    if (f == nullptr) { return; }
    std::fputs(line, f);
    std::fclose(f);
}

// Does the host say it draws a CASCADE_ROW_FLAG_KEY row as a key?
bool hostDrawsRowKeys() {
    if (g_host == nullptr || !CASCADE_HOST_HAS(g_host, get_state)) { return false; }
    CascadeReceiverState s;
    std::memset(&s, 0, sizeof s);
    s.structSize = static_cast<uint32_t>(sizeof s);
    if (g_host->get_state(g_host->ctx, &s) != CASCADE_API_OK) { return false; }
    return (s.flags & CASCADE_STATE_ROW_KEYS) != 0u;
}

void registerCommands() {
    if (g_registered || g_host == nullptr || !CASCADE_HOST_HAS(g_host, add_command)) { return; }
    if (!hostDrawsRowKeys()) { return; }
    g_registered = g_host->add_command(g_host->ctx, kKeyId, "SET LEFT EDGE") == CASCADE_API_OK;
}

void takePresses() {
    if (g_host == nullptr || !CASCADE_HOST_HAS(g_host, poll_command)) { return; }
    uint32_t id = 0;
    while (g_host->poll_command(g_host->ctx, &id) == 1) {
        char line[48];
        std::snprintf(line, sizeof line, "key %u\n", id);
        logLine(line);
    }
}

void hostAttach(const CascadeHostApi* h) {
    g_host = h;
    registerCommands();
}

const CascadeHostClientApi kHostClient = {
    static_cast<uint32_t>(sizeof(CascadeHostClientApi)),
    &hostAttach,
};

// --- THE IMAGE DECODER: a picture, complete, offered once --------------------------------------------

struct Decoder {
    bool pending = true;
    uint8_t pixels[kPictureW * kPictureH];
};

void* imageCreate(double, double) {
    Decoder* d = new (std::nothrow) Decoder();
    if (d == nullptr) { return nullptr; }
    for (uint32_t y = 0; y < kPictureH; ++y) {
        for (uint32_t x = 0; x < kPictureW; ++x) {
            d->pixels[y * kPictureW + x] = static_cast<uint8_t>((x * 255u) / (kPictureW - 1u));
        }
    }
    return d;
}

void imageProcess(void*, const float*, size_t) {}

int32_t imagePoll(void* handle, CascadeImage* out) {
    Decoder* d = static_cast<Decoder*>(handle);
    if (d == nullptr || !d->pending) { return 0; }
    d->pending = false;
    out->width = kPictureW;
    out->height = kPictureH;
    out->format = CASCADE_IMAGE_GRAY8;
    out->stride = kPictureW;
    out->complete = 1;
    out->sequence = 1u;
    out->pixels = d->pixels;
    return 1;
}

void imageRelease(void*, const CascadeImage*) {}

int32_t imagePollText(void*, char*, size_t) { return 0; }

void imageDestroy(void* handle) { delete static_cast<Decoder*>(handle); }

const CascadeImageDecoderApi kImageDecoder = {
    static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)),
    CASCADE_INPUT_AUDIO,
    0.0,  // requiredRateHz: any rate
    0.0,  // preferredRateHz
    &imageCreate,
    &imageProcess,
    nullptr,  // retune: this decoder does not care
    &imagePoll,
    &imageRelease,
    &imagePollText,
    &imageDestroy,
};

// --- THE IMAGE INPUT: a click on the picture, in the picture's own pixels ----------------------------

void imagePress(uint32_t x, uint32_t y) {
    char line[64];
    std::snprintf(line, sizeof line, "press %u %u\n", x, y);
    logLine(line);
}

const CascadeImageInputApi kImageInput = {
    static_cast<uint32_t>(sizeof(CascadeImageInputApi)),
    &imagePress,
};

// --- THE RECORDER: a fax instrument with a two-column memory -----------------------------------------

int g_instrumentHandle = 0;
void* instrumentCreate(void) { return &g_instrumentHandle; }

int32_t instrumentPollState(void*, CascadeInstrumentState* out) {
    // The host asks every frame; ask the host again for the commands too (add_command relabels an
    // id it already has) and take what has been pressed.
    registerCommands();
    takePresses();
    out->flags = CASCADE_INSTRUMENT_FLAG_LOCK;
    out->seq = 1u;
    out->values[0] = 576.0;  // IOC
    out->values[1] = 120.0;  // lines per minute
    out->values[2] = 100.0;  // lines received
    out->values[3] = 0.0;    // tuning offset
    std::snprintf(out->text[0], CASCADE_INSTRUMENT_TEXT_CHARS, "%s", "PICTURE");
    return 1;
}

uint32_t memoryColumns(void*, char headings[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS]) {
    std::snprintf(headings[0], CASCADE_PANEL_CELL_CHARS, "%s", "Time");
    std::snprintf(headings[1], CASCADE_PANEL_CELL_CHARS, "%s", "Event");
    return 2u;
}

void put(CascadePanelRow* out, uint32_t& n, uint32_t cap, uint32_t flags, const char* a,
         const char* b) {
    if (n >= cap) { return; }
    CascadePanelRow r;
    std::memset(&r, 0, sizeof r);
    r.kind = CASCADE_ROW_CELLS;
    r.flags = flags;
    std::snprintf(r.cells[0], CASCADE_PANEL_CELL_CHARS, "%s", a);
    std::snprintf(r.cells[1], CASCADE_PANEL_CELL_CHARS, "%s", b);
    out[n++] = r;
}

int32_t memoryPollRows(void*, CascadePanelRow* out, uint32_t cap) {
    registerCommands();
    takePresses();
    const bool keys = hostDrawsRowKeys();
    uint32_t n = 0;
    put(out, n, cap, 0u, "12:00", "Last chart");
    // The key is a key only when the host says it draws them, as the contract asks of a plugin.
    put(out, n, cap, keys ? CASCADE_ROW_KEY_FLAGS(kKeyId) : 0u, "12:01", "SET LEFT EDGE");
    return static_cast<int32_t>(n);
}

void instrumentDestroy(void*) {}

const CascadeInstrumentApi kInstrument = {
    static_cast<uint32_t>(sizeof(CascadeInstrumentApi)), "Fax Picture Recorder",
    CASCADE_INSTRUMENT_FAX, &instrumentCreate, &instrumentPollState, &memoryColumns,
    &memoryPollRows, &instrumentDestroy,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_IMAGE_DECODER, static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)),
     &kImageDecoder},
    {CASCADE_CAP_INSTRUMENT, static_cast<uint32_t>(sizeof(CascadeInstrumentApi)), &kInstrument},
    {CASCADE_CAP_IMAGE_INPUT, static_cast<uint32_t>(sizeof(CascadeImageInputApi)), &kImageInput},
    {CASCADE_CAP_HOST_CLIENT, static_cast<uint32_t>(sizeof(CascadeHostClientApi)), &kHostClient},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Fax Picture Fixture",
    "1.0.0",
    "FoxSDR tests",
    "PolyForm-Noncommercial-1.0.0",
    CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_INSTRUMENT | CASCADE_CAP_IMAGE_INPUT |
        CASCADE_CAP_HOST_CLIENT,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}
