# The FoxSDR plugin host API (host API levels 1 and 2)

This is the guide for plugin authors to what FoxSDR 0.99.31 added to the plugin
contract: a plugin can now read the whole receiver, ask to change it, mark the
spectrum, keep its own settings, talk to the user, offer keys of its own, and
process the audio the user hears. Level 2 (FoxSDR 0.99.74) adds one thing, with
a section of its own: a plugin can hand the host **audio to transmit**
([Transmitting](#transmitting-host-api-level-2)). The contract itself is
[`src/core/plugin_abi.h`](../src/core/plugin_abi.h) - one MIT-licensed C
header, the same file in the plugin repository's `include/`. Where this guide
and the header disagree, the header is right; please report the guide.

The worked example is **Host API tour** (`src/hostapi_example/` in the plugin
repository). It calls every function below from the place and the thread a real
plugin would, and its test drives the real module against a fake host.

## Contents

1. [What changed, and what did not](#what-changed-and-what-did-not)
2. [Getting the table, and asking what it offers](#getting-the-table-and-asking-what-it-offers)
3. [Threads](#threads)
4. [Permission](#permission)
5. [Answers](#answers)
6. [Reading the receiver](#reading-the-receiver)
7. [Changing the receiver](#changing-the-receiver)
8. [The stream clock](#the-stream-clock)
9. [Processing the audio](#processing-the-audio)
10. [Transmitting (host API level 2)](#transmitting-host-api-level-2)
11. [Marks on the spectrum and waterfall](#marks-on-the-spectrum-and-waterfall)
12. [Your own settings](#your-own-settings)
13. [Messages](#messages)
14. [Keys on your plate](#keys-on-your-plate)
15. [The patch page](#the-patch-page)
16. [A minimal plugin](#a-minimal-plugin)
17. [What the host does not offer](#what-the-host-does-not-offer)
18. [Checklist before you ship](#checklist-before-you-ship)

## What changed, and what did not

FoxSDR 0.99.74 adds three optional `CascadePreset.flags` bits without changing
ABI 3 or the preset structure. `CASCADE_PRESET_KEEP_TUNED_ON_START` (0x2) on a
plugin's first declared preset makes automatic start/window opening apply that
preset's reception settings at the current device-centre-plus-VFO frequency.
It applies even on a listed channel, so stale mode or bandwidth is corrected,
and takes priority over saved user presets for this automatic setup. Explicit
preset presses, including saved user presets, retain their normal frequency
behavior. `CASCADE_PRESET_FLAT_AUDIO` (0x4) sets receiver de-emphasis to Off,
for automatic setup and explicit preset presses. `CASCADE_PRESET_SHOW_TEXT_OUTPUT`
(0x8) opens the shared Decoder output window for a text decoder even when its
own settings panel or another window is also open. Plugins without
`CASCADE_CAP_DECODER` ignore that request. Older hosts ignore these bits;
plugins that omit them retain the existing automatic tuning behavior.

**The plugin ABI is still 3.** Nothing about the descriptor changed, no
existing table changed, and every plugin built before level 1 loads and runs
unchanged - the host's test suite loads all 27 published plugin binaries to
prove it (`tests/test_plugin_abi3_compat.cpp`).

Two things grew, in the two ways ABI 3 was designed to grow:

- **`CascadeHostApi` grew at its end.** The host table a plugin receives through
  `CASCADE_CAP_HOST_CLIENT` used to end at `unix_time_ms`. It now carries
  `apiLevel`, `knownCapabilities`, `hostName`, `hostVersion` and 24 more
  functions after it. Its first member, `structSize`, is its version: a plugin
  may touch a member only if `structSize` covers it.
- **A new capability bit**, `CASCADE_CAP_AUDIO_PROCESSOR` (0x800), for a plugin
  that transforms the demodulated audio in place.

The rule for the future is the same one: **members are only ever appended**,
never reordered, removed or retyped, and a new plugin-side facility is a new
capability bit. A function that turns out wrong stays where it is, documented as
superseded, and its replacement is appended.

### Why not ABI 4

ABI 4 with the host also accepting 3 would have meant two descriptor dialects in
the loader for ever, and a rebuild of every published plugin to get anything
new - the flag day ABI 3 was restructured to end. Nothing here needed it: the
extension is purely additive, and `structSize` already made the table
versionable.

## Getting the table, and asking what it offers

Declare `CASCADE_CAP_HOST_CLIENT` and keep the pointer `attach()` hands you:

```c
static _Atomic(const CascadeHostApi *) g_host;   /* std::atomic in C++ */

static void my_attach(const CascadeHostApi *host) {
    atomic_store(&g_host, host);
}
```

`attach()` runs before any of your capabilities' `create()`, and **may run
again** - after a rescan, a source change, and once more at startup when the
host has loaded its saved configuration. It always passes the same pointer, so
storing it again is harmless; make anything else you do there idempotent.

Before calling any level-1 member, ask whether the host has it:

```c
if (CASCADE_HOST_HAS(host, get_state)) {        /* covered AND non-NULL */
    CascadeReceiverState s = {0};
    s.structSize = sizeof s;
    if (host->get_state(host->ctx, &s) == CASCADE_API_OK) { ... }
}
```

`CASCADE_HOST_COVERS(host, member)` is the size check alone, for data members
such as `apiLevel`. On a host older than level 1, `structSize` ends at
`unix_time_ms` and every level-1 check says no - so a plugin built against this
header still loads there, keeps doing whatever needs only the old functions,
and should say plainly what it cannot do (the Host API tour's panel reads
"level 1 not offered").

`knownCapabilities` is the host's `CASCADE_CAP_ALL_KNOWN`: the capability bits
it will actually use. `apiLevel` names the generation of the table's semantics
(this header: `CASCADE_HOST_API_LEVEL` = 2; level 2 added the transmit capability and
three flags and changed no function). `hostName` and `hostVersion` are for
messages; branch on `CASCADE_HOST_HAS` and `apiLevel`, never on the version text.

## Threads

Every level-1 function may be called from **any thread** - your own worker, the
GUI thread (inside `poll_rows`, `poll_tracks`, `poll_state`), or the real-time
DSP thread inside `process()` - with **one exception**: `settings_get` and
`settings_set` refuse the real-time thread with `CASCADE_API_WRONG_THREAD`,
because they copy up to 4 KB and may allocate.

What makes "any thread" true, and what it costs:

| Function group | Where it runs | What it costs the caller |
|---|---|---|
| `get_state`, `get_gain`, `get_sample_rates`, `get_stream_info` | on your thread | a lock-free read of a snapshot the GUI thread publishes each frame; a read that keeps overlapping a publish answers `BUSY` after a bounded number of retries rather than wait |
| every `set_*` | validated on your thread, **applied on the GUI thread's next frame** | a bounded copy into a fixed queue (256); `BUSY` when full |
| `log`, `set_marker`, `*_command`, `clear_markers` | on your thread | a bounded copy under a host lock that is never held across plugin code or I/O; fixed-size storage, no allocation |
| `settings_get`, `settings_set` | on your thread, never the real-time one | a copy of up to 4 KB under a host lock |

**Nothing is ever called back.** The host never calls your code to tell you
something changed: you ask (`get_state`'s counters, `poll_command`). A callback
would have to run on some host thread, holding some host state, into a module
that might be mid-unload.

**Nothing you call waits for plugin work** - the host's own threads never wait on
yours, and you never wait on theirs beyond a bounded copy.

The existing four functions keep their existing rules. In particular
`request_tune` still retunes on the calling thread and is documented for the GUI
thread; from any other thread use `set_frequency`, which is queued.

**After unload.** A call made through a table the host has shut down - the
plugin was stopped, its module is being unloaded, the application is exiting -
answers `CASCADE_API_DETACHED` and does nothing. Strings you pass are copied
before the call returns, so nothing the host keeps can point into your image.

## Permission

Three grants, all per plugin (keyed on the module's file name), all **off by
default**, all shown on the module's plate in **Fitted modules**:

| Grant | The key the user presses | What it allows |
|---|---|---|
| **tune** | GRANT RECEIVER CONTROL | `request_tune`, `set_frequency`, `set_vfo_offset` |
| **settings** | GRANT RADIO SETTINGS | `set_mode`, `set_bandwidth`, `set_squelch`, `set_sample_rate`, `set_gain`, `set_device_agc`, `set_running`, `set_volume`, `set_muted` |
| **transmit** (level 2) | GRANT TRANSMIT | handing the host audio to transmit (`CASCADE_CAP_TX_AUDIO`) - and only while the operator has also armed the TRANSMIT page; see [Transmitting](#transmitting-host-api-level-2) |

They are separate so that a grant a user already gave a satellite tracker for
Doppler correction keeps meaning only that. The settings key appears only on a
plugin that has asked for one of those functions at least once; the first
request is refused with `DENIED`, and that refusal is what makes the key appear.
Tell the user which key would help - the Host API tour logs a warning naming it.
The transmit key is offered the same way, to a plugin whose `active()` has
returned non-zero at least once.

Reads, markers, settings, messages and keys need no grant: none of them changes
what the receiver does. A **stopped** plugin is refused everything.

The grant is checked twice: when you ask (so you get `DENIED` at once) and again
when the GUI thread applies the request, so a grant revoked - or a plugin
stopped - in between stops the request from acting.

## Answers

Every level-1 function returns an `int32_t`. `CASCADE_API_OK` (0) or a positive
count is success; the rest are:

| Code | Value | Meaning |
|---|---|---|
| `CASCADE_API_DENIED` | -1 | no grant, or the plugin is stopped |
| `CASCADE_API_OUT_OF_RANGE` | -2 | a value outside what the host accepts |
| `CASCADE_API_NO_DEVICE` | -3 | needs an open radio, and there is none |
| `CASCADE_API_FAILED` | -4 | the host could not do it |
| `CASCADE_API_BAD_ARGUMENT` | -5 | NULL, NaN or infinity, a malformed struct, a bad key |
| `CASCADE_API_UNSUPPORTED` | -6 | this host or this radio has no such thing |
| `CASCADE_API_BUSY` | -7 | a bounded queue is full, or a read kept overlapping; try later |
| `CASCADE_API_DETACHED` | -8 | this table has been shut down |
| `CASCADE_API_NOT_FOUND` | -9 | no such settings key, marker or command |
| `CASCADE_API_LIMIT` | -10 | a per-plugin quota is full |
| `CASCADE_API_WRONG_THREAD` | -11 | not allowed on the real-time thread |

The first four equal `CASCADE_TUNE_*`, so code that understands `request_tune`
understands them.

### In/out structs

Every struct starts with `structSize`, **set by you** to `sizeof` as you
compiled it. The host reads or writes only the first `min(its sizeof, yours)`
bytes, and for a struct it fills, writes the number it filled back into
`structSize`. So these structs can grow later without either side reading past
what the other allocated. A `structSize` smaller than the level-1 struct is
`BAD_ARGUMENT`.

## Reading the receiver

```c
CascadeReceiverState s = {0};
s.structSize = sizeof s;
host->get_state(host->ctx, &s);
```

`CascadeReceiverState` carries the centre frequency, the VFO offset, `tunedHz`
(centre plus offset - what is heard), the active source's sample rate, the
bandwidth, the squelch threshold, the volume, `signalDb` (channel power in dB
full scale), `sMeter` (0..1, exactly the S-meter bar the user sees, which maps
`signalDb` over -120..0 dB - the host has no calibrated dBm scale, so there are
no S-units), the demodulator (`CASCADE_DEMOD_*`), how many gain stages there
are, the radio's name, and flags: running, a radio open (rather than the signal
generator), muted, the radio's AGC on and available, squelch open, stereo, and
**this plugin's** tune grant, settings grant and stopped state.

**Change notification is a counter, not a callback.** `seq` moves whenever any
of the above except the two measurements (`signalDb`, `sMeter`) changes, and
four group counters say which: `tuneSeq` (centre, offset), `modeSeq` (mode,
bandwidth, squelch), `deviceSeq` (the radio, its rate and rate list, gains, AGC,
running) and `audioSeq` (volume, mute). Keep the last one you saw and compare.
Counters never go backwards within a session and start above zero, so 0 can mean
"never read".

`get_gain(ctx, index, &info)` describes gain stage `index`: its name (pass it to
`set_gain` verbatim), range, step, unit (`CASCADE_GAIN_UNIT_DB`, or
`_STEPS` for radios whose gain is not in decibels) and the **readback**, not the
last request. `get_sample_rates(ctx, out, cap)` returns how many rates the open
radio offers and copies up to `cap` of them; call it with `cap` 0 to ask the
count. Both answer "none" (`NOT_FOUND`, 0) with no radio open.

### Targets you hear but cannot place yet (0.99.75)

A track source (`CASCADE_CAP_TRACK_SOURCE`) used to be able to publish only
targets that had a position, so an ADS-B decoder had to hide every aircraft whose
position message had not arrived. From FoxSDR 0.99.75 `get_state` reports
`CASCADE_STATE_LISTS_UNPLACED_TRACKS` (0x00008000) in every state it fills, and a
plugin that sees it may publish such an aircraft anyway: set
`CASCADE_TRACK_FLAG_NO_POSITION` (0x00000004) in `CascadeTrack::flags` and leave
`latDeg` and `lonDeg` NaN. Fill in what you do know (`id`, `label`, `altM`,
`ageMs`, `kind`).

The host **lists** such a target, below the placed ones, dimmed, with its altitude
and the words "no position yet", and counts it separately in the list's heading
("12 targets, 3 heard without a position"). It never draws it, never fits the map
to it, never counts it in the coverage map or as "on screen", and gives it no
trail (a `CascadePath` whose id is a no-position target's is dropped). It fades and
is dropped on the same `ageMs` rule as a placed target of its kind; when the
position arrives, publish the track again without the flag. The host treats any
track whose coordinates are not finite as having no position, flag or not, so a
plugin that leaves NaN in without the flag is listed rather than drawn at NaN - but
it must not rely on that: **a host older than 0.99.75 drops such a track**, so
publish them only when the bit is set, and keep working (placed targets only) when
it is not.

### Panel rows the user can press (0.99.77)

A panel (`CASCADE_CAP_PANEL`) used to be read-only: the host drew the rows and a
click on one went nowhere. The IBOC Radio plugin lists its eight HD programmes in
its panel, and a tester asked for the obvious thing - "if I click on HD2 on the
screen below, it should select that program, instead of having to go into the
plugin settings and change the Programme to 1". From FoxSDR 0.99.77 a table row
(`CASCADE_ROW_CELLS`) can be a **key bound to one of your commands**:

```c
host->add_command(host->ctx, 12, "HD2");              /* once, as for a plate key */
...
row.kind  = CASCADE_ROW_CELLS;
row.flags = CASCADE_ROW_KEY_FLAGS(12) | CASCADE_ROW_FLAG_GOOD;   /* a key for command 12, lit */
...
while (host->poll_command(host->ctx, &id) == 1) { if (id == 12) choose_programme(2); }
```

`CASCADE_ROW_FLAG_KEY` (0x00000008) marks the row and the command id rides in
bits 8..15 of `flags` (`CASCADE_ROW_KEY_ID_MASK` 0x0000FF00, shifted by
`CASCADE_ROW_KEY_ID_SHIFT`); `CASCADE_ROW_KEY_FLAGS(id)` builds the two together and
`CASCADE_ROW_KEY_ID(flags)` reads the id back. The id is in `flags` and not a new
field because `CascadePanelRow` has no `structSize` - the host allocates the array
and you write it - so it cannot grow. That makes the id 1..255; `GOOD`, `WARN` and
`MUTED` still apply to a key.

The host draws the whole row as a key (it lights under the pointer, and the cell
texts stay as you wrote them) and delivers a press exactly as it delivers a press of
a key on your plate: it queues the command id, and you take it with `poll_command`
from wherever you already run regularly - your `poll_rows` is the natural place.
Three things are not keys, and are drawn as the text they always were: a heading or
a separator, a row whose id is 0, and **a row whose id you never registered with
`add_command`** (a key the host would refuse to press). Pressing it never reaches
you, and neither does a press while the plugin is stopped.

**Use it only when you see the bit.** From 0.99.77 `get_state` reports
`CASCADE_STATE_ROW_KEYS` (0x00010000) in every state it fills, with no grant. A
host older than that draws a flagged row as plain text - harmless, but a heading
that says "press one to choose" would be a lie - so show that wording, and
register the commands only the keys need, when the bit is set:

```c
CascadeReceiverState s = {0};
s.structSize = sizeof s;
int keys = host->get_state(host->ctx, &s) == CASCADE_API_OK &&
           (s.flags & CASCADE_STATE_ROW_KEYS) != 0;
```

**The command limit went from 8 to 16 in the same release**
(`CASCADE_MAX_COMMANDS_PER_PLUGIN`), because a chooser for eight programmes needs
eight commands beside the keys a plugin already had. A host before 0.99.77 refuses
the ninth `add_command` with `CASCADE_API_LIMIT`: register the keys you can live
without last, or register the row keys only when `CASCADE_STATE_ROW_KEYS` is set, as
above. An instrument's memory rows (`CASCADE_CAP_INSTRUMENT` with a row feed) follow
the same contract and behave the same way. The web remote does not show panel rows
at all, so a flagged row is not delivered to a browser.

## Changing the receiver

Every `set_*` is a **request**: validated and permission-checked at once,
queued, and applied by the GUI thread at the start of its next frame through the
same code a click on the window, the web API or CAT goes through - so you get
exactly the clamps and readbacks a user would. `CASCADE_API_OK` means accepted
and queued, not done; watch `get_state`'s counters to see it land (normally
within one frame, about 16 ms). Requests from one plugin are applied in the
order they were made.

| Function | Grant | Accepts | Notes |
|---|---|---|---|
| `set_frequency(hz)` | tune | 0 < hz < 1e12 | puts the tuned channel on `hz` the way the frequency readout does: the VFO offset is kept, the radio's centre moves |
| `set_vfo_offset(hz)` | tune | \|hz\| < 1e8 | clamped into the live band |
| `set_mode(mode)` | settings | `CASCADE_DEMOD_NFM`..`_RAW` | moves the bandwidth to that mode's default, as the mode keys do; follow with `set_bandwidth` to choose another |
| `set_bandwidth(hz)` | settings | 100 Hz..10 MHz | clamped to the live channel |
| `set_squelch(db)` | settings | -200..+20 dB | -200 is always open |
| `set_sample_rate(hz)` | settings | 8 kS/s..61.44 MS/s | `NO_DEVICE` without a radio; the radio may round or refuse - read it back |
| `set_gain(name, db)` | settings | within the stage's range, half a step of slack | `UNSUPPORTED` for a name the radio has not got |
| `set_device_agc(on)` | settings | | `UNSUPPORTED` when the radio has no AGC |
| `set_running(on)` | settings | | start or stop the receiver |
| `set_volume(v)` | settings | 0..1 | the user's volume dial |
| `set_muted(on)` | settings | | the user's mute |

NaN and infinity are `BAD_ARGUMENT` everywhere.

## The stream clock

What `process()` cannot see: **when** its samples were, and whether the stream
it has been counting is still the one it started counting.

```c
CascadeStreamInfo si = {0};
si.structSize = sizeof si;
host->get_stream_info(host->ctx, &si);       /* fine inside process() */
```

An **epoch** is one unbroken stream. It advances whenever the host rebuilds the
decoder instances - a sample-rate change, a source change, a rescan, the patch
page taking the radio - which is exactly when a decoder is destroyed and created
again. Within an epoch, `iqFrames` and `audioFrames` count every sample handed to
the receiver's I/Q and audio taps from `epochStartUnixMs`, so sample *n* of the
I/Q tap was captured at about `epochStartUnixMs + 1000 * n / iqRateHz` ("about":
USB buffering puts the true capture time some milliseconds earlier).
`outputRateHz` and `outputFrames` describe the speakers' stream. When the
receiver's own decoders are not running (the patch page has the radio), the
rates read zero and the epoch moves.

This is the **receiver's** clock. An instance the patch page created is fed from
its own radio at the rate its `create()` was told.

`get_stream_info` is lock-free - the host is holding its runner lock while it
calls your `process()`, and this function does not take it.

## Processing the audio

`CASCADE_CAP_AUDIO_PROCESSOR` puts your plugin **in** the audio chain:

```c
typedef struct CascadeAudioProcessorApi {
    uint32_t structSize;          /* sizeof(CascadeAudioProcessorApi) */
    uint32_t flags;               /* reserved, 0 - anything else is refused */
    const char *title;            /* "Voice filter": shown where the host lists it */
    void *(*create)(uint32_t rateHz, uint32_t channels);
    void (*process)(void *handle, float *interleaved, size_t frames);
    void (*destroy)(void *handle);
} CascadeAudioProcessorApi;
```

- **Where**: after the host's own AGC, squelch, notch, auto-notch and noise
  reduction; before a plugin's `AUDIO_OUT` takeover, the patch page's audio, the
  volume, the mute, the recorder, the web stream and the sound device. So a
  processor refines what the user hears, and everything the user can observe of
  the audio is the processed audio. It is **after** the point decoders are fed:
  no processor can reshape what a decoder measures.
- **What**: 32-bit float, interleaved, `channels` per frame, at `rateHz` - 48000
  and 2 today, but honour what `create()` is told. In place, same length; a
  processor with look-ahead carries its own delay line. A rate or channel change
  is a `destroy()` and a fresh `create()`.
- **Several** run in load order (sorted file name), each fed the previous one's
  output.
- **Safety**: any NaN or infinity you return is replaced with silence before the
  next processor or the speakers see it, and counted.
- **Bypass**: a stopped plugin gets no instance.
- **Threads**: `create`/`destroy` on the control thread (the settings store is
  allowed there - the Host API tour reads its limiter threshold in `create()`);
  `process` on the real-time thread under the usual rules: no blocking, no
  allocation, no I/O, no exceptions.

A host older than level 1 ignores the bit. A plugin that also declares a decoder
still loads there; a processor-only plugin is refused there as "built for a
newer version of FoxSDR", which is true.

## Transmitting (host API level 2)

`CASCADE_CAP_TX_AUDIO` (0x4000) lets a plugin - a packet TNC, a Winlink-compatible
link - send audio **through the host's transmitter**. It is the first capability
that can put a signal on the air, so it is built to be the hardest one to use by
accident, and the rule it changes is stated plainly in
[TRANSMITTING.md](TRANSMITTING.md): the transmitter used to be keyed only by a
hand, and is now keyed by *a hand, or a plugin the operator has granted and
armed*.

```c
typedef struct CascadeTxAudioApi {
    uint32_t structSize;   /* sizeof(CascadeTxAudioApi) */
    uint32_t sampleRateHz; /* 48000 - this host takes no other rate */
    uint32_t channels;     /* 1 */
    uint32_t flags;        /* 0 - anything else is refused */
    int32_t (*pull)(void *handle, float *mono, size_t frames);
    int32_t (*active)(void *handle);
} CascadeTxAudioApi;
```

- **No `create`/`destroy`.** Like `CASCADE_CAP_AUDIO_OUT` it rides on your first
  decoder instance (`CASCADE_CAP_DECODER`, then `_IQ_DECODER`, then
  `_IMAGE_DECODER`); `handle` is the one that decoder's `create()` returned. A
  plugin that declares the bit with none of the three is **refused at load** with
  that reason.
- **`active()`** is non-zero while you want the transmitter keyed. It is called
  once per audio block on the real-time thread, whether or not you hold any
  grant - that is how the host learns you have asked, and what makes the GRANT
  TRANSMIT key appear on your plate. Polling costs you nothing and keys nothing.
  Read state you have already decided; do not decide in it.
- **`pull()`** is called **only while the host has keyed the transmitter for
  you**, never before, on the real-time thread, never concurrently with
  `process()`/`active()` on the same handle. It returns the frames written
  (at most `frames`), `0` for "nothing right now" (the host sends silence and keeps
  the key only while `active()` says so), or a negative number for "stopped for
  good" (never called again). Each call asks for the shortfall of a 100 ms lead,
  at most 4800 frames; the audio reaches the air about 100 ms after you return it.
  The usual real-time rules: no blocking, no allocation, no I/O, no exceptions.
- **Keep `active()` non-zero for as long as you want the key.** When it goes to zero
  the host stops asking for audio, plays out what you already returned (at most
  that 100 ms), and opens the key - so the last sample you hand over is on the
  air. Every other reason to unkey is immediate and discards what was queued.

### What has to be true for the host to key

All of these at once, on every frame; any one false and the key opens:

1. **The grant.** The user pressed GRANT TRANSMIT on your plate. Per plugin, saved
   by module file name, off by default. `get_state` reports
   `CASCADE_STATE_TX_GRANTED`.
2. **The operator's arming window.** On the TRANSMIT page the operator presses
   ARM FOR PLUGINS: 5 MIN, 15 MIN or 60 MIN. The window closes itself, is never
   saved, and is refused when no transmit radio is open or no plugin holds a
   grant. `get_state` reports `CASCADE_STATE_TX_ARMED` to plugins that declared
   this capability.
3. **The TRANSMIT page is on screen** and the application's frame loop is running.
4. **Your `active()` is non-zero and recent** (the host will not act on a reading
   more than half a second old - a stopped receiver is not a request).
5. **The radio is open and has not faulted**, and **no hand is on the PTT, the
   LATCH or the web remote's key** - a hand always has precedence.
6. **No fault since the window was armed.**

And it **unkeys** on: your `active()` going to zero (after the tail above), a
negative `pull()`, the window closing or running out, the page closing, the radio
closing or faulting, the application's frame loop stalling, the grant being taken
back, or **60 seconds of continuous key** - the transmitter's own latch cap, not
a second constant. When the cap fires the window closes with it, so a plugin that
holds `active()` high for ever is cut once and stays cut until the operator arms
again. While you are keyed `get_state` reports `CASCADE_STATE_TRANSMITTING`; the
flags are published once a frame, so you see a change within about 16 ms.

### What you do not choose

**Frequency, power and mode.** The transmitter is on the TRANSMIT page's
frequency (or the receiver's dial), at its power, in its mode - or, with the
page's **DATA (flat)** tick on (the default), **NFM with the pre-emphasis off**,
whatever mode the page shows: a data modem's tones have to reach the far receiver
flat. The host replaces a NaN or an infinity with silence and clamps your samples
to +/-1 before the modulator sees them. If you need a different frequency or
mode, tell the user in your own words and let them set it.

### Checklist for a transmitting plugin

- Check `CASCADE_HOST_HAS(host, get_state)` and read `CASCADE_STATE_TX_GRANTED`
  and `_TX_ARMED`; say plainly what the user must do (grant, arm) when they are
  clear, instead of sitting there asking.
- Keep running: a plugin the user has not opened (AUTO) is dormant and is never
  asked, so give the plugin a window, or ask the user to tick **Keep running**.
- Hand the host exactly the audio you want sent and no more; pad your own tail
  if the far end needs one.
- Expect the host to cut you at any moment (the window, the page, a fault, the
  cap) and be ready for `pull()` simply not being called again until the next
  key.

A host older than level 2 ignores the bit and never calls either function; test
`knownCapabilities & CASCADE_CAP_TX_AUDIO`.

## Marks on the spectrum and waterfall

```c
CascadeMarker m = {0};
m.structSize = sizeof m;
m.id = 1;                           /* yours; setting an id again replaces it */
m.kind = CASCADE_MARKER_POINT;      /* or CASCADE_MARKER_SPAN with widthHz */
m.freqHz = 145.500e6;
m.colourRgba = 0x40C0FFFF;          /* 0 = the host's own plugin colour */
snprintf(m.label, sizeof m.label, "calling");
host->set_marker(host->ctx, &m);
```

Up to 64 per plugin (`LIMIT` past that). `CASCADE_MARKER_FLAG_DASHED`;
`CASCADE_MARKER_FLAG_SPECTRUM_ONLY` keeps one off the waterfall. The host draws
them under its own tuning furniture and never lets them take a click, floors the
alpha so a mark cannot be invisible, clips a span to the view, and removes every
mark of a plugin that is stopped or unloaded. `remove_marker(id)` and
`clear_markers()` do what they say.

## Your own settings

```c
char buf[64];
int32_t n = host->settings_get(host->ctx, "station", buf, sizeof buf);
if (n >= 0 && n < (int32_t)sizeof buf) { /* buf holds it, NUL-terminated */ }
host->settings_set(host->ctx, "station", "IO93");
host->settings_set(host->ctx, "station", NULL);   /* delete */
```

- Stored in the application's config file, keyed on your plugin's **name** (the
  descriptor's), so they survive an update, which changes the file name.
- Keys: 1..63 bytes of `[A-Za-z0-9._-]`. Values: UTF-8, under 4096 bytes. 64
  keys per plugin. A plugin sees only its own keys.
- `settings_get` returns the value's full length and copies what fits, always
  NUL-terminated and never cut inside a character, so a return `>= cap` means it
  was cut. Pass a NULL buffer and `cap` 0 to measure.
- **Not on the real-time thread** (`WRONG_THREAD`).
- Written to disk by the host's own config save, within a few seconds. The
  application's last save happens **before** plugins are destroyed at exit, so
  store a setting when it changes, not from `destroy()`.
- The host loads the saved values before it re-attaches plugins at startup, so a
  plugin that reads its settings in `attach()` finds them - on the attach after
  the configuration has loaded (see "may run again", above).

### Fields the host draws

`CASCADE_CAP_SETTINGS_UI` (0x1000) lets you describe a small form - up to 16
`CascadeSettingSpec` entries - instead of asking the user to edit anything by
hand. The host draws one field per entry wherever your controls appear, writes
each accepted edit into the store above, and advances `settings_seq`, so you
know to read the keys again with `settings_get`. The kinds:

| Kind | The host draws | Stored as |
| --- | --- | --- |
| `CASCADE_SETTING_TEXT` | a text box, up to `maxLength` UTF-8 bytes | the text |
| `CASCADE_SETTING_NUMBER` | a number box | decimal text |
| `CASCADE_SETTING_BOOL` | a checkbox | `"1"` or `""` |
| `CASCADE_SETTING_PATH` (0.99.74) | a text box and a **BROWSE FILES** key | the path as UTF-8 text, at most `maxLength` bytes |

A `PATH` field is for a file the user has to point you at - the program you
drive, say. The key opens the system's file dialog (Windows' own; on Linux
`zenity`, else `kdialog`, and with neither installed the host draws the box
alone). A chosen path goes down the same route as a typed one, and a cancel
changes nothing. The host never opens or checks the file, and does not require
it to exist: that is yours to do when you read the value back. A `placeholder`
ending in `.exe` makes the dialog offer programs first. **A host older than
0.99.74 does not know kind 3 and skips the field** (an unknown kind is skipped,
never refused), so a plugin that uses it must still behave when the value was
never set.

## Messages

```c
host->log(host->ctx, CASCADE_LOG_WARN, "No position fix yet - set the receiver's position.");
```

`INFO` is a line in the Decoder output window, labelled with your plugin's name.
`WARN` and `ERROR` are that too, and the newest one is also shown on your plate
in Fitted modules (`ERROR` as an alarm). `DEBUG` is accepted and dropped. Text is
cut at 511 bytes on a character boundary; a newline starts a new line. The queue
holds 256 messages between frames (`BUSY` past that).

**Messages are shown, not collected**: the host does not write them into its
diagnostic log or into any report it can send.

## Keys on your plate

```c
host->add_command(host->ctx, 1, "Scan now");   /* or relabel id 1 */
...
uint32_t id;
while (host->poll_command(host->ctx, &id) == 1) { run_command(id); }
```

Up to 16 keys (8 before FoxSDR 0.99.77, which refuses the ninth with `LIMIT`),
drawn on your plate in Fitted modules in the order you added them, two to a line,
labelled in your own words (the host does not translate them). A command can also
be bound to a row of your panel, so the user presses the row instead of a key on
your plate ([Panel rows the user can press](#panel-rows-the-user-can-press-09977)).
A press is queued
(16 deep; the oldest is dropped past that) until you take it with
`poll_command`, which you should call from wherever your plugin already runs
regularly - a `poll_rows`, a `poll_state`, a `process()`. `remove_command(id)`
withdraws a key and any presses of it still waiting. A stopped or unloaded
plugin's keys disappear.

## The patch page

Every decoder you declare appears in the patch page's parts bin as it always
has, and runs there with the same table: the level-1 functions work from inside
a patch instance's `process()` too (it runs on a real-time thread, so the
settings store refuses it there as well). Two things differ on the patch page:
`get_stream_info` describes the receiver's chain, not your patch instance's
stream, and the receiver's own processors are not in the patch page's audio.
The host test suite runs a level-1 plugin's decoder on the patch page and
requires every call it makes from there to be answered.

## A minimal plugin

A complete plugin that meters the audio, reads the receiver from its panel, and
offers one key that tunes up 25 kHz. It degrades on an older host.

```cpp
#include <atomic>
#include <cmath>
#include <cstdio>
#include "plugin_abi.h"

namespace {
std::atomic<const CascadeHostApi*> g_host{nullptr};
std::atomic<float> g_db{-200.0f};

void attach(const CascadeHostApi* h) {
    g_host.store(h);
    if (CASCADE_HOST_HAS(h, add_command)) { h->add_command(h->ctx, 1, "Up 25 kHz"); }
}

void* create(uint32_t) { static int t; return &t; }
void process(void*, const float* x, size_t n) {           // real-time thread
    double s = 0; for (size_t i = 0; i < n; ++i) s += double(x[i]) * x[i];
    if (n) g_db.store(float(10.0 * std::log10(s / double(n) + 1e-20)));
}
int32_t pollText(void*, char*, size_t) { return 0; }
void destroy(void*) {}

void* panelCreate() { static int t; return &t; }
uint32_t columns(void*, char h[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS]) {
    std::snprintf(h[0], CASCADE_PANEL_CELL_CHARS, "Tuned");
    std::snprintf(h[1], CASCADE_PANEL_CELL_CHARS, "Level");
    return 2;
}
int32_t rows(void*, CascadePanelRow* out, uint32_t cap) {  // GUI thread, every frame
    const CascadeHostApi* h = g_host.load();
    if (cap == 0) return 0;
    out[0] = CascadePanelRow{};
    std::snprintf(out[0].cells[1], CASCADE_PANEL_CELL_CHARS, "%.1f dB", g_db.load());
    if (!CASCADE_HOST_HAS(h, get_state)) {
        std::snprintf(out[0].cells[0], CASCADE_PANEL_CELL_CHARS, "needs FoxSDR 0.99.31");
        return 1;
    }
    CascadeReceiverState s{};
    s.structSize = sizeof s;
    if (h->get_state(h->ctx, &s) != CASCADE_API_OK) return 1;
    uint32_t id;
    while (h->poll_command(h->ctx, &id) == 1) {
        if (id == 1 && h->set_frequency(h->ctx, s.tunedHz + 25e3) == CASCADE_API_DENIED) {
            h->log(h->ctx, CASCADE_LOG_WARN, "Grant RECEIVER CONTROL to use Up 25 kHz.");
        }
    }
    std::snprintf(out[0].cells[0], CASCADE_PANEL_CELL_CHARS, "%.6f MHz", s.tunedHz / 1e6);
    return 1;
}

const CascadeHostClientApi kHost = {sizeof(CascadeHostClientApi), &attach};
const CascadeDecoderApi kDec = {sizeof(CascadeDecoderApi), 0, &create, &process, &pollText, &destroy};
const CascadePanelApi kPanel = {sizeof(CascadePanelApi), "Tiny", &panelCreate, &columns, &rows, &destroy};
const CascadeCapabilityEntry kCaps[] = {
    {CASCADE_CAP_HOST_CLIENT, sizeof kHost, &kHost},
    {CASCADE_CAP_DECODER, sizeof kDec, &kDec},
    {CASCADE_CAP_PANEL, sizeof kPanel, &kPanel},
};
const CascadePluginDesc kDesc = {sizeof(CascadePluginDesc), CASCADE_PLUGIN_ABI_VERSION,
    "Tiny", "1.0.0", "You", "MIT",
    CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_DECODER | CASCADE_CAP_PANEL, 3, kCaps};
}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(uint32_t v) {
    return v == CASCADE_PLUGIN_ABI_VERSION ? &kDesc : nullptr;
}
```

(Wrap every entry point in `try { ... } catch (...) { ... }` in a real plugin -
no exception may cross the boundary. The Host API tour shows the full shape.)

## What the host does not offer

Deliberately: no file access, no network access, no device enumeration or
opening, no drawing surface. A plugin that needs a file or a socket uses its
own; routing them through the host would only launder them. A plugin that needs
a picture uses the image, panel, instrument and map capabilities the header
already has.

## Checklist before you ship

- Every level-1 call is behind `CASCADE_HOST_HAS`, and the plugin says what it
  cannot do on an older host rather than going quiet.
- No `settings_*` call on the real-time thread; no allocation, blocking or I/O
  in `process()`.
- A refused request tells the user which grant would help.
- A plugin that transmits reads `CASCADE_STATE_TX_GRANTED` / `_TX_ARMED` and tells
  the user what is missing; it never assumes it will be keyed (see
  [Transmitting](#transmitting-host-api-level-2)).
- Settings are stored when they change, not in `destroy()`.
- The table pointer is kept in an atomic (attach and the real-time thread both
  read it) and `attach()` is idempotent.
- Test against a fake host with a full table, an old-size table
  (`structSize = offsetof(CascadeHostApi, apiLevel)`) and no table - the Host
  API tour's test is a template.
