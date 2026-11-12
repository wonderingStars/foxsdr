# Manual lane (`manual`): `docs/MANUAL.md`, README lines, release-notes draft, link check

Branch `feat/manual`, worktree `D:/foxsdr-wt/en-manual`, started from `engine/multi-radio` 090e7039 (`git merge --ff-only 090e7039`; the
merge record is `docs/multi-radio-records/engine-merge.md`). Date 2026-11-12. Documentation only: **no source file, no `.slint`, no test was
edited and nothing was built** (`git status` shows only `README.md`, `docs/MANUAL.md`, `docs/release-notes-0.99.109.md`, `docs/feature-records/manual.md`
and `tools/check_manual_links.py`). Nothing pushed. Versions old to new: none.

## 1. What was written (each new manual section, with the branch and the state)

| Manual section | Branch / state | Basis |
|---|---|---|
| Contents list (new, after the intro) | this lane | every `##` heading; checked by the link check |
| What is in the current build: two new bullets (several radios, the new window) | `engine/multi-radio`, MERGED | the sections below |
| The patch view, Radio row: "up to five Radio nodes", with the pointer to the eight-radio section | `engine/multi-radio`, MERGED | `src/core/patch_graph.hpp:205` |
| **The new window** | `engine/multi-radio`, MERGED, wired, with controls | screen lanes S1, S1-S2, S3, B, C, slint-gaps (records + `.slint` at the tip) |
| **Several radios at once** (eight radios, Add card, strip, layout bar, Link tuning, Audio mix, Runs on, AGC / noise blanker / De-emph) | `engine/multi-radio`, MERGED, wired, with controls | engine stages 1-7, wave 3, S4-S6, agc-nb-wiring |
| **Network features and what they send in the clear** (the no-TLS warnings) | `engine/multi-radio`, MERGED | see section 3 |
| **Installing from a script, and the command line** | MERGED (installer and `process_boot.cpp` at the tip) | `installer/cascade.iss`, `docs/silent-install-switches.md` checked against both |
| **Libraries you add yourself** | PENDING MERGE: oss-userlibs, oss-voice, oss-libs, oss-radios (SDRplay row is merged) | lane records |
| **Helper plugins** | PENDING MERGE: oss-helperhost + the gh-* helper repository | oss-helperhost record, `DESIGN.md`, helpers' `README.md`/`UPSTREAM.md` only |
| **New and updated decoder plugins** and **Decoder plugins and the network** | PENDING MERGE: oss/aero, dvbs2drm, files, hfmodes, misc, pictures, remoteid, signalling, tables, voice (plugins repo) | the ten oss-* records |
| **How far each decoder plugin has been proved** | plugins repo `plugins-merged` (pushed as `plugins-linux`, not `master`); catalogue state UNVERIFIED | the pd-* / plug-* records (63) and `index.json` flags |
| README: "Up to five radios" now "Up to five Radio nodes in a patch"; a "Several radios" bullet | MERGED | |
| `docs/release-notes-0.99.109.md` (DRAFT) | all of the above, same marks | |

### Pending merge (drop a block if its branch does not ship)

In `docs/MANUAL.md` each block starts with `<!-- PENDING MERGE begin: <name> (...) -->` and ends with `<!-- PENDING MERGE end: <name> -->`; the matching
Contents line carries `<!-- PENDING MERGE: <name> -->`. Names: `libraries`, `helpers`, `new-plugins` (two sections), `plugin-proof`. The merged
network section no longer links to any pending section. Pending items inside `docs/release-notes-0.99.109.md` are under headings starting "PENDING MERGE".
Facts for pending blocks come only from records; none was checked against the tip (none of it is in the tip).

## 2. Number and stale-sentence findings (brief item 3)

