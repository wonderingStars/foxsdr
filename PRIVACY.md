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

**A copy installed from the Microsoft Store asks the Microsoft Store instead,
never foxsdr.com** (since 0.99.53). Once per launch, when **Settings ->
Updates -> Ask the Microsoft Store for updates at startup** is ticked, FoxSDR
asks Windows' own Store service
(`Windows.Services.Store.StoreContext.GetAppAndOptionalStorePackageUpdatesAsync`)
whether it has a newer package for this copy. Nothing of FoxSDR's is sent in
that request - no version string, no identifier, nothing: Windows identifies
the app to Microsoft by the package identity it already holds, as it does for
the Store's own background updates. The answer (whether an update exists, and
whether it is marked mandatory) stays in the application. Nothing is
downloaded or installed unless you press **Install from the Microsoft Store**
*and* confirm the dialog Windows shows; the download and install are then
Microsoft's, under Microsoft's privacy statement. Untick the box and the
Store is not asked.

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
- **Each usage report is sent once.** To remember that it has, FoxSDR leaves
  an empty file named `telemetry-sent-` plus 16 hexadecimal characters beside
  `config.json`. The characters are a checksum of the report already
  described here; the file holds nothing, and the next report replaces it.
  A second small file, `telemetry-stalls`, appears there only if the display
  has frozen on you: it holds your install identifier and a count (see
  **Display stalls, in full** below) and is removed when you turn reporting off.
- **No personal data is collected**, and no IP address or location is recorded.
- **Crash and freeze reports are written to your machine, and — if you leave
  Diagnostics on — the report *text* is sent on the next start.** Not from
  inside the crash: the program that just failed cannot safely open a network
  connection, so the report waits on disk until the next time you open FoxSDR.
  Exactly what it contains is listed field by field below. **A full memory dump
  is never sent, under any setting**, and switching Diagnostics off stops the
  sending as completely as it stops the writing.
- **Nothing about what you listen to is ever collected** — no frequencies, no
  positions, no decoded messages. Not when reporting is on, not ever. This is
  about FoxSDR's *own* reporting (usage, crash, feature and bug reports); a
  fitted plugin is separate code with its own network access and its own
  choices, and **Plugins and your receiver's position** below covers what one
  can read and do with it.
- **The update check is ON by default, and you can turn it off.** Once per
  launch the application asks foxsdr.com whether a newer version exists. It
  sends **the version you are running and nothing else** — no identifier, no
  install id, no cookie kept — and it is not the usage report; the two share
  nothing. Nothing is downloaded or installed without you pressing a button.
  A copy from the Microsoft Store asks the **Microsoft Store** instead, once
  per launch when its box is ticked, and never foxsdr.com; nothing of ours is
  sent (see **The update check** above).
- The application makes two other network requests, both part of fetching the
  plugin catalogue: to GitHub (raw.githubusercontent.com) for the public list,
  the first time you open the plugin store window in a session and whenever
  you press **CHECK NOW** in it — nothing is fetched at startup — and, since
  FoxSDR 0.99.47, to foxsdr.com for a short regional list of plugins offered
  only in some countries. A plugin on that list downloads from foxsdr.com
  instead of GitHub. foxsdr.com decides what to answer from the two-letter
  country code Cloudflare works out from your connection's address, and it
  stores neither the code nor the address and records nothing about the
  request. Neither request carries an identifier, and the foxsdr.com request
  is not made when you have pointed the store at a different catalogue.
- The Satellites plugin, when it is fitted and tracking, fetches orbital
  element sets from CelesTrak about every twelve hours, falling back to a
  copy at foxsdr.com/tle/all.tle when CelesTrak does not answer. The request
  carries no identifier.
- **The Airband section's airport lookup makes no network request.** The
  airport frequency table (the FAA's for the United States, OurAirports' for
  everywhere else) is built into FoxSDR. "Nearest airports" compares your
  receiver position with that table on your machine. How long each frequency
  has been heard is written beside it in `bookmarks.json`, on your machine,
  and is never sent.
