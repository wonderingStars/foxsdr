# FoxSDR patches to the vendored GLFW

GLFW is vendored at tag 3.4 (`third_party/THIRD_PARTY.md`) with the one change
listed here. It is fenced in the source by `FOXSDR PATCH` comments naming it,
so `grep -rn "FOXSDR PATCH" third_party/glfw/src` finds every changed line.
Anything not fenced is upstream 3.4, byte for byte (apart from line endings,
which follow this checkout's `core.autocrlf`).

## wayland-terminate-order - unload the Wayland libraries after the disconnect

**Files:** `src/wl_init.c` (`_glfwTerminateWayland`) and `src/egl_context.c`
(`_glfwTerminateEGL`). Linux Wayland only: the Windows, macOS and X11 code is
not touched, and the one changed condition in `egl_context.c` is false off
Wayland, so it behaves as 3.4 there.

**This is upstream's fix, not ours.** It is glfw/glfw commit
`162896e5b9a40dc382c5c438cd12c90a5ff86ddd` (2025-11-14, "Wayland: free modules
at end of terminate function", fixes issue #2744, merged from PR #2788).
It first appears in a release in GLFW 3.5.1 (tagged 2026-07-31; commit
`162896e5b9` is an ancestor of that tag, which is 55 commits past it). The
vendored tree is 3.4, and moving to 3.5.1 is a library upgrade (113 commits
past 3.4, with its own API changes) rather than the one-function change the
fault calls for, so the fix is applied by hand and the other post-3.4 changes
are deliberately left out. In particular upstream's `b579ea6792` (also unload
libwayland-client itself) is not applied: nothing here needs it, and unloading
the library every proxy belongs to is a larger step than the fault calls for.

**Why.** FoxSDR 0.99.59 on Linux (kernel 7.0, two reports from one user,
2026-10-02 and 2026-10-04) faulted with SIGSEGV at the very end of a normal
shutdown: three `libwayland-client.so.0` frames on top of
`_glfwTerminateWayland` (+1208 of 1260 bytes, its last libwayland call),
`terminate`, `AppWindow::run`, `main`. In 3.4 `_glfwTerminateWayland` unloads
libEGL (`_glfwTerminateEGL`, its first call), libdecor, libwayland-egl,
libxkbcommon and libwayland-cursor, and only then calls
`wl_display_flush` and `wl_display_disconnect`. `wl_display_disconnect`
releases the display's event queues, and for every event still queued it runs
`destroy_queued_closure`, which reads `closure->message->signature`; the
`wl_message` it points at is static data in the module that defined the
object, which has just been unmapped. That is `wl_display_disconnect` ->
`wl_event_queue_release` -> `destroy_queued_closure`: three libwayland frames,
the shape of the report, and the same stack, statement for statement, as
glfw/glfw issue #2744, which its reporters reduced to a ten-line program and
saw on NVIDIA drivers under KDE and GNOME (a maintainer could not reproduce it
on AMD, and the original reporter's AMD laptop was clean). Why events are
still queued at that point is upstream's account, not something measured here:
that thread's reproduction needs a few swaps with no event poll before the
terminate, and FoxSDR's frame loop likewise ends on a swap with no poll after
it.

**What it changes.**
1. `_glfwTerminateEGL` still calls `eglTerminate` first but no longer unloads
   libEGL when the platform is Wayland (`&& _glfw.platform.platformID !=
   GLFW_PLATFORM_WAYLAND`).
2. In `_glfwTerminateWayland` the five `_glfwPlatformFreeModule` calls (EGL,
   libdecor, libwayland-egl, libxkbcommon, libwayland-cursor) move from the
   middle of the function to its end, after `wl_display_disconnect` and the two
   `close` calls, and the EGL one joins them. Every call that USES one of those
   modules (`libdecor_unref`, the xkb unrefs, `wl_cursor_theme_destroy`) stays
   where it was, still before the unload. The wl_display and xdg objects are
   destroyed in the same order as before.

**What it does not change.** The order in which Wayland objects are destroyed,
what is destroyed, or anything before the first `wl_*_destroy`. No other
platform.

**Not verified here.** No Linux machine was available when this was applied, so
the two Wayland-only hunks are UNCOMPILED, and the crash was not reproduced:
`egl_context.c`'s hunk compiles on every platform (it is built on Windows, where
the condition is simply false); `wl_init.c` is only built on Linux. The change
is upstream's own text, reviewed line by line against 3.4 and against
upstream's diff. Whether the reporting user's driver is the NVIDIA case of
issue #2744 was not known; the match is the stack, not a measurement of their
machine. Residual risk from upstream's own approach: libEGL is now unloaded
after the display is gone, so a driver that touches the display from its unload
code would fault there instead.

**The test.** `tests/test_glfw_wayland_terminate.cpp` reads `wl_init.c` and
`egl_context.c` and requires every module unload in `_glfwTerminateWayland` to
come after `wl_display_disconnect`, none before it, and the EGL unload in
`_glfwTerminateEGL` to be guarded against Wayland. It is a statement about the
order in the source, not a reproduction of the fault (which needs a Wayland
desktop), and it runs on every platform. It goes red against plain 3.4.

**Re-applying on a GLFW upgrade.**
1. Vendor the new upstream unmodified first (`THIRD_PARTY.md`).
2. Check whether it already contains the fix: GLFW 3.5.1 and later include
   commit `162896e5b9` and need no patch - `grep -n "_glfwPlatformFreeModule"
   src/wl_init.c` shows the calls after `wl_display_disconnect` and `src/
   egl_context.c` guards the EGL one. If so, delete this patch, this file's
   entry, the fenced comments, the paragraph in `THIRD_PARTY.md` and the test
   (or keep the test: it is the same property, and its two "FOXSDR PATCH"
   fence checks are the only lines that would then have to go).
3. Otherwise re-apply the two hunks from this file's description, in the same
   places, and run `test_glfw_wayland_terminate`.
