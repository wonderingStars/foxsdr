# Transmitting

The ADALM-Pluto transmitter and the rules about when it may be keyed.

Back to the [README](../README.md).

## Transmitting

**FoxSDR transmits, through an ADALM-Pluto, over the same IIOD protocol it
receives on** — no libiio, no SoapySDR, no vendor library anywhere in the path.
CW, AM, narrow FM, USB and LSB, from a microphone or from a built-in test tone.

**This transmits.** An ADALM-Pluto puts out about +7 dBm — roughly five
milliwatts — and whether you may radiate it on the frequency you have set is
your responsibility and your country's licensing authority's, not this
software's. The same sentence is on the page, once, where the controls are.

**Where it is.** SIGNAL PATH (F1) → **Transmit**, which opens a page. The switch
on the rail is a switch and a lamp; every control that can put RF out of a
connector is on the page, together, where you can see all of them at once.

### The rules about when it is keyed

These are the part worth reading, and they are enforced in the code rather than
described in it — `src/core/transmitter.hpp` carries the same list beside the
implementation.

- **Only a hand, or a plugin the operator has granted and armed, can key it** —
  a PTT held down, or a LATCH switch deliberately closed, at this machine or
  (since 0.95.1) on the web remote's own held key; or (since 0.99.74) a plugin
  that holds the TRANSMIT grant, inside an arming window the operator opened on
  this page (see [Plugins that transmit](#plugins-that-transmit)). Nothing
  else: not a plugin that has not been granted and armed, not a command line,
  and not a config file. This used to read "not a plugin" without the
  qualification; it changed on the owner's written approval of 2026-10-12, for a
  Winlink-compatible radio link, and the rules below are what make the new
  sentence as hard to satisfy by accident as the old one was.
- **Nothing is restored.** The mode, the power, the input, the split and the
  tone are saved and come back; the key is not saved, and there is no field in
  `config.json` that could carry it. Neither is a plugin's arming window: the
  grant is saved (it is a decision about a piece of software), the window is not
  (it is a decision about *now*). A file that has one anyway changes nothing
  — `tests/test_config.cpp` loads exactly such a file and checks that it does
  not.
- **Letting go stops it.** The PTT is held, not clicked, so releasing the
  mouse anywhere — including off the key and outside the window — opens it. So
  does closing the page, switching rail banks, or the window losing focus: the
  key request is rebuilt from nothing on every frame rather than remembered.
- **The latch releases itself after a minute.** Somebody who walks away from a
  latched transmitter with a live microphone is transmitting the room. A
  plugin's key has the same minute (it is the same constant, not a second one).
- **A frozen window releases it.** The GUI thread stamps a timestamp every
  frame and the transmit thread watches for it; if the frames stop, the
  transmit thread silences the radio itself. This is the one failure in which
  nothing on the GUI thread could do it, because the thing that would normally
  release the key is the thing that has stopped.
- **A fault releases it.** A board that stops answering unkeys rather than
  being retried.
- **The spacebar is a PTT**, but only while the TRANSMIT page has focus — and
  it is rebindable in SYSTEM → Settings like every other key.

### What the driver does to the board

- **Opening a board leaves it QUIET, without being asked.** A Pluto keeps
  whatever the last program left in it, so one handed over at 0 dB attenuation
  with its transmit oscillator up would start radiating the moment anything fed
  the DAC. `open()` writes the attenuation to the board's **own** maximum and
  powers the TX LO down before it returns, and says so in the log.
- **Keying is the LAST step of starting.** The port, the frequency, the rate,
  the internal DDS shutdown and the buffer all happen while the board is still
  at maximum attenuation; the power you asked for is written after every one of
  them. So a failure halfway through leaves a silent radio, which is what
  `tests/test_pluto_tx.cpp` checks by refusing one of those writes.
- **Stopping silences before it tidies**, in that order: attenuation to
  maximum first, then the oscillator down, then the buffer. The reverse would
  leave a keyed board transmitting whatever the DAC held for as long as the
  tidying took. The destructor does the same, for a caller who forgot.
- **A board that will not say how quiet it can be is not keyed.** Everything
  above is written in terms of the board's own published maximum attenuation;
  without one there is no number to write, so FoxSDR refuses to transmit
  through it rather than inventing a figure or skipping the step.
- **The power control is an ATTENUATION.** An AD9361 publishes 0 dB as *full
  output* and −89.75 dB as its quietest. The slider runs left-quiet to
  right-loud so nothing on the panel has to be read backwards, and a value the
  driver cannot honour — out of range, hand-edited, a units mix-up — lands on
  **silence**, not on the maximum. That is the opposite of what every receive
  gain in this product does, and it is deliberate.
- **The board's own test tones are switched off.** An AD9361 has internal DDS
  generators, and a Pluto left with them running plays those instead of
  whatever is written to it — which looks exactly like a driver that is
  sending nothing.

### The modulators

At 48 kHz, in `src/dsp/modulator.cpp`, then interpolated to whatever the board
is clocked at.

- **CW** — a keyed carrier, with a 5 ms raised-cosine edge. That shaping is the
  substance of the mode: a carrier switched on in one sample is a step, and a
  step's spectrum is everywhere. Measured 70 dB quieter 4–6 kHz off frequency
  than the same burst keyed hard.
- **AM** — carrier and both sidebands at a modulation index of 0.9, so an audio
  peak cannot take the envelope through zero.
- **NFM** — 2.5 kHz deviation with 750 µs pre-emphasis and a hard limit on the
  instantaneous deviation, applied *after* the pre-emphasis. Checked against the
  Bessel functions: at a 1 kHz tone and full deviation the carrier and the first
  three sidebands read 0.049 / 0.497 / 0.446 / 0.217, against J0…J3(2.5) of
  0.0484 / 0.4971 / 0.4461 / 0.2166.
- **USB / LSB** — the analytic signal through a 129-tap Blackman-windowed
  Hilbert transformer, measured at 69 dB of opposite-sideband suppression.
- **A test tone**, because the first question about any transmit path is "is
  anything coming out at all", and the answer must not depend on a microphone
  being plugged in and talked into. It is also the default input: a transmitter
  that came up pointed at a microphone is one that would put a room on the air
  the first time somebody pressed the big key to see what it did.

### Getting 48 kHz onto a 2.5 MS/s DAC

In two stages, because the obvious tool is the wrong one — a rational
resampler asked for 2,083,333 / 48,000 would build two million polyphase
branches. An integer interpolation by round(sink ÷ audio) with a real
windowed-sinc anti-imaging filter does the work; a linear interpolation covers
the half-percent left over, which is exact to parts per million against a signal
that is already oversampled fifty times. Measured first-image rejection: 110 dB.

### Receiving while transmitting

The Pluto is full duplex and the receiver keeps running, which is the point of
keeping it running. Whether you **hear** it is a switch on the page, off by
default, because a receiver on the frequency it is transmitting on is a howl.
**SPLIT** unlinks the transmit frequency from the dial — what a repeater, a
pile-up and a satellite each need — and the key stays lit while it is on,
because that is the state in which you are not listening where you are
transmitting.

### The web remote gets a key, and it is a key you have to keep holding

0.95.0 gave the browser a light and no switch, on the argument that a key
closable from anywhere on the network is a transmitter anybody who reaches the
page can operate. 0.95.1 gives it the switch, with the authorisation, the
failsafe and the argument that were said to be its price.

**What the page has.** A PTT under **Transmit** in the control column, the same
licence sentence the desktop page carries, and a line beside the key that reads
`no transmitter`, `ready`, or `ON AIR` with the hold counting down.

**The rules it is allowed under, none of which the desktop key needs:**

- **It is held, and it expires.** `POST /api/control {"transmitPtt":true}` buys
  **two seconds** (`Transmitter::kRemotePttHoldMs`) and no more; the page
  re-asserts every 500 ms while your finger is down. A tab that is closed,
  hidden, backgrounded, frozen by a phone or simply disconnected stops
  extending the hold and the key opens by itself. `{"transmitPtt":false}` opens
  it at once, and so does the pointer coming up anywhere, the window losing
  focus, or the tab being hidden.
- **It cannot latch.** A body carrying `transmitLatch` is refused by name, not
  ignored: a latch keeps a radio keyed with nobody touching anything, and
  "nobody touching anything" is the ordinary state of a machine at the far end
  of a network.
- **It needs a transmitter you opened, on a page you have open.** The request
  is refused with `409` and a sentence unless the application is publishing
  `transmitAvailable` — a radio open **and** the TRANSMIT page on screen. So a
  remote key can only ever be closed while the operator has the transmitter in
  front of them, and closing the page revokes it.
- **It is behind the same gate as everything else here**, which on an
  off-machine bind means a password (`net/web_policy.hpp` refuses that bind
  without one).
- **Everything that releases the local key releases this one** — a fault, the
  dead-man's handle, the radio being closed or swapped, the application
  shutting down — **and so does the web server stopping**, which queues a
  release rather than waiting for the hold to expire.
- **Nothing else changed.** No config, no startup and no queued request can
  close it: `transmitPtt` is a live instruction that is refused against the
  state of this instant, never a setting, and `config.json` still has nowhere
  to put a key.

`/api/status` carries `transmitting`, `transmitAvailable` and
`transmitRemoteHold` (milliseconds left on the hold, 0 when it is not held), so
a remote listener seeing a dead band still knows the reason whether or not they
are the one keying it.

### Plugins that transmit

A plugin that declares `CASCADE_CAP_TX_AUDIO` (host API level 2; see
[PLUGIN-API.md](PLUGIN-API.md)) can hand FoxSDR audio to send, through this
transmitter. It never keys anything itself: it says it *wants* the transmitter
(`active()`), and FoxSDR decides. For a plugin to key the radio, **all** of
these have to be true at once, on every frame:

1. **The plugin holds the TRANSMIT grant.** A per-plugin key, **GRANT
   TRANSMIT**, on the plugin's page in Fitted modules. It is off by default and
   is offered only after the plugin has asked at least once (its `active()` has
   been non-zero), so a plugin that never wanted the transmitter never has the
   key on its plate. The grant is saved by module file name, like the receiver
   control and radio settings grants, and can be withdrawn at any moment.
2. **The operator has armed the page.** On this page, **ARM FOR PLUGINS** with
   **5 MIN**, **15 MIN**, **60 MIN** or **OFF**. The window counts down on the
   page with the name of the plugin it lets in. It closes by itself, never
   survives a restart (there is nowhere in `config.json` to put it), and is
   refused, with the reason printed under the keys, when no transmit radio is
   open or no plugin holds the grant.
3. **This page is on screen** (open and not rolled up) and the application's
   frame loop is running.
4. **The plugin's own `active()` is non-zero and recent.** A reading more than
   half a second old is not a request.
5. **The radio is open and has not faulted**, and **nothing else is keyed**: the
   PTT, the LATCH and the web remote each take the key, and the plugin's audio
   goes with it.

Any one false and nothing is keyed. And these *end* a plugin's transmission,
each at once and each writing a line in the log:

- the plugin lets go (`active()` goes to zero) — after the audio FoxSDR has
  already taken from it (at most a tenth of a second) is played out;
- its audio reports a permanent error (`pull()` returns a negative number);
- the arming window running out, **OFF**, or the page closing, being rolled up,
  or the frame loop stalling for more than a second;
- the radio being closed or faulting, or any fault the transmit thread meets;
- the TRANSMIT grant being withdrawn;
- **a minute of continuous key** (`Transmitter::kLatchTimeout`, the latch's own
  number). When it fires the arming window closes with it, and the plugin stays
  cut until the operator arms again: a plugin that holds the key for ever is cut
  once, not once a minute.

**What the plugin cannot choose.** The frequency is this page's (or the
receiver's dial); the power is this page's slider; the mode is this page's — or,
with the **DATA (flat)** tick on (the default), **NFM with the pre-emphasis
off**, whatever mode is selected, because a data modem's tones must reach the far
receiver flat. The microphone and the tone are never affected by that tick. The
audio is clamped to ±1 and any NaN or infinity becomes silence before the
modulator sees it. A second plugin that also wants the transmitter waits, and is
told so on its plate; exactly one holds it at a time.

**What a plugin is told**, through `get_state`: that it holds the grant, that
the window is open, and that it is keyed (the last two only if it declared the
capability).

**One thing to know.** A plugin the user has not opened is dormant (the run
states of 0.99.73) and is never asked whether it wants the transmitter. A
transmitting plugin should have a window, or be set to **Keep running**.

### How it is verified

There is no Pluto on the bench this was written on and nothing on it may
transmit, so the proof is the conversation and the arithmetic rather than a
spectrum analyser. `tests/test_pluto_tx.cpp` runs a fake `iiod` on the loopback
interface — a real socket, real threads, written from the daemon's own
published grammar — and checks every command, in order, on both connections:
that opening quietens, that keying is last, that stopping silences first, that a
refused write leaves the board quiet, that the destructor does it for a caller
who forgot, and that a board which vanishes mid-transmission faults, stops and
is silenced through whatever connection still works. The sample bytes are read
back off the wire and decoded with the format the board published.
`tests/test_modulator.cpp` measures every mode against its closed form;
`tests/test_transmitter.cpp` drives the whole path into a sink that records what
it was given and exercises each of the release rules above, including pulling
the frames away and watching the transmit thread silence the radio by itself.
The plugin key has its own tests: `tests/test_tx_audio.cpp` turns every
condition above off on its own against the other seven (all 256 combinations of
the decision, and one run of the real transmitter into a recording radio per
release rule: the cap, a fault, the frozen window, a radio that will not key, a
hand taking the key, hostile samples), and `tests/test_tx_audio_app.cpp` does
the same through the real application window, the real plugin runner and a real
plugin module that counts every call the host makes to it, so "it was never
pulled from" is read off the module and not off what the application says.

**What is NOT verified:** nothing has been into an antenna, a dummy load or a
second receiver. No real board has been keyed, no real modulation has been
heard, and the output power, the spectral purity and the actual behaviour of a
Pluto's DAC and filters are all unmeasured. What is established is what FoxSDR
sends and when.
