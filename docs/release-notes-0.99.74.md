**POCSAG now opens its settings and received messages together, and configures the receiver automatically.** With POCSAG 1.0.3 fitted, opening or starting the plugin selects NFM, its declared bandwidth and flat audio while keeping a manually tuned paging frequency. The plugin defaults to text, offers numeric and function-code modes on its own settings page, and detects baud rate and polarity automatically. Its UK preset tunes 153.350 MHz with a 20 kHz bandwidth. Explicit preset buttons still tune their named frequencies; existing plugins keep their behavior.

Verified on Windows with 65 decoder checks, nine host test suites, a known text recording through the actual application, and live message output from a USRP B200 at 153.350 MHz. The specific transmission originally reported as missing was not available for comparison; receive windows without traffic produced no message rows. RF gain was observed rather than optimized by this change.

---

**SHA-256 of `foxsdr-setup-0.99.74.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.74-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.74-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.74-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.74-linux-arm64.tar.gz`:**
`TBD`

## Verification details

POCSAG 1.0.3 or newer can configure reception in FoxSDR 0.99.74 or newer when opened or started without moving away
from a manually tuned paging channel. Its optional preset flags apply the
declared demodulator and bandwidth even when the frequency already matches a
preset, and request flat audio by switching receiver de-emphasis Off. Explicit
preset buttons still tune their named frequencies; other plugins retain their
existing behavior.

An additional opt-in preset flag opens the shared Decoder output window for a
text decoder alongside its own settings panel. POCSAG uses it so the settings
page and received message text are both visible. Plugins without text output
ignore the flag; existing plugins that omit it retain their normal windows.

The plugin ABI remains 3. Synthetic decoder and isolated application checks
establish the software behavior; live reception depends on the signal available
to the radio.

Windows verification used the actual POCSAG 1.0.3 DLL: 65 decoder checks and
nine relevant host test suites passed. Removing the reception setup and
message-window changes reproduced their regression failures; restored builds
passed. A separately generated FM IQ recording displayed `POCSAG TEXT TEST`
in the real application's Decoder output window with the settings page also
visible. Its saved configuration read back 153.350 MHz, NFM, 20 kHz bandwidth
and de-emphasis Off. Tests used isolated application profiles.

One ten-second USRP B200 capture at 153.350 MHz also produced coherent
alphanumeric output at 2400 bit/s when replayed through the final application
and plugin. Another capture produced no message rows. Displayed row counts
include repeated playback and are not counts of distinct transmissions. The
specific transmission reported as missing was not available for comparison.

Direct Soapy/UHD reception on the same B200 also ran successfully with
`SOAPY_SDR_PLUGIN_PATH` both cleared and set. The clear-variable run produced
live message output; the later set-variable run produced no rows during its
receive window. Both used 2 MS/s, TX/RX and a displayed PGA gain of 30 dB,
without stream-error or overflow entries. Different receive windows do not
establish a cause for the difference in message counts. RF gain was observed,
not optimized by this change. These verification runs used isolated profiles;
the saved user profile was not replaced.
