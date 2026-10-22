# Running foxsdr-engine as a service: getting it, settings, provisioning the web and CAT servers, stopping it, exit codes, a sample systemd unit, what it sends

`foxsdr-engine` is the receiver with no window: the same engine `cascade` runs, started from a command line. This is its
guide, and a statement of its behaviour. The systemd unit below is a sample, not shipped code: nothing here is installed by
any package, and the Linux half has NOT been run on a real machine with a real radio (see "What is verified" at the end,
and "Open questions" after it).

## Getting it

- **Linux tarballs** (x86_64 and arm64; not in 0.99.84 or earlier) hold `foxsdr-engine` beside `cascade`:

  ```
  foxsdr-<version>-linux-<arch>/
      cascade  foxsdr-engine  README.md  LICENSE  PRIVACY.md
      resources/bandplans/*.json
      resources/firmware/aor/NOTICE
  ```

  Both programs read `resources/` from beside themselves (executable-relative: the band-plan overlay and AOR's firmware
  path), so keep the layout; the engine run from elsewhere on a copy of the binary alone has an empty band-plan overlay.
  The AppImage is the window application's launcher and does not carry it. The same libraries the tarball already needs
  apply (SoapySDR, OpenSSL, libunwind, ALSA); a machine that will only ever run the engine needs no OpenGL, X11 or Wayland
  to *build* it (below), and `foxsdr-engine` itself imports none of them.
- **Windows**: not in the installer or the Microsoft Store package (a decision, O4: it is for Linux and the Raspberry Pi).
  Build it from source if you want it: it is a normal target of the Windows build (`cmake --build ... --target foxsdr-engine`).
- **From source, with no window**: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCASCADE_BUILD_INTERFACE=OFF`
  then `cmake --build build`: only the engine's packages are needed, no OpenGL or desktop libraries, and no tests are
  built. [BUILDING.md](BUILDING.md) has the details and the package list.

## Running it, and where it keeps things

```
foxsdr-engine [--seconds N] [--config PATH | --use-config] [--selftest] [--version]
              [--web-enable [--web-bind ADDR] [--web-port N] [--web-user NAME] [--web-password-file FILE|-]]
              [--cat-enable [--cat-bind-all]]
```

- **With no `--config` and no `--use-config` it is hermetic**: the built-in signal generator, muted (a service has no business
  playing a demo tone at a sound card), no settings file read, written or locked, nothing written to the disk by the engine,
  nothing sent. A test run, not a service. (On a Linux machine whose ALSA routes to PulseAudio - `libasound2-plugins` - the
  sound-server client library itself creates `$XDG_CONFIG_HOME/pulse/cookie` when the audio output is opened; seen on Ubuntu under
  WSL2, and it is the library's file, not the engine's.)
- **`--config PATH`** names the settings file (the same JSON the window application writes); **`--use-config`** is the
  per-user file the window application uses (Linux: `$XDG_CONFIG_HOME/foxsdr/config.json`, or `~/.config/foxsdr/config.json`
  when that is unset). With either, the engine takes a lock beside the file first - a second `foxsdr-engine`, or the window
  application, on the same file is refused with a sentence and exit 1 - restores the receiver from it as the window does,
  keeps it saved while it runs, and writes its clean-exit marker last of all. A service wants `--config` on a file of its own
  (the unit below); `--use-config` is for running "headless" with a desktop user's settings while the window is not open.
- **`--seconds N`** ends the run after N seconds, in order, exit 0. Without it the run goes on until it is stopped from
  outside (next section).
- **`--version`** prints `FoxSDR <version>`, the line `cascade --version` prints, and exits 0. **`--selftest`** runs the
  headless DSP self-test, as `cascade --selftest` does.
- Its journal is its standard error: a line when it is running, the settings-file line, a line when it stops. Under systemd
  that is the journal.
- **Crash and freeze reports stay on the machine.** With Diagnostics on in its settings file the engine writes them as the
  window does and never uploads them; `foxsdr-reports` reads them against the symbol archive. What it does send, and how to
  stop that, is in [PRIVACY.md](../PRIVACY.md) (the section on `foxsdr-engine`) and under "Open questions" below.

## Setting up the web server and the CAT listener (stage 7e)

A service has no settings panel, so the two servers are set up from the command line. The flags say what the panels would
say, and are submitted the same way: as one `FOXAPI_OP_SERVER_CONFIG` on a local session with the ADMIN grant, after the
settings are restored and **before the receiver starts**, so the configuration is settled (the password hashed, the listeners
started or refused) before anything is receiving.

```
foxsdr-engine --config /var/lib/foxsdr/config.json \
              --web-enable [--web-bind ADDR] [--web-port N] [--web-user NAME] [--web-password-file FILE | --web-password-file -] \
              --cat-enable [--cat-bind-all]
