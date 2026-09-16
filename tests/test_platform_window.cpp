// test_platform_window.cpp - the desktop PlatformWindow, driven for real.
//
// WHAT THIS IS FOR. gui/platform_window.hpp is a seam: 60 glfw* call sites from
// app_window.cpp now reach GLFW through it, so an Android EGL shell can take
// GLFW's place without app_window.cpp changing. A seam is only as good as the
// implementation behind it, and the failure mode a seam invites is the quiet
// one - a method that compiles, returns a plausible value, and does nothing.
// So every method that can be checked WITHOUT A USER is checked here against a
// real window: the size it was asked for, the title it was given, a clipboard
// round trip, a clock that goes forwards, a close request that is visible to
// shouldClose(), and a cursor that remembers what it was told.
//
// What is deliberately NOT here: anything that needs a window manager, a
// pointer, a second monitor or a person. iconify/maximise/restore, focus,
// attention and the torn-off-page methods all depend on a desktop this suite
// does not have (it runs under Xvfb with no WM), and a check that cannot fail
// is decoration. They are exercised by the application itself - app_smoke
// drives the real frame loop through the same interface, and a method that
// crashed or did nothing there would take the smoke test with it.
//
// NO GLFW HEADER IS INCLUDED BELOW, and that is part of the test: if the
// implementation header ever starts leaking one, this file is where it shows.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "gui/platform_window_glfw.hpp"
#include "test_check.hpp"

