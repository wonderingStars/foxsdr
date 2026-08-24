// Implementation of core/plugin_host.hpp. See that header for the safety
// contract - in particular for what this code deliberately does NOT protect
// against.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_host.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// For PluginRepo::compareVersions. The catalogue's version ordering is REUSED
// here rather than reimplemented: a second comparator would eventually
// disagree with the one the update check uses about which of two versions is
// newer, and then the host would keep a plugin the updater calls stale.
#include "core/plugin_repo.hpp"

#if defined(_WIN32)
#include <process.h>  // _getpid, for the write probe's temp name
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace cascade::core {

namespace {

// A descriptor may declare a handful of capabilities; it may not declare
// thousands. The bound exists so a corrupt or hostile capabilityCount cannot
// make the host walk arbitrary memory looking for a table.
constexpr uint32_t kMaxCapabilityEntries = 32u;

// Finds the table a plugin supplied for one capability bit, or nullptr.
//
// Delegates to the accessor defined in plugin_abi.h rather than repeating the
// walk. That matters more than it looks: the plugins and their tests read the
// same array, and two implementations of "what counts as a malformed entry"
// would eventually disagree about some edge - a duplicate, a multi-bit entry,
// a null table - and the disagreement would show up as a plugin the tests
// accept and the host silently ignores. One definition, in the header both
// sides already include.
const void* findCapabilityTable(const CascadePluginDesc* desc, uint32_t bit) {
    return cascade_plugin_capability(desc, bit);
}

// ---------------------------------------------------------------------------
// Platform shim. Everything OS-specific is confined to these four functions
// so the loading logic below reads the same on both platforms.
// ---------------------------------------------------------------------------

#if defined(_WIN32)

using NativeModule = HMODULE;
constexpr NativeModule kNoModule = nullptr;

std::string lastOsError() {
    const DWORD err = ::GetLastError();
    std::string msg = std::error_code(static_cast<int>(err), std::system_category()).message();
    // Windows messages carry a trailing CRLF and a period; trim so the reason
    // reads as one line in a log or a tooltip.
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ')) {
        msg.pop_back();
    }
    return msg + " (error " + std::to_string(err) + ")";
}

NativeModule openModule(const fs::path& p) {
    // Suppress the OS's modal hard-error dialogs FOR THIS THREAD across the
    // load. Loading an untrusted file is exactly the situation where Windows
    // may put up a "Bad Image" or "no disk in drive" box, and a message box
    // nobody can click - a headless run, a service, a user whose window is
    // behind the main one - is an indefinite hang instead of a recorded
    // error. Thread-scoped, not SetErrorMode, so this never changes the
    // behaviour of the rest of the application.
    DWORD previousMode = 0;
    const BOOL haveOld = ::SetThreadErrorMode(
        SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previousMode);

    // LoadLibraryW, not the narrow form: a user's plugins directory can sit
    // under a name the ANSI codepage cannot spell.
    //
    // LOAD_WITH_ALTERED_SEARCH_PATH makes the plugin's OWN directory the
    // first place its dependencies are searched, so a plugin may ship the
    // DLLs it needs beside itself instead of demanding they be installed
    // system-wide. It does not widen the search for the host's own
    // dependencies, which are already loaded by this point.
    const NativeModule m =
        ::LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    // Restore without clobbering the failure the caller is about to report.
    const DWORD err = ::GetLastError();
    if (haveOld) {
        ::SetThreadErrorMode(previousMode, nullptr);
    }
    ::SetLastError(err);
    return m;
}

void closeModule(NativeModule m) { ::FreeLibrary(m); }

CascadePluginQueryFn findEntry(NativeModule m) {
    FARPROC p = ::GetProcAddress(m, CASCADE_PLUGIN_ENTRY_NAME);
    return reinterpret_cast<CascadePluginQueryFn>(reinterpret_cast<void*>(p));
}

#else

using NativeModule = void*;
constexpr NativeModule kNoModule = nullptr;

std::string lastOsError() {
    const char* e = ::dlerror();
    return e != nullptr ? std::string(e) : std::string("unknown dynamic loader error");
}

NativeModule openModule(const fs::path& p) {
    ::dlerror();  // clear any stale error so lastOsError() reports ours
    // RTLD_LOCAL: a plugin's symbols must not leak into the global namespace
    // where they could interpose on the host's or on another plugin's.
    return ::dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
}

void closeModule(NativeModule m) { ::dlclose(m); }

CascadePluginQueryFn findEntry(NativeModule m) {
    ::dlerror();
    void* p = ::dlsym(m, CASCADE_PLUGIN_ENTRY_NAME);
    return reinterpret_cast<CascadePluginQueryFn>(p);
}

#endif

// ---------------------------------------------------------------------------
// The one call the host makes into plugin code, guarded.
// ---------------------------------------------------------------------------

#if defined(_MSC_VER)
// MSVC forbids __try in a function that needs C++ object unwinding, hence
// this stripped-down helper: no locals with destructors, nothing but the
// call. Returns true on a clean return; on a structured exception it returns
// false and reports the code, and the caller unmaps the module rather than
// trying to carry on with a plugin that has already faulted.
//
// This catches C++ exceptions too (MSVC raises them as SEH), which is the
// point: the ABI forbids throwing across the boundary, and a plugin that
// does it anyway must not terminate the host during a routine directory
// scan.
bool callQueryGuarded(CascadePluginQueryFn fn, const CascadePluginDesc** out,
                      unsigned long* code) {
    __try {
        *out = fn(static_cast<uint32_t>(CASCADE_PLUGIN_ABI_VERSION));
        return true;
    } __except (*code = static_cast<unsigned long>(GetExceptionCode()),
                EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return false;
    }
}
#endif

// Calls the entry point. On success `out` holds whatever the plugin returned
// (possibly null - that is the plugin declining, not a failure of this call).
// On failure `error` explains and the module must be unmapped.
bool safeQuery(CascadePluginQueryFn fn, const CascadePluginDesc** out, std::string& error) {
#if defined(_MSC_VER)
    unsigned long code = 0;
    if (!callQueryGuarded(fn, out, &code)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08lX", code);
        error = std::string(CASCADE_PLUGIN_ENTRY_NAME) +
                " raised a structured exception (code " + buf +
                ") - the plugin is faulty and was not loaded";
        return false;
    }
    return true;
#else
    try {
        *out = fn(static_cast<uint32_t>(CASCADE_PLUGIN_ABI_VERSION));
        return true;
    } catch (...) {
        // An exception escaping a C entry point is undefined behaviour that
        // we happen to be able to observe here; refuse the plugin.
        *out = nullptr;
        error = std::string(CASCADE_PLUGIN_ENTRY_NAME) +
                " threw an exception - the plugin is faulty and was not loaded";
        return false;
    }
#endif
}

// Loads and validates one candidate file. Never throws; every failure path
// produces a record with `loaded == false` and a populated `error`.
LoadedPlugin loadOne(const fs::path& p) {
    LoadedPlugin rec;
    rec.path = p.string();

    NativeModule mod = openModule(p);
    if (mod == kNoModule) {
        rec.error = "cannot be loaded as a module: " + lastOsError();
        return rec;
    }

    CascadePluginQueryFn query = findEntry(mod);
    if (query == nullptr) {
        // The overwhelmingly likely cause is an unrelated DLL that happens to
        // live in the plugins directory, so say what was missing rather than
        // calling the file broken.
        rec.error = std::string("not a cascade plugin: it exports no ") +
                    CASCADE_PLUGIN_ENTRY_NAME;
        closeModule(mod);
        return rec;
    }

    const CascadePluginDesc* desc = nullptr;
    std::string queryError;
    if (!safeQuery(query, &desc, queryError)) {
        rec.error = queryError;
        closeModule(mod);
        return rec;
    }

    const PluginRejection why = validatePluginDesc(desc);
    if (why != PluginRejection::None) {
        rec.error = describePluginRejection(why, desc);
        // A refused plugin does not get to stay in the address space: its
        // DllMain already ran, but leaving it mapped would also leave its
        // static initialisers, threads and hooks alive for no benefit.
        closeModule(mod);
        return rec;
    }

    // Strings are COPIED, not aliased: the records outlive unloadAll(), and a
    // UI that still shows "you had plugin X loaded" after an unload must not
    // be reading a freed image.
    rec.name = desc->name;
    rec.version = desc->version;
    rec.author = desc->author;
    rec.licence = desc->licence;
    rec.capabilities = desc->capabilities;
    // Only the tables whose capability bit was DECLARED are propagated.
    // validatePluginDesc has already proven each declared bit has a usable
    // table; a table supplied without its bit is dropped here, so nothing
    // downstream can call into a facility the plugin did not claim.
    rec.decoder = (desc->capabilities & CASCADE_CAP_DECODER) != 0u
                      ? static_cast<const CascadeDecoderApi*>(
                            findCapabilityTable(desc, CASCADE_CAP_DECODER))
                      : nullptr;
    rec.iqDecoder = (desc->capabilities & CASCADE_CAP_IQ_DECODER) != 0u
                        ? static_cast<const CascadeIqDecoderApi*>(
                              findCapabilityTable(desc, CASCADE_CAP_IQ_DECODER))
                        : nullptr;
    rec.imageDecoder = (desc->capabilities & CASCADE_CAP_IMAGE_DECODER) != 0u
                           ? static_cast<const CascadeImageDecoderApi*>(
                                 findCapabilityTable(desc, CASCADE_CAP_IMAGE_DECODER))
                           : nullptr;
    rec.trackSource = (desc->capabilities & CASCADE_CAP_TRACK_SOURCE) != 0u
                          ? static_cast<const CascadeTrackSourceApi*>(
                                findCapabilityTable(desc, CASCADE_CAP_TRACK_SOURCE))
                          : nullptr;
    rec.panel = (desc->capabilities & CASCADE_CAP_PANEL) != 0u
                    ? static_cast<const CascadePanelApi*>(
                          findCapabilityTable(desc, CASCADE_CAP_PANEL))
                    : nullptr;
    rec.hostClient = (desc->capabilities & CASCADE_CAP_HOST_CLIENT) != 0u
                         ? static_cast<const CascadeHostClientApi*>(
                               findCapabilityTable(desc, CASCADE_CAP_HOST_CLIENT))
                         : nullptr;
    rec.preset = (desc->capabilities & CASCADE_CAP_PRESET) != 0u
                     ? static_cast<const CascadePresetApi*>(
                           findCapabilityTable(desc, CASCADE_CAP_PRESET))
                     : nullptr;
    rec.basemap = (desc->capabilities & CASCADE_CAP_BASEMAP) != 0u
                      ? static_cast<const CascadeBasemapApi*>(
                            findCapabilityTable(desc, CASCADE_CAP_BASEMAP))
                      : nullptr;
    rec.trackInfo = (desc->capabilities & CASCADE_CAP_TRACK_INFO) != 0u
                        ? static_cast<const CascadeTrackInfoApi*>(
                              findCapabilityTable(desc, CASCADE_CAP_TRACK_INFO))
                        : nullptr;
    rec.nativeHandle = static_cast<void*>(mod);
    rec.loaded = true;
    return rec;
}

bool isNonEmpty(const char* s) { return s != nullptr && s[0] != '\0'; }

// A rate an IQ decoder may legally ask for: 0 ("any rate") or inside the
// range the host's own DSP chain can be driven at.
//
// Written as a positive test (>= min && <= max) rather than as a negation,
// because the negation would ACCEPT NaN: every comparison against NaN is
// false, so "reject if r < min || r > max" lets a NaN straight through and
// into a divisor on the DSP thread. Infinity is excluded by the same
// comparisons. This is the whole reason the check is a named function.
bool iqRateOk(double r) {
    if (r == 0.0) {
        return true;
    }
    return r >= CASCADE_IQ_RATE_MIN_HZ && r <= CASCADE_IQ_RATE_MAX_HZ;
}

// The audio equivalent, for the ABI 3 image decoder, whose rates are doubles
// even when it asks for an audio stream. Positive test for the NaN reason
// documented on iqRateOk - the negation would accept NaN.
bool audioRateOk(double r) {
    if (r == 0.0) {
        return true;
    }
    return r >= static_cast<double>(CASCADE_AUDIO_RATE_MIN_HZ) &&
           r <= static_cast<double>(CASCADE_AUDIO_RATE_MAX_HZ);
}

}  // namespace