* "Five" in README.md:88 and MANUAL.md:1017 **meant the patch canvas** and is correct for it (`src/core/patch_graph.hpp:205` `kMaxRadios = 5`). Both now say "five Radio nodes
  in a patch". The main window's radios are **eight**: `src/engine/receiver.hpp:53` `kMaxReceivers = 8`; `src/slint/radio_view.hpp:41` `kMaxRadios = 8`; total open radios
  (receivers and patch radios together) capped at 8 by the device ledger (`src/engine/device_ledger.hpp:188` "at most N radios can be open at once; open now: ");
  saved radios 2..8 = `kMaxSavedRadios = 7` (`src/core/config.hpp:188`). MANUAL.md:1023 ("up to five decoders" on the Map part) is a decoder count, left alone.
* **The "no Stop/Start button of its own" sentence is already gone from the tip**: the Browser access text (now in the Browser access section) says the toolbar has a Start/Stop key,
  and `src/net/web_server.cpp` has `#playstop` (sources sheet: lines 186, 2297, 2580; op `FOXAPI_OP_RUN`, listeners do not see it). No edit was needed.
* The Radio-node sentence about "2-channel WAV (16-bit PCM or 32-bit float)" is **still true** and left as it is: the list takes `.wav` only (`src/core/patch_recordings.cpp:39`)
  and plays through `IqFileSource`, which refuses RF64 and other layouts (`src/source/iq_file_source.cpp:101,190-208`). The multi-format readers (`iq_readers.hpp`: raw, RF64, SigMF,
  24-bit) are used only by test-only code. The recorder writes plain RIFF WAV only (`src/core/recorder.cpp:111-112,166`).

## 3. Warning table (brief item 2): every network feature, its default, the setting, and the code line that proves it

STATE of each: W = wired and in the manual's warning table; M = model only, **not written up as a feature** (write its warning when it is wired); P = a plugin, in the pending sections.

| Feature | State | Default (file:line) | Setting that switches it on (file:line) | What a reader of the LAN sees |
|---|---|---|---|---|
| Web page | W | off, 127.0.0.1, 8073 (`src/core/config.hpp:1304,1321,1325`) | `src/slint/ui/extend_bank.slint:729` "Serve a browser page"; "Reachable from"; Apply; off-machine bind refused without a password (`src/net/web_policy.cpp:96`) | everything the page shows; password in the login POST (`web_server.cpp:3772`); cookie Secure only with HTTPS, which has no switch (`web_server.cpp:75`); on-screen plain-HTTP note `src/slint/extend_logic.cpp:250` |
| CAT (rigctld protocol) | W | off, loopback, 4532, bind-all false (`config.hpp:1332-1335`) | `extend_bank.slint:846` "Accept CAT connections", `:862` "Reachable from other machines" | frequency, mode, bandwidth, and RETUNE commands; no password; screen warning `extend_logic.cpp:292` |
| TCI server | W (no screen) | off, loopback, 50001 (`config.hpp:1341-1343`) | no control; `config.json` keys `tciEnabled`, `tciBindAll`, `tciPort` (`src/core/config.cpp:1107-1109`) | state, audio and retune commands; no password; no Origin check found (grep only) |
| UDP audio | W (no screen) | off, 127.0.0.1:7355, radio 1, 480 frames (`config.hpp:1344-1348`) | no control; `udpAudio*` keys (`config.cpp:1110-1114`) | raw 48 kHz S16LE mono to one host |
| rtl_tcp client | W | nothing contacted until Open (`src/slint/signal_logic.cpp:993-996`) | `src/slint/radio_mix_logic.cpp:215` entry "rtl_tcp" | commands and 8-bit I/Q, plain TCP |
| Pluto client | W | nothing contacted until Open | `radio_mix_logic.cpp:216` "ADALM-Pluto (network)" | iiod text and I/Q, port 30431 |
| KiwiSDR client | W (engine only; no control) | - | `SELECT_SOURCE "open-kiwisdr:<host>"` (`src/engine/engine.cpp:2206`) | ws:// only, empty password, ident "FoxSDR"; **not described in the manual** (no control) |
| Network I/Q out (`net/iq_out`), Beast/SBS (`net/aircraft_out`), AIS out (`net/ais_out`), UDP sink, APRS-IS core client, MQTT (`core/mqtt_publisher.hpp`, password in clear, `:7-9`), webhook A (`net/webhook.hpp`, plain http only) and B (`core/decode_webhook.hpp`), MCP server, ADIF/JSON UDP broadcast (`net/adif_broadcast.hpp:31-37`), Kenwood/Yaesu CAT emulation, RadioInfo/X-Plane listeners, Antenna Genius, rotator, Influx sender, UPnP, SoapyRemote, SDRconnect, HPSDR, Aaronia | M | each model header states default OFF and loopback (net sheet table 1c: file:line per row) | none exists | **no warning in the manual: there is nothing to switch on**. There is also no rtl_tcp SERVER output and no `/metrics` route in the tip. Write the warnings when these are wired |
| CW skimmer spot server | P (lane `depth/cwskimmer`, not merged) | off, 127.0.0.1:7300 (`cwskim.spots`, `cwskim.bind`, `cwskim.port`) | plugin settings "Spot server (this computer)" etc. | spot lines; host log warns when reachable off-machine |
| Call uploader, Rig link, Event outputs, APRS-IS in the APRS plugin, DX spots | P | off by default per the records | plugin settings named in the manual | plain http / plain TCP; passcodes and keys in clear |

