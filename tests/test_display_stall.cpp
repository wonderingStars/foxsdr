// A stalled display is not a hung application.
//
// WHAT THIS IS FOR. Field report "hang ntdll.dll @ cascade::gui::AppWindow::run"
// (0.96.3, Windows 11 26200, 1177 s uptime): the log carries "GLFW error 65544:
// Win32: Failed to query display settings" and then the GUI thread sits in
// glfwSwapBuffers inside atio6axx.dll - AMD's display driver - for more than
// five seconds, and the hang watchdog files a report. A monitor had gone away.
// Nothing in FoxSDR was wrong.
//
// AND THE FIX IS NOT "IGNORE SwapBuffers". An earlier 0.96.2 report ALSO showed
// SwapBuffers on top and was a dead SDRplay service - a real fault, which must
// keep being reported. The two are told apart by what lies UNDER the wait, and
// that decision is the first half of this test.
//
// The second half is the two places the application gets out of the watchdog's
// way before a report is ever written: a bounded grace after a display change,
// and an unbounded one while the window is not being shown. Neither can be
// staged from ctest against a real monitor, so the policy is a state machine
// (gui/present_grace.hpp) driven directly here - including the transitions
// between the two reasons, because they share ONE counted pause and an
// unbalanced pair would leave the watchdog disarmed for the rest of a session.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/hang_watchdog.hpp"
#include "gui/present_grace.hpp"

#include "test_check.hpp"

#include <string>
#include <vector>