// ---------------------------------------------------------------------------
// Validation - pure, and the only place the compatibility policy lives
// ---------------------------------------------------------------------------

PluginRejection validatePluginDesc(const CascadePluginDesc* desc) {
    if (desc == nullptr) {
        return PluginRejection::NullDescriptor;
    }
    // Frozen-offset fields first (see the header's note on check order).
    if (desc->abiVersion != static_cast<uint32_t>(CASCADE_PLUGIN_ABI_VERSION)) {
        return PluginRejection::AbiVersionMismatch;
    }
    if (desc->structSize != static_cast<uint32_t>(sizeof(CascadePluginDesc))) {
        return PluginRejection::DescStructSizeMismatch;
    }
    // Only now is the rest of the struct known to be laid out as we expect.
    if (!isNonEmpty(desc->name)) {
        return PluginRejection::MissingName;
    }
    if (!isNonEmpty(desc->version)) {
        return PluginRejection::MissingVersion;
    }
    if (desc->author == nullptr) {
        return PluginRejection::MissingAuthor;
    }
    if (!isNonEmpty(desc->licence)) {
        return PluginRejection::MissingLicence;
    }
    if (desc->capabilities == 0u) {
        return PluginRejection::NoCapabilities;
    }
    if (desc->capabilityCount == 0u || desc->capabilityCount > kMaxCapabilityEntries) {
        return PluginRejection::CapabilityCountOutOfRange;
    }
    if (desc->capabilityTables == nullptr) {
        return PluginRejection::MissingCapabilityTables;
    }

    // ABI 3: unknown capability bits are IGNORED, not refused. The host reads
    // only tables it recognises and never dereferences one it does not, so a
    // plugin built for a later host is usable here for whatever it has in
    // common. What is NOT acceptable is a plugin that provides this host
    // nothing at all, which is caught by `usable` at the end.
    unsigned usable = 0;

    // One block per capability bit, each self-contained: a plugin may declare
    // any combination, and a table is examined only when its bit is set. The
    // checks inside a block run table-size FIRST for the same reason
    // abiVersion runs first at descriptor level - until the size agrees, the
    // remaining fields are at offsets this host has not confirmed.
    if ((desc->capabilities & CASCADE_CAP_DECODER) != 0u) {
        const void* raw = findCapabilityTable(desc, CASCADE_CAP_DECODER);
        if (raw == nullptr) {
            return PluginRejection::MissingDecoderApi;
        }
        const auto* d = static_cast<const CascadeDecoderApi*>(raw);
        if (d->structSize != static_cast<uint32_t>(sizeof(CascadeDecoderApi))) {
            return PluginRejection::DecoderStructSizeMismatch;
        }
        if (d->requiredRateHz != 0u && (d->requiredRateHz < CASCADE_AUDIO_RATE_MIN_HZ ||
                                        d->requiredRateHz > CASCADE_AUDIO_RATE_MAX_HZ)) {
            return PluginRejection::DecoderRateOutOfRange;
        }
        if (d->create == nullptr || d->process == nullptr || d->poll_text == nullptr ||
            d->destroy == nullptr) {
            return PluginRejection::MissingDecoderFunction;
        }
        ++usable;
    }

    if ((desc->capabilities & CASCADE_CAP_IQ_DECODER) != 0u) {
        const void* raw = findCapabilityTable(desc, CASCADE_CAP_IQ_DECODER);
        if (raw == nullptr) {
            return PluginRejection::MissingIqDecoderApi;
        }
        const auto* q = static_cast<const CascadeIqDecoderApi*>(raw);
        if (q->structSize != static_cast<uint32_t>(sizeof(CascadeIqDecoderApi))) {
            return PluginRejection::IqDecoderStructSizeMismatch;
        }
        if (!iqRateOk(q->requiredRateHz) || !iqRateOk(q->preferredRateHz)) {
            return PluginRejection::IqDecoderRateOutOfRange;
        }
        // retune is DELIBERATELY absent from this list: the ABI makes it
        // optional and the host null-checks it at every call site. Every
        // other pointer is mandatory.
        if (q->create == nullptr || q->process == nullptr || q->poll_text == nullptr ||
            q->destroy == nullptr) {
            return PluginRejection::MissingIqDecoderFunction;
        }
        ++usable;
    }

    if ((desc->capabilities & CASCADE_CAP_IMAGE_DECODER) != 0u) {
        const void* raw = findCapabilityTable(desc, CASCADE_CAP_IMAGE_DECODER);
        if (raw == nullptr) {
            return PluginRejection::MissingImageDecoderApi;
        }
        const auto* im = static_cast<const CascadeImageDecoderApi*>(raw);
        if (im->structSize != static_cast<uint32_t>(sizeof(CascadeImageDecoderApi))) {
            return PluginRejection::ImageDecoderStructSizeMismatch;
        }
        if (im->inputKind != CASCADE_INPUT_AUDIO && im->inputKind != CASCADE_INPUT_IQ) {
            return PluginRejection::ImageDecoderBadInputKind;
        }
        // The rate bounds depend on which stream it asked for: an SSTV decoder
        // wanting 44.1 kHz of audio and an LRPT decoder wanting 140 kS/s of
        // I/Q are both legitimate, and checking one against the other's range
        // would reject a correct plugin.
        const bool rateOk = (im->inputKind == CASCADE_INPUT_IQ)
                                ? (iqRateOk(im->requiredRateHz) && iqRateOk(im->preferredRateHz))
                                : (audioRateOk(im->requiredRateHz) &&
                                   audioRateOk(im->preferredRateHz));
        if (!rateOk) {
            return PluginRejection::ImageDecoderRateOutOfRange;
        }
        if (im->create == nullptr || im->process == nullptr || im->poll_image == nullptr ||
            im->release_image == nullptr || im->poll_text == nullptr || im->destroy == nullptr) {
            return PluginRejection::MissingImageDecoderFunction;
        }
        ++usable;
    }

    // A source of things to draw on the map. No rate to check: it is not fed
    // a signal, which is exactly why a satellite tracker can be a plugin.
    if ((desc->capabilities & CASCADE_CAP_TRACK_SOURCE) != 0u) {
        const void* raw = findCapabilityTable(desc, CASCADE_CAP_TRACK_SOURCE);
        if (raw == nullptr) {
            return PluginRejection::MissingTrackSourceApi;
        }
        const auto* t = static_cast<const CascadeTrackSourceApi*>(raw);
        if (t->structSize != static_cast<uint32_t>(sizeof(CascadeTrackSourceApi))) {
            return PluginRejection::TrackSourceStructSizeMismatch;
        }
        // poll_paths is DELIBERATELY absent from this list: most sources have
        // no polylines, the ABI makes it optional, and the host null-checks it
        // at the call site.
        if (t->create == nullptr || t->poll_tracks == nullptr || t->destroy == nullptr) {
            return PluginRejection::MissingTrackSourceFunction;
        }
        ++usable;
    }

    if ((desc->capabilities & CASCADE_CAP_PANEL) != 0u) {
        const void* raw = findCapabilityTable(desc, CASCADE_CAP_PANEL);
        if (raw == nullptr) {
            return PluginRejection::MissingPanelApi;
        }
        const auto* p = static_cast<const CascadePanelApi*>(raw);
        if (p->structSize != static_cast<uint32_t>(sizeof(CascadePanelApi))) {
            return PluginRejection::PanelStructSizeMismatch;
        }
        if (p->create == nullptr || p->columns == nullptr || p->poll_rows == nullptr ||
            p->destroy == nullptr) {
            return PluginRejection::MissingPanelFunction;
        }
        // The title is what the window is CALLED. An empty one would give the
        // user an anonymous window they cannot identify among several.
        if (!isNonEmpty(p->title)) {
            return PluginRejection::MissingPanelFunction;
        }
        ++usable;
    }

    if ((desc->capabilities & CASCADE_CAP_HOST_CLIENT) != 0u) {
        const void* raw = findCapabilityTable(desc, CASCADE_CAP_HOST_CLIENT);
        if (raw == nullptr) {
            return PluginRejection::MissingHostClientApi;
        }
        const auto* h = static_cast<const CascadeHostClientApi*>(raw);
        if (h->structSize != static_cast<uint32_t>(sizeof(CascadeHostClientApi))) {
            return PluginRejection::HostClientStructSizeMismatch;
        }
        if (h->attach == nullptr) {
            return PluginRejection::MissingHostClientFunction;
        }
        // NOT counted toward `usable`. Being able to ask the host for things
        // is not, on its own, doing anything for the user: a plugin that only
        // declares this contributes no decode, no map target and no window.
        // Counting it would let an empty plugin load and look functional.
    }

    if ((desc->capabilities & CASCADE_CAP_PRESET) != 0u) {
        const void* raw = findCapabilityTable(desc, CASCADE_CAP_PRESET);
        if (raw == nullptr) {
            return PluginRejection::MissingPresetApi;
        }
        const auto* p = static_cast<const CascadePresetApi*>(raw);
        if (p->structSize != static_cast<uint32_t>(sizeof(CascadePresetApi))) {
            return PluginRejection::PresetStructSizeMismatch;
        }
        if (p->count == nullptr || p->get == nullptr) {
            return PluginRejection::MissingPresetFunction;
        }
        // NOT counted toward `usable`, for the same reason as the host client:
        // telling the user where to tune is not, by itself, decoding anything.
        // A plugin offering only presets would load and look like a decoder.
    }

    if ((desc->capabilities & CASCADE_CAP_BASEMAP) != 0u) {
        const void* raw = findCapabilityTable(desc, CASCADE_CAP_BASEMAP);
        if (raw == nullptr) {
            return PluginRejection::MissingBasemapApi;
        }
        const auto* b = static_cast<const CascadeBasemapApi*>(raw);
        if (b->structSize != static_cast<uint32_t>(sizeof(CascadeBasemapApi))) {
            return PluginRejection::BasemapStructSizeMismatch;
        }
        if (b->create == nullptr || b->get_tile == nullptr || b->release_tile == nullptr ||
            b->poll_text == nullptr || b->destroy == nullptr) {
            return PluginRejection::MissingBasemapFunction;
        }
        // REFUSED WITHOUT ATTRIBUTION, and this is the one string in the whole
        // ABI whose absence is fatal rather than cosmetic. Map imagery is
        // overwhelmingly OpenStreetMap-derived and therefore ODbL, which
        // REQUIRES attribution; a basemap that could omit it would quietly put
        // the user in breach. Putting imagery in a plugin is what keeps that
        // obligation attached to whoever supplies the tiles, so the host will
        // not draw a basemap it cannot credit.
        if (!isNonEmpty(b->attribution)) {
            return PluginRejection::MissingBasemapAttribution;
        }
        if (b->tileSize < 64u || b->tileSize > CASCADE_TILE_SIZE_MAX ||
            b->maxZoom < b->minZoom || b->maxZoom > 24u) {
            return PluginRejection::BasemapBadTileSize;
        }
        ++usable;
    }

    if ((desc->capabilities & CASCADE_CAP_TRACK_INFO) != 0u) {
        const void* raw = findCapabilityTable(desc, CASCADE_CAP_TRACK_INFO);
        if (raw == nullptr) {
            return PluginRejection::MissingTrackInfoApi;
        }
        const auto* t = static_cast<const CascadeTrackInfoApi*>(raw);
        if (t->structSize != static_cast<uint32_t>(sizeof(CascadeTrackInfoApi))) {
            return PluginRejection::TrackInfoStructSizeMismatch;
        }
        if (t->create == nullptr || t->get_info == nullptr || t->release_info == nullptr ||
            t->poll_text == nullptr || t->destroy == nullptr) {
            return PluginRejection::MissingTrackInfoFunction;
        }
        // Counted toward `usable`: enriching the targets on the map is a real
        // job on its own, unlike a preset (which only points at one) or the
        // host-client table (which is a permission, not a function).
        ++usable;
    }

    // Every bit it declared is one this host has never heard of. Loading it
    // would put a plugin in the list that can never do anything, so say what
    // is actually wrong instead: it was built for a host newer than this one.
    if (usable == 0) {
        return PluginRejection::NoUsableCapability;
    }
    return PluginRejection::None;
}