- **A plugin can read your receiver's approximate position only if it says so
  when fitted**, and the Fitted modules / Plugin store windows show which ones
  do. See **Plugins and your receiver's position** below.
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
| Operating system | `Windows 10.0.22631` | Which platforms are actually used, Windows or Linux, and which OS versions still matter. |
| Architecture | `x64` | As above. |
| How it was installed | `installer` | One of `store` (Microsoft Store), `installer` (the Windows installer), `appimage`, `tarball` (the two Linux downloads) or `android`. Read from the running program, not recorded at install time. It cannot tell foxsdr.com from GitHub, because they serve the same files. |
| First run | `2026-09-29` | The day - never the time - your install identifier was created, in UTC. **Deleted with the identifier** when you turn reporting off. Tells new users from long-standing ones. Empty on copies whose identifier is older than FoxSDR 0.99.47. |
| First version | `0.99.47` | The version that created the identifier - which download you started from. Deleted with it, like the first run. |
| Launch count | `12` | Whether the software gets used more than once. |
| Crash count | `1` | How often it fails. |
| Display stalls | `0` | How many times the window froze because the display driver was waiting, not FoxSDR (a monitor switched off, a graphics-driver reset, a remote session reconnecting) - a bare number, with no stack, no driver name and no time of day, and zero for almost everyone. |
| Session length | `3600` seconds | Whether sessions are minutes or hours. |
| SDR model | `uhd b200` | Which radios to prioritise. **Serial numbers are stripped** before sending. |
| Demodulators used | `WFM: 3000s` | Which modes justify further work. |
| Panels opened | `map, decoded` | Which features are used. |
| Installed plugins | `ADS-B 1.0.0` | Which decoders justify further work. |

That is the complete list. The payload is asserted field-by-field by an
automated test (`tests/test_telemetry.cpp`), in **both** directions, and the
same test reads this table: a field added to the report without a row here
fails it, and so does a row here that the report does not send. A new field
cannot be added without that test failing and this document being updated with
it.

**Display stalls, in full (since 0.99.62).** A freeze that FoxSDR's freeze detector decides was
the display driver waiting (see `kind: stall` under *Crash and freeze reports*)
is kept on your machine and never uploaded. What the usage report carries about
those is the number of them, and nothing else. To make sure a freeze you end
from the taskbar is still counted, the number is kept in a small file named
`telemetry-stalls` beside `config.json`, holding your install identifier and that
one number; it exists only once there has been a stall to count. After the
report that carries a number has been accepted by the server the number is taken
off; if the report could not be sent it stays and goes with the next one.
Switching usage reporting off removes the file, and a file left by an earlier
identifier is ignored. The detector is part of **Diagnostics**: with Diagnostics
off nothing is detected, so nothing is counted.

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

## Beta tester usage — what is sent when a tester code is set

This is a **separate, independent** switch from usage reporting above — a
different credential, a different purpose, and turning one on or off has no
effect on the other. It exists only for people who have joined the tester
list on foxsdr.com and been given a tester code or a tester link: pasting
that code into Beta tester (System) is what turns this on, and it is tied to
that one tester's own entry so the owner can tell which features they said
they would cover they actually used. **With no code entered, nothing on this
page is collected or sent, ever.**