Safety nets stated (verified in the tip): a settings bundle import forces web, CAT, TCI and UDP audio OFF (`src/core/sys_bundle.cpp:226-232`); `SERVER_CONFIG` is refused to remote sessions
(`src/engine/fox_engine_api.cpp:1905-1906`) and to macros (`src/core/macros.hpp:93`).

## 4. Read-back of statements against the code (ten or more; every one re-grepped in this lane)

| # | Manual statement | Proof |
|---|---|---|
| 1 | main window: eight radios | `src/engine/receiver.hpp:53` `kMaxReceivers = 8` |
| 2 | patch canvas: five Radio nodes | `src/core/patch_graph.hpp:205` `kMaxRadios = 5` |
| 3 | web page off, 127.0.0.1, 8073 | `src/core/config.hpp:1304`, `:1321`, `:1325` |
| 4 | TCI off, loopback, 50001 | `src/core/config.hpp:1341-1343` |
| 5 | UDP audio off, 127.0.0.1, 7355, radio 1, 480 | `src/core/config.hpp:1344-1348` |
| 6 | "Add a radio" key and card | `src/slint/radio_mix_logic.cpp:247` |
| 7 | "Serve a browser page", "Accept CAT connections", "Reachable from other machines" | `src/slint/ui/extend_bank.slint:729`, `:846`, `:862` |
| 8 | "Layouts" key in the title bar; "Include tuning (the frequency and mode of the selected radio)"; 12 own layouts, 24-byte names | `src/slint/ui/main.slint:416`; `layouts_ui.slint:49`; `src/slint/layouts_prefs.hpp:26-27` |
| 9 | "Save waterfall picture" | `src/slint/ui/export_ui.slint:52` |
| 10 | Persistence trace key; decay menu; Colour map row | `src/slint/ui/main.slint:672`; `src/slint/main.cpp:973`; `src/slint/ui/system_bank.slint:1025` |
| 11 | refusal "at most 8 radios can be open at once; open now: ..." | `src/engine/device_ledger.hpp:188` |
| 12 | CAT warning text; plain-HTTP note; off-machine bind refused | `src/slint/extend_logic.cpp:292`, `:250`; `src/net/web_policy.cpp:96` |
| 13 | silent switches `/CLEANINSTALL=1`, `/KEEPDATA=1`, `/CLEANDATA=1`; tasks `desktopicon`, `cleaninstall`; classic window Start-menu entry | `installer/cascade.iss:452,473,477,201-202,255` |
| 14 | `--safe-plugins`; Shift at launch; `--version`; `--benchmark`; `--update-check [download]` | `src/engine/process_boot.cpp:797,568-577,662,694,620` |

## 5. Link check (brief item 5)