const char* pluginRejectionMessage(PluginRejection r) {
    switch (r) {
        case PluginRejection::None:
            return "accepted";
        case PluginRejection::NullDescriptor:
            return "the plugin declined this host version (its query returned null)";
        case PluginRejection::AbiVersionMismatch:
            return "built against a different plugin ABI version";
        case PluginRejection::DescStructSizeMismatch:
            return "plugin descriptor size does not match this host's";
        case PluginRejection::ReservedNotZero:
            // Retained so the enum stays stable for callers; ABI 3 removed the
            // reserved field, so nothing produces this any more.
            return "reserved descriptor field is not zero";
        case PluginRejection::CapabilityCountOutOfRange:
            return "descriptor declares an implausible number of capability tables";
        case PluginRejection::MissingCapabilityTables:
            return "descriptor declares capabilities but supplies no capability table";
        case PluginRejection::NoUsableCapability:
            return "every capability it declares is one this host does not know - it was "
                   "built for a newer version of FoxSDR";
        case PluginRejection::MissingImageDecoderApi:
            return "declares CASCADE_CAP_IMAGE_DECODER but supplies no image decoder table";
        case PluginRejection::ImageDecoderStructSizeMismatch:
            return "image decoder table size does not match this host's";
        case PluginRejection::ImageDecoderBadInputKind:
            return "image decoder declares neither audio nor IQ input";
        case PluginRejection::ImageDecoderRateOutOfRange:
            return "image decoder asks for a sample rate this host cannot deliver";
        case PluginRejection::MissingImageDecoderFunction:
            return "image decoder table has a null function pointer";
        case PluginRejection::MissingTrackSourceApi:
            return "declares CASCADE_CAP_TRACK_SOURCE but supplies no track table";
        case PluginRejection::TrackSourceStructSizeMismatch:
            return "track source table size does not match this host's";
        case PluginRejection::MissingTrackSourceFunction:
            return "track source table has a null function pointer";
        case PluginRejection::MissingPanelApi:
            return "declares CASCADE_CAP_PANEL but supplies no panel table";
        case PluginRejection::PanelStructSizeMismatch:
            return "panel table size does not match this host's";
        case PluginRejection::PanelBadColumnCount:
            return "panel declares an impossible number of columns";
        case PluginRejection::MissingPanelFunction:
            return "panel table has a null function pointer or no window title";
        case PluginRejection::MissingHostClientApi:
            return "declares CASCADE_CAP_HOST_CLIENT but supplies no table";
        case PluginRejection::HostClientStructSizeMismatch:
            return "host-client table size does not match this host's";
        case PluginRejection::MissingHostClientFunction:
            return "host-client table has a null attach pointer";
        case PluginRejection::MissingPresetApi:
            return "declares CASCADE_CAP_PRESET but supplies no table";
        case PluginRejection::PresetStructSizeMismatch:
            return "preset table size does not match this host's";
        case PluginRejection::MissingPresetFunction:
            return "preset table has a null function pointer";
        case PluginRejection::MissingBasemapApi:
            return "declares CASCADE_CAP_BASEMAP but supplies no table";
        case PluginRejection::BasemapStructSizeMismatch:
            return "basemap table size does not match this host's";
        case PluginRejection::MissingBasemapFunction:
            return "basemap table has a null function pointer";
        case PluginRejection::MissingBasemapAttribution:
            return "basemap supplies no attribution string, which map imagery "
                   "licences require";
        case PluginRejection::BasemapBadTileSize:
            return "basemap declares an unusable tile size or zoom range";
        case PluginRejection::MissingTrackInfoApi:
            return "declares CASCADE_CAP_TRACK_INFO but supplies no table";
        case PluginRejection::TrackInfoStructSizeMismatch:
            return "track-info table size does not match this host's";
        case PluginRejection::MissingTrackInfoFunction:
            return "track-info table has a null function pointer";
        case PluginRejection::MissingName:
            return "descriptor has no name";
        case PluginRejection::MissingVersion:
            return "descriptor has no version string";
        case PluginRejection::MissingAuthor:
            return "descriptor has a null author (use \"\" if unknown)";
        case PluginRejection::MissingLicence:
            return "descriptor declares no licence, which this host requires";
        case PluginRejection::NoCapabilities:
            return "descriptor declares no capabilities";
        case PluginRejection::UnknownCapability:
            return "descriptor declares a capability this host does not know";
        case PluginRejection::MissingDecoderApi:
            return "declares CASCADE_CAP_DECODER but supplies no decoder table";
        case PluginRejection::DecoderStructSizeMismatch:
            return "decoder table size does not match this host's";
        case PluginRejection::DecoderRateOutOfRange:
            return "decoder requests an implausible audio sample rate";
        case PluginRejection::MissingDecoderFunction:
            return "decoder table has a null function pointer";
        case PluginRejection::MissingIqDecoderApi:
            return "declares CASCADE_CAP_IQ_DECODER but supplies no IQ decoder table";
        case PluginRejection::IqDecoderStructSizeMismatch:
            return "IQ decoder table size does not match this host's";
        case PluginRejection::IqDecoderRateOutOfRange:
            return "IQ decoder requests a sample rate this host cannot deliver";
        case PluginRejection::MissingIqDecoderFunction:
            return "IQ decoder table has a null function pointer (only retune may be null)";
    }
    return "unknown rejection";
}