**The link, and the confirmation prompt.** The tester portal's "Link FoxSDR"
button opens `foxsdr://beta?t=<app token>` in your browser, which Windows
hands to FoxSDR. The `foxsdr:` scheme is registered by the installer, not by
the application itself — for the ordinary, machine-wide install (the default;
it asks for administrator approval) it is registered **for the machine**,
so it works no matter which Windows account opens the link; only a
`/CURRENTUSER` install registers it for your own account alone
(`installer/cascade.iss`'s `[Registry]` section, `Root: HKA`). A Store install
declares the same activation in its manifest. **Nothing is stored the moment
the link is opened.** FoxSDR first asks the site who the token belongs to —
one network request, `GET /api/beta/app-token/me`, naming nothing but the
token itself — and only once that answers does it show an in-window prompt
naming that tester — *"Link this FoxSDR to beta tester NAME?"*, or, if one is
already linked, *"This FoxSDR is linked to OLDNAME. Replace with NAME?"* —
with **Link** and **Not now** buttons. Only pressing **Link** writes anything
to your configuration file; a token the site does not recognise shows an
error and stores nothing. This two-step design exists because a link opened
by *any* web page (not only the real portal) could otherwise bind your copy
to someone else's tester entry with no warning — showing the name first, and
requiring your own click, is what a silent bind would be missing. If FoxSDR
is not already running, the same file it would otherwise hand to a running
copy is read back and the same network request and the same prompt happen at
start-up instead — nothing is stored, and nothing is bound, before you see
the prompt and press Link, but the one lookup request happens first, not
after.

Testers who set up sharing by pasting the older 32-character code keep
working exactly as before: on the next launch, FoxSDR quietly exchanges that
code for the newer 40-character kind behind the scenes (the site recognises
it as the same tester every time this runs, so nothing changes if it happens
more than once), and the old code is then forgotten. Nothing about what is
collected or sent changes because of this exchange; it only changes the
shape of the one credential involved. If the exchange cannot reach the site,
your existing code keeps working and the exchange is retried at the next
launch; if the site says the code is no longer valid, it is cleared.

The hidden `--link-tester` command-line switch reveals the same paste box
this section always had, for a tester whose browser cannot hand FoxSDR a
`foxsdr://` link (Linux, today, or a blocked/dismissed browser prompt) — it
takes the code the portal's "Link FoxSDR" button displays and goes through
the exact same confirm-by-name prompt above. It does nothing on its own: run
with no code to paste, it shows an empty box and nothing more.

One report per session, saved when the session ends and **sent at the next
launch** — the same timing usage reporting's own launch report uses, and for
the same reason: a network call on the way out can hang the application while
you are trying to close it, so nothing here ever tries to send on the way out.
The code itself is shown on screen only masked (the first four and last four
characters, e.g. `4f9c…c2a1`) and the field that enters it hides what is typed
into it, the same way a password field does:

| Field | Example | Why |
|---|---|---|
| Tester code | `4f9c…c2a1` (shown masked; sent in full) | Your own tester-portal credential — either shape, the pasted 32-character code or the 40-character one a confirmed link produces — so the report can be linked to your entry. Never logged, never included in a diagnostics bundle or a crash report, and shown on screen only masked. |
| Application version | `0.99.x` | Which build the session ran. |
| Platform | `windows` | Which platform the session ran on. |
| Architecture | `x64` | As above. |
| Session start | `2026-09-28T12:34:56Z` | When the session began - the site stores this as when your tester entry was last used, so it is the one field here that updates something about your entry rather than only being logged. |
| Session length | `42` minutes | How long the session ran. |
| Features used | `spectrum, modes, bookmarks` | Which of the named beta areas you actually exercised this session, from a fixed list the site defines. |
| Plugins used | `pocsag 1.2.0, 6 minutes` | Which installed decoders ran, their version, and how long each was actually fed samples — never merely installed. A decoder this build cannot match to the published catalogue (a side-loaded or hand-built plugin) is reported under the fixed id `sideloaded` with no version, rather than under a name invented for it. |
| Radio kinds used | `rtlsdr` | Which radio hardware kinds were opened this session — never a serial number, never args. |

Exactly the same exclusions as usage reporting above apply here: **no
frequency, no decoded content, no position.** Your IP address is used only to
limit how often reports are accepted from one address and is not stored
alongside anything a report contains. The payload is asserted field-by-field
by an automated test (`tests/test_tester_usage.cpp`), so a new field cannot be
added without that test failing and this document being updated with it.

If the site answers that the code is no longer valid, sending stops
immediately and the app says so on screen; the code itself is kept so it can
be corrected rather than being silently cleared. Up to three reports that
could not be sent (a network failure, or the site asking to try later) are
kept and retried at the next launch; older ones are dropped first. A report
queued under a code that is later replaced or removed is discarded rather
than sent under the new one (or under none) — it would otherwise land on the
wrong tester's entry, or on nobody's. The one exception is an older pasted
code replaced by a link that FoxSDR has itself confirmed, earlier in the same
session, belongs to the same tester: those reports are kept under the link,
and the prompt says in advance which of the two will happen.

Removing the code, or pressing **Unlink** once a link has been confirmed,
deletes it from your configuration file, discards anything still queued to
send, and stops everything above immediately — nothing further is collected
from that point, and the SYSTEM > Beta tester section disappears again
unless `--link-tester` was used to reveal it. Unlike an invalid pasted code
(kept on screen so you can fix it), a link the site later says is no longer
valid is cleared outright — nothing is left to fix in place, and a fresh
link from the portal is what produces a new one.

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
| `ppm` | `+1.5 by retuning` | Whether the radio's crystal correction is switched on, its value in parts per million and how it is applied (`in the radio` or `by retuning`); `off` when it is not. A property of the radio's crystal, **not a frequency** - it says nothing about what you listen to. Since 0.99.56. |
| `plugin` | `ADS-B 1.1.0` | Plugins are third-party code running inside the application, and which one was loaded has already been the answer to real faults. |
| `log-path`, `crash-dir` | `%LOCALAPPDATA%\FoxSDR\logs/foxsdr.log` | So you know where the rest of it is. Since 0.99.33 the base directory is written as `%LOCALAPPDATA%` (`$HOME` or `$XDG_STATE_HOME` on Linux) rather than the real path, so your account name is not in it; a folder elsewhere is shown with the account name replaced by `<user>`. |
| `last-run-unclean` | `yes` | Whether the previous session ended without shutting down. |
| `launches`, `crashes` | `12`, `1` | The same two counters the usage report already keeps. |
| `log-lines-total` | `4011` | How much of the log the report is *not* carrying. |
| `sdrplay-service` | `stopped, manual start (SDRplayAPIService)` | Since 0.99.55: what Windows says the SDRplay API Service is doing — its state, its startup type and the name Windows knows it by — or `not installed`, or `not applicable` off Windows. SDRplay radios only work while that service runs, and it is the first thing to ask about when one will not open. It is read from the Windows Service Control Manager without administrator rights and says nothing about you or your radio. |
| `audio-output` | `open, MME, 2 channels` | Since 0.99.61: whether a sound output is open and serving its stream, through which Windows audio driver model (`MME`, `Windows WASAPI`...) and in what layout; `opening`, `stopped - the stream died and is being reopened` or `none - no output device has opened` otherwise, and `, restarted N times` after the watchdog has had to reopen it. **The device's own name is never written** — Windows hands the application labels such as `Headset (Alice's AirPods Pro)`, which are often a person's name. The first thing to ask when someone reports no sound from a radio that is plainly receiving. |
| `volume` | `80%` | Since 0.99.61: the volume control. A volume of nothing is silence from a healthy receiver. |
| `audio-muted` | `no`, `you`, `a decoder plugin` | Since 0.99.61: whether the audio is muted and WHO muted it — `you` (the Mute key), `a decoder plugin` (an I/Q decoder holds the audio down while the receiver is on one of its presets), `transmit key`, or several joined with `+`. A plugin is described and **not named**, because its name would say which band the receiver was tuned to. |
| `squelch` | `-50 dB, closed (signal -63 dB)` | Since 0.99.61: the squelch threshold, whether the gate is open, and the signal level the gate is judging when the receiver has measured one. Decibels against full scale: a **level, not a frequency** — it says nothing about what you listen to. A gate that stays closed is complete silence from a healthy receiver. |
| `patch-radios` | `none`, `1 (rtlsdr)`, `2 (rtlsdr, soapy)` | Since 0.99.62: how many radios the patch page has running, and the **driver kind** of each - `rtlsdr`, `soapy`, `sdrplay`, `iqfile` (a recording) or `siggen` (the generator) - or `none`. **Kinds only**: never a radio's name, its serial number, the address or arguments it was opened with, the name of a recording, or a frequency - the line is built from the part of the patch node's device key before its first vertical bar, reduced to lower-case letters and digits, so the part after it is never read. `source` describes the receiver, whose radio the patch page may have been handed, so a fault on a patch radio's thread otherwise reads as a fault with no radio at all (a 0.99.59 report of one said `source: siggen` and `device-open: no`). It is in the report file on your machine and in the bundle; it is **not** one of the fields the automatic crash report sends (see *What is sent when a report is uploaded*). |
| the log | the last 256 lines | State changes — source opened, rate set, plugin started — never signal content, and **never the name or path of a file you opened**. Since 0.99.61 it also says when a sound output opens or is refused (`audio: output opened - MME, 2 channels, 48000 S/s, system default`): the driver model, the layout and the rate, **never the name of your sound device**. When an I/Q file fails to reopen the log records that it did not reopen; the file name stays on screen, where you already know it. Since 0.89.0 the log also records what the radio driver said (`soapy:` lines) and what its libraries printed to the standard error stream (`vendor:` lines), with serial numbers stripped and the digits of any line mentioning a frequency masked — see the `log` row under *What is sent when a report is uploaded* for the exact rule. Since 0.99.33 the bundle's log is scrubbed by exactly that rule, the same as an uploaded report's, because a bundle is made to be pasted somewhere. Since 0.99.59 that rule also replaces every network address and computer name with `<host>`, keeping the port after it, and the number of a serial port with `#` (`COM#`, `/dev/ttyUSB#`) - see the same row for where it looks. |
| the previous session's log | the last 80 lines of the session before this one, scrubbed | Since 0.99.62, under a heading of its own after the log: the end of the log of the session BEFORE this one, read from the log files on your machine (`foxsdr.log`, `foxsdr.1.log` and `foxsdr.2.log`, in the folder the Diagnostics section shows). At most 80 lines and 12 KiB, the newest kept; sessions are told apart by the line FoxSDR writes when it starts, which names its version and build. It exists because a window that froze, was ended from the taskbar and was then restarted is gone from the log of the session you report from. **Every line goes through the same scrub as the log above** - serial numbers, account names in paths, network addresses and computer names, quoted names, and every number on a line that mentions a frequency - and the heading says in words when there is no earlier session, or when it cannot be found (for instance when Diagnostics was switched on part-way through this session). **With Diagnostics off nothing is read and neither this row nor the next is added**, not even a heading. Only ever in the bundle you copy or choose to attach; never in the automatic crash report. |
| the reports on this machine | one line per report, the newest ten | Since 0.99.62, under a heading of its own: for each of the newest ten `crash-*` and `hang-*` files in the reports folder, its kind (`crash`, `hang` or `stall`), how long ago it was written, the version that wrote it, how long that session had been running, how long the interface was stalled (a freeze) or the reason and code (a crash), its grouping signature, and what became of sending it - `sent`, `duplicate`, `local-only`, `backoff`, `rate-limited`, `failed`, `abandoned`, `too-large`, `expired`, `refused`, or `none` when nothing has swept it. It is the answer to "did the watchdog fire, and did the report reach you". **No stack, no log lines, no module list, no context block, no file name and no path**: nothing is read from a report beyond those fields, and a memory dump or a saved copy of this bundle in the same folder is never opened. Each line is passed through the same scrub as the log. Only ever in the bundle you copy or choose to attach; never in the automatic crash report. |

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
Since 0.99.34 its `reason` line also names the radio driver that was being
asked when the process died (an installed driver's short name, such as `uhd` or
`sdrplay` - never a device, a serial number or anything about you), or, for the
search that asks every driver at once, the drivers still being asked when it
stopped; and its `signature` is derived from that driver name in place of a
faulting module, so each driver's failures group on their own. The `reason`
line can also end with what that small process's own crash handler said it
died of, in one short line - the kind of fault, its code, and the program
module and offset it happened at (for example `access violation 0xC0000005 at
libusb-1.0.dll+0x10490`). Those are the same facts its own report carries, and
nothing else.
That
small process can also write a report of its own, into the same folder and with
the same fields, and only when diagnostics are switched on: it is handed the
folder by the session that started it and is told nothing at all when the
setting is off. Since 0.99.62 the session writes its own report of that
death only when the small process's report does not already cover it, so one
death is one report rather than up to three, and the small process's own
report says on the end of its `reason` line the things the session's used to:
which search it was asked for (every driver, one named driver, or the list of
drivers), which try it was, and that the application survived it - for example
`access violation - enumeration child, driver=uhd, attempt 1 (contained)`, the
driver being the same installed driver's short name as above. That is the same
`reason` field, so nothing is added to what is sent. These used to be either a
dead application or nothing at all;
none of them adds a field, and all are governed by the same switch in
**Settings → Diagnostics** as every other report on this page.

