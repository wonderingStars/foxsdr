# FoxSDR 0.99.82

**FoxSDR 0.99.82 is a repair release built from this month's crash and freeze reports. The freezes reported on 0.99.81 are fixed: changing frequency or applying a plugin preset no longer stalls the window while the signal path is busy, a radio scan can no longer freeze the window when the SDRplay service stops answering (Windows) or a USB device wedges its system entry (Linux), and on Wayland a hidden or minimised window no longer blocks for minutes in the buffer swap. LSB and USB now actually reject the other sideband - below 1 kHz the opposite sideband used to come through at nearly full level. A machine with no working OpenGL driver now gets a plain-words error instead of a silently vanishing process, a rare Linux audio-device shape no longer crashes on open, and bookmark Import accepts the path forms people really paste and drop, naming the exact file it tried when it fails. New: hold Shift and roll the wheel over the spectrum or waterfall to slide the tuned band along the band, one mode step per notch - the view follows a zoomed passband, and letting go of the wheel leaves your hand in charge.**

Still not fixed: two 2026-10-05/06 fast-fail crash reports from 0.99.64/65 remain unexplained (no report of that shape has arrived from any newer build, which carry the diagnostics that would name the faulting module), and the one 0.99.75 AppImage start-up death can only be diagnosed if it recurs on 0.99.78 or later. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.82.exe`:**
`b488dcd967c9c48ab98942856a1de2d935ec511f92f3b7554504dc0c2aa2bd42`

**SHA-256 of `FoxSDR-0.99.82-x86_64.AppImage`:**
`b2dcbc24ab329049855d5194fb239941e807e6b98e90a0f70f935dc1e22880c9`

**SHA-256 of `foxsdr-0.99.82-linux-x64.tar.gz`:**
`a9d1f6e8c6ce9dca10a2636f90f4cc31bc22d457b92def02355fc1ab2d750e6c`

**SHA-256 of `FoxSDR-0.99.82-aarch64.AppImage`:**
`36c83fa7828030f201edbf244ba6448dcacf83d4da64b698ec1a2d3b8e6ae464`

**SHA-256 of `foxsdr-0.99.82-linux-arm64.tar.gz`:**
`46d1f14e7c20e1cbcf480485f2ddcdb3072dee62111d9e5ef7db41e177638077`

## Features and fixes, 2026-10-19

**Changing frequency or applying a preset no longer freezes the window.** Two 0.99.81 reports showed the window frozen for over five seconds - one retuning from the toolbar, one applying a plugin preset that woke ~28 decoders - both waiting for the lock the signal-processing thread holds for an entire audio block. Every setter the window calls now posts its change and returns at once; the signal path applies it at the next block boundary, so a set-then-read still reads the value just set and nothing tears mid-block. The retune handed to decoder plugins is delivered the same way (latest request wins), which is also where the plugin contract always said it runs. The related "audio underruns when changing frequency" report is the same contention and should improve with it.

**A radio scan cannot freeze the window any more - on either platform.** On Windows, when the SDRplay service stopped answering, the abandoned enumeration kept a lock the window then waited on forever (one user killed the process, which filed a second, misleading crash report). The window now never waits on that lock: it reads what it needs with a bounded try, a wedged enumeration is skipped and retried after the existing hold-off, and a merely slow device is waited out as before. On Linux, a USB device stuck mid-reset could block the window inside a sysfs attribute read; those reads now run on a bounded walk that skips a wedged device, names it in the log, and lists it again by itself once it recovers.

**Wayland: a hidden window no longer blocks in the buffer swap.** Five freeze reports (a Raspberry Pi 5 and a Kali desktop) showed the window stalled for minutes "in present", then recovering - the shape of GLFW 3.4's Wayland swap waiting indefinitely for a frame callback the compositor never sends to a window it is not showing. The two upstream GLFW commits that fix this (released in GLFW 3.5.1) are applied to the vendored copy: a hidden window skips the swap on a 20 ms timeout and the application keeps running. Every hang and crash report also now carries `window-state:` (visible/minimised/focused, and how stale that knowledge is) and `window-system:` lines, so any remaining stall arrives saying what the window was doing.

**LSB and USB now reject the opposite sideband.** A listener reported both behave as double-sideband, and measurement proved them right where it matters: at 100 Hz audio the opposite sideband leaked at full level, at 500 Hz it was only 3 dB down. The sideband filter is now placed from the wanted band's edges, putting the whole opposite sideband in the stopband - worst measured leak is now 55 dB down. The trade is the conventional SSB low cut (about -6 dB at 300 Hz) and somewhat more CPU in SSB modes.

**A machine with no working OpenGL driver is told so.** Four start-up reports, confirmed by the new window diagnostics as "WGL: The driver does not appear to support OpenGL", used to end in a silently vanishing process - and a crash report that wrongly said the application was "ended by another process". FoxSDR now shows a plain-words error naming the common causes (no graphics driver installed, a remote desktop session without GPU support) and the exact log file to send, and the report says the application ended itself because the window could not be created. Exit code 1 without that record is now worded honestly as either an outside kill or a self-exit, rather than asserting the former.

**A Linux audio device reporting zero periods no longer crashes the application.** One 0.99.81 report showed a divide-by-zero inside the audio library's ALSA backend when a device reported a maximum of zero periods; the vendored library now refuses such a device cleanly and FoxSDR shows the existing "output could not be opened" message. Unusable open requests (absurd rates, channel counts, latencies) are refused before they reach the library at all.

**Bookmark Import accepts the path shapes people actually use.** Seventeen input forms failed with "could not be opened": paths pasted with a trailing newline, surrounding spaces or quotes, `file://` addresses from a Linux file manager (including percent-encoded and Unicode names), and every relative path containing a folder - which was looked up only against the process working directory. All now open; relative paths are also resolved against the recordings folder where Export writes, and a failure names the full path that was tried, so the next report is diagnosable.

**New: slide the tuned band with Shift + wheel.** Requested on 2026-10-18: hold Shift and roll the wheel over the spectrum or the waterfall to move the passband one mode step per notch (12.5 kHz in NFM, 100 kHz in WFM, 9 kHz in AM, 1 kHz in SSB/CW), the same raster clicks snap to. At the edge of the view's reach the radio itself is retuned through the same path the toolbar uses. A zoomed view pans just enough to keep the band in sight for a moment after each slide - and any drag, pan or zoom of your own cancels that at once. Plain wheel and Ctrl + wheel zoom exactly as before. Also fixed on the way: double-clicking a deeply zoomed waterfall could tune, unzoom, and then mis-tune hundreds of kHz away from the release of the same double-click; the second release no longer tunes.

One new tooltip line is translated in all 33 languages (machine-translated, pending native review). The diagnostics document describes the new report fields, and PRIVACY.md lists them.