std::string describePluginRejection(PluginRejection r, const CascadePluginDesc* desc) {
    std::string s = pluginRejectionMessage(r);
    if (desc == nullptr) {
        return s;
    }
    switch (r) {
        case PluginRejection::AbiVersionMismatch:
            // BOTH numbers, spelled out. This is the message a third-party
            // author sees the day the host's ABI moves - "host requires
            // exactly 2, plugin reports 1" tells them what to do, where a
            // bare "incompatible plugin" starts a support thread.
            s += " (host requires exactly " + std::to_string(CASCADE_PLUGIN_ABI_VERSION) +
                 ", plugin reports " + std::to_string(desc->abiVersion) +
                 ") - rebuild the plugin against this host's plugin_abi.h";
            break;
        case PluginRejection::DescStructSizeMismatch:
            s += " (host " + std::to_string(sizeof(CascadePluginDesc)) + " bytes, plugin " +
                 std::to_string(desc->structSize) + ")";
            break;
        case PluginRejection::NoUsableCapability: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%08X",
                          desc->capabilities & ~static_cast<unsigned>(CASCADE_CAP_ALL_KNOWN));
            s += std::string(" (it declares only ") + buf +
                 ", and this host knows none of those)";
            break;
        }
        case PluginRejection::DecoderStructSizeMismatch: {
            const auto* d = static_cast<const CascadeDecoderApi*>(
                findCapabilityTable(desc, CASCADE_CAP_DECODER));
            if (d != nullptr) {
                s += " (host " + std::to_string(sizeof(CascadeDecoderApi)) + " bytes, plugin " +
                     std::to_string(d->structSize) + ")";
            }
            break;
        }
        case PluginRejection::IqDecoderStructSizeMismatch: {
            const auto* q = static_cast<const CascadeIqDecoderApi*>(
                findCapabilityTable(desc, CASCADE_CAP_IQ_DECODER));
            if (q != nullptr) {
                s += " (host " + std::to_string(sizeof(CascadeIqDecoderApi)) + " bytes, plugin " +
                     std::to_string(q->structSize) + ")";
            }
            break;
        }
        case PluginRejection::IqDecoderRateOutOfRange: {
            const auto* q = static_cast<const CascadeIqDecoderApi*>(
                findCapabilityTable(desc, CASCADE_CAP_IQ_DECODER));
            if (q != nullptr) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              " (required %.6g Hz, preferred %.6g Hz; 0 or %.6g..%.6g Hz)",
                              q->requiredRateHz, q->preferredRateHz, CASCADE_IQ_RATE_MIN_HZ,
                              CASCADE_IQ_RATE_MAX_HZ);
                s += buf;
            }
            break;
        }
        case PluginRejection::ImageDecoderStructSizeMismatch: {
            const auto* im = static_cast<const CascadeImageDecoderApi*>(
                findCapabilityTable(desc, CASCADE_CAP_IMAGE_DECODER));
            if (im != nullptr) {
                s += " (host " + std::to_string(sizeof(CascadeImageDecoderApi)) +
                     " bytes, plugin " + std::to_string(im->structSize) + ")";
            }
            break;
        }
        case PluginRejection::CapabilityCountOutOfRange:
            s += " (" + std::to_string(desc->capabilityCount) + " declared, limit " +
                 std::to_string(kMaxCapabilityEntries) + ")";
            break;
        default:
            break;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Per-plugin identity, and the set of plugins the user has stopped
