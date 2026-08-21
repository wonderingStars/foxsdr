// Implementation of source/soapy_modules.hpp. See that header for the bug
// this exists to fix and why one step is not enough.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_modules.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace cascade::source {

namespace {

std::string envOrEmpty(const char* name) {
#ifdef _WIN32
    // getenv is fine here - these are ordinary path variables and the process
    // does not modify them concurrently. _dupenv_s would only add noise.
    char* v = nullptr;
    std::size_t n = 0;
    if (_dupenv_s(&v, &n, name) != 0 || v == nullptr) { return {}; }
    std::string out(v);
    std::free(v);
    return out;
#else
    const char* v = std::getenv(name);
    return (v != nullptr) ? std::string(v) : std::string();
#endif
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) { return b; }
    return (fs::path(a) / b).string();
}

// Case-insensitive on Windows, where two spellings of the same directory are
// the same directory and adding both would be noise in the diagnostics.
bool samePath(const std::string& a, const std::string& b) {
#ifdef _WIN32
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') { x = static_cast<char>(x - 'A' + 'a'); }
        if (y >= 'A' && y <= 'Z') { y = static_cast<char>(y - 'A' + 'a'); }
        if (x == '\\') { x = '/'; }
        if (y == '\\') { y = '/'; }
        if (x != y) { return false; }
    }
    return true;
#else
    return a == b;
#endif
}

}  // namespace

std::vector<std::pair<std::string, std::string>> candidateVendorRoots(
    const std::function<std::string(const char*)>& getenv) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto add = [&out](const std::string& name, const std::string& root) {
        if (!root.empty()) { out.emplace_back(name, root); }
    };

    // SOAPY_SDR_ROOT first: a user who set it has named their install
    // explicitly, and nothing this code guesses should outrank that.
    const std::string explicitRoot = getenv("SOAPY_SDR_ROOT");
    add("SOAPY_SDR_ROOT", explicitRoot);

    // PothosSDR: a machine-wide installer, so it lands in Program Files.
    // ProgramW6432 as well as ProgramFiles because a 32-bit process sees the
    // x86 directory under the latter, and while this build is 64-bit that has
    // been an expensive assumption elsewhere.
    const std::string pf = getenv("ProgramFiles");
    const std::string pf64 = getenv("ProgramW6432");
    add("PothosSDR", pf.empty() ? std::string() : joinPath(pf, "PothosSDR"));
    if (!pf64.empty() && !samePath(pf64, pf)) {
        add("PothosSDR", joinPath(pf64, "PothosSDR"));
    }

    // radioconda installs per user by default, and its SoapySDR tree sits
    // under Library/ in the conda layout.
    const std::string home = getenv("USERPROFILE");
    if (!home.empty()) {
        add("radioconda", joinPath(joinPath(home, "radioconda"), "Library"));
        add("miniforge", joinPath(joinPath(home, "miniforge3"), "Library"));
    }
    const std::string localApp = getenv("LOCALAPPDATA");
    if (!localApp.empty()) {
        add("radioconda", joinPath(joinPath(localApp, "radioconda"), "Library"));
    }
    const std::string programData = getenv("ProgramData");
    if (!programData.empty()) {
        add("radioconda", joinPath(joinPath(programData, "radioconda"), "Library"));
    }

    // A conda environment that is already active names itself, which covers
    // every layout the guesses above miss.
    const std::string conda = getenv("CONDA_PREFIX");
    if (!conda.empty()) { add("conda environment", joinPath(conda, "Library")); }

    return out;
}

std::vector<VendorRoot> resolveVendorRoots(
    const std::vector<std::pair<std::string, std::string>>& candidates, const std::string& abi,
    const std::function<bool(const std::string&)>& isDirectory) {
    std::vector<VendorRoot> out;
    for (const auto& [name, root] : candidates) {
        VendorRoot v;
        v.name = name;
        v.root = root;
        v.moduleDir = joinPath(joinPath(joinPath(root, "lib"), "SoapySDR"), "modules" + abi);
        v.binDir = joinPath(root, "bin");
        if (!isDirectory(v.moduleDir)) { continue; }

        // The same root reached by two candidate names (an active conda
        // environment that is also the default radioconda location) is one
        // installation, listed once.
        const bool already = std::any_of(out.begin(), out.end(), [&v](const VendorRoot& e) {
            return samePath(e.moduleDir, v.moduleDir);
        });
        if (already) { continue; }
        out.push_back(std::move(v));
    }
    return out;
}

