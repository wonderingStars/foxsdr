# FoxSDR 0.99.74 development notes

**POCSAG opens its settings and received messages together, and configures the receiver automatically in this development build.** With POCSAG 1.0.3 fitted, opening or starting the plugin selects NFM, its declared bandwidth and flat audio while keeping a manually tuned paging frequency. The plugin defaults to text, offers numeric and function-code modes on its own settings page, and detects baud rate and polarity automatically. Its UK preset tunes 153.350 MHz with a 20 kHz bandwidth. Explicit preset buttons still tune their named frequencies; existing plugins keep their behavior.

Verified on Windows with 65 decoder checks, nine host test suites, a known text recording through the actual application, and live message output from a USRP B200 at 153.350 MHz. The specific transmission originally reported as missing was not available for comparison; receive windows without traffic produced no message rows. RF gain was observed rather than optimized by this change.

**Opening Language & country no longer freezes the window while it looks for Chinese, Japanese and Korean fonts.** To list each language by its own name (日本語, 한국어) the window has to know which of those languages this computer can draw, and to find out it read Microsoft YaHei, JhengHei, Yu Gothic and Malgun Gothic from the Windows font folder (14 to 21 MB each) inside one frame, one byte at a time, and then read the ones it used a second time to put them in the text atlas. On a laptop with a slow disk that was a frozen window of many seconds, and the freeze monitor caught it twice in the field: 0.99.70 (signature CE8EBCD986C97CEB, stopped reading a font file) and 0.99.71 (signature D28A155E1A128889, a 13.6 second frame in the frame-start step, 32 plugins fitted and the audio starving). A font file is now read in one call (YaHei, 19.7 MB: 85 to 89 ms before, 11 ms after, on the development desktop with the file already in the system cache), each file is read once, and the search for the first usable font of each of the four scripts runs on a thread of its own, started when the section opens. With every font read made to take 400 ms the call that opens the list returned in 0.1 ms, where the same call took 2.9 s before. The trade is one or two frames of boxes: until the search finishes (39 ms in a run of the real application on this desktop, longer on a slow disk) the native names are drawn as boxes, the rows can already be picked, and a language the computer turns out not to be able to draw is refused afterwards with the same message as before. Two things remain. The thread that draws the window still works out which script each language needs (6 to 8 ms for the 34 listed here). And a user whose Windows language, or the language of the country chosen in this section, is Chinese, Japanese or Korean while the interface is in another language waits for the search once, on the section's first frame, for about the length of four of those file reads, because the two lines that ask whether that language can be drawn ask the exact question (668 ms and 674 ms measured with every read made to take 150 ms; the freeze threshold is 5 s, so it is reached only if a disk takes about a second for each 20 MB file); the previous code read the same four files on the window thread in the next frame, byte by byte and twice over, and those two lines are not yet moved. The search keeps the file it found for each script only until the list's names are in the text atlas: the files it merely looked at (JhengHei and Yu Gothic, 35 MB, because YaHei already draws their names) are then let go, so the memory held after opening the list is what it always was, and choosing Japanese or Traditional Chinese afterwards reads its file once more, in one call.

The host source is committed for development. No FoxSDR 0.99.74 application
release, installer, website deployment or Microsoft Store submission accompanies
the POCSAG plugin-store publication.

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

The language list's font search is covered by `test_font_scan`, which makes every
font read slow on purpose and checks that each system font file is opened once
(and that choosing Japanese afterwards opens its file a second time, and no other
file, with nothing held in memory beyond the atlas in between),
that asking for the names returns at once while the files are being read, that
the three-way answer (Yes, No, Checking) agrees with the exact one when the search
is done, that the one-call file read returns the bytes a byte-by-byte read does,
that the search finds the faces the on-demand walk finds, and that a search still
running when the program exits is joined rather than terminated. Against the
previous code it failed 9 checks, among them the 2.9 s return and the two reads of
`msyh.ttc` and `malgun.ttf`; with the change it passes, and removing each of the
behaviours it is named after (waiting in `applyPending`, the kept bytes, the
release of the kept files the names did not use, the non-blocking answer, the
exit-time join, the full-length read, the search's first candidate) makes it fail
again. The 350-test suite passed on Windows before the release of the unused kept
files was added; after it, the eight font, i18n and CJK tests were run again and
pass. A run of the
real application with the section open and a Japanese country chosen started the
search, adopted its answer 39 ms later, merged the names' faces on the next frame
and shut down cleanly. Not verified: a slow disk (only a simulated one), and the
Linux build, which only CI compiles.