// ---------------------------------------------------------------------------

std::size_t resolveDuplicatePlugins(std::vector<LoadedPlugin>& records) {
    // Pass one: the winner for each declared id.
    //
    // Only LOADED records take part. A refused candidate never had its
    // descriptor read, so it has no id at all, and collapsing several of them
    // would hide one broken file behind another.
    //
    // A linear scan of a handful of ids is the right data structure here: a
    // plugins directory holds a few files, and this keeps the FIRST-wins
    // tie-break obvious rather than hiding it in a map's ordering.
    std::vector<std::pair<std::string, std::size_t>> winners;  // id -> index
    for (std::size_t i = 0; i < records.size(); ++i) {
        const LoadedPlugin& r = records[i];
        if (!r.loaded || r.name.empty()) {
            continue;
        }
        bool seen = false;
        for (std::pair<std::string, std::size_t>& w : winners) {
            if (w.first != r.name) {
                continue;
            }
            seen = true;
            // STRICTLY greater, so an equal version leaves the incumbent in
            // place: the first record wins the tie, and scan() sorted its
            // candidates by path, so that is the same file on every run.
            if (PluginRepo::compareVersions(r.version, records[w.second].version) > 0) {
                w.second = i;
            }
            break;
        }
        if (!seen) {
            winners.emplace_back(r.name, i);
        }
    }

    // Pass two: turn off every loaded record that is not its id's winner.
    std::size_t skipped = 0;
    for (std::size_t i = 0; i < records.size(); ++i) {
        LoadedPlugin& r = records[i];
        if (!r.loaded || r.name.empty()) {
            continue;
        }
        std::size_t keep = i;
        for (const std::pair<std::string, std::size_t>& w : winners) {
            if (w.first == r.name) {
                keep = w.second;
                break;
            }
        }
        if (keep == i) {
            continue;
        }

        const LoadedPlugin& kept = records[keep];
        // SHORT AND FACTUAL, and it has to carry three things: which plugin
        // this is, which file is running instead, and what the user may do
        // about the file in front of them. The Plugins section prints this
        // under the file's own name, and offers Remove on the same row.
        r.error = "Ignored: another copy of \"" + r.name + "\" is installed. Version " +
                  kept.version + " in " + pluginKey(kept) + " is loaded; this file is " +
                  r.version + " and is not running. It can be deleted with Remove.";
        r.loaded = false;
        // Every borrowed pointer goes: they point into an image the caller is
        // about to unmap. nativeHandle is deliberately LEFT SET - it is how
        // the caller knows there is a module here to unmap.
        r.decoder = nullptr;
        r.iqDecoder = nullptr;
        r.imageDecoder = nullptr;
        r.trackSource = nullptr;
        r.panel = nullptr;
        r.hostClient = nullptr;
        r.preset = nullptr;
        r.basemap = nullptr;
        r.trackInfo = nullptr;
        ++skipped;
    }
    return skipped;
}