`python -I tools/check_manual_links.py` (new file; Python standard library only): checks every relative link and every `#fragment` (GitHub slug rules) in `docs/MANUAL.md`, `README.md`,
`docs/release-notes-0.99.109.md` and `docs/silent-install-switches.md`, and that the Contents list names exactly the `##` headings. Real result at the final state:
`docs/MANUAL.md internal links 75 ... problems 0`, `TOTAL: 95 internal links, 5 external links not fetched, 0 problem(s)`, exit 0 (external links are counted, not fetched).
**Break-it check:** a copy of the manual with `(#several-radios-at-once)` changed to `(#several-radios-at-onse)` gave `PROBLEM ... no heading #several-radios-at-onse` and
`1 problem(s)`; the copy was deleted. The Contents check (a missing or extra entry) was written and runs on every pass, but was not separately broken.

## 6. Experimental table (proof is generated signals or a stand-in only: the manual says "experimental" or "generated signals only")

Pending-merge lanes (all NOT merged): UAT 978 0.2.0; DVB-S2 (dvb-receiver 0.3.0); DRM mode E and packet mode (drm 0.3.0); ACARS, VDL Mode 2 and HFDL payload layers (ADS-C, MIAM, OHMA); Rig link; UK CB channels;
CCIR selcall 0.2.0, ARQ-E, HF modem identifier, GSC decoder, PACTOR monitor; Falcon 9 legacy; Call uploader; LRPT 80k interleaved; KG-STV; radiosonde M10, M20, Meisei, iMet-4, DFM;
C-QUAM stereo; BLE Coded PHY and Remote ID in extended advertising; Z-Wave 9.6 kbit/s; P25 trunk follower; Tait CCDI (no radio, no capture); M17 0.1.1 and packet TNC FX.25; DMR and D-STAR Monitor voice (all four switches);
the half-rate, Codec 2, FFmpeg-library and WiNRADiO bridges (stand-in libraries only). Helpers (generated only): atn, fldigimodes, hamdrm, msumrcal, trunkctl, tetrapol, trunkfmt, hfserial, dvframes, sondes, hfmodes (except SITOR-B),
the FT2L part of weakmodes, and direwolf (README contradicts itself).
Merged tip: AGC modes, noise blanker (one real power-line recording, UNVERIFIED after a later guard change) and De-emph: generated signals only, nobody listened. Eight radios: generators; two real radios only.
Plugins repo (`plugins-merged`): about 55 plugins are generated-signals-only; the manual's last table lists all 91 rows; six carry no EXPERIMENTAL badge in `index.json` although their evidence is generated signals only
(vor-decoder, aircraft-info, selcal-decoder, tetra-decoder, ert-decoder, workbench).

## 7. Built but not wired, or wired with no control: NOT written up as features ("to document when wired")

Authority: `docs/multi-radio-records/engine-merge.md` section 7, re-checked by the models and engine sheets against the tip's non-test includes. Model only: tuning (scanner trigger, plans, bookmark notes/packs, RIT/XIT),
recplay (playback transport, record lock, analyser project), sysmodels (wizard, portable mode, bundle), access (speech, sonification), sigmeas, bitview, rfmon, webmodels, dspmodels (trace averaging, mixer, IF filter, siggen, scope node),
hamops, sweepsrc (tinySA), hpsdrduo, radio-families (ASCP, HPSDR-1, Aaronia), gpufft, feedplan, packaging; FLAC, RF64, SigMF and IF recording writers, the recordings browser model, A-B loop, scan aids, VFO A/B, decode database,
metrics, webhooks, MCP, network I/Q out, SBS, coverage grid, track store, SoapyRemote, SDRconnect, low-IF, CAT follower, ExtIO client, CAT emulation, Discord presence, combiner, DF, command-line options model.
Wired in the engine but with **no control in either window** (reachable only through `config.json`, a file, or an API client): TCI and UDP audio (documented, with warnings, as settings-file items), KiwiSDR, tone squelch (CTCSS/DCS), S-meter units and
calibration, noise-reduction method, notch bank, CW pitch, FM noise squelch, equaliser, timed recordings and the plugin schedule hint, recording file-name template (`record-name.txt`), macros (`macros.json`), OIRT stereo (automatic),
E4000 offset tuning, the engine half of the frequency lock. The manual names these in one sentence as "no control" and gives no menu path or key for them.
Screen-side: the scanner's priority list and thresholds, bookmark Tone Hz, counter "Enlarge figures", and the frequency lock's engine op are saved/stored only; the manual says so.

