// plugin_reload_phases.hpp - the fixed names of the steps of a plugin reload.
//
// WHY THIS EXISTS. Field report 7591C65EFB69DD94 (0.99.81, Windows 10): the GUI
// thread stalled past the watchdog's excuse cap with frame scope
// `plugins-reload`, and the report could not say WHICH of the reload's waits had
// held it. The stalled thread's stack ended in system modules (an unwind broken
// under a Detours hook that the report's own `foreign-modules:` line named), the
// PhaseClock's one log line is only written when the reload ENDS - a reload
// still stuck has written nothing - and docs/DIAGNOSTICS.md ("A plugin rescan
// runs on the GUI thread") lists six different owners that wait could have
// belonged to, with six different fixes. The design decision recorded there
// waits for exactly this evidence: "a change of that weight is made for the
// phase the field shows, not for the one the reading suggests". These names are
// that field's closed vocabulary.
//
// WHO USES IT. AppWindow's reload path opens each step under one of these names
// on a PUBLISHING PhaseClock (core/phase_clock.hpp), the freeze watchdog reads
// the published name when it files a report (the `reload-phase:` line,
// core/hang_watchdog.cpp), and the uploader forwards it only when it is one of
// these words (core/crash_upload.cpp) - the same closed-list rule the
// `frame-scope` field follows, so a hand-edited report cannot make the field
// carry text from the machine.
//
// FIXED WORDS ONLY. A name is a step of this program, never a plugin, a file, a
// path, a radio or a frequency (PRIVACY.md). The seven step names are the same
// words the reload's own log line has used since 0.99.63, so the log and the
// report describe one reload in one vocabulary; `check` is the listing
// rescanPluginsIfChanged makes to decide whether a reload is needed at all,
// which runs under the same frame scope but is not a lap of the reload's log
// line.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PLUGIN_RELOAD_PHASES_HPP
#define CASCADE_CORE_PLUGIN_RELOAD_PHASES_HPP

#include <cstring>

namespace cascade::core {

// The steps, in the order the reload runs them (docs/DIAGNOSTICS.md, "A plugin
// rescan runs on the GUI thread" - the same table). Each is a different kind of
// wait with a different owner.
inline constexpr const char* kReloadPhaseCheck = "check";            // the listing that decides
inline constexpr const char* kReloadPhaseDecoders = "decoders";      // every decoder's destroy()
inline constexpr const char* kReloadPhasePatch = "patch";            // flushNow(): the DSP thread and the patch radios' readers
inline constexpr const char* kReloadPhasePanels = "panels and map";  // panel/track destroy(), the basemap's worker joins
inline constexpr const char* kReloadPhaseUnload = "unload";          // FreeLibrary: DllMain under the loader lock
inline constexpr const char* kReloadPhaseInventory = "inventory";    // the folder listed, every file re-hashed
inline constexpr const char* kReloadPhaseLoad = "load";              // LoadLibrary of every module
inline constexpr const char* kReloadPhaseRestart = "restart";        // the log lines, the instances created again

inline constexpr const char* const kPluginReloadPhaseNames[] = {
    kReloadPhaseCheck,  kReloadPhaseDecoders,  kReloadPhasePatch, kReloadPhasePanels,
    kReloadPhaseUnload, kReloadPhaseInventory, kReloadPhaseLoad,  kReloadPhaseRestart};

inline constexpr int kPluginReloadPhaseCount =
    static_cast<int>(sizeof(kPluginReloadPhaseNames) / sizeof(kPluginReloadPhaseNames[0]));

// Whether `name` is one of the steps above, exactly - the validator the uploader
// applies before the word becomes bytes that leave the machine. Case-sensitive,
// no trimming: the writers emit these words verbatim, so anything else is not
// theirs.
inline bool isPluginReloadPhaseName(const char* name) {
    if (name == nullptr) { return false; }
    for (const char* p : kPluginReloadPhaseNames) {
        if (std::strcmp(p, name) == 0) { return true; }
    }
    return false;
}

}  // namespace cascade::core

#endif  // CASCADE_CORE_PLUGIN_RELOAD_PHASES_HPP
