# Running foxsdr-engine as a service: provisioning the web and CAT servers, stopping it, exit codes, a sample systemd unit

This is a sample and a statement of behaviour, not shipped code: nothing here is installed by any package, and the Linux
half has NOT been run on a real machine with a real radio (see "What is verified" at the end). The packaging, install paths
and a full headless guide are stage 7g's.

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
