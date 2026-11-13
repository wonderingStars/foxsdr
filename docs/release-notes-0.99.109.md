# FoxSDR 0.99.109 — DRAFT, NOT FOR PUBLICATION

**DRAFT written 2026-11-12 from the lane records and the merged tree `engine/multi-radio` 090e7039.** Nothing here is
promised unless it carries the state mark MERGED (built on `engine/multi-radio`, with a control in the window or a setting
you can reach) and even then it has not been through the release gate, the real-mouse pass over every control, or a real
second radio. Items marked PENDING MERGE are on lane branches that are not merged: drop an item whose branch does not ship.
Every item whose proof is generated signals or a stand-in program says so. Receive-only: no item describes transmitting.
Version numbers, release date, install proof and gate results are not filled in. See `docs/MANUAL.md` for the detail of each
item (the section is named in brackets).

## WHAT CHANGED

### In the window (MERGED on `engine/multi-radio`, wired, with a control)

- **Several radios at once.** The main window opens up to eight radios, shown one, two or four at a time, with a status-bar
  **Add a radio** key, a selector, **STOP ALL** and **START ALL**, **Link tuning**, **Start together**, an **Audio mix** with
  a route, mute, solo and volume for each radio, **Ctrl+1** to **Ctrl+8** to select a radio, and a **Runs on** choice for each
  decoder. The patch canvas keeps its own limit of five Radio nodes. Proof: generator radios, and a B200 with an RTL-SDR for
  30 minutes with the sound off; nobody has listened to several radios. An older version reads only radio 1 from a saved
  settings file that has several radios. [Several radios at once]
- **AGC, a noise blanker and FM de-emphasis** in the SIGNAL bank's Audio filters section. Proof: generated signals only.
  [Several radios at once]
- **Saved layouts** (a **Layouts** key in the title bar, two built in: Airband and HF DX) and **waterfall export** to a PNG
  picture or a CSV or FSW data file (TOOLS > Export). [The new window]
- **More spectrum and waterfall choices** at the foot of SYSTEM > Display: six colour maps including a blue-yellow-safe one,
  waterfall speed, time stamps, a Persistence and a Min hold trace, trace shape, peak labels, band zoom, SNR, channel power,
  frequency-tracker and signal-history readouts, a reference trace and a compact window; A and B markers in the right-click
  menus. These choices are **not** saved between sessions. [The new window]
- **Four analysis keys on the Demod scope:** WATERFALL, AUTOCORR, CEPSTRUM and EYE. [The new window]
- **Scanner lockouts, a scan log, mute while scanning, bookmark memories and eight quick memories,** a tuning-step lock, a
  delta lock, the 8.33 kHz channel name on the airband, a frequency-lock key (it stops this window only) and a status-bar
  clock. The scanner's priority list and thresholds are saved but not used yet. [The new window]
- **App menu** (click the title bar's brand): Keyboard shortcuts sheet, a Recordings list, Help. **High contrast** skin,
  **Digit grouping**, **Copy** on the decoder output, zoom on plugin pictures, **BROWSE FILES** on a plugin's file setting,
  dropping a file on the window. [The new window]
- **Plugin mark names** on the spectrum and the waterfall are stacked in rows with a **+N** tab for the ones that do not fit.
  [The new window]
- **Install and command line documented:** silent-install and silent-uninstall switches (`/CLEANINSTALL=1`, `/CLEANDATA=1`,
  `/KEEPDATA=1`), `--version`, `--safe-plugins` (also Shift held at start) and the environment variables. [Installing from a
  script, and the command line]

### In the settings file only (MERGED, wired, no control in either window)

- **A TCI server and UDP audio** (receive only, off by default, loopback by default, **no password and no encryption**) are
  switched on by keys in `config.json`. No real TCI program has been tried with it. [Network features and what they send in
  the clear]
- Wired in the engine but with no control, so not offered as features: tone squelch, S-meter units and calibration, a choice
  of noise-reduction method, a notch bank, a CW pitch, FM noise squelch, an equaliser, timed recordings, a recording file-name
  template and macros.

### Documented, not new

- The manual now has a **contents list**, a warning for every network feature that exists in this build, and the radio-count
  wording corrected: eight radios in the main window, five Radio nodes on the patch canvas.

### PENDING MERGE: libraries you add yourself (branches oss-userlibs, oss-voice, oss-libs, oss-radios)

- Libraries FoxSDR does not ship and you add yourself: mbelib (half-rate voice, for the DMR and D-STAR Monitor), Codec 2 (FreeDV
  receive), FFmpeg libraries (DAB+ sound). Tested only against stand-in libraries, except Codec 2's plain decode, which ran
  against the real file. [Libraries you add yourself]

### PENDING MERGE: helper plugins (branch oss-helperhost and the separate GPL repository)

- A **Helper bridge** plugin that runs a separate GPL decoder program over a pipe. No real helper has been run through FoxSDR;
  how a user obtains and installs one is not decided. [Helper plugins]

### PENDING MERGE: new and updated decoder plugins (branches oss/aero, dvbs2drm, files, hfmodes, misc, pictures, remoteid, signalling, tables, voice)

- **EXPERIMENTAL, generated signals or stand-in programs only:** UAT 978 MHz, DVB-S2, DRM+ mode E, the ACARS, VDL Mode 2 and
  HFDL payload layers, the new selcall sets, ARQ-E, PACTOR identification (a detector of the Signal identifier), Falcon 9 legacy recordings, KG-STV (a mode of the SSTV decoder), more
  radiosonde types, C-QUAM AM stereo, Bluetooth LE Coded PHY and Remote ID, P25 trunk following, Tait CCDI monitoring, the
  half-rate and D-STAR voice of the DMR and D-STAR Monitor, Rig link and the Call uploader.
- Z-Wave (40 and 100 kbit/s; a protocol of Short-Range Frames) has been seen in one real recording; Bluetooth LE 1M met three real recordings.
  [New and updated decoder plugins]
- Still unsettled: two lanes both call the ADS-B decoder 1.12.0.

### Not in this release

- Nothing in the lists above transmits. Pending-merge items that are not built (and so not promised): Inmarsat Aero, FIS-B
  products, DVB-T2, MDC-1200, RADE, P25 phase 2, Wi-Fi Remote ID.

## Known issues (from the records; to be re-checked before publication)

- The new window's display choices (colour map, speed, trace, readouts, compact) are not saved.
- The scanner's priority list, per-entry thresholds and the bookmark memory's Tone Hz are saved but not applied.
- **Enlarge figures** and **Enlarge every reading** on the counter are saved but do not enlarge anything.
- At 800 pixels wide the strip, tab row and title bar are wider than the window in 20 to 30 languages.
- The Web page is plain HTTP; the HTTPS and listener switches described in the manual do not exist yet.
- No second real radio, no real browser, Hamlib or TCI program, and no real-mouse pass have been used with this build.
