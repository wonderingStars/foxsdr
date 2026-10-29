// viewport_policy.hpp - whether a torn-off page gets a real operating system
// window, and what to say when it cannot.
//
// WHY THIS IS A FUNCTION AND NOT THREE `if`s IN run(). A tester on Windows
// 10.0.22621 at 144 dpi dragged a page out and the application died:
//
//     GLFW error 65543: WGL: Failed to create OpenGL context
//     ...access violation in glfwGetWin32Window
//
// Their driver refused to make a SECOND OpenGL context sharing the first, and
// the ImGui GLFW backend used the nullptr it got back. The backend is guarded
// now (see ImGui_ImplGlfw_ViewportWindowCreationFailures), but a guarded
// backend only stops the crash: what the application has to do afterwards is
// stop asking for windows it cannot have, and SAY SO, or the user is left with
// a page that has silently vanished.
//
// Three inputs decide it and all three can be proved without a display:
//   - FOXSDR_SINGLE_VIEWPORT, the developer switch the self-capture uses;
//   - the startup PROBE, which asks the driver for one 1x1 shared context
//     before any page can be torn off - a machine that will never allow it
//     should never be offered the option;
//   - the runtime FAILURE count, because a driver can start refusing later
//     (a GPU reset, a display change, a laptop switching adapters).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace cascade::gui {

enum class ViewportDecision {
    Enabled,              // torn-off pages get their own windows: the normal answer
    SingleEnvVar,         // FOXSDR_SINGLE_VIEWPORT is set
    ProbeFailed,          // the startup probe could not get a second shared context
    RuntimeFailure,       // refused while running, and refused often enough to stay off
    RuntimeFailureRetry,  // refused while running; tear-off will be offered again
};

// HOW MANY REFUSALS A SESSION GETS BEFORE TEAR-OFF STAYS OFF. Both field
// reports of the mid-session refusal (2026-10-29, two Intel UHD machines)
// carried Win32 error 0xc00705aa under the GLFW 65543 - the NTSTATUS wrapping
// of ERROR_NO_SYSTEM_RESOURCES, a transient shortage, in one log two seconds
// after the display came back from 13 minutes off - not a driver that cannot
// share a context at all (the startup probe, which stays sticky, catches that
// machine). So a refusal rests the feature rather than ends it: the first two
// are retried after a wait, the third is taken as the machine's answer.
inline constexpr int kViewportRefusalsBeforePermanent = 3;

// The wait before tear-off is offered again, by how many refusals the session
// has had: a minute after the first (a display waking or a GPU reset has long
// settled), ten after the second (twice was not bad luck), and below zero -
// never - once the count says it is permanent.
inline double viewportRetryDelaySec(int refusalsThisSession) {
    if (refusalsThisSession <= 0 || refusalsThisSession >= kViewportRefusalsBeforePermanent) {
        return -1.0;
    }
    return refusalsThisSession == 1 ? 60.0 : 600.0;
}

// The whole decision, pure.
//
// `singleEnv` is FOXSDR_SINGLE_VIEWPORT as getenv returns it - null when
// unset. Unset, empty and "0" all mean "not set", which is the reading the
// application has always used and which keeps `set FOXSDR_SINGLE_VIEWPORT=0`
// from quietly disabling the feature it looks like it is enabling.
//
// THE ORDER IS THE POINT. The switch wins over everything, so a self-capture
// run behaves the same on every machine. A failed probe beats the runtime
// count because with viewports never enabled the count can never rise. And a
// runtime failure is last because it is the only one that can arrive after the
// first frame - `refusalsThisSession` (how many times the session has been
// refused, this one included) is what decides whether that failure rests the
// feature or ends it.
inline ViewportDecision viewportDecision(const char* singleEnv, bool probeMadeASecondContext,
                                         int creationFailures, int refusalsThisSession) {
    if (singleEnv != nullptr && singleEnv[0] != '\0' && singleEnv[0] != '0') {
        return ViewportDecision::SingleEnvVar;
    }
    if (!probeMadeASecondContext) { return ViewportDecision::ProbeFailed; }
    if (creationFailures > 0) {
        return refusalsThisSession < kViewportRefusalsBeforePermanent
                   ? ViewportDecision::RuntimeFailureRetry
                   : ViewportDecision::RuntimeFailure;
    }
    return ViewportDecision::Enabled;
}

inline bool viewportsEnabled(ViewportDecision d) { return d == ViewportDecision::Enabled; }

// THE LINE THE LOG GETS, and it is the only account the user will ever have of
// why their map came back inside the main window. Null when viewports are on,
// because there is nothing to explain. The GLFW error number is quoted on
// purpose: it is what the log two lines above already says, and a reader
// matching the two is the whole reason this line exists.
inline const char* viewportDecisionLine(ViewportDecision d) {
    switch (d) {
        case ViewportDecision::Enabled:
            return nullptr;
        case ViewportDecision::SingleEnvVar:
            return "viewports: single (FOXSDR_SINGLE_VIEWPORT set)";
        case ViewportDecision::ProbeFailed:
            return "viewports: this display would not give a second OpenGL context (GLFW 65543) "
                   "- every page stays inside the main window";
        case ViewportDecision::RuntimeFailure:
            return "viewports: the display could not create a second OpenGL context (GLFW 65543) "
                   "- every page stays inside the main window for this session";
        case ViewportDecision::RuntimeFailureRetry:
            return "viewports: the display could not create a second OpenGL context (GLFW 65543) "
                   "- every page stays inside the main window, and tear-off will be offered again";
    }
    return nullptr;
}

}  // namespace cascade::gui