```

| Flag | Meaning |
| --- | --- |
| `--web-enable` | switch the web server on (the page's default binding is `127.0.0.1`, port `8073`, user `admin`) |
| `--web-bind ADDR` | the address to listen on: an IPv4 address, `::1`, `::` or `localhost` (a host name is refused) |
| `--web-port N` | the port, 1024 to 65535 |
| `--web-user NAME` | the account name the login wants |
| `--web-password-file FILE` / `-` | the **name of a file** that holds the password, or `-` for standard input |
| `--cat-enable` | switch the CAT (rigctl-style) listener on, on `127.0.0.1`, port `4532` |
| `--cat-bind-all` | with `--cat-enable`: listen on every interface. **CAT has no password**: anything that can reach the port can retune the receiver |

Every web value flag needs `--web-enable`, and `--cat-bind-all` needs `--cat-enable`; a line that breaks that is refused, not
quietly dropped.

**The password is never on the command line.** `--web-password-file` takes a file name; there is no flag whose value is the
password, because an argument sits in the process list, in shell history and in a unit file. The file holds the password on one
line: one trailing line end (LF or CR LF) is not part of it, nothing else is trimmed (a space is a character); an empty file,
a second line, a NUL byte and bytes that are not UTF-8 are refused, and so is a password under 8 characters. Keep the file
readable by the service user alone (`chmod 600`). The plaintext is held in memory only from the read to the hand-over to the
engine, which hashes it (PBKDF2, the same record the settings panel writes) and overwrites its copy; it is in no log line and no
message. A mistaken `--web-password=SECRET` is refused as an unknown argument and shown up to the `=` only.

**The bind policy is not worked around.** A bind that is not loopback, with no password set, is refused by the same policy the
settings panel uses, and its sentence is printed as it is:

```
foxsdr-engine: the web server was not started: set a password before allowing access from other machines - binding 0.0.0.0 would let anyone on the network tune and listen through this receiver
```

(The real text has an em dash where the hyphen is.) The exit code is 1, the receiver is never started, and a settings file is **left exactly as it was** (a refused command line
is not a setting). A port that something else holds is refused the same way, with the machine's sentence. Loopback may run with
no password (the line `foxsdr-engine: web on 127.0.0.1:8073 (no password: loopback only)` says so).

**Persistence.** With a settings file (`--config PATH`, `--use-config`) what the flags set is kept: it is written by the run's
forced first save and restored by a later run **with no provisioning flags** - give the flags once, then start the service with
`--config` alone. Without one (a hermetic run) the flags apply for the life of the process and **nothing is written**; the
first line the run prints says `for this run only`. A password hash is stored in the settings file as the panel stores it; the
plaintext never is.

Example (first run, then the unit's own command):

```
umask 077; printf '%s\n' 'a long passphrase' > /var/lib/foxsdr/web.pw
foxsdr-engine --config /var/lib/foxsdr/config.json --web-enable --web-bind 0.0.0.0 --web-port 8073 --web-password-file /var/lib/foxsdr/web.pw --seconds 3
foxsdr-engine --config /var/lib/foxsdr/config.json        # what the service runs: the web server comes back from the file
```

## How the engine is stopped

`foxsdr-engine` run with no `--seconds` goes on until it is ended from outside. What ends it **in order** is:

| Platform | What asks | Handled by |
| --- | --- | --- |
| Linux | `SIGTERM` (`systemctl stop`, `kill`), `SIGINT` (Ctrl+C), `SIGHUP` (the terminal closing) | `core/stop_signal.cpp`, armed by boot (step 14) |
| Windows | Ctrl+C, Ctrl+Break, a closing console | the console handler the window application uses (`core/console_close.cpp`) |

The handler only asks; the engine's wait loop reads the request and runs the same stop `--seconds` ends in: sound-card
closes no longer waited for, the servers and the control thread stopped, the final settings save, the pipeline stopped, the
plugins released, **the clean-exit marker written last of all**, the last save waited for, the drain. It prints
`foxsdr-engine: stop requested (SIGTERM), stopping` first and `foxsdr-engine: stopped after N s` when it is done, and exits `0`.
A signal-driven stop is a clean exit: the next start counts no crash. A kill (`SIGKILL`, End task, the out-of-memory killer)
is not a stop: the marker stays false and the next start counts the crash.

**A second signal ends the process at once** (Linux). The first one only asks; a later one is handed back to whatever was
there before the engine (the exit recorder's handler when a sentinel is armed, else the default), so an operator who asks twice
is never held by a stop that will not finish. A signal that was ignored when the engine started (`nohup`) stays ignored. On
Windows a second Ctrl+C does nothing more; the budget below is what ends a wedged stop.

A signal that arrives while the engine is still restoring its settings (before the receiver is up) is held and acted on at the
first wait, a few seconds at most; a second one ends the process.

## The stop is bounded

Every stop - signal, `--seconds`, a failed start - runs under the window teardown's own budget: `HangWatchdog::kShutdownThresholdMs`,
**53 s**. When a report may be written (`FOXSDR_DIAG_DIR`, or the stored Diagnostics switch on a run that is allowed to
write) a hang report with every thread's stack is written when the budget is crossed. **5 s later** (`ShutdownGuard::kEndGraceMs`)
the process is ended with **exit code 3** after one line on stderr naming the budget. So a stop takes at most 58 s.
If it was wedged before the marker was written (the plugin unload, the pipeline join, a driver's close), the marker is
false and the next start counts the run as a crash.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | the run was asked to end and did (`--seconds` elapsed, or a stop signal / console event); a tool mode that passed; `--version` |
| 1 | a refused argument; a tool mode that failed; the settings file is in use by another program; the provisioning was refused or its password file could not be read; the engine could not be started |
| 2 | a helper mode refused its arguments |
| 3 | the stop did not finish within its budget and the process was ended |
| 128 + N (shell) / killed by signal N | a second stop signal, or a kill |

## A sample systemd unit

Directive meanings checked against systemd.service(5) and systemd.kill(5) (freedesktop.org, current manual). Paths, the user and
the groups are placeholders for your install.

```ini
# /etc/systemd/system/foxsdr-engine.service   (SAMPLE - adjust paths, user and groups)
[Unit]
Description=FoxSDR headless engine
After=network.target