**A freeze report:**

| Field | Example | Why |
|---|---|---|
| `kind` | `hang` or `stall` | As above. `stall` means the wait was inside the display driver or the window system rather than inside FoxSDR - a monitor switched off, a resolution change, a remote session, or on Linux a window the compositor is not showing. Those are kept on your machine and never sent; only a bare count of them is in the usage report (*Display stalls*, above). |
| `note` | `the gui thread did not complete a frame within the threshold` | The same distinction in one sentence, so a report says what it is without anyone having to know the codes. |
| `stalled-ms` | `7213` | How long the interface had been unresponsive. |
| `threshold-ms` | `5000` | What it was measured against, so the number above can be judged. |
| `signature` | `7C04…` | As above. |
| `threads` | `9` | How many stacks follow. |

After that header both add the call stacks — every thread for a freeze, the
faulting one for a crash — and the list of loaded modules with their build
identifiers. Those are addresses inside program code and identifiers of
compiled files. They describe FoxSDR, not you.

Since 0.99.63 a freeze report also names a frame inside a module that was loaded
after that list was last refreshed - a graphics driver the system reloaded, a
vendor DLL, a shell extension - by the module's file name (never a path) and the
distance into it, as it names every other frame. Before, such a frame was a bare
address. The module is not added to the list. A file name can say that a piece of
software is installed on your computer, so it is stated here; it is the same kind
of name the list and the frames above already carry for drivers loaded at
start-up.

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
| `kind` | `crash` or `hang` | Which of the two documents it is. A freeze whose `kind` is `stall` - the display driver was waiting, not FoxSDR - is never uploaded at all; it stays on your machine, and only how many there were reaches the usage report. |
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
| `log` | the last log lines | State changes — source opened, rate set, plugin started — never signal content, and **never the name or path of a file you opened**. Since 0.89.0 this also includes what the **radio driver** said (lines beginning `soapy:`) and what the driver's libraries printed to the standard error stream (lines beginning `vendor:`), because a radio going quiet is diagnosed from those lines and from nothing else. They are recorded as the driver wrote them, with two exceptions applied before anything is kept: a serial number after the word "serial" is replaced by `<stripped>`, and every digit on a line that mentions a frequency, tuning or hertz is replaced by `#`, so a driver's own "setting center frequency" line cannot carry what you were listening to. At most twenty such lines a second are kept; the rest are counted and the count is logged. **Since 0.99.33 every line is scrubbed again as the report is put together, whoever wrote it** — FoxSDR's own lines as well as the driver's: serial numbers (after the word "serial", after `SN:` or `S/N:`, after SoapySDR's ` :: ` in a device label) and the last part of a Windows USB device id (`USB\VID_…&PID_…\<this>`) become `<stripped>`; the account name in a file path becomes `<user>`; anything in single quotes — the names you give patch nodes and speakers — becomes `'<name>'`; and on any line that mentions hertz, a frequency, tuning, a centre, the VFO, a range, an offset, a carrier, a preset, transmitting or keying, every number becomes `#` unless it is marked as a sample rate, a time, a level, a size or a count, is a version, an id or an error code, or is part of a name such as `R820T`. Since 0.99.44 the names of the plugins the same report already lists under `plugins` (such as `406 MHz Beacons 1.0.0`) are left as they are wherever a line mentions them - a plugin's name is not something you tuned to - and every other number on that line is judged by the rule above. **Since 0.99.59 network addresses and computer names are scrubbed too**: every IPv4 or IPv6 address becomes `<host>`, and so does a computer or host name where FoxSDR and the network radio drivers write one - after a URL scheme such as `tcp://`, `rtsp://` or `https://` (a user name and password with it), after `ip:`, `host=`, `hostname=`, `remote=`, `rtltcp=`, `server=`, `uri=`, `addr=` or `address=`, after "at", "to" or "from" when it is plainly a host name, in double quotes after the word "host", under a local-network suffix such as `.local` or `.lan`, and at the start of a Windows network path (`\\<host>\share`). The port number after an address is kept, because it says which service was asked, not who asked; so is this computer's own loopback address (`127.0.0.1`, `0.0.0.0`, `::1`, `localhost`), which names nobody. Since 0.99.59 the number of a serial port is masked as well: `COM5` is sent as `COM#` and `/dev/ttyUSB0` as `/dev/ttyUSB#` - a report needs to know that a port was tried, opened or refused, not which number your computer gave it. A version number is not mistaken for an address: a four-part version such as `1.99.58.0` is kept when it is joined to a name or follows the word "version", "package", "firmware" or "build". Lines in which the USB driver lists the devices plugged into your computer are left out, with one line saying how many. |
| `threads` | `[{id, frames:[{module, buildId, offset}]}]` | The call stacks, as file names and offsets. Addresses inside program code; they describe FoxSDR, not you. Since 0.99.63 a frame inside a module loaded after start-up (a graphics driver the system reloaded, a vendor DLL, a shell extension) carries that file's name, never a path, and an offset; before, it carried an empty module and an address inside the process. |

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
"Email or callsign" line, an **"Attach the diagnostics log"** box, and the
same SEND key. Nothing is sent until you press SEND, and the typed text and
contact line live in memory only: never in `config.json`. Nothing you typed
is ever queued, kept across a restart, or retried on its own - the ONE
exception, and it never touches your words, is described further down: if
you attached the log and the site refuses it, FoxSDR resends the same report
without it, once, immediately. The diagnostics log line records only which
kind was sent, how many characters the message carried and what the server
answered (`problem report (bug): sent, 42 characters, HTTP 200`, or the
failure in the same shape) - it never records the words you typed, whether
or not you attached the log itself.

Earlier versions of this page said the report carried no log - it never did,
but people kept believing it did, and telling us so made bugs harder to fix,
not easier. Since 2026-09-28 you can choose to attach it: **ticked by default
when you report a bug, unticked for a dislike**, and yours to change either
way. A **"Show what will be sent"** toggle shows the exact text - the same
words the SYSTEM > Diagnostics **Copy diagnostics** button copies - before you
press SEND, so nothing about it is a surprise. That text is the bundle
described in full under *Crash and freeze reports — what they contain*
above (the `log-path`/`crash-dir` row there is what says exactly how your
account name is handled in it), and is additionally capped here at 65536
bytes - a log longer than that keeps its header and its newest lines, and
says plainly that older lines were dropped.

It is **not** a crash or freeze report. A report typed here carries no stack
and no crash report; if FoxSDR crashed, the crash report described above is a
separate thing with its own switch. The diagnostics log, when you choose to
attach it, is the same context a crash report carries (version, commit,
operating system, mode, the loaded plugins, whether a device was open) plus
your recent log lines, the end of the previous session's log and one line per
crash or freeze report on your machine (the last two rows of the bundle's table
above; with Diagnostics off the bundle has neither) - never a frequency, never a
hardware serial, never the install identifier telemetry uses.

### What is sent when you report a bug or a dislike

One request, to `https://foxsdr.com/api/problem-report`, only when you press
SEND:

| Field | Example | Why |
|---|---|---|
| `schema` | `1` | Which version of this list the report follows. |
| `kind` | `bug` or `dislike` | Which of the two you chose at the top of the page: something that does not work, or something that works as designed and that you would rather it did not. |
| `text` | `The waterfall freezes when I change the sample rate` | What you typed, trimmed of leading and trailing blank space. Between 10 and 100000 characters. |
| `contact` | `g4xyz@example.com`, or empty | An email address or callsign, ENTIRELY OPTIONAL, so we can ask a follow-up question. Up to 120 characters. Kept on screen after a successful send. |
| `version` | `0.99.44` | Which release, so a bug already fixed is not chased again. |
| `platform` | `windows`, `linux` or `android` | Which build sent it. |
| `arch` | `x64` or `arm64` | As above. |
| `diagnostics` | the same text **Copy diagnostics** produces, or ABSENT | OPTIONAL. Present only when you left "Attach the diagnostics log" ticked when you pressed SEND - absent, not an empty string, otherwise. It is exactly the bundle above (*Crash and freeze reports*), with the same `<user>` handling that table's `log-path`/`crash-dir` row describes, additionally capped here at 65536 bytes (the newest lines kept, the oldest dropped, and said so). Never anything the diagnostics bundle does not already contain: no install identifier, no plugin list beyond what that bundle lists - with ONE addition you choose separately: when you have run the SDRplay diagnostic and ticked "Attach the SDRplay diagnostic too", that file follows the bundle inside this same field (described under *The SDRplay diagnostic* below), within the same 65536 bytes. |

That is the complete list: a feature request's six fields plus `kind`, plus
the one optional `diagnostics` field above. No install identifier, no
hardware serial, no config beyond what the diagnostics log already lists when
attached, no frequency, no location. It is asserted **in both directions** by
`tests/test_problem_report.cpp`, which also reads this table and requires the
two lists to match. The server answers, rate-limits and waits exactly as it
does for a feature request (above), and keeps the same record plus the kind
and, when you attached it, the log: never your IP address.

**If the site does not yet know the `diagnostics` field** (an older release,
or a moment before this contract's own change reaches it), it refuses the
whole request rather than silently ignoring the field. FoxSDR notices that
exact refusal and tries again, once, immediately, with the log left out - and
says so plainly on the page - rather than leaving your bug report unsent over
a field the site does not yet accept.

### The SDRplay diagnostic

**SYSTEM > Diagnostics > Run SDRplay diagnostic** (or `cascade --sdrplay-probe
<file>`) steps an SDRplay RSP through its sample rates, bands, LNA states,
antennas and IF modes once, times every call to the SDRplay API, and writes
one text file on YOUR machine, in the reports folder. Nothing is sent by
running it. The file holds the FoxSDR version, the operating system, the API
version, the radio's model and hardware number, **a hash of its serial number
(never the serial itself)**, the settings the API reported, the timing and
answer of every API call, how many samples arrived, and the service's own
events (gain changes, overloads). No frequency you tuned to, nothing
received, and paths go through the same `<user>` masking as the bundle. It
leaves the machine only if you tick **"Attach the SDRplay diagnostic too"** on
the report page, inside the `diagnostics` field above, and "Show what will be
sent" shows it first. The bias tee is switched on during the test only if you
tick it AND answer a second question that says it powers the antenna socket.

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
  real application wrote. Since 0.99.59 a report or a diagnostics bundle
  carries even the port name without its number ("COM#", "/dev/ttyUSB#"); the
  log on your own computer keeps it.
- **Your IP address or any location derived from it.** A network request
  necessarily reaches the server from an address, because that is how the
  internet works; nothing records or stores it, the receiving endpoint reads no
  connection information, and it writes no request logs.
- **Hardware serial numbers.** The SDR model is useful; the serial identifies
  your individual radio, and is removed.
- Your name, your machine's name, your user account, or any file path.
- **Patch presets.** The patch page's named presets and its "(previous
  patch)" slot — their names and the patches in them — are stored only in
  `config.json` on your own machine, and nothing about them is sent anywhere.

## Plugins and your receiver's position

Everything above is about what FoxSDR itself sends. A fitted plugin is
separate, third-party code that runs inside the application with every
privilege the application has (see the plugin ABI's own licence notice and
the Plugin store's "Declared by the maker, not enforced" notice) - it is not
sandboxed, and this section is about what one CAN reach, not a guarantee of
what any particular one DOES with it.

- **Only a plugin that declares it can read your receiver's position, and
  only that one gets to.** The plugin interface added a separate permission
  bit for this (`CASCADE_CAP_RECEIVER_LOCATOR`, FoxSDR 0.99.43) precisely so
  that a plugin which only wants to decode a signal, or only wants to move the
  tuning dial, is never handed it - it has to ask, in its own published
  description, before the host will ever give it one. Which fitted plugins
  ask is shown as **"Can read your receiver's locator"** on that plugin's own
  card in the Fitted modules window and the Plugin store, wherever its other
  permissions (such as moving the receiver) are shown.
- **What it gets is a 6-character Maidenhead grid square, not a point.** That
  is a rectangle roughly 5 minutes of longitude by 2.5 minutes of latitude -
  a few kilometres across at temperate latitudes, not an address and not a
  precise fix. It reflects wherever the receiver's position was last set -
  a GPS fix, "Set RX here" on the map, or a position you typed in - and is
  empty whenever none has been set.
- **What a plugin does with it is up to the plugin**, exactly like anything
  else it decodes or computes: some may keep it entirely on your machine,
  and some report it onward. The plugin this permission was added for is a
  PSK Reporter-style FT8 reporting plugin, whose whole purpose is to publish
  your callsign, grid square and what you decoded to a public spotting
  service (pskreporter.info) - which is exactly how that kind of reporting
  works, and exactly why the permission is visible rather than silent.
  **Nothing is sent anywhere by it until you both fill in a callsign and turn
  reporting on** - a plugin field left at its default never becomes a network
  request, per the plugin ABI's own rule for exactly this shape of setting.
- **FoxSDR's own reporting (above) never carries it.** The usage, crash,
  feature and bug reports have no field for it and never will; this section
  is only about what a plugin's OWN code may read and do, through the
  interface every plugin uses, separate from anything this application
  reports about itself.

## Turning it off

**Settings → Usage reporting**, and untick it. Reporting is on by default, so
this is the switch that stops it. Turning it off deletes the install
identifier, so if you ever turn it back on you get a new one that cannot be
linked to the old one. It also stops the five-minute "still running" beat,
which is armed only while the identifier exists.

The other transmissions have their own switches, described in their own
sections above: **Settings → Diagnostics** for crash and freeze reports,
**Settings → Updates** for the version check, and **Settings → Beta tester**
for the tester usage report — remove the code there and nothing further is
collected. Unlike usage reporting, beta tester usage starts OFF and stays off
until a code is entered; there is nothing to turn off on a machine where one
never was.

Nothing else in the application is affected: no feature depends on any of
them being on, and nothing nags you about having turned one off.

## Where the data goes

To `https://telemetry.foxsdr.com`, a Cloudflare Worker operated by the FoxSDR
project, which aggregates the counters above. The source is in
`telemetry-worker/` in this repository so you can read what the receiving end
does with it.

Beta tester usage goes to `https://foxsdr.com/api/tester-usage` instead, on
the site itself rather than the anonymous-counters Worker, because it is
linked to your tester entry there rather than aggregated anonymously. Opening
a tester link or using `--link-tester` additionally reaches
`https://foxsdr.com/api/beta/app-token/me` (to look up the name shown in the
confirmation prompt) and, once, `https://foxsdr.com/api/beta/app-token` (to
exchange an older pasted code for the newer kind) — both carry the token
being asked about and nothing else. The one-shot file a tester link writes to
hand the token to an already-running FoxSDR (`link-request`, beside
`config.json`) never leaves your machine and is deleted the moment it is
read.

## Data protection

The usage and crash reports contain no personal data, so there is nothing to
request access to, correct or erase — there is no record anywhere that can be
connected to you. If you would like the install identifier removed from
future reports, turn usage reporting off; if you would like it removed from
past ones, contact us with the identifier and it will be deleted.

Beta tester usage is **not** anonymous — it is linked, by the code you
pasted, to your own entry on the tester list, and foxsdr.com's own privacy
notice for that list covers what is kept and for how long. Removing the code
from Beta tester stops any further report; to have past ones associated with
your entry removed, ask through the tester portal the same way you would ask
to be removed from the list itself.