std::string pluginKey(const LoadedPlugin& p) {
    return fs::path(p.path).filename().string();
}

bool PluginStopSet::contains(const std::string& key) const {
    // An empty key is what a record with no path produces, and it must never
    // match: otherwise one stray "" in a hand-edited config would stop every
    // path-less plugin at once.
    if (key.empty()) { return false; }
    return std::find(keys_.begin(), keys_.end(), key) != keys_.end();
}

// ---------------------------------------------------------------------------
// PluginHost
// ---------------------------------------------------------------------------

PluginHost::~PluginHost() { unloadAll(); }

std::string PluginHost::exePluginDir() {
#if defined(_WIN32)
    // MAX_PATH is not the limit on modern Windows; grow until it fits rather
    // than truncating an installation under a deep path.
    std::wstring buf(512, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return "plugins";
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        if (buf.size() > 65536) {
            return "plugins";
        }
        buf.resize(buf.size() * 2);
    }
    const fs::path exe(buf);
    return (exe.parent_path() / "plugins").string();
#else
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec || exe.empty()) {
        return "plugins";
    }
    return (exe.parent_path() / "plugins").string();
#endif
}

std::string PluginHost::userPluginDir() {
#if defined(_WIN32)
    // LOCALAPPDATA, not APPDATA, and the difference matters: APPDATA roams to
    // every machine the user signs in to, and a plugin is a native binary
    // built for one architecture. ConfigStore may roam its JSON; this cannot.
    const char* base = std::getenv("LOCALAPPDATA");
    if (base == nullptr || *base == '\0') {
        base = std::getenv("APPDATA");  // ancient or stripped environments
    }
    if (base == nullptr || *base == '\0') {
        return {};  // no per-user location exists; the caller keeps the exe one
    }
    return (fs::path(base) / "foxsdr" / "plugins").string();
#else
    // XDG_DATA_HOME rather than XDG_CONFIG_HOME for the same reason: this is
    // installed program data, not configuration.
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg != nullptr && *xdg != '\0') {
        return (fs::path(xdg) / "foxsdr" / "plugins").string();
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
        return (fs::path(home) / ".local" / "share" / "foxsdr" / "plugins").string();
    }
    return {};