namespace {

using cascade::gui::CursorShape;
using cascade::gui::GlfwPlatformWindow;
using cascade::gui::PlatformWindow;

// The window every block below runs against. 320x240 with a floor well under
// it, so the size limit cannot be what a size check is measuring.
PlatformWindow::CreateInfo testWindow() {
    PlatformWindow::CreateInfo info;
    info.width = 320;
    info.height = 240;
    info.minWidth = 160;
    info.minHeight = 120;
    info.title = "test_platform_window";
    return info;
}

// A window comes up with the size and the name it was asked for, and it comes
// up HIDDEN - which is not a detail. The main window is created invisible on
// purpose so its title bar can be taken off before anything sees it (0.84.1
// opened with the picture off the top of the window until a resize fixed it),
// and create() carries that contract now.
void testAWindowOpensHiddenAtTheSizeAndNameItWasAskedFor() {
    GlfwPlatformWindow win;
    CHECK(win.create(testWindow()));

    CHECK(!win.visible());

    int w = 0;
    int h = 0;
    win.windowSize(w, h);
    std::printf("window size %dx%d\n", w, h);
    CHECK(w == 320);
    CHECK(h == 240);

    int fw = 0;
    int fh = 0;
    win.framebufferSize(fw, fh);
    std::printf("framebuffer %dx%d\n", fw, fh);
    CHECK(fw > 0);
    CHECK(fh > 0);

    CHECK(win.title() == std::string("test_platform_window"));
    win.setTitle("renamed");
    CHECK(win.title() == std::string("renamed"));

    // Pixels per screen coordinate. One on this bench, more on a scaled
    // display; what is pinned is that it is a real positive number, because a
    // zero here would divide the layout by nothing on the first platform that
    // reports it.
    float sx = 0.0f;
    float sy = 0.0f;
    win.contentScale(sx, sy);
    std::printf("content scale %.3f x %.3f\n", static_cast<double>(sx), static_cast<double>(sy));
    CHECK(sx > 0.0f);
    CHECK(sy > 0.0f);

    win.destroy();
}

// THE CLOCK. Everything this application measures in time - the frame loop, the
// config debounce, the present-grace window, every telemetry duration - is this
// one number, so it has to exist, run forwards, and actually advance. "Never
// goes backwards" alone would be satisfied by a constant, which is why the
// third check is here.
void testTheClockRunsForwards() {
    GlfwPlatformWindow win;
    CHECK(win.create(testWindow()));

    const double t0 = win.time();
    CHECK(t0 >= 0.0);
    double t1 = t0;
    // Busy-wait rather than sleep: the point is that THIS clock moves, and a
    // sleep measured by another clock would be testing the other one. A window
    // system whose timer did not advance would spin here until the bound.
    for (int i = 0; i < 200000000 && !(t1 > t0); ++i) { t1 = win.time(); }
    std::printf("clock %.9f -> %.9f\n", t0, t1);
    CHECK(t1 > t0);
    CHECK(win.time() >= t1);

    win.destroy();
}

// THE CLOSE REQUEST. The rail's close key does not exit the process - it asks
// the window to close, exactly as the desktop's own close button would, and the
// run loop sees it on its next turn and shuts the receiver down cleanly. If
// requestClose() and shouldClose() ever stopped agreeing, the key would do
// nothing at all and the only symptom would be a button that does not work.
void testACloseRequestIsVisibleToTheFrameLoop() {
    GlfwPlatformWindow win;
    CHECK(win.create(testWindow()));

    CHECK(!win.shouldClose());
    win.requestClose();
    CHECK(win.shouldClose());

    win.destroy();
}

// THE CLIPBOARD, round trip. Not used by AppWindow - on the desktop the ImGui
// backend owns the clipboard and talks to GLFW itself - but the Android backend
// owns no clipboard at all, so the shell will have to supply one through this
// pair. A round trip is the whole contract: what went in comes back out.
void testTheClipboardRoundTrips() {
    GlfwPlatformWindow win;
    CHECK(win.create(testWindow()));

    const std::string sent = "FoxSDR clipboard round trip 12345";
    win.setClipboardText(sent.c_str());
    const std::string back = win.clipboardText();
    std::printf("clipboard read back %zu bytes\n", back.size());
    CHECK(back == sent);

    // Empty is a legal value and must not come back as the previous one.
    win.setClipboardText("");
    CHECK(win.clipboardText().empty());

    win.destroy();
}

// THE POINTER. Visibility is a real platform setting and reads back through the
// platform; the shape is remembered by the implementation, because GLFW has no
// way to ask a window what cursor it is wearing and a setter nobody can read
// back is a setter nobody can test.
void testTheCursorRemembersWhatItWasTold() {
    GlfwPlatformWindow win;
    CHECK(win.create(testWindow()));

    CHECK(win.cursorVisible());
    win.setCursorVisible(false);
    CHECK(!win.cursorVisible());
    win.setCursorVisible(true);
    CHECK(win.cursorVisible());

    CHECK(win.cursorShape() == CursorShape::Arrow);
    const CursorShape shapes[] = {CursorShape::TextInput, CursorShape::Hand, CursorShape::ResizeEW,
                                  CursorShape::ResizeNS, CursorShape::ResizeAll,
                                  CursorShape::Arrow};
    for (const CursorShape s : shapes) {
        win.setCursorShape(s);
        CHECK(win.cursorShape() == s);
    }

    // The pointer's position is whatever the bench's is; what is pinned is that
    // asking for it is safe and answers finite numbers, because the input
    // ledger (FOXSDR_DEBUG_INPUT) prints them every frame it is on.
    double cx = -1.0;
    double cy = -1.0;
    win.cursorPos(cx, cy);
    CHECK(cx == cx);  // not NaN
    CHECK(cy == cy);

    win.destroy();
}

// THE DISPLAY-ERROR COUNTER, which pauses hang reporting across a desktop
// reconfiguration. Nothing reconfigures a display during a test, so what is
// pinned is the property the frame loop depends on: it starts at zero and is
// monotonic, because the loop stores the value and compares the next one
// against it - a counter that could go backwards would make a change look like
// no change.
void testTheDisplayErrorCounterStartsAtZeroAndDoesNotGoBackwards() {
    GlfwPlatformWindow win;
    CHECK(win.create(testWindow()));

    const unsigned first = win.displayErrorCount();
    CHECK(first == 0u);
    win.pollEvents();
    CHECK(win.displayErrorCount() >= first);

    win.destroy();
}

// THE DESKTOP-ONLY GROUP, as far as a bench with no window manager can see it.
// supportsMultiViewport() is what an Android implementation will answer false
// to, and isMainWindowHandle() is what stops scope mode hiding the window it is
// drawing in - a handle that is not a window (null, or anything else) is never
// the main one.
void testTheDesktopOnlyGroupAnswersForThisPlatform() {
    GlfwPlatformWindow win;
    CHECK(win.create(testWindow()));

    CHECK(win.supportsMultiViewport());
    CHECK(!win.isMainWindowHandle(nullptr));

    // A null viewport handle is tolerated everywhere, because a refused
    // torn-off window leaves exactly that behind and 0.95.0 died on it.
    int vw = -1;
    int vh = -1;
    CHECK(!win.viewportFramebufferSize(nullptr, vw, vh));
    CHECK(vw == 0);
    CHECK(vh == 0);
    win.iconifyViewport(nullptr);
    win.setViewportVisible(nullptr, true);
    win.setViewportVisible(nullptr, false);

    // The current context is the one create() made current, and putting it back
    // is what the frame loop does around every torn-off page it draws.
    void* const ctx = win.currentRenderContext();
    CHECK(ctx != nullptr);
    win.setCurrentRenderContext(ctx);
    CHECK(win.currentRenderContext() == ctx);

    // Whether this driver will share a second context is the machine's answer,
    // not ours - both answers are legal and the application copes with either
    // (gui/viewport_policy.hpp). What is pinned is that ASKING does not lose
    // the main context, which is the bug the restore inside the probe exists
    // for: a run that lost it would draw nothing at all.
    const bool second = win.probeSecondRenderContext();
    std::printf("second shared context: %s\n", second ? "yes" : "no");
    CHECK(win.currentRenderContext() == ctx);

    // Windows only; false everywhere else, and the draw code asks
    // frame::installed() before it draws a single key.
#if defined(_WIN32)
    CHECK(win.installNativeFrame());
#else
    CHECK(!win.installNativeFrame());
#endif

    win.destroy();
}

// A WINDOW CAN BE DESTROYED TWICE, AND ASKED THINGS AFTERWARDS. run() destroys
// it explicitly inside the shutdown budget and the destructor destroys it
// again; every getter has to answer something harmless in between rather than
// dereference a freed window, or a shutdown would end in a fault at the last
// possible moment - the one place a crash is hardest to tell from a clean exit.
void testDestroyIsIdempotentAndTheGettersStaySafe() {
    GlfwPlatformWindow win;
    CHECK(win.create(testWindow()));
    win.destroy();
    win.destroy();

    int w = -1;
    int h = -1;
    win.windowSize(w, h);
    CHECK(w == 0);
    CHECK(h == 0);
    CHECK(!win.shouldClose());
    CHECK(!win.visible());
    CHECK(!win.maximised());
    CHECK(win.title().empty());
    CHECK(win.clipboardText().empty());
    // Safe to call, and nothing to do.
    win.requestClose();
    win.setTitle("ignored");
    win.iconify();
    win.restore();
}

}  // namespace

int main() {
    // Each block makes its own window and destroys it: GLFW is a singleton, so
    // sharing one across blocks would let a failure in one leave the next
    // running against a half-torn-down library. Six inits in a row is cheap and
    // it also proves create() after destroy() works, which run() relies on.
    testAWindowOpensHiddenAtTheSizeAndNameItWasAskedFor();
    testTheClockRunsForwards();
    testACloseRequestIsVisibleToTheFrameLoop();
    testTheClipboardRoundTrips();
    testTheCursorRemembersWhatItWasTold();
    testTheDisplayErrorCounterStartsAtZeroAndDoesNotGoBackwards();
    testTheDesktopOnlyGroupAnswersForThisPlatform();
    testDestroyIsIdempotentAndTheGettersStaySafe();
    return testSummary("test_platform_window");
}