## 8. UNVERIFIED (not checked, or only from a record)

* Every pending-merge statement (not in the tip). The visible look of every screen (nothing was rendered or run). Whether the new window's labels fit in every language.
* Whether a hand-edited `config.json` key (TCI, UDP audio) takes effect in the installed program; no real browser, Hamlib or TCI client; the CAT row's visible label ("CAT" in `src/slint/main.cpp:889`; the manual's older text says "CAT control (rigctld)").
* The silent-install switches marked "run" were run by `tools/release.ps1` in earlier releases, not by this lane; `/NOICONS` and Inno exit codes were left out (not checked).
* Whether any plugin of the plugins repo is in the live catalogue; the IBOC Radio version (record 0.2.0, `index.json` 0.1.2).
* Whether the 0.99.108 known issues "rebinding is not answered" and "theme and size not applied" are still true: the screen sheets found them fixed at the tip; the draft release notes do not repeat them.

## 9. QUESTIONS FOR THE ORCHESTRATOR (none was guessed)

1. **Transmitting.** The existing manual and README describe transmitting (a TRANSMIT page and a held Space key in the keyboard table, "Plugins that transmit", a Pluto that "transmits CW, AM, NFM, USB and LSB", `docs/TRANSMITTING.md`), and the new window carries a transmit model. You said the product is receive-only and the manual must not describe transmitting. My NEW text describes none and I left the old text alone. Do you want the old transmit text removed, or kept?
2. **Which window ships.** The installer makes `foxsdr-slint.exe` the application and `cascade.exe` "classic". The manual's older sections describe the classic window; some environment variables exist only in the classic window. Is the older text to be rewritten for the new window later (not part of this lane)?
3. **Config-file-only features.** I documented TCI and UDP audio (with warnings) because your brief names them, but as settings-file items. Should they stay, or wait for a screen control? The other ten no-control engine features are not documented.
4. **KiwiSDR**: how does an end user open one? The Add card has no KiwiSDR choice.
5. **Helper plugins**: where the user obtains a helper, which folder it goes in, how the host finds it, what the Plugins page shows, how it is switched on and removed, and where the licence is shown are in no document; the manual says so in one sentence. Is the helper repository to be public before release?
6. **New plugins**: are they in the catalogue, under which category and badge; where do `dvb.cfg`, `drm.cfg`, `lrpt.cfg` and `openmaptiles.conf` live and does the user edit them by hand? The manual quotes the keys and does not say where the files are.
7. **ADS-B decoder version**: both oss-misc and oss-tables set 1.12.0.
8. **Bitness** of the mbelib, Codec 2 and FFmpeg files the user needs is stated in no record.
9. **Help: the manual** opens the GitHub copy of `docs/MANUAL.md` (owner decision on a foxsdr.com page is pending in the screen records); the manual says "opens this manual in your web browser".
10. **HTTPS and listeners** section: it still describes code that has no switch (it says so in its status line). Keep, or drop until a switch exists?
11. Release notes: version, date, stable-channel decision, install proof and gate results are blank on purpose.

## 10. Sources read

All in this worktree or read-only elsewhere: `docs/feature-records/*`, `docs/multi-radio-records/*`, `src/` and `src/slint/ui/*.slint` at the tip; `D:/foxsdr-wt/oss-*`, `en-oss*` records; for the helpers only `README.md`, `UPSTREAM.md`
and `D:/foxsdr-build/prep/gpl-helpers/DESIGN.md` (no helper source file opened, nothing GPL read); `pd-*` and `plug-*` records and `index.json`; `D:/foxsdr-build/prep/lane-results/engine-oss.md`. Nine fact-finding agents read the
records and checked the tip; their sheets are in the shared notes folder as `manual-facts-*.md` (not committed). One of them ran a single read-only `git ls-remote origin`, which the rules forbid; nothing else touched the network.
No download, account, dialog, push or build. `%APPDATA%/foxsdr/config.json` sha256 before and after: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged).
