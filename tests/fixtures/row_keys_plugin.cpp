// A REAL plugin module for tests/test_row_keys_app.cpp (0.99.77): a programme chooser written the
// way the IBOC Radio plugin's panel is - HD1..HD4 listed as rows, each one a KEY the user can
// press (CASCADE_ROW_FLAG_KEY, the command id in bits 8..15 of the row's flags) - against the
// plugin ABI and nothing else.
//
// THE TESTER'S WORDS (a US tester via the owner, 2026-10-13): "If I click on HD2 on the screen
// below, it should select that program, instead of having to go into the plugin settings and
// change the Programme to 1."
//
// It keeps the contract the header sets a plugin: it draws its rows as keys ONLY when get_state
// says the host does (CASCADE_STATE_ROW_KEYS), and registers its keys with add_command, so a host
// from before 0.99.77 shows the same rows as plain text and the plugin still works.
//
// Its panel ("Row Keys Panel", two columns), in order - each row is there to prove one rule:
//   PROGRAMMES                heading                              - never a key, whatever its flags
//   HD1  playing              CELLS, KEY(11), GOOD while chosen    - a registered id: pressable, and
//                                                                    the emphasis still applies
//   HD2  press to choose      CELLS, KEY(12)                       - a registered id: pressable
//   HD3  unregistered id      CELLS, KEY(99)                       - an id the plugin never added:
//                                                                    drawn as text, NOT pressable
//   HD4  plain row            CELLS, MUTED, id 13 in bits 8..15    - an old-style row whose upper bits
//                             but NOT the key flag                  are not zero (13 is registered):
//                                                                    text, unchanged
//
// The same rows are also the memory of an INSTRUMENT ("Row Keys Instrument", the generic face), the
// host's other table of rows, drawn by the same code.
//
// Commands: 1 "HD next" (a plate key, not a row), 11 "HD1", 12 "HD2", 13 "HD4" (no row is a key
// for it: it is the id left in the old-style row's upper bits). With ROW_KEYS_FIXTURE_MANY set
// it registers sixteen (ids 1..16, labelled "Key 1".."Key 16"), which is the cap from 0.99.77, so
// the module's page in Fitted modules can be checked for drawing all of them.
//
// A press is turned into a line: every id poll_command returns is appended, as "press <id>\n", to
// the file ROW_KEYS_FIXTURE_LOG names, so the test reads what the plugin was actually handed. A
// press of 11 or 12 also moves the GOOD emphasis to that programme (what the real plugin does by
// choosing it).
//
// ROW_KEYS_FIXTURE_NO_STATE_CHECK makes it behave as a plugin that did not read the bit (it marks
// its rows as keys regardless, and registers its commands regardless) - the case of a plugin
// written for a host that has the feature, loaded by one that does not.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "plugin_abi.h"

