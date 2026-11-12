# FoxSDR patches to the vendored PortAudio

PortAudio is vendored at commit `147dd722548358763a8b649b3e4b41dfffbcfbb6`
(`third_party/THIRD_PARTY.md`) with the changes listed here. They are fenced in
the source by a `FOXSDR PATCH` comment naming it, so `grep -rn "FOXSDR PATCH"
third_party/portaudio/src` finds every changed line. Anything not fenced is
upstream at that commit, byte for byte (apart from line endings, which follow
this checkout's `core.autocrlf`).

## alsa-zero-periods - refuse a device that reports zero periods instead of dividing by it

**File:** `src/hostapi/alsa/pa_linux_alsa.c`, in
`PaAlsaStreamComponent_DetermineFramesPerBuffer`. Linux ALSA only: the file is
not compiled on Windows (WASAPI and WMME only) or macOS, and the one added line
is a `PA_UNLESS` that is false for every device that reports a usable period
count, so it behaves as upstream there.

**This is our own change, not a cherry-pick.** No upstream fix for this exact
division was identified: it was written without network access, so upstream's
history could not be searched, and nothing in the pinned tree has one.
Upstream's only defence at the pinned commit is `assert( maxPeriods > 1 )` on
the line above the patch. Whether a later PortAudio release changed it is not
known here - check before treating this as permanent (see re-applying, below).

**Why.** FoxSDR 0.99.81 on Linux x64 (Kali, kernel 7.1.5; one report,
2026-10-16) faulted with SIGFPE, `si_code` 8 (`FPE_INTDIV`, integer divide by
zero), fully symbolised:

    PaAlsaStreamComponent_DetermineFramesPerBuffer.constprop.0.isra.0 +216
    <- PaAlsaStream_Configure <- OpenStream <- Pa_OpenStream
    <- cascade::sink::AudioOut::openLocked <- AudioOut::open
    <- core::patch::makeDeviceDest <- AppWindow::patchPublishSets

`AudioOut` opens with `paFramesPerBufferUnspecified` (0). With no user buffer
size, the divisions that use it (`PaAlsa_AlignForward`'s `v % align`, and the
`assert( ... % framesPerHostBuffer )` lines) are skipped, guarded or compiled
out, and the ONE division left on the live path is

    framesPerHostBuffer = (bufferSize / numPeriods);

`numPeriods` is `PA_MIN(maxPeriods, PA_MAX(minPeriods, numPeriods_))`, where
`maxPeriods` is what `snd_pcm_hw_params_get_periods_max` says the device
supports. It is zero exactly when `maxPeriods` is zero. The release build is
`-DNDEBUG`, so `assert( maxPeriods > 1 )` is gone and nothing stops it.
**Verified:** this is the only reachable division by a value that can be zero for
this call (the function's other divisions are by the constant 2, and the
duplex-only ones in `PaAlsaStream_DetermineFramesPerBuffer` use the global
`numPeriods_`, which nothing here sets to zero). **Inferred, not verified:** that
the reporting machine's device reported zero maximum periods. The report does
not carry the device's parameters, the same session had opened ALSA output
successfully 7 s earlier (a different device or route can answer differently),
and no ALSA device here can be asked. What the stack and the fault code prove is
a zero divisor in that function; the line above is the only candidate.

**What it changes.** One line, after `maxPeriods` is read and before it is
used: `PA_UNLESS( maxPeriods > 0, paInternalError );`. A zero maximum now makes
`Pa_OpenStream` return `paInternalError` ("Internal PortAudio error") through
the function's ordinary `error:` exit, the same code and idiom as the
`PA_UNLESS( framesPerHostBuffer != 0, paInternalError )` that follows in
`PaAlsaStream_DetermineFramesPerBuffer`. `AudioOut::open` words it as "output
could not be opened - ALSA refused the stream: Internal PortAudio error", counts
it in the health ledger as a refused output, and returns false, which the
Pipeline's PortAudio watchdog and the patch page already treat as a dead audio
device ("no audio") rather than a crash. The zero is refused, not clamped to
one: a device that says it supports no periods cannot be configured, so
continuing would only fail at a later ALSA call with a less useful message.

**What it does not change.** Any device that reports one or more periods:
`maxPeriods > 0` is true and the code that follows is upstream's. Not changed
and not needed: `minPeriods`, the duplex path (its divisions use the global
`numPeriods_`, not this clamp), and the `PA_ALSA_USE_OBSOLETE_HOST_BUFFER_CALC` code
(not compiled).

**Not verified here.** Linux ALSA code cannot be built or run on the
development machine. What was done instead, once, outside the repository: the
function's text was extracted from this file (before and after the patch) and
compiled with GCC against stubbed `alsa_snd_pcm_hw_params_*` calls and
`-DNDEBUG`. Unpatched with `maxPeriods` 0, the process exits with
`0xC0000094` (`STATUS_INTEGER_DIVIDE_BY_ZERO`, the Windows form of the same
fault); patched, the same call returns `paInternalError` (-9986) with
`framesPerPeriod` untouched at 0; with `maxPeriods` 4, 2 and 1 both versions
return `paNoError` with identical period sizes (107, 215 and 431 frames at
48 kHz with a 9 ms suggested latency). Only a real ALSA device that reports zero periods verifies it end
to end, and CI's Linux build verifies that the file still compiles.

**The test.** `tests/test_portaudio_alsa_periods.cpp` reads the vendored file
and requires the guard to sit between the read of `maxPeriods` and the clamp,
every division by `numPeriods` to follow it, and this entry and the fence to be
present. It is a statement about the text of the patch (it runs on every
platform) and goes red against plain upstream. The caller's half of the fix
(a rate or latency that is not a usable number is refused before
`Pa_OpenStream`) is in `tests/test_audio_out.cpp`.

**Re-applying on a PortAudio upgrade.**
1. Vendor the new upstream unmodified first (`THIRD_PARTY.md`).
2. Look at `PaAlsaStreamComponent_DetermineFramesPerBuffer` in the new
   `pa_linux_alsa.c`: if `numPeriods` can no longer reach the division as zero
   (a guard on `maxPeriods`, or a `PA_MAX( ..., 1 )` on the clamp), delete this
   patch, this entry, the fence, the `THIRD_PARTY.md` sentence and the test.
3. Otherwise add the one `PA_UNLESS( maxPeriods > 0, paInternalError );` line
   after `assert( maxPeriods > 1 );` and run `test_portaudio_alsa_periods`.

## open-stream-list-lock - a lock around PortAudio's list of open streams

**File:** `src/common/pa_front.c` (all platforms). `AddOpenStream`, `RemoveOpenStream` and `CloseOpenStreams` take a small lock
(`SRWLOCK` on Windows, a static `pthread_mutex_t` elsewhere, both statically initialised and never destroyed) for the list
operation alone; `CloseOpenStreams` reads the head under it and calls `Pa_CloseStream` outside it. Three functions are added, none
declared in `portaudio.h`: `PaFoxsdr_OpenStreamCount` (the list's length, read under the lock) and
`PaFoxsdr_OpenStreamListLockForTest` / `...UnlockForTest` (tests only).

**Why.** Upstream's list has no lock: `Pa_OpenStream` adds to it as its last act and `Pa_CloseStream` removes from it as its
first. `sink/pa_init.hpp` guards both with `PaStreamListGuard`, but `AudioOut` and `AudioIn` take it with `NoWait` (the GUI thread
must never wait behind a hung driver), and a `NoWait` that finds the lock busy goes ahead WITHOUT it: two writers on one list.
`test_engine_api_merge` runs 24 engines at once, each opening and closing the default output on a worker, so that happens: three of
three runs under load ended in an access violation (symbolised from the faulting offsets with the PDB: `Pa_Terminate+0x64` at
`pa_front.c:396`, the `CloseOpenStreams` call, and `Pa_CloseStream+0x48` at `pa_front.c:1379`, `RemoveOpenStream`, reading
addresses `0x17` and `0xFFFFFFFFFFFFFFFF`, called from `sink::paTerminateShared`) after 160 to 260 s, when the last engine's
`Pa_Terminate` closed a list node that a racing writer had left pointing at a freed stream. The lock makes the list correct whatever
the callers do; it is held for microseconds and never across a host API's open or close, so a driver that hangs cannot hold it.

**What it does not change.** Anything a caller sees: `Pa_OpenStream`, `Pa_CloseStream` and `Pa_Terminate` return what they did.
`PaStreamListGuard` stays (it also serialises the host APIs' own work, and its timing rules are tested by
`test_pa_stream_list_guard`).

**The test.** `tests/test_pa_open_list_race.cpp` (A) takes the list lock by hand and requires an open and a close on other threads
to wait for it (red with the lock removed: "the open waited: 0, the close waited: 0"), and (B) opens and closes 1,440 streams on 12
threads with no guard at all and requires the list to hold exactly what is open. It skips, saying so, on a machine with no
output device.

**Re-applying on a PortAudio upgrade.** Vendor the new upstream unmodified first. If upstream's list has its own lock by then,
delete this patch, this entry and the fence, keep the test (it then proves upstream's). Otherwise re-apply the three fenced
functions and the lock.