[Service]
Type=simple
User=foxsdr
Group=foxsdr
# The radio's device nodes: the groups your udev rules give them (plugdev, dialout, audio, ...).
SupplementaryGroups=plugdev audio
# /var/lib/foxsdr, owned by the service user and created by systemd.
StateDirectory=foxsdr
ExecStart=/opt/foxsdr/foxsdr-engine --config /var/lib/foxsdr/config.json
# The engine's stderr is its journal; it installs no capture of its own.

# systemctl stop sends SIGTERM (the default KillSignal): an orderly stop, exit 0. The engine's own budget for a stop is
# 53 s + 5 s; the manager's timeout must be longer than that, or SIGKILL arrives first and the marker is left false.
TimeoutStopSec=90
# SIGTERM to the engine alone; whatever else is left in the unit's cgroup (a sentinel, an enumeration child) is killed at
# the end. The default, control-group, sends the stop signal to every process of the unit.
KillMode=mixed

# Restart after a crash, a kill or exit code 3, not after a clean exit and not after systemctl stop.
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Notes on the choices:

- `Restart=on-failure` does not restart exit code `0`, and does not restart a stop by `systemctl stop`; it does restart exit code
  `3` (a wedged stop) and a crash. A process that dies of `SIGTERM`, `SIGINT`, `SIGHUP` or `SIGPIPE` counts as a clean exit to
  systemd unless `SuccessExitStatus=` says otherwise, so a second stop signal is not restarted either.
