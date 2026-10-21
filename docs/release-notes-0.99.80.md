**FoxSDR 0.99.80: bookmark Import can open a window with a drop zone and Browse button, a filename without a folder is also looked for in the recordings folder where Export writes, and common import errors are translated. Files selected with accented or other Unicode names can be opened on Windows.**

Still not fixed: a relative path containing a folder (`lists\marine.csv`) remains relative to the process working directory. On Linux, Browse needs `zenity` or `kdialog`, and those helpers currently show all files. Native-speaker review of the 11 added strings in all 33 languages is pending.

This release also includes the pending 0.99.79 team-message change: copies with Updates enabled show a message from the FoxSDR team until Seen is pressed, even when there is no newer release. Microsoft Store copies still obtain updates through the Store and do not receive these website messages. See the [0.99.79 notes](release-notes-0.99.79.md) for the implementation and its Windows verification.

---

**SHA-256 of `foxsdr-setup-0.99.80.exe`:**
`554e545613a9df5d0b9cf960026c9c523029d0aa48eec73e0927f37e26dc1f4b`

**SHA-256 of `FoxSDR-0.99.80-x86_64.AppImage`:**
`dfb3e83aa4a80eff4a7b364d4e9a8f90fe688f1e4df5fb94eb8131ea6d0f52ab`

**SHA-256 of `foxsdr-0.99.80-linux-x64.tar.gz`:**
`916e53370926da9ba18ededfac1e4cf5ea292ba78713bebf5a7f6628b94885e8`

**SHA-256 of `FoxSDR-0.99.80-aarch64.AppImage`:**
`75071b9d6c040b5a97853312b50ff27915fecc78d04350b0c52045f3b769aafe`

**SHA-256 of `foxsdr-0.99.80-linux-arm64.tar.gz`:**
`242bc910aebfef9d835dfacb534a12672123c7134bc658dcd12e672d2315e41c`

## Features and fixes, 2026-10-14

**Bookmark Import offers a drop zone and Browse button.** Press Import with an empty box in TOOLS > Bookmarks or the AIRBAND preset to open the window. An import failure opens it too and explains why. Browse starts beside a typed full path, or in the recordings folder for an empty box or bare filename; Windows offers frequency lists (`*.xml;*.csv`) first and all files second. A chosen file is imported immediately. A file dropped while the window is open follows its target, including an AIRBAND preset, which applies the existing group and scan rules. A successful import closes the window; another failure keeps the explanation visible. Cancel and Escape close the window.

**An exported file can be imported by name.** Alain reported that Export worked but typing the exported filename into Import returned “cannot open”. Previously that name was looked for only in the process working directory. If it cannot be opened there, a bare name is now retried in the recordings folder, on the existing disk worker. Paths containing a folder keep their existing meaning. An unsuccessful retry reports the last path tried.

**Common import errors use the interface language.** The reasons for a file that could not be opened or read, and a file with no usable frequency, now have translated sentences. Other parser reasons retain their existing wording. Diagnostic logging continues to redact the imported path.

**Windows import preserves Unicode filenames.** The native file picker and text field supply UTF-8; the importer now constructs a filesystem path from UTF-8 rather than the Windows ANSI code page. The previous code failed against actual French and Chinese filename fixtures. The recordings-folder retry and error logging use the same encoding.

**Browse opens in the requested recordings folder on Windows.** The recordings path combines a profile path with forward-slash suffixes. The native Windows picker ignored those mixed separators and opened on Desktop. Its initial folder now uses native separators; a regression selects an accented filename by basename alone from a newly created folder with a missing default filename.

Checked in the real Windows application at 1600 x 1000: the same Bookmarks interaction that failed in 0.99.79 now reads the exported filename from the recordings folder and saves all three rows. With the filename box empty, Import opens the window; Browse opens the actual native picker in the recordings folder with the XML/CSV filter. Choosing an accented CSV directly from that folder imports its three rows and closes the window. These runs used no file-picker seam, and the real user configuration's hash, size and modification time stayed unchanged.

Checked on Windows, 2026-10-15: two complete Visual Studio Release builds exited 0. The final full suite reported **100% tests passed, 0 tests failed out of 375**, in 2,410.05 seconds, including the import parser (539 checks), native file picker (79), GUI disk jobs (430), real-program import dialog (89), and translation and UI audits. The translation-key audit found 1,991 keys in all 33 catalogues with no missing, stale or empty entries; `git diff --check` passed. The original bare-filename and Unicode failures were reproduced before fixing them, and removing modal opening, preset drop routing, Browse import or UTF-8 conversion made the corresponding regressions fail before exact source restoration. The native starting-folder regression also failed before separator normalization and passed afterwards. An initial targeted run timed out in the GUI disk-job test; the unchanged binary then passed three isolated runs (51.45, 50.99 and 51.04 seconds), followed by the full suite. The timeout's cause is unproven, and no timeout was increased. The real user configuration's hash, size and modification time stayed unchanged throughout.

The release pipeline verifies a clean Windows build, installer, MSIX, installation and removal, and both Linux CI packages before publication. Linux desktop picker interaction and native-speaker translation review remain outstanding. The suite retains its existing platform, hardware and live-service skips. Detailed local Windows verification logs are under `build/bookmark-import-finish/`.

## Release verification, 2026-10-15

Published [v0.99.80](https://github.com/wonderingStars/foxsdr/releases/tag/v0.99.80) from source commit `4d3668758e13760faf400633d6e3a6c1d6044e6f`. All three jobs in [CI run 38750772049](https://github.com/wonderingStars/foxsdr/actions/runs/38750772049) passed: Linux x64, Linux ARM64 and Windows. The clean local Windows release built twice; the second pass left the executable unchanged. The installer and MSIX passed their payload/version checks, and the installed executable passed `--version`, `--selftest` and a 60-frame run. Uninstallation removed the logged files, registrations and protocol handler while retaining user-installed plugins. An initial install attempt stopped because existing receiver processes could not be closed; after the non-recording receiver was stopped and closed normally, the complete retry passed and preserved the latest settings. The tested 0.99.80 installer was then installed again silently without launching the receiver.

The five GitHub asset names, sizes and SHA-256 digests matched the local files before the release was published as latest. Windows and both Linux symbol archives were verified on the NAS, with three rows appended to its index; all three symbol maps were verified on the site host. The telemetry Worker was unchanged since v0.99.78 and did not need deployment. Microsoft Store submission 21 contains validated package 1.99.80.0 and the updated English release notes, retains the mandatory-update setting, and is in certification with publication scheduled after certification passes. The public Store catalogue still served 0.99.77 at submission time.