std::string pluginPathWith(const std::string& existing, const std::vector<VendorRoot>& roots,
                           char separator) {
    // What is already there, so nothing is added twice and nothing is lost.
    std::vector<std::string> have;
    std::string cur;
    for (char c : existing) {
        if (c == separator) {
            if (!cur.empty()) { have.push_back(cur); }
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) { have.push_back(cur); }

    std::vector<std::string> added;
    for (const VendorRoot& v : roots) {
        const bool known = std::any_of(have.begin(), have.end(), [&v](const std::string& e) {
            return samePath(e, v.moduleDir);
        }) || std::any_of(added.begin(), added.end(), [&v](const std::string& e) {
            return samePath(e, v.moduleDir);
        });
        if (!known) { added.push_back(v.moduleDir); }
    }
    if (added.empty()) { return {}; }

    std::string out = existing;
    for (const std::string& a : added) {
        if (!out.empty() && out.back() != separator) { out.push_back(separator); }
        out += a;
    }
    return out;
}

const std::vector<VendorRoot>& ensureVendorModulesVisible(const std::string& abi) {
    static std::vector<VendorRoot> adopted;
    static std::once_flag once;
    std::call_once(once, [&abi]() {
        const auto exists = [](const std::string& p) {
            std::error_code ec;
            return fs::is_directory(fs::path(p), ec);
        };
        adopted = resolveVendorRoots(candidateVendorRoots(envOrEmpty), abi, exists);
        if (adopted.empty()) { return; }

#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        const std::string updated = pluginPathWith(envOrEmpty("SOAPY_SDR_PLUGIN_PATH"), adopted, sep);
        if (!updated.empty()) {
#ifdef _WIN32
            // SetEnvironmentVariable, not _putenv: SoapySDR reads the value
            // with GetEnvironmentVariable, and the CRT's copy of the
            // environment is not guaranteed to be the same one.
            ::SetEnvironmentVariableA("SOAPY_SDR_PLUGIN_PATH", updated.c_str());
#else
            ::setenv("SOAPY_SDR_PLUGIN_PATH", updated.c_str(), 1);
#endif
        }

#ifdef _WIN32
        // THE SECOND HALF, without which finding the module achieves nothing.
        // rtlsdrSupport.dll links rtlsdr.dll and libusb-1.0.dll from the
        // vendor's bin directory; if those cannot be resolved the module is
        // found and then fails to load, which reports as no device in exactly
        // the same way as not finding it at all.
        //
        // AddDllDirectory needs the process to be using the default-directories
        // search order, which is not the default for LoadLibrary without flags.
        // Setting it here affects only subsequent loads, and the modules are
        // loaded later, on first enumeration.
        //
        // IT IS ALSO PROCESS-WIDE AND ONE-WAY: from here on, every plain-name
        // load in this process - any thread, any library, ours or a
        // dependency's - searches the application directory, the
        // AddDllDirectory set and System32, and no longer PATH or the current
        // directory. That is a large blast radius for one line, so every load
        // in the tree that can run after it was enumerated and then MEASURED,
        // with a two-DLL probe loaded before and after this call in separate
        // processes (one-way means one process can only answer once):
        //
        //   - SoapySDR.dll itself is the CALLER, not a victim. The plain-name
        //     LoadLibraryA in SoapySource::runtimeAvailable runs first and
        //     pins the module; a plain-name load of an ALREADY-LOADED module
        //     still returns it afterwards (measured), which is also how the
        //     /DELAYLOAD:SoapySDR.dll thunks resolve on every later call. The
        //     ordering is the immunity - nothing may call into SoapySDR, or
        //     reach this function, ahead of that load.
        //   - Plugins load through PluginHost's LoadLibraryExW(absolute path,
        //     LOAD_WITH_ALTERED_SEARCH_PATH). Measured unaffected: a
        //     dependency sitting BESIDE the plugin still resolves after this
        //     call, which is exactly the arrangement plugin_host.cpp offers a
        //     plugin author. One shipped beside a PATH-only dependency would
        //     break, but none is: the example plugin imports KERNEL32 alone,
        //     and the CRT a /MD build would add lives in System32.
        //   - GLFW (user32, dwmapi, shcore, ntdll, dinput8, xinput*,
        //     opengl32, vulkan-1), Dear ImGui (opengl32) and PortAudio
        //     (dsound, avrt, ksuser) load by plain name, and every one of
        //     those is a System32 DLL, which stays in the search set.
        //   - The vendor modules, which SoapySDR loads with LoadLibrary(path)
        //     and no flags, are the reason for the loop below: their
        //     dependencies used to be found on PATH and now must come from the
        //     AddDllDirectory set, so every adopted root contributes its bin
        //     directory - pinned by the test of that name, because a module
        //     directory added without one would be the regression this line
        //     could actually cause.
        ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        for (const VendorRoot& v : adopted) {
            std::error_code ec;
            if (!fs::is_directory(fs::path(v.binDir), ec)) { continue; }
            const std::wstring w = fs::path(v.binDir).wstring();
            ::AddDllDirectory(w.c_str());
        }
#endif
    });
    return adopted;
}

}  // namespace cascade::source