- `Type=simple` reports "started" as soon as the process forks; the engine's own "running" line is on the journal.
- Only `--config PATH` (or `--use-config`) gives the engine a settings file; without either it is hermetic: a muted signal
  generator, nothing read or written. A service wants the first. The provisioning flags above are given once, on a
  run with `--config`; the unit's own `ExecStart` then needs none of them.

## What is verified, and what is not

- Windows: Ctrl+Break through a shared console stops a persistent and a hermetic run with exit 0 and the marker true; a wedged
  stop is ended at its budget with exit 3, marker false, the next start counting the crash, a hang report written; the production
  budget measured at 58.0 s (tests `test_engine_main_sigterm`, `test_engine_main_sigterm_wedged`). A closing console is not tested
  (a test cannot close its own console); the system gives it 4.5 s, shorter than the budget.
- Linux: `core/stop_signal.cpp` composed with the exit recorder was compiled and run as a stand-alone harness under WSL2 (Ubuntu,
  g++ 15.2): the first signal sets the request and the recorded exit is `Exited 0`; a second is recorded `Signalled SIGTERM` and
  ends the process. That is **WSL evidence, not a Linux machine's, and not inside foxsdr-engine**: the engine itself, the
  spawned-process tests' POSIX halves (SIGTERM/SIGINT/SIGHUP, a second SIGTERM), the POSIX ending of the budget guard (`_exit` after
  the exit record is written), the unit above, a cgroup's `SIGTERM` reaching a sentinel child, and anything on arm64 have never run.
- Packaging and build (stage 7g). **Windows**: the configure of the engine-only build (`-DCASCADE_BUILD_INTERFACE=OFF`) with OpenGL
  disabled succeeds and the same configure with the option ON fails on OpenGL, so the first result is not vacuous; the OFF tree has no
  `cascade`, `cascade_gui`, `cascade_shell`, `cascade_interface`, `imgui` or `glfw` target and no OpenGL or GLFW cache entry; the
  staged `resources/bandplans` and `resources/firmware` land byte for byte (ctest `engine_only_configure`); the engine's staging and
  symbols are asserted by `test_engine_packaging`, and the tarball check proves it can fail by `release_tarball_check_selftest`.
  **Linux, under WSL2 only** (Ubuntu 26.04, kernel 6.18.54.1-microsoft-standard-WSL2, g++ 15.2, x86_64; OpenGL and the desktop
  libraries are installed on that machine, and the OpenGL package was disabled with `-DCMAKE_DISABLE_FIND_PACKAGE_OpenGL=ON`): the OFF
  configure and build of `foxsdr-engine` succeed (that tree has no `cascade`; the engine's ELF needs `libasound`, `libSoapySDR`,
  `libssl`, `libcrypto`, `libunwind` and the C and C++ runtimes and nothing of OpenGL, X11 or Wayland); `foxsdr-engine --version`
  prints `FoxSDR 0.99.84`; a hermetic `--seconds 2` run on a machine with **no sound card** (ALSA printed its complaints; the engine
  reported running, made 125 passes and stopped with exit 0); the symbol archive holds the engine's own build id with line tables and
  `main()` resolves to `src/engine_main.cpp`; and the Package and Check-the-tarball steps, taken verbatim from `build.yml` and run on
  the real ON-build binaries, produce and accept a tarball that holds both programs, the band plans and the firmware notice, and refuse
  it when the engine, or the band plans, are dropped from the `cp`.
