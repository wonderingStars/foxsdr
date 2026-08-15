// Implementation of core/plugin_host.hpp. See that header for the safety
// contract - in particular for what this code deliberately does NOT protect
// against.
//
// SPDX-License-Identifier: MIT
#include "core/plugin_host.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace cascade::core {

namespace {

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
    rec.decoder = desc->decoder;
    rec.nativeHandle = static_cast<void*>(mod);
    rec.loaded = true;
    return rec;
}

bool isNonEmpty(const char* s) { return s != nullptr && s[0] != '\0'; }

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
    if (desc->reserved != 0u) {
        return PluginRejection::ReservedNotZero;
    }
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
    if ((desc->capabilities & ~static_cast<uint32_t>(CASCADE_CAP_ALL_KNOWN)) != 0u) {
        return PluginRejection::UnknownCapability;
    }

    // With CASCADE_CAP_DECODER the only defined bit, the two checks above
    // (non-zero, no unknown bits) already imply the decoder bit is set. The
    // condition is written out anyway so that adding a second capability -
    // which necessarily bumps the ABI, since it grows the descriptor - needs
    // no rethink here.
    const bool claimsDecoder = (desc->capabilities & CASCADE_CAP_DECODER) != 0u;
    if (claimsDecoder && desc->decoder == nullptr) {
        return PluginRejection::MissingDecoderApi;
    }
    if (claimsDecoder) {
        const CascadeDecoderApi* d = desc->decoder;
        if (d->structSize != static_cast<uint32_t>(sizeof(CascadeDecoderApi))) {
            return PluginRejection::DecoderStructSizeMismatch;
        }
        if (d->requiredRateHz != 0u && (d->requiredRateHz < 1000u || d->requiredRateHz > 1000000u)) {
            return PluginRejection::DecoderRateOutOfRange;
        }
        if (d->create == nullptr || d->process == nullptr || d->poll_text == nullptr ||
            d->destroy == nullptr) {
            return PluginRejection::MissingDecoderFunction;
        }
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
            return "reserved descriptor field is not zero";
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
            s += " (host " + std::to_string(CASCADE_PLUGIN_ABI_VERSION) + ", plugin " +
                 std::to_string(desc->abiVersion) + ")";
            break;
        case PluginRejection::DescStructSizeMismatch:
            s += " (host " + std::to_string(sizeof(CascadePluginDesc)) + " bytes, plugin " +
                 std::to_string(desc->structSize) + ")";
            break;
        case PluginRejection::UnknownCapability: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%08X",
                          desc->capabilities & ~static_cast<unsigned>(CASCADE_CAP_ALL_KNOWN));
            s += std::string(" (unknown bits ") + buf + ")";
            break;
        }
        case PluginRejection::DecoderStructSizeMismatch:
            if (desc->decoder != nullptr) {
                s += " (host " + std::to_string(sizeof(CascadeDecoderApi)) + " bytes, plugin " +
                     std::to_string(desc->decoder->structSize) + ")";
            }
            break;
        default:
            break;
    }
    return s;
}

// ---------------------------------------------------------------------------
// PluginHost
// ---------------------------------------------------------------------------

PluginHost::~PluginHost() { unloadAll(); }

std::string PluginHost::defaultPluginDir() {
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
        p.loaded = false;
    }
    plugins_.clear();
}

}  // namespace cascade::core
