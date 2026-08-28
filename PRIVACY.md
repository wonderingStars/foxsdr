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
- The application makes one other network request, only when you press
  **Browse** in the Plugin store panel, to fetch the plugin catalogue.

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
| `log-path`, `crash-dir` | paths under `%LOCALAPPDATA%` | So you know where the rest of it is. |
| `last-run-unclean` | `yes` | Whether the previous session ended without shutting down. |
| `launches`, `crashes` | `12`, `1` | The same two counters the usage report already keeps. |
| `log-lines-total` | `4011` | How much of the log the report is *not* carrying. |
| the log | the last 256 lines | State changes — source opened, rate set, plugin started — never signal content, and **never the name or path of a file you opened**. When an I/Q file fails to reopen the log records that it did not reopen; the file name stays on screen, where you already know it. |

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
happened to it, including when the search then succeeded on a second try. That
small process can also write a report of its own, into the same folder and with
the same fields, and only when diagnostics are switched on: it is handed the
folder by the session that started it and is told nothing at all when the
setting is off. These used to be either a dead application or nothing at all;
none of them adds a field, and all are governed by the same switch in
**Settings → Diagnostics** as every other report on this page.

**A freeze report:**

| Field | Example | Why |
|---|---|---|
| `kind` | `hang` | As above. |
| `stalled-ms` | `7213` | How long the interface had been unresponsive. |
| `threshold-ms` | `5000` | What it was measured against, so the number above can be judged. |
| `signature` | `7C04…` | As above. |
| `threads` | `9` | How many stacks follow. |

After that header both add the call stacks — every thread for a freeze, the
faulting one for a crash — and the list of loaded modules with their build
identifiers. Those are addresses inside program code and identifiers of
compiled files. They describe FoxSDR, not you.

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
| `kind` | `crash` or `hang` | Which of the two documents it is. |
| `version` | `0.62.0` | Which release. |
| `commit` | `98a9d7d617a7` | Which build. Only the commit names a build; the offsets below are meaningless against the wrong one. |
| `buildId` | `651FD5EB…C528` | Which *link*. Two builds of one version have different code at the same offsets. This identifies the compiled file, not you or your machine. |
| `module`, `offset` | `cascade.exe`, `1179648` | Where it failed, as a file name and a distance into that file. Not an address in your memory. |
| `signature` | `A31F…` (16 hex digits) | Groups repeats of one bug. Derived from the fault kind, the faulting module and the offset — never from the time and never from anything about you. |
| `os`, `arch` | `Windows 10.0.22631`, `x64` | Whether a fault is specific to a Windows version. |
| `installId` | `4f9c…`, **or empty** | The same anonymous identifier the usage report uses, so the receiving end can stop one machine flooding it. **If usage reporting is off there is no identifier and this is sent empty** — a crash report never creates one. |
| `plugins` | `[{name, version, buildId}]` | Plugins are third-party code running inside the application, and which one was loaded has already been the answer to real faults. |
| `context` | `mode`, `source`, `sampleRate`, `deviceOpen`, `sdrModel` | What the receiver was doing. **`sampleRate` is the sample rate, not a tuned frequency.** Serial numbers are stripped from `sdrModel`, exactly as in the usage report. |
| `log` | the last log lines | State changes — source opened, rate set, plugin started — never signal content, and **never the name or path of a file you opened**. |
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
- **Your position**, or the position of anything you receive.
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
linked to the old one.

Nothing else in the application is affected: no feature depends on reporting
being on, and nothing nags you about having turned it off.

## Where the data goes

To a Cloudflare Worker operated by the FoxSDR project, which aggregates the
counters above. The source is in `telemetry-worker/` in this repository so you
can read what the receiving end does with it.

## Data protection

The reports contain no personal data, so there is nothing to request access
to, correct or erase — there is no record anywhere that can be connected to
you. If you would like the install identifier removed from future reports,
turn usage reporting off; if you would like it removed from past ones, contact
us with the identifier and it will be deleted.