- The existing `test_engine_main_*` tests, built and run on Linux under WSL2 for the first time (stage 7g changed none of them; nothing was
  fixed): 9 of 13 pass, including `test_engine_main_sigterm` (a real SIGTERM, SIGINT and SIGHUP, and a second signal) and
  `test_engine_main_sigterm_wedged` (the 53 s + 5 s budget ended a wedged stop; 98.8 s on that machine), `config_roundtrip`,
  `clean_exit_marker`, `damaged_config`, `helpers`, `password_not_in_argv`, `version`, `links_headless`. 4 fail, each on one or two
  checks of many: `args`, `provision` and `smoke` on "the isolated profile holds only what the test made" - a hermetic run leaves
  `pulse/cookie` there (reproduced by hand: `foxsdr-engine --seconds 1` and `--selftest` do, `--version` does not; PulseAudio's client
  library writes it when the audio output opens, through ALSA's pulse plugin); and `second_instance_refused` on "the refused engine's
  stderr does not contain `running`" - the JACK library PortAudio loads prints "jack server is not running or cannot be started" on a
  machine with libjack and no server. All four are about the machine the test ran on, not the engine's behaviour, and are the
  tests' to adapt for a Linux host with an audio stack. Whether the Linux CI jobs, whose audio set-up is different, show the same
  is not known until they run.

## Open questions (recorded, not decided here)

- **What a headless engine sends, and the Worker's "kind" (owner decision O8, open).** Decision O6 is that a headless engine uploads
  no crash report by default, and that holds: the crash-report sweep is the window's, nothing in `foxsdr-engine` calls it, and
  `PRIVACY.md` says so. **That is not the same as "sends nothing".** With a settings file (`--config`, `--use-config`) the engine's
  restore runs the same `Engine::telemetryStartup` the window's does. Usage reporting is on by default in a settings file, so a first
  run on a fresh file wrote an install identifier and a pending usage report into it (`telemetryEnabled`, `telemetryInstallId`,
  `telemetryPending`, measured), and a later `--config` run opened two TLS connections to the telemetry endpoint to send it
  (measured on Windows, with `FOXSDR_TELEMETRY_URL` pointed at an https address on a local TCP listener, because the client refuses
  plain http); a hermetic run made none; a settings file holding `{"telemetryEnabled": false}` made none in two runs and kept the
  setting. The engine arms no update check and no "still running" beat (`EngineService::start`, 7c: a service on the desktop's install
  id would double-count one machine, and the beat wants a "kind" the Worker may not have). What the Worker takes is in
  `telemetry-worker/worker.js`, read only where it records the install channel (`ch`, at most 12 characters): the client fills it with
  `installer`, `store`, `appimage` or `tarball` (`installChannel()`, `core/telemetry.cpp`), so a `foxsdr-engine` from the tarball
  reports as `tarball`, as the window application from the same tarball does: **its usage record carries nothing that tells the two
  apart**. A "kind" is a Worker change and a deploy (RELEASING.md, procedure step 2); the rest of the Worker is unread and nothing
  Worker-facing was implemented. For the owner: should a headless engine's settings default usage reporting OFF (no panel on which
  anyone chose it)? should its record say which program it came from (a new `ch` word that fits 12 characters, or a new field)? With
  `--use-config` the identifier is the window's, so the window's own opt-out silences both.
- **The site's crash feed and a second module name.** A report names its module (`foxsdr-engine`, `foxsdr-engine.exe`) and its build
  id; whether the site's feed accepts and resolves a module other than `cascade` is not known from this tree (the site is another
  repository), and a headless engine uploads no report by default, so nothing is sent to find out. `release.ps1 -Steps symbols` sends
  the engine's symbol files and index rows to the NAS (the archive is ours) and **not** its map to the Pi's symbol directory (the
  site's side). Reports are read locally: `foxsdr-reports` against the archive.
- **Linux, arm64, the Raspberry Pi.** Nothing of the engine has run on a Raspberry Pi, on an arm64 machine or on a real Linux machine
  (all the Linux evidence above is WSL2). The `arm64` job in `build.yml` is the first run that will build and start it on aarch64 (its
  tarball step runs `foxsdr-engine --version`); `linux-engine-only` is the first run of the engine-only package list on a runner. The
  performance baseline on real hardware (owner decision O10) is the owner's.
- **Headless tracks, tiles (7f).** Deferred: a headless engine serves no track list or track details until a headless interface
  thread is written (the survey's 7f). The engine API's table is complete for what the engine itself knows.
