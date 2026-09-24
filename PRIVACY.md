# Privacy

FoxSDR collects nothing about you unless you switch it on, and this document
lists exactly what it sends when you do.

## The update check

Once per launch, FoxSDR asks `https://foxsdr.com/api/update` whether a newer
version exists. The whole request is:

    GET /api/update?v=0.55.0

That is the entire payload: the version running. No identifier of any kind is
sent, no cookie is stored, and the answer is not cached anywhere that could be
correlated with a later request. It is a separate thing from the usage report
and shares no state with it.

The answer names the newest build, its checksum, and what changed since your
version - which is shown to you in the application, so you can decide whether
an update is worth the interruption rather than being told only that one
exists.

**Nothing is downloaded or installed unless you press the button.** When you
do, the installer is fetched over https and its SHA-256 is checked against the
digest the server published *before* the file is given a name anything could
run. A download that does not match is deleted.

Turn it off under **Settings -> Updates**. With it off the application never
contacts the update service, and never learns that a new version exists.

Why it defaults to on: version 0.55.0 fixed a fault that stopped every earlier
build from detecting any radio at all. Of the 49 people who had downloaded one
of those builds, 46 never returned to the website, and there was no way to
reach them - the downloads are anonymous, which is the point. Most of them are
probably still running it. A check that defaulted to off would have reached
exactly as many.

## The short version

- **Usage reporting is ON by default, and you can turn it off.** It sends the
  anonymous counts listed below and nothing else. Switching it off stops all
  reporting and deletes the identifier described below.
- **While the application is open it also sends a small "still running" beat
  every five minutes** — the install identifier, the version, and nothing
  else — so we can count how many copies are running at a given moment.
  It is governed by the same switch: reporting off means no beats, ever.
- **No personal data is collected**, and no IP address or location is recorded.
- **Crash and freeze reports are written to your machine, and — if you leave
  Diagnostics on — the report *text* is sent on the next start.** Not from
  inside the crash: the program that just failed cannot safely open a network
  connection, so the report waits on disk until the next time you open FoxSDR.
  Exactly what it contains is listed field by field below. **A full memory dump
  is never sent, under any setting**, and switching Diagnostics off stops the
  sending as completely as it stops the writing.
- **Nothing about what you listen to is ever collected** — no frequencies, no
  positions, no decoded messages. Not when reporting is on, not ever.
- **The update check is ON by default, and you can turn it off.** Once per
  launch the application asks foxsdr.com whether a newer version exists. It
  sends **the version you are running and nothing else** — no identifier, no
  install id, no cookie kept — and it is not the usage report; the two share
  nothing. Nothing is downloaded or installed without you pressing a button.
- The application makes one other network request, to fetch the plugin
  catalogue from GitHub (raw.githubusercontent.com): the first time you
  open the plugin store window in a session, and whenever you press
  **CHECK NOW** in it. Nothing is fetched at startup.
- The Satellites plugin, when it is fitted and tracking, fetches orbital
  element sets from CelesTrak about every twelve hours, falling back to a
  copy at foxsdr.com/tle/all.tle when CelesTrak does not answer. The request
  carries no identifier.
- **A feature request is sent only when you press SEND on the REQUEST A
  FEATURE page.** It carries the text you typed, an optional contact line, and
  which build you are running - never an identifier, never a frequency, never
  the log, never anything from your config. Exactly what it contains is listed
  field by field below.
- **A bug report or a dislike is sent only when you press SEND on the REPORT A
  BUG / DISLIKE page.** It carries which of the two you chose, the text you
  typed, an optional contact line, and which build you are running - the same
  as a feature request plus that one choice, and nothing else. It is not a
  crash report and carries nothing a crash report does. Listed field by field
  below.

## What is sent when usage reporting is enabled

One report per launch, describing the session that just finished:

| Field | Example | Why |
|---|---|---|
| Install identifier | `4f9c…` (32 random hex characters) | Tells 100 users apart from one user launching 100 times. Generated at random on your machine on first run, and **deleted when you turn reporting off**, so a later change of mind cannot be joined back to it; not derived from your hardware, network or name. |
| Application version | `0.48.0` | Whether people update, and whether an auto-updater is needed. |
| Operating system | `Windows 10.0.22631` | Which platforms are actually used, and whether a Linux build is worth building. |
| Architecture | `x64` | As above. |
| Launch count | `12` | Whether the software gets used more than once. |
| Crash count | `1` | How often it fails. |
| Session length | `3600` seconds | Whether sessions are minutes or hours. |
| SDR model | `uhd b200` | Which radios to prioritise. **Serial numbers are stripped** before sending. |
| Demodulators used | `WFM: 3000s` | Which modes justify further work. |
| Panels opened | `map, decoded` | Which features are used. |
| Installed plugins | `ADS-B 1.0.0` | Which decoders justify further work. |

That is the complete list. The payload is asserted field-by-field by an
automated test (`tests/test_telemetry.cpp`), so a new field cannot be added
without that test failing and this document being updated with it.

### The "still running" beat

While the application is open, and only while usage reporting is on, it sends
one very small message every five minutes:

| Field | Example | Why |
|---|---|---|
| Install identifier | `4f9c…` (the same one as above) | So five beats from one copy count as one running copy, not five. |
| Application version | `0.65.0` | So "running now" can be split by version. |
| Beat marker | `1` | Tells the receiving end this is a beat, not a session report. |

Nothing from the session rides along: no modes, no panels, no radio model, no
durations. The launch report above exists because it cannot say who still has
the application open — a report is written when the application *starts* — and
the beat exists solely to answer that one question honestly. Beats are stored
apart from the session reports, so none of the usage figures change meaning.
This payload is held to exactly these three fields by the same automated test
as the launch report, and the same switch stops it: reporting off, no beats.

## Crash and freeze reports — what they contain

If FoxSDR crashes or freezes it writes a file on your machine. With Diagnostics
switched on, the contents of that file are also sent to us on the **next**
start — never from inside the failure itself. The complete list of what is sent
is in *What is sent when a report is uploaded*, further down; the memory dump is
not in it and never will be.