#endif
}

bool PluginHost::directoryIsWritable(const std::string& dir) {
    std::error_code ec;
    if (dir.empty()) {
        return false;
    }
    const fs::path p(dir);

    // Probe the directory when it is there, and the parent that would have to
    // hold it when it is not. Probing the parent is what makes a FIRST run
    // answer correctly: the plugins directory does not exist yet in either the
    // portable case or the installed one, and the two must not look alike.
    fs::path probeDir = p;
    if (!fs::is_directory(probeDir, ec)) {
        probeDir = p.parent_path();
        if (probeDir.empty()) {
            probeDir = fs::path(".");
        }
        ec.clear();
        if (!fs::is_directory(probeDir, ec)) {
            return false;
        }
    }

    // THE ONLY HONEST TEST IS A WRITE. An installed program directory is
    // readable, listable and traversable, and reports itself a perfectly good
    // directory right up to the moment a file is created in it.
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    fs::path probe = probeDir / (".foxsdr-write-probe." + std::to_string(pid));
    ec.clear();
    fs::remove(probe, ec);  // debris from a killed earlier run
    bool ok = false;
    {
        std::ofstream f(probe, std::ios::binary | std::ios::trunc);
        if (f) {
            f.put('x');
            f.flush();
            ok = static_cast<bool>(f);
        }
    }
    ec.clear();
    fs::remove(probe, ec);  // leaves nothing behind either way
    return ok;
}

