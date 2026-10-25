# FoxSDR 0.99.91

**FoxSDR no longer reports a false freeze when starting an update installer on Windows and the Windows shell takes time to respond.**

The installer launch now runs on a joined worker while FoxSDR continues taking Windows sent messages. The launch result still returns to the update banner, and a failed launch still reports failure. Links and the radio driver's installation prompt use the same handling. File pickers retain their native dialog loop.

This does not change detection of a genuinely unresponsive application window. FoxSDR still waits for the shell operation to return before continuing its frame loop.

---

**SHA-256 of `foxsdr-setup-0.99.91.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.91-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.91-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.91-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.91-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-25

**Waiting for the Windows shell no longer looks like a frozen window.** A shell launch can block without processing the application's window messages. The update banner now keeps those messages flowing while the shell call runs on a COM-initialised worker. The existing user-wait watchdog pause stays in effect, and ordinary hung-window detection remains unchanged.

**Launch completion and errors remain visible.** Shell activation completes before the worker exits. FoxSDR waits for the result, including failures, before continuing. Posted input remains queued for the normal frame loop.