Reports live in `%LOCALAPPDATA%\FoxSDR\crashes\`, and the rotating application
log in `%LOCALAPPDATA%\FoxSDR\logs\`. **Settings → Diagnostics** shows both
paths, turns the whole thing off, and has a **Copy diagnostics** button that
puts exactly the contents below on your clipboard so you can read it before you
send it to anyone.

A report contains these fields and no others:

| Field | Example | Why |
|---|---|---|
| `generated` | `2026-08-25 14:02:11` | When the bundle was made. |
| `version` | `0.61.0` | Which release. |
| `commit` | `5ba13f6d0c86`, or `5ba13f6d0c86-dirty` | Which build. A version names a release; only the commit names a build, and the offsets in a report are meaningless against the wrong one. The `-dirty` suffix means the tree had uncommitted changes, so that commit is the nearest tree rather than the exact one. |
| `os` | `Windows 10.0.22631` | Whether a fault is specific to a Windows version. |
| `arch` | `x64` | As above. |
| `mode` | `WFM` | What the receiver was doing. |
| `source` | `soapy` | Generator, I/Q file, or a real radio. |
| `sample-rate` | `2400000` | Faults that only appear at high rates. **This is the sample rate, not a tuned frequency** — see below. |
| `device-open` | `yes` | Whether a radio was in use. |
| `sdr-model` | `uhd b200` | Which radio. **Serial numbers are stripped**, exactly as in the usage report. |
| `plugin` | `ADS-B 1.1.0` | Plugins are third-party code running inside the application, and which one was loaded has already been the answer to real faults. |
| `log-path`, `crash-dir` | `%LOCALAPPDATA%\FoxSDR\logs/foxsdr.log` | So you know where the rest of it is. Since 0.99.33 the base directory is written as `%LOCALAPPDATA%` (`$HOME` or `$XDG_STATE_HOME` on Linux) rather than the real path, so your account name is not in it; a folder elsewhere is shown with the account name replaced by `<user>`. |
| `last-run-unclean` | `yes` | Whether the previous session ended without shutting down. |
| `launches`, `crashes` | `12`, `1` | The same two counters the usage report already keeps. |
| `log-lines-total` | `4011` | How much of the log the report is *not* carrying. |
| the log | the last 256 lines | State changes — source opened, rate set, plugin started — never signal content, and **never the name or path of a file you opened**. When an I/Q file fails to reopen the log records that it did not reopen; the file name stays on screen, where you already know it. Since 0.89.0 the log also records what the radio driver said (`soapy:` lines) and what its libraries printed to the standard error stream (`vendor:` lines), with serial numbers stripped and the digits of any line mentioning a frequency masked — see the `log` row under *What is sent when a report is uploaded* for the exact rule. Since 0.99.33 the bundle's log is scrubbed by exactly that rule, the same as an uploaded report's, because a bundle is made to be pasted somewhere. |

A crash or freeze report written by the application itself carries the same
context block as the table above — the same bytes, so the two cannot drift —
and adds a header of its own. **A crash report:**

| Field | Example | Why |
|---|---|---|
| `kind` | `crash` | Which of the two documents this is. |
| `reason` | `access violation` | What went wrong, in words. |
| `code` | `0xC0000005` | The same thing as a number, because the words are a lookup table and the number is not. |
| `address` | `0x00007FF6…` | Where it faulted. An address inside program code. |
| `signature` | `A31F…` (16 hex digits) | Groups repeats of one bug together. Derived from the fault kind, the faulting module and the offset inside it — never from the time and never from anything about you. |
| `thread` | `24180` | Which thread faulted. An operating-system thread number, meaningless outside that dead process. |

**Crash reports are also written by a session that did not die**, and they
carry exactly the fields above and nothing extra. The `reason` line says which.
The first is a fault raised inside a third-party radio driver while the
application was looking for hardware, which is caught and turned into "no
devices found" instead of a crash. The second is the device search itself,
which runs in a small separate process so that a driver falling over cannot
take the session with it — when that process dies, the session records what
happened to it, including when the search then succeeded on a second try. When that happens the
report carries NO call stack at all, and says so: the fault was in the other
process, and this one has nothing to show. It adds the two `child-` lines in
the process block below and nothing else, and no memory dump is written for it.
That
small process can also write a report of its own, into the same folder and with
the same fields, and only when diagnostics are switched on: it is handed the
folder by the session that started it and is told nothing at all when the
setting is off. These used to be either a dead application or nothing at all;
none of them adds a field, and all are governed by the same switch in
**Settings → Diagnostics** as every other report on this page.

**A freeze report:**

| Field | Example | Why |
|---|---|---|
| `kind` | `hang` or `stall` | As above. `stall` means the wait was inside the display driver or the window system rather than inside FoxSDR - a monitor switched off, a resolution change, a remote session, or on Linux a window the compositor is not showing. Those are kept on your machine and never sent. |
| `note` | `the gui thread did not complete a frame within the threshold` | The same distinction in one sentence, so a report says what it is without anyone having to know the codes. |
| `stalled-ms` | `7213` | How long the interface had been unresponsive. |
| `threshold-ms` | `5000` | What it was measured against, so the number above can be judged. |
| `signature` | `7C04…` | As above. |
| `threads` | `9` | How many stacks follow. |

After that header both add the call stacks — every thread for a freeze, the
faulting one for a crash — and the list of loaded modules with their build
identifiers. Those are addresses inside program code and identifiers of
compiled files. They describe FoxSDR, not you.

Since 0.89.0 both also carry a short **process** block, after the stack in a
crash report and before the stacks in a freeze report:

| Field | Example | Why |
|---|---|---|
| `uptime-sec` | `2731` | How many seconds the application had been running. A fault forty seconds after a rate change and one three hours in are different bugs at the same address. |
| `fault-thread-own` | `yes`, `no` or `unknown` | Crash reports only. Whether any frame of the faulting thread's stack lies in FoxSDR's own executable: `no` means a thread a radio driver created and ran entirely in its own code, which a report could not previously say. `unknown` when the stack could not be walked. |
| `child-exit-code` | `0xC0000005` | Child-process faults only (0.96.4). What the small device-search process died of - the same number Windows gave it. |
| `child-attempt` | `1` | Child-process faults only. Which try it was, since the search is retried once. |

Both are about the program. Neither is about you.

Every one of those three lists — the bundle, the crash header and the freeze
header — is asserted field-by-field by an automated test, in **both**
directions: a field added to a report fails the test just as loudly as a field
this document claims and the report stopped emitting. The bundle is held by
`tests/test_diagnostics.cpp`; the crash header by `tests/test_crash_capture.cpp`,
against a report from a real fault in a real child process; the freeze header by
`tests/test_diag_hang.cpp`, against a report from a real stall. The first of
those also asserts the absence of the things below.

**A full memory dump is off by default.** If you switch it on
(Settings → Diagnostics) a `.dmp` file is written *beside* the text report. A
memory dump is a copy of the program's memory and can contain file names, window
titles and received signal data, so it is written **locally only** and is never
sent by the application under any setting. If it is ever useful, you will be
asked for it and you can decide. The uploader opens files named `crash-*.txt`
and `hang-*.txt` and nothing else, which `tests/test_crash_upload.cpp` asserts by
putting a `.dmp` full of recognisable bytes beside a report and requiring that
none of them appear in any request.

## What is sent when a report is uploaded

One request per report, on the **next** start after the failure, to
`https://foxsdr.com/api/crash`. It is the report text above, as JSON:

| Field | Example | Why |
|---|---|---|
| `schema` | `1` | Which version of this list the request follows. |
| `kind` | `crash` or `hang` | Which of the two documents it is. A freeze whose `kind` is `stall` - the display driver was waiting, not FoxSDR - is never uploaded at all; it stays on your machine. |
| `version` | `0.62.0` | Which release. |
| `commit` | `98a9d7d617a7` | Which build. Only the commit names a build; the offsets below are meaningless against the wrong one. |
| `buildId` | `651FD5EB…C528` | Which *link*. Two builds of one version have different code at the same offsets. This identifies the compiled file, not you or your machine. |
| `module`, `offset` | `cascade.exe`, `1179648` | Where it failed, as a file name and a distance into that file. Not an address in your memory. |
| `signature` | `A31F…` (16 hex digits) | Groups repeats of one bug. Derived from the fault kind, the faulting module and the offset — never from the time and never from anything about you. |
| `os`, `arch` | `Windows 10.0.22631`, `x64` | Whether a fault is specific to a Windows version. |
| `reason` | `access violation`, or `fault in a third-party SDR module, absorbed…` | The report's own reason line, verbatim. This is what separates a fault the application **survived** (a driver fault absorbed by the vendor-call guard) from one that killed it — without it the two are indistinguishable rows. Empty for a freeze report, which has no such line. |
| `code` | `0xC0000005` | The Windows exception code, verbatim from the report. A code, not content. Empty for a freeze report. |
| `installId` | `4f9c…`, **or empty** | The same anonymous identifier the usage report uses, so the receiving end can stop one machine flooding it. **If usage reporting is off there is no identifier and this is sent empty** — a crash report never creates one. |
| `plugins` | `[{name, version, buildId}]` | Plugins are third-party code running inside the application, and which one was loaded has already been the answer to real faults. |
| `context` | `mode`, `source`, `sampleRate`, `deviceOpen`, `sdrModel`, `uptimeSec`, `faultThreadOwn` | What the receiver was doing. **`sampleRate` is the sample rate, not a tuned frequency.** Serial numbers are stripped from `sdrModel`, exactly as in the usage report. `uptimeSec` (since 0.89.0) is how many seconds the application had been running — a fault forty seconds in and one three hours in are different bugs. `faultThreadOwn` is `"true"` when the thread that faulted was running FoxSDR's own code, `"false"` when it was a thread a radio driver created and ran entirely in its own code, and empty for a freeze report or a stack that could not be walked. Neither says anything about you. |
| `log` | the last log lines | State changes — source opened, rate set, plugin started — never signal content, and **never the name or path of a file you opened**. Since 0.89.0 this also includes what the **radio driver** said (lines beginning `soapy:`) and what the driver's libraries printed to the standard error stream (lines beginning `vendor:`), because a radio going quiet is diagnosed from those lines and from nothing else. They are recorded as the driver wrote them, with two exceptions applied before anything is kept: a serial number after the word "serial" is replaced by `<stripped>`, and every digit on a line that mentions a frequency, tuning or hertz is replaced by `#`, so a driver's own "setting center frequency" line cannot carry what you were listening to. At most twenty such lines a second are kept; the rest are counted and the count is logged. **Since 0.99.33 every line is scrubbed again as the report is put together, whoever wrote it** — FoxSDR's own lines as well as the driver's: serial numbers (after the word "serial", after `SN:` or `S/N:`, after SoapySDR's ` :: ` in a device label) and the last part of a Windows USB device id (`USB\VID_…&PID_…\<this>`) become `<stripped>`; the account name in a file path becomes `<user>`; anything in single quotes — the names you give patch nodes and speakers — becomes `'<name>'`; and on any line that mentions hertz, a frequency, tuning, a centre, the VFO, a range, an offset, a carrier, a preset, transmitting or keying, every number becomes `#` unless it is marked as a sample rate, a time, a level, a size or a count, is a version, an id or an error code, or is part of a name such as `R820T`. Lines in which the USB driver lists the devices plugged into your computer are left out, with one line saying how many. |
| `threads` | `[{id, frames:[{module, buildId, offset}]}]` | The call stacks, as file names and offsets. Addresses inside program code; they describe FoxSDR, not you. |

That is the complete list. It is asserted **in both directions** by
`tests/test_crash_upload.cpp`: a field added to the request fails the test just
as loudly as a field this table claims and the request stopped sending. The same
test also reads **this document** and requires the two lists to match, so a field
cannot be added to the code and the table without the sentence explaining it, or
removed from the code and left in the table.

**What is never in it:** the memory dump, any frequency you tuned to, anything
decoded, your position, your IP address, your name, your machine name, or any
file path.

**What happens if it cannot be sent.** Nothing is lost and nothing is retried
for ever. Each report gets a small `.upload` file beside it saying what happened
in plain words — `sent`, `duplicate`, `backoff`, `failed`, `abandoned`. A report
that fails is retried on later starts up to three times and then left alone,
with the report itself untouched so you can still read it or send it yourself.

**How often.** At most five reports a day from one machine, and the same fault
only once a day however many times it happens — so a machine stuck in a crash
loop stops sending on its own rather than being told to. If the server does ask
us to wait, we wait.

**Switching Diagnostics off stops all of it**: no report is written, no
directory is created, no `.upload` file appears, and no connection is made.

### Reading the reports back

The reports are stored as the module names and offsets above. Turning an offset
into a function and a line needs the debug database from that exact build, which
is kept on our own machines and **is never uploaded anywhere** — the resolution
happens locally, in `tools/report-reader`, not on the server.

## Feature requests

The main screen has a **REQUEST A FEATURE** key near the foot of the STATUS
column, above the REPORT A BUG / DISLIKE key and the maker's plate. Pressing it opens a page with a text box, an optional
"Email or callsign" line, and a SEND key. Nothing is sent until you press
SEND - not while you type, not when you open the page, and never in the
background - and there is no queue: a request that fails to send is still
sitting in the text box exactly as you typed it, so you can try again rather
than being told it will be retried for you.

The typed text and the typed contact line live in memory only, for as long as
the page is open. Neither is ever written to `config.json`, and neither
reaches the diagnostics log - the log records only that a send happened, how
many characters it carried and what the server answered
(`feature request: sent, 42 characters, HTTP 200`, or the failure in the same
shape), never the words themselves.

### What is sent when you request a feature

One request, to `https://foxsdr.com/api/feature-request`, only when you press
SEND:

| Field | Example | Why |
|---|---|---|
| `schema` | `1` | Which version of this list the request follows. |
| `text` | `Please add a squelch tail hang timer` | What you typed, trimmed of leading and trailing blank space. Between 10 and 100000 characters. |
| `contact` | `g4xyz@example.com`, or empty | An email address or callsign, ENTIRELY OPTIONAL, so we can follow up. Up to 120 characters. Kept on screen after a successful send, in case you file a second request. |
| `version` | `0.99.0` | Which release, so a request against an old build is not chased in the current one. |
| `platform` | `windows`, `linux` or `android` | Which build filed it. |
| `arch` | `x64` or `arm64` | As above. |

That is the complete list. No install identifier, no hardware, no log, no
config, no frequency, no location, no plugin list - a feature request carries
nothing this application knows about your machine or your session, only what
you just typed and which build you are running. It is asserted **in both
directions** by `tests/test_feature_request.cpp`: a field added to the
request fails the test just as loudly as a field this table claims and the
request stopped sending, and the same test reads this document and requires
the two lists to match.

**How the server answers.** Success or a readable sentence explaining why
not - the same sentence is shown on the page. If the server asks us to wait
(too many requests from this connection), FoxSDR waits and disables SEND for
that long; otherwise SEND is disabled again for 30 seconds after every
attempt, successful or not, so filing five requests in five seconds is not
something the button lets you do by accident.

**What we keep on our end.** The text, the contact line if you gave one, the
version, the platform, the architecture, when it arrived, and your country if
our server's existing geolocation resolves one from the connection - **never
your IP address itself**. Third-party text is HTML-escaped before it is shown
on our admin page, because it is exactly that: something you wrote, not
something we generated.

## Bug reports and dislikes

Directly under REQUEST A FEATURE is a **REPORT A BUG / DISLIKE** key, the
same size, on the same terms. Its page asks first what you are reporting -
**Something is broken (bug)** or **Something I dislike** - with neither
chosen until you choose one, then has the same text box, the same optional
"Email or callsign" line and the same SEND key. Nothing is sent until you
press SEND, nothing is queued or retried, and the typed text and contact line
live in memory only: never in `config.json`, never in the diagnostics log,
which records only which kind was sent, how many characters it carried and
what the server answered (`problem report (bug): sent, 42 characters, HTTP
200`, or the failure in the same shape).

It is **not** a crash or freeze report. A report typed here carries no stack,
no log, no loaded-plugin list and no identifier; if FoxSDR crashed, the crash
report described above is a separate thing with its own switch.

### What is sent when you report a bug or a dislike

One request, to `https://foxsdr.com/api/problem-report`, only when you press
SEND:

| Field | Example | Why |
|---|---|---|
| `schema` | `1` | Which version of this list the report follows. |
| `kind` | `bug` or `dislike` | Which of the two you chose at the top of the page: something that does not work, or something that works as designed and that you would rather it did not. |
| `text` | `The waterfall freezes when I change the sample rate` | What you typed, trimmed of leading and trailing blank space. Between 10 and 100000 characters. |
| `contact` | `g4xyz@example.com`, or empty | An email address or callsign, ENTIRELY OPTIONAL, so we can ask a follow-up question. Up to 120 characters. Kept on screen after a successful send. |
| `version` | `0.99.20` | Which release, so a bug already fixed is not chased again. |
| `platform` | `windows`, `linux` or `android` | Which build sent it. |
| `arch` | `x64` or `arm64` | As above. |

That is the complete list: a feature request's six fields plus `kind`. No
install identifier, no hardware, no log, no config, no frequency, no
location, no plugin list. It is asserted **in both directions** by
`tests/test_problem_report.cpp`, which also reads this table and requires the
two lists to match. The server answers, rate-limits and waits exactly as it
does for a feature request (above), and keeps the same record plus the kind:
never your IP address.

## What is never sent

These are design constraints, not current policy:

- **Frequencies you tune to.** What somebody listens to is the most sensitive
  thing this software knows. In the United Kingdom, intercepting a message you
  are not authorised to receive, or disclosing its contents, is an offence
  under section 48 of the Wireless Telegraphy Act 2006. This applies to crash
  and freeze reports as strictly as it does to usage reports: no tuned
  frequency, no bookmark and no centre frequency appears in one, which
  `tests/test_diagnostics.cpp` asserts by searching for them.
- **The names of files you open.** A recording's file name is your own data —
  it can name a service, a place, or a frequency. `tests/test_diagnostics.cpp`
  checks this against the **real** log written by the **real** application,
  not against log lines the test supplied itself: it starts FoxSDR with a saved
  I/Q file that is no longer there, which is the most ordinary way to reach
  that path, and requires no part of the name to appear. That test exists
  because the failure message used to be logged verbatim, and it contained the
  full path.
- **Anything decoded** — pager messages, satellite traffic, aircraft, vessels.
- **Your position**, or the position of anything you receive. Reading the
  receiver position from a GPS (0.86.0) logs the port name, the baud rate,
  sentence and satellite counts and any error — never the position and never
  the text of an NMEA sentence. Only a port NAME is logged ("COM3",
  "/dev/ttyUSB0"): the port field accepts any text, and anything that is not
  shaped like a port — a typed file path, a pipe — is logged as its kind and
  length ("(a typed device path, 27 chars)"), so a path with your user name in
  it cannot ride into a report. `tests/test_gps_reader.cpp` asserts both
  against the log ring and `tests/test_gps_app.cpp` against the log file the
  real application wrote.
- **Your IP address or any location derived from it.** A network request
  necessarily reaches the server from an address, because that is how the
  internet works; nothing records or stores it, the receiving endpoint reads no
  connection information, and it writes no request logs.
- **Hardware serial numbers.** The SDR model is useful; the serial identifies
  your individual radio, and is removed.
- Your name, your machine's name, your user account, or any file path.

## Turning it off

**Settings → Usage reporting**, and untick it. Reporting is on by default, so
this is the switch that stops it. Turning it off deletes the install
identifier, so if you ever turn it back on you get a new one that cannot be
linked to the old one. It also stops the five-minute "still running" beat,
which is armed only while the identifier exists.

The other two transmissions have their own switches, described in their own
sections above: **Settings → Diagnostics** for crash and freeze reports, and
**Settings → Updates** for the version check.

Nothing else in the application is affected: no feature depends on any of
them being on, and nothing nags you about having turned one off.

## Where the data goes

To `https://telemetry.foxsdr.com`, a Cloudflare Worker operated by the FoxSDR
project, which aggregates the counters above. The source is in
`telemetry-worker/` in this repository so you can read what the receiving end
does with it.

## Data protection

The reports contain no personal data, so there is nothing to request access
to, correct or erase — there is no record anywhere that can be connected to
you. If you would like the install identifier removed from future reports,
turn usage reporting off; if you would like it removed from past ones, contact
us with the identifier and it will be deleted.