std::string PluginHost::choosePluginDir(const std::string& exeDir, const std::string& userDir,
                                        bool exeDirWritable) {
    if (exeDirWritable) {
        return exeDir;
    }
    if (userDir.empty()) {
        return exeDir;
    }
    return userDir;
}

std::string PluginHost::defaultPluginDir() {
    const std::string exeDir = exePluginDir();
    return choosePluginDir(exeDir, userPluginDir(), directoryIsWritable(exeDir));
}

bool PluginHost::hasPluginExtension(const std::string& filename) {
    const auto endsWith = [&filename](const char* ext, bool caseInsensitive) {
        const std::size_t n = std::char_traits<char>::length(ext);
        if (filename.size() <= n) {
            return false;  // ".dll" alone is not a plugin, it is an extension
        }
        std::size_t off = filename.size() - n;
        for (std::size_t i = 0; i < n; ++i) {
            char a = filename[off + i];
            char b = ext[i];
            if (caseInsensitive) {
                if (a >= 'A' && a <= 'Z') {
                    a = static_cast<char>(a - 'A' + 'a');
                }
            }
            if (a != b) {
                return false;
            }
        }
        return true;
    };
#if defined(_WIN32)
    // Windows filenames are case-insensitive, so "MyPlugin.DLL" must match.
    return endsWith(".dll", true);
#elif defined(__APPLE__)
    return endsWith(".dylib", false) || endsWith(".so", false);
#else
    return endsWith(".so", false);
#endif
}

void PluginHost::scan(const std::string& dir) {
    unloadAll();
    directory_ = dir;

    std::error_code ec;
    // The non-throwing overloads throughout: a missing plugins directory is
    // the normal case for most users and must not cost an exception, let
    // alone escape to the caller.
    if (!fs::is_directory(dir, ec) || ec) {
        return;
    }

    std::vector<fs::path> candidates;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return;
    }
    const fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;  // a directory that changed under us: keep what we found
        }
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc) {
            continue;
        }
        if (!hasPluginExtension(it->path().filename().string())) {
            continue;
        }
        candidates.push_back(it->path());
    }

    // Deterministic order: directory iteration order is filesystem-defined,
    // and a UI list that reshuffles between runs looks broken.
    std::sort(candidates.begin(), candidates.end());

    plugins_.reserve(candidates.size());
    for (const fs::path& p : candidates) {
        plugins_.push_back(loadOne(p));
    }

    // ONE VERSION OF A PLUGIN RUNS, NEVER TWO. See resolveDuplicatePlugins in
    // the header for the whole policy and for why this deletes nothing.
    if (resolveDuplicatePlugins(plugins_) > 0) {
        // Unmap what it turned off. A skipped module is exactly as unwelcome
        // in the address space as a refused one: its DllMain has already run,
        // but leaving it mapped would leave its static initialisers, threads
        // and hooks alive for no benefit - and it is the older code.
        for (LoadedPlugin& p : plugins_) {
            if (!p.loaded && p.nativeHandle != nullptr) {
                closeModule(static_cast<NativeModule>(p.nativeHandle));
                p.nativeHandle = nullptr;
            }
        }
    }
}

void PluginHost::scanDefault() { scan(defaultPluginDir()); }

std::size_t PluginHost::loadedCount() const {
    std::size_t n = 0;
    for (const LoadedPlugin& p : plugins_) {
        if (p.loaded) {
            ++n;
        }
    }
    return n;
}

void PluginHost::unloadAll() {
    for (LoadedPlugin& p : plugins_) {
        if (p.loaded && p.nativeHandle != nullptr) {
            closeModule(static_cast<NativeModule>(p.nativeHandle));
        }
        p.nativeHandle = nullptr;
        p.decoder = nullptr;
        p.iqDecoder = nullptr;
        p.loaded = false;
    }
    plugins_.clear();
}

}  // namespace cascade::core
