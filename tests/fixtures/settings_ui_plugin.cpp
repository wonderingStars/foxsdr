// A REAL plugin module for tests/test_plugin_settings_ui.cpp and for the
// end-to-end settings-form run: a text decoder that declares
// CASCADE_CAP_SETTINGS_UI (0.99.43) and reads the values back the way a real
// plugin would - through the CascadeHostApi table it is handed in attach().
// It also declares CASCADE_CAP_RECEIVER_LOCATOR, modelling the FT8/PSK
// Reporter case this whole feature was built for: a plugin that needs both a
// user-typed field (a callsign) AND the receiver's own grid square, and must
// ask for the second one explicitly rather than receiving it just because it
// is a CASCADE_CAP_HOST_CLIENT.
//
// Built by tests/CMakeLists.txt as a MODULE (a .dll / .so the host loads with
// PluginHost::scan, exactly like a downloaded plugin), never linked into
// anything. It includes ONLY plugin_abi.h, as every third-party plugin must.
//
// Its form, in order - each entry is there to prove one rule:
//   callsign  TEXT, maxLength 12, placeholder, default ""      - the ordinary case
//   power     NUMBER, default "5"                               - a default that shows
//   report    BOOL, default ""                                  - a checkbox, off
//   future    kind 99                                           - a later host's kind:
//                                                                 SKIPPED, not refused
//   bigger    structSize + 8                                    - a later host's entry
//                                                                 size: SKIPPED
//   callsign  a second entry for the same key                   - SKIPPED (one box
//                                                                 per value)
//
// It also declares a PANEL (one empty column) purely so it owns a window of
// its own, where the host draws the form a third time.
//
// What it DECODES is its own settings: whenever settings_seq has moved since
// it last looked, the next poll_text returns one line
//   "SETTINGS seq=<n> callsign=<v> power=<v> report=<v>\n"
// read with settings_get (a NOT_FOUND key reads as its spec default, as the
// header tells a plugin to), so the test - and a person watching the Decoder
// output window - sees exactly what the plugin sees.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstring>
#include <new>

#include "plugin_abi.h"

namespace {

const CascadeHostApi* g_host = nullptr;

void hostAttach(const CascadeHostApi* h) { g_host = h; }

const CascadeHostClientApi kHostClient = {
    static_cast<uint32_t>(sizeof(CascadeHostClientApi)),
    &hostAttach,
};

const CascadeSettingSpec kSpecs[] = {
    {static_cast<uint32_t>(sizeof(CascadeSettingSpec)), "callsign", "Callsign",
     CASCADE_SETTING_TEXT, 12u, "e.g. G4ABC", ""},
    {static_cast<uint32_t>(sizeof(CascadeSettingSpec)), "power", "Power (W)",
     CASCADE_SETTING_NUMBER, 0u, "watts", "5"},
    {static_cast<uint32_t>(sizeof(CascadeSettingSpec)), "report", "Report spots",
     CASCADE_SETTING_BOOL, 0u, "", ""},
    {static_cast<uint32_t>(sizeof(CascadeSettingSpec)), "future", "From a later host", 99u, 0u,
     "", ""},
    {static_cast<uint32_t>(sizeof(CascadeSettingSpec) + 8u), "bigger", "Bigger entry",
     CASCADE_SETTING_TEXT, 8u, "", ""},
    {static_cast<uint32_t>(sizeof(CascadeSettingSpec)), "callsign", "Callsign again",
     CASCADE_SETTING_TEXT, 12u, "", ""},
};

const CascadeSettingsUiApi kSettingsUi = {
    static_cast<uint32_t>(sizeof(CascadeSettingsUiApi)),
    static_cast<uint32_t>(sizeof(kSpecs) / sizeof(kSpecs[0])),
    kSpecs,
};

struct Instance {
    uint64_t lastSeq = 0;
    char line[512] = {};
    size_t len = 0;
    size_t read = 0;
};

// settings_get, with NOT_FOUND read as the spec's default.
void readSetting(const char* key, const char* def, char* out, size_t cap) {
    out[0] = '\0';
    if (g_host == nullptr || !CASCADE_HOST_HAS(g_host, settings_get)) { return; }
    const int32_t rc = g_host->settings_get(g_host->ctx, key, out, cap);
    if (rc == CASCADE_API_NOT_FOUND || rc < 0) {
        std::snprintf(out, cap, "%s", def);
    }
}

void* create(uint32_t) { return new (std::nothrow) Instance(); }

void process(void*, const float*, size_t) {}

int32_t pollText(void* handle, char* buf, size_t cap) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || buf == nullptr || cap == 0u) { return 0; }
    if (s->read == s->len && g_host != nullptr && CASCADE_HOST_HAS(g_host, settings_seq)) {
        const uint64_t seq = g_host->settings_seq(g_host->ctx);
        if (seq != s->lastSeq) {
            s->lastSeq = seq;
            char call[64];
            char power[64];
            char report[64];
            readSetting("callsign", "", call, sizeof(call));
            readSetting("power", "5", power, sizeof(power));
            readSetting("report", "", report, sizeof(report));
            const int n = std::snprintf(s->line, sizeof(s->line),
                                        "SETTINGS seq=%llu callsign=%s power=%s report=%s\n",
                                        static_cast<unsigned long long>(seq), call, power,
                                        report);
            s->len = n > 0 ? static_cast<size_t>(n) : 0u;
            if (s->len >= sizeof(s->line)) { s->len = sizeof(s->line) - 1u; }
            s->read = 0;
        }
    }
    size_t avail = s->len - s->read;
    if (avail == 0u) { return 0; }
    if (avail > cap) { avail = cap; }
    std::memcpy(buf, s->line + s->read, avail);
    s->read += avail;
    return static_cast<int32_t>(avail);
}

void destroy(void* handle) { delete static_cast<Instance*>(handle); }

const CascadeDecoderApi kDecoder = {
    static_cast<uint32_t>(sizeof(CascadeDecoderApi)), 0u, &create, &process, &pollText, &destroy,
};

// A PANEL, so the plugin has a window of its own and the form's third surface
// (the folded "Plugin settings" header in that window) can be driven too. One
// column, no rows: the window exists to carry the form.
int g_panelHandle = 0;
void* panelCreate(void) { return &g_panelHandle; }
uint32_t panelColumns(void*, char headings[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS]) {
    std::snprintf(headings[0], CASCADE_PANEL_CELL_CHARS, "%s", "Status");
    return 1u;
}
int32_t panelPollRows(void*, CascadePanelRow*, uint32_t) { return 0; }
void panelDestroy(void*) {}

const CascadePanelApi kPanel = {
    static_cast<uint32_t>(sizeof(CascadePanelApi)), "Settings Fixture Panel", &panelCreate,
    &panelColumns, &panelPollRows, &panelDestroy,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_PANEL, static_cast<uint32_t>(sizeof(CascadePanelApi)), &kPanel},
    {CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder},
    {CASCADE_CAP_HOST_CLIENT, static_cast<uint32_t>(sizeof(CascadeHostClientApi)), &kHostClient},
    {CASCADE_CAP_SETTINGS_UI, static_cast<uint32_t>(sizeof(CascadeSettingsUiApi)), &kSettingsUi},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Settings Form Fixture",
    "1.0.0",
    "FoxSDR tests",
    "PolyForm-Noncommercial-1.0.0",
    CASCADE_CAP_DECODER | CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_SETTINGS_UI | CASCADE_CAP_PANEL |
        CASCADE_CAP_RECEIVER_LOCATOR,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}
