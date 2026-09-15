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
    Enabled,         // torn-off pages get their own windows: the normal answer
    SingleEnvVar,    // FOXSDR_SINGLE_VIEWPORT is set
    ProbeFailed,     // the startup probe could not get a second shared context
    RuntimeFailure,  // one was refused while the application was running
};

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
// first frame.
inline ViewportDecision viewportDecision(const char* singleEnv, bool probeMadeASecondContext,
                                         int creationFailures) {
    if (singleEnv != nullptr && singleEnv[0] != '\0' && singleEnv[0] != '0') {
        return ViewportDecision::SingleEnvVar;
    }
    if (!probeMadeASecondContext) { return ViewportDecision::ProbeFailed; }
    if (creationFailures > 0) { return ViewportDecision::RuntimeFailure; }
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
    }
    return nullptr;
}

}  // namespace cascade::gui