namespace {

const CascadeHostApi* g_host = nullptr;
bool g_registered = false;
uint32_t g_chosen = 11u;  // the programme that is "playing"

bool envOn(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

// Does the host say it draws a CASCADE_ROW_FLAG_KEY row as a key?
bool hostDrawsRowKeys() {
    if (envOn("ROW_KEYS_FIXTURE_NO_STATE_CHECK")) { return true; }
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
    bool all = true;
    if (envOn("ROW_KEYS_FIXTURE_MANY")) {
        for (uint32_t id = 1; id <= 16u; ++id) {
            char label[CASCADE_COMMAND_LABEL_CHARS];
            std::snprintf(label, sizeof label, "Key %u", id);
            all = all && g_host->add_command(g_host->ctx, id, label) == CASCADE_API_OK;
        }
    } else {
        all = g_host->add_command(g_host->ctx, 1u, "HD next") == CASCADE_API_OK && all;
        all = g_host->add_command(g_host->ctx, 11u, "HD1") == CASCADE_API_OK && all;
        all = g_host->add_command(g_host->ctx, 12u, "HD2") == CASCADE_API_OK && all;
        all = g_host->add_command(g_host->ctx, 13u, "HD4") == CASCADE_API_OK && all;
    }
    g_registered = all;
}

void hostAttach(const CascadeHostApi* h) {
    g_host = h;
    registerCommands();
}

const CascadeHostClientApi kHostClient = {
    static_cast<uint32_t>(sizeof(CascadeHostClientApi)),
    &hostAttach,
};

void recordPress(uint32_t id) {
    const char* path = std::getenv("ROW_KEYS_FIXTURE_LOG");
    if (path == nullptr || path[0] == '\0') { return; }
    std::FILE* f = std::fopen(path, "ab");
    if (f == nullptr) { return; }
    std::fprintf(f, "press %u\n", id);
    std::fclose(f);
}

void takePresses() {
    if (g_host == nullptr || !CASCADE_HOST_HAS(g_host, poll_command)) { return; }
    uint32_t id = 0;
    while (g_host->poll_command(g_host->ctx, &id) == 1) {
        recordPress(id);
        if (id == 11u || id == 12u) { g_chosen = id; }
    }
}

int g_panelHandle = 0;
void* panelCreate(void) { return &g_panelHandle; }

uint32_t panelColumns(void*, char headings[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS]) {
    std::snprintf(headings[0], CASCADE_PANEL_CELL_CHARS, "%s", "Programme");
    std::snprintf(headings[1], CASCADE_PANEL_CELL_CHARS, "%s", "State");
    return 2u;
}

void put(CascadePanelRow* out, uint32_t& n, uint32_t cap, uint32_t kind, uint32_t flags,
         const char* a, const char* b) {
    if (n >= cap) { return; }
    CascadePanelRow r;
    std::memset(&r, 0, sizeof r);
    r.kind = kind;
    r.flags = flags;
    std::snprintf(r.cells[0], CASCADE_PANEL_CELL_CHARS, "%s", a);
    std::snprintf(r.cells[1], CASCADE_PANEL_CELL_CHARS, "%s", b);
    out[n++] = r;
}

int32_t panelPollRows(void*, CascadePanelRow* out, uint32_t cap) {
    // The keys may be added a moment after attach (the host has to be ready to take them), so
    // ask again until they are in; add_command relabels an id it already has.
    registerCommands();
    takePresses();
    const bool keys = hostDrawsRowKeys();
    uint32_t n = 0;
    put(out, n, cap, CASCADE_ROW_HEADING, keys ? CASCADE_ROW_KEY_FLAGS(11u) : 0u,
        keys ? "PROGRAMMES (press one to choose)" : "PROGRAMMES", "");
    put(out, n, cap, CASCADE_ROW_CELLS,
        (keys ? CASCADE_ROW_KEY_FLAGS(11u) : 0u) | (g_chosen == 11u ? CASCADE_ROW_FLAG_GOOD : 0u),
        "HD1", g_chosen == 11u ? "playing" : "press to choose");
    put(out, n, cap, CASCADE_ROW_CELLS,
        (keys ? CASCADE_ROW_KEY_FLAGS(12u) : 0u) | (g_chosen == 12u ? CASCADE_ROW_FLAG_GOOD : 0u),
        "HD2", g_chosen == 12u ? "playing" : "press to choose");
    put(out, n, cap, CASCADE_ROW_CELLS, keys ? CASCADE_ROW_KEY_FLAGS(99u) : 0u, "HD3",
        "unregistered id");
    // An old-style row, with the bits above the emphasis flags NOT zero: id 13 (a command the
    // plugin DID register, so a host that read the id without the flag would make this a key) is in
    // bits 8..15 and CASCADE_ROW_FLAG_KEY is not set, as a plugin written before 0.99.77 may have
    // left them. The flag is what makes a row a key, so this one is text and pressing it hands the
    // plugin nothing.
    put(out, n, cap, CASCADE_ROW_CELLS,
        CASCADE_ROW_FLAG_MUTED | (13u << CASCADE_ROW_KEY_ID_SHIFT), "HD4", "plain row");
    return static_cast<int32_t>(n);
}

void panelDestroy(void*) {}

const CascadePanelApi kPanel = {
    static_cast<uint32_t>(sizeof(CascadePanelApi)), "Row Keys Panel", &panelCreate,
    &panelColumns, &panelPollRows, &panelDestroy,
};

// THE SAME ROWS AS AN INSTRUMENT'S MEMORY (CASCADE_CAP_INSTRUMENT, the generic face, which has
// nothing to read): the host draws an instrument's row feed with the panel's own drawing, so a key
// row beneath an instrument's face is the same key and is pressed the same way.
int32_t instrumentPollState(void*, CascadeInstrumentState*) { return 0; }

const CascadeInstrumentApi kInstrument = {
    static_cast<uint32_t>(sizeof(CascadeInstrumentApi)), "Row Keys Instrument",
    CASCADE_INSTRUMENT_GENERIC, &panelCreate, &instrumentPollState, &panelColumns, &panelPollRows,
    &panelDestroy,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_PANEL, static_cast<uint32_t>(sizeof(CascadePanelApi)), &kPanel},
    {CASCADE_CAP_INSTRUMENT, static_cast<uint32_t>(sizeof(CascadeInstrumentApi)), &kInstrument},
    {CASCADE_CAP_HOST_CLIENT, static_cast<uint32_t>(sizeof(CascadeHostClientApi)), &kHostClient},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Row Keys Fixture",
    "1.0.0",
    "FoxSDR tests",
    "PolyForm-Noncommercial-1.0.0",
    CASCADE_CAP_PANEL | CASCADE_CAP_INSTRUMENT | CASCADE_CAP_HOST_CLIENT,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}