namespace {

using cascade::core::HangWatchdog;

bool stall(const std::vector<const char*>& frames) {
    return HangWatchdog::isDisplayPresentationStall(frames.data(),
                                                    static_cast<int>(frames.size()));
}

void checkTheDecision() {
    // THE REPORT THIS EXISTS FOR, as its frames actually came back: a wait at
    // the top, the AMD display driver under it, then opengl32 and the backend.
    CHECK(stall({"ntdll.dll", "atio6axx.dll", "opengl32.dll", "cascade.exe"}));

    // The same shape on the other two vendors' stacks, because a fix that only
    // recognised the machine that reported it would leave the next user of a
    // different GPU filing the identical false report.
    CHECK(stall({"ntdll.dll", "nvoglv64.dll", "opengl32.dll", "cascade.exe"}));
    CHECK(stall({"win32u.dll", "ig9icd64.dll", "opengl32.dll", "cascade.exe"}));
    CHECK(stall({"KERNELBASE.dll", "dxgi.dll", "cascade.exe"}));
    // Case, because the loader reports names as the file system has them and
    // the same machine has produced both spellings in one module table.
    CHECK(stall({"NTDLL.DLL", "ATIO6AXX.DLL", "OPENGL32.DLL"}));

    // THE 0.96.2 SDRplay REPORT, WHICH MUST STAY A HANG. SwapBuffers is on top
    // of the stack in both, and the whole difference is that below the wait
    // here there is this application and a vendor SDR service, not a display
    // driver. Getting this one wrong would silently stop reporting a real
    // fault class - which is worse than the false positive being fixed.
    CHECK(!stall({"ntdll.dll", "sdrplay_api.dll", "cascade.exe", "cascade.exe"}));

    // An ordinary deadlock: a wait, and this application all the way down.
    CHECK(!stall({"ntdll.dll", "cascade.exe", "cascade.exe"}));

    // BUSY IS NOT STALLED. A thread spinning inside a display driver is burning
    // a core in it, which is a fault worth reporting, not a wait on a display.
    CHECK(!stall({"atio6axx.dll", "opengl32.dll", "cascade.exe"}));

    // A display module far enough down is the frame loop, which is under EVERY
    // stall this application can have - so the scan is bounded and a hit past
    // the end of it does not count.
    std::vector<const char*> deep{"ntdll.dll"};
    for (int i = 0; i < HangWatchdog::kDisplayStallScanFrames + 4; ++i) {
        deep.push_back("cascade.exe");
    }
    deep.push_back("opengl32.dll");
    CHECK(!stall(deep));

    // LINUX. Field report "hang cascade @ hangCaptureSignalHandler" (0.97.0,
    // Linux 7.2, RTL-SDR): the GUI thread waiting in poll() inside
    // libwayland-client, under Mesa's eglSwapBuffers - a Wayland compositor
    // withholds frame callbacks from a surface it is not showing, so the swap
    // waits for as long as the window is out of sight. These are its frames as
    // the capture now hands them over (the capture's own handler and the
    // kernel's signal trampoline removed): three libc frames of the poll
    // syscall, then the Wayland client, then Mesa.
    CHECK(stall({"libc.so.6", "libc.so.6", "libc.so.6", "libwayland-client.so.0",
                 "libwayland-client.so.0", "libwayland-client.so.0", "libEGL_mesa.so.0",
                 "libEGL_mesa.so.0", "libEGL_mesa.so.0", "cascade"}));
    // The same wait under X11/GLX and under NVIDIA's own driver.
    CHECK(stall({"libc.so.6", "libxcb.so.1", "libX11.so.6", "libGLX_mesa.so.0", "cascade"}));
    CHECK(stall({"libc.so.6", "libnvidia-glcore.so.580.95", "libEGL_nvidia.so.0", "cascade"}));
    CHECK(stall({"libc.so.6", "radeonsi_dri.so", "libGLX_mesa.so.0", "cascade"}));
    // And on Linux, too, a wait under a RADIO library or this application
    // stays a hang: that is the SDRplay service or a deadlock, not a display.
    CHECK(!stall({"libc.so.6", "libsdrplay_api.so.3", "cascade", "cascade"}));
    CHECK(!stall({"libc.so.6", "libusb-1.0.so.0", "cascade"}));
    CHECK(!stall({"libc.so.6", "cascade", "cascade"}));
    // GLib shares its first letters with libGL and is not a display: a wait
    // in its main loop is a portal dialog or this application.
    CHECK(!stall({"libc.so.6", "libglib-2.0.so.0", "cascade"}));
    CHECK(stall({"libc.so.6", "libGL.so.1", "cascade"}));
    CHECK(stall({"libc.so.6", "libGLdispatch.so.0", "cascade"}));
    // Busy in Mesa is not waiting on a display.
    CHECK(!stall({"libEGL_mesa.so.0", "libc.so.6", "cascade"}));
    // THE REPORT AS 0.97.0 ACTUALLY SENT IT, capture frames and all: the top
    // frame is this application's own signal handler, which is not a wait -
    // so that shape must never be what the classification is handed.
    CHECK(!stall({"cascade", "libc.so.6", "libc.so.6", "libwayland-client.so.0",
                  "libEGL_mesa.so.0"}));

    // Degenerate input answers no rather than crashing: a walk that yielded
    // nothing, and a frame the module table could not resolve.
    CHECK(!HangWatchdog::isDisplayPresentationStall(nullptr, 4));
    CHECK(!stall({}));
    CHECK(!stall({"ntdll.dll", nullptr, nullptr}));
    CHECK(!stall({nullptr, "opengl32.dll"}));
}

// ---------------------------------------------------------------------------
// The pause the application takes so the report is never written at all.
// ---------------------------------------------------------------------------
struct Hooks {
    int pauses = 0;
    int resumes = 0;
    int held() const { return pauses - resumes; }
};

cascade::gui::PresentGrace makeGrace(Hooks& h, double graceS) {
    return cascade::gui::PresentGrace([&h] { ++h.pauses; }, [&h] { ++h.resumes; }, graceS);
}

void checkDisplayChangeBracket() {
    Hooks h;
    cascade::gui::PresentGrace g = makeGrace(h, 10.0);

    // An ordinary frame pauses nothing: the watchdog must be armed for the
    // 99.99% of frames in which nothing has happened.
    g.update(100.0, false, false);
    CHECK(!g.paused());
    CHECK(h.pauses == 0);

    // THE DISPLAY CHANGES. The pause is in force for the very first frame
    // after it - the one whose present call stalls - and not a frame later.
    g.update(101.0, true, false);
    CHECK(g.paused());
    CHECK(g.reason() == cascade::gui::PresentGrace::Reason::DisplayChange);
    CHECK(h.pauses == 1);

    // Held across the grace, and taken exactly ONCE however many frames run
    // inside it: HangWatchdog counts pauses, so one per frame at 60 Hz would be
    // six hundred pauses and one resume.
    for (int i = 0; i < 100; ++i) { g.update(101.0 + 0.05 * i, false, false); }
    CHECK(g.paused());
    CHECK(h.pauses == 1);
    CHECK(h.resumes == 0);

    // AND IT ENDS. A pause that never expires is the watchdog switched off, and
    // a display that never comes back is worth a report.
    g.update(111.5, false, false);
    CHECK(!g.paused());
    CHECK(h.held() == 0);

    // A second change during a grace RESTARTS it rather than being ignored:
    // the remaining time of the first settling says nothing about the second.
    g.update(200.0, true, false);
    CHECK(g.paused());
    g.update(205.0, true, false);   // 5 s in, restarted: now good to 215
    CHECK(g.paused());
    g.update(212.0, false, false);  // past the FIRST deadline, inside the new
    CHECK(g.paused());
    g.update(215.5, false, false);
    CHECK(!g.paused());
    CHECK(h.held() == 0);
}

void checkIconifiedRule() {
    Hooks h;
    cascade::gui::PresentGrace g = makeGrace(h, 10.0);

    // A MINIMISED WINDOW IS NOT EXPECTED TO PRESENT, and the rule is NOT
    // bounded: a window minimised for an hour is a window minimised for an
    // hour. Ten minutes of frames here would be a report every time if this
    // used the display-change grace.
    g.update(10.0, false, true);
    CHECK(g.paused());
    CHECK(g.reason() == cascade::gui::PresentGrace::Reason::Hidden);
    g.update(3610.0, false, true);
    CHECK(g.paused());
    CHECK(h.pauses == 1);

    // Restored: armed again on the very next frame.
    g.update(3611.0, false, false);
    CHECK(!g.paused());
    CHECK(h.held() == 0);

    // THE TWO REASONS SHARE ONE PAUSE, which is the whole reason this is a
    // state machine and not two booleans. Minimising during a display grace and
    // restoring after it has expired must not release a pause that was taken
    // once, twice.
    g.update(4000.0, true, false);   // display change: paused
    CHECK(h.pauses == 2);
    g.update(4001.0, false, true);   // ...and now also minimised
    CHECK(g.paused());
    CHECK(h.pauses == 2);            // still the same single pause
    g.update(4020.0, false, true);   // the grace is long gone; still hidden
    CHECK(g.paused());
    g.update(4021.0, false, false);  // shown again, grace expired
    CHECK(!g.paused());
    CHECK(h.held() == 0);
    CHECK(h.pauses == 2);
    CHECK(h.resumes == 2);
}

void checkReleaseBalancesAtTeardown() {
    Hooks h;
    {
        cascade::gui::PresentGrace g = makeGrace(h, 10.0);
        g.update(1.0, false, true);
        CHECK(g.paused());
        // The teardown gets its OWN budget and must be MEASURED, so a window
        // that was minimised at quit must not carry its pause into it - the
        // 120 s CAT-shutdown freeze this whole mechanism exists for would be
        // invisible again.
        g.release();
        CHECK(!g.paused());
        CHECK(h.held() == 0);
        // Releasing twice is a no-op, not a stray resume that would un-pause
        // somebody else's.
        g.release();
        CHECK(h.resumes == 1);
    }
    // ...and the destructor of a still-paused one releases it.
    CHECK(h.held() == 0);

    Hooks h2;
    {
        cascade::gui::PresentGrace g = makeGrace(h2, 10.0);
        g.update(1.0, true, false);
        CHECK(g.paused());
    }
    CHECK(h2.held() == 0);
}

}  // namespace

int main() {
    checkTheDecision();
    checkDisplayChangeBracket();
    checkIconifiedRule();
    checkReleaseBalancesAtTeardown();
    return testSummary("test_display_stall");
}
