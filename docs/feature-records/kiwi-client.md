# KiwiSDR listen-only client (pre-release feature 4): build record

Branch `feat/kiwi-client`, from `b9208fba` (engine/multi-radio). Built 2026-11-11.
Stand-alone: nothing here is wired into the engine, the settings or the screen.

## WHAT WAS BUILT

New files only; no existing file was edited and no CMake line was added (`src/source/*.cpp`
and `tests/test_*.cpp` are globbed; every test is `headless` by default).

| File | What |
|---|---|
| `src/source/kiwisdr_protocol.hpp/.cpp` | Pure functions, no sockets, no threads, no httplib. Command builders; the frame parser; IMA ADPCM; the key=value parser; the limits; every decision beyond the outline in `kiwi::assumed`. |
| `src/source/kiwisdr_client.hpp/.cpp` | `cascade::source::KiwiSdrClient`: the connection. |
| `tests/kiwisdr_fake_server.hpp` | `kiwifake::FakeKiwi`: a scripted loopback stand-in receiver (RFC 6455 upgrade, per-connection script thread, records the client's masked text frames with arrival times, can send any chunking, fragments, a hostile length, a reset). |
| `tests/test_kiwisdr_protocol.cpp` | Protocol tests (183 checks). |
| `tests/test_kiwisdr_client.cpp` | Client tests against the stand-in and against the vendored `httplib::Server::WebSocket` (about 370 checks). |

### Public interface (`KiwiSdrClient`)

```cpp
struct Options { host="127.0.0.1"; port=8073; password; identUser="FoxSDR"; adpcm=true;
                 connectWait=5 s; stallWait=15 s; keepAliveInterval=1 s; timestamp=0 /*now*/; };
bool open(const Options&);                       // connect + upgrade + opening commands + start threads
bool tune(mode, lowCutHz, highCutHz, frequencyHz); // "SET mod= low_cut= high_cut= freq="
void close();                                    // bounded 2.5 s, idempotent, any thread
size_t read(int16_t* dst, size_t n, ms wait = 0); // decoded mono samples, one consumer thread
Status status();  State state();                 // Idle Connecting Playing RefusedBusy RefusedPassword
                                                 // ServerDown Redirected ConnectFailed Dropped Malformed Closed
                                                 // Status{state, code, redirect, detail}
bool haveSmeter(); uint16_t smeterRaw(); double rssiDbm();
uint64_t sequenceGaps(), missedFrames(), audioFrames(), samplesDelivered(),
         droppedSamples(), ignoredFrames(), keepAlivesSent();
std::string mode(); double frequencyHz();
static int audioRateHz();  static uint64_t threadsAbandoned();
```

Politeness is in the client, not left to the caller: it sends `ident_user=FoxSDR`; one connection
per object (a second `open()` returns false); no reconnect after a drop, a stall or a refusal; a
redirect is reported (`status().redirect`) and never followed; `too_busy`, `badp` (any code but
"accepted") and `down` stop the keep-alive, shut the socket and are sticky. The waterfall (W/F)
and I/Q mode are not built: W/F and EXT frames are counted and dropped, and `buildTune` refuses
the mode `iq`.

Protocol layer (`kiwi::`): `buildAuth / buildIdentUser / buildTune / buildCompression /
buildAudioRate / buildKeepAlive / buildSoundPath`; `parseFrame(data, len, AdpcmState&, ParsedFrame&)`
returning `ParseError {None, TooShort, TooLarge, UnknownTag, BadPayload}`; `decodeAdpcm`;
`parseKeyValues`; `rssiDbm`. Limits: `kMaxFrameBytes` 64 KiB (refused before any decode or
allocation), MSG text 4 KiB, password 128, user name 64, redirect target 512.

## TESTS

Build: `cmake --build D:/foxsdr-build/feat-kiwi --config Release --parallel 4` (configured like
`mr-dsp`: Visual Studio 17 2022, x64, vcpkg toolchain, Release, `CASCADE_BUILD_SLINT=ON`).

**Results (all real, this session, Windows, Release):**

- `test_kiwisdr_protocol`: `183 checks, 0 failed`. `test_kiwisdr_client`: `372 checks, 0 failed` (about 17.6 s).
- Twenty runs in a row: `ctest --test-dir D:/foxsdr-build/feat-kiwi -C Release -R kiwisdr --repeat until-fail:20 --timeout 120` gave `100% tests passed out of 2`, `Total Test time (real) = 351.56 sec`, and the log holds 20 passes of `test_kiwisdr_client` and 20 of `test_kiwisdr_protocol`.
- `test_rtl_tcp_source` still passes (`Passed 23.30 sec` alone, `23.27 sec` inside the gate). No code shared with rtl_tcp was touched.
- Warnings: a forced recompile of the two sources and two tests logged zero lines containing `warning`; the whole-tree build (`BUILD_EXIT 0`) has none that name a kiwisdr file.
- Close bound, measured: `close() with a stalled server took 13 ms`; `close() during a stalled upgrade took 1 ms` (the bound is 2500 ms).

**What the tests cover (client):** the opening commands in order and the exact text; the path `/<digits>/SND`; every client frame masked; tune after connect and refused tunes send nothing; PCM payload exact; ADPCM against the hand vector (0x70 0x8F gives 0, 11, -19, -23, derived from the public tables) and, in the protocol test, against an independent reference encoder written in the test, across frames of varying size; S-meter and RSSI; sequence gaps and wrap; frames cut across TCP segments (after the header, inside the payload), sent as WebSocket fragments with pings between, two in one write; a stall (reported as dropped, "silent"); a drop by reset and by close frame, with no reconnect for a second afterwards; `too_busy`, `badp` (codes 1 and 7, and 0 as accepted), `down`, `redirect` (reported, the second stand-in never sees a connection); a malformed header, half a PCM sample, an oversized MSG, 200 KiB and 1 MiB audio frames and an over-limit EXT; a declared 20 MiB length (dropped by the library); unknown tags ignored; keep-alive cadence at the default interval (3 to 4 in 3.6 s, each gap 700 to 1400 ms); close bounded while the server stalls, during a stalled upgrade (`open()` on another thread) and from the destructor; two clients on one stand-in independent; ring overflow counted; `read()` wait bounded; connect failures (closed port, bad host strings, HTTP 404); a never-opened object; and the whole thing once through the vendored `httplib::Server::WebSocket` route instead of the stand-in. Protocol test: builders, header parse, flags, ADPCM saturation both ways, every MSG form, the 64 KiB limit exactly, and every prefix of every frame kind plus 12000+ random buffers placed against an inaccessible guard page (a read past the end would crash).

**Break-it checks** (behaviour removed, test run, behaviour restored with `git checkout`, rebuilt):

| Rule | Removed how | Result |
|---|---|---|
| stop on `too_busy` | the client ignores a too_busy message and keeps reading | `test_kiwisdr_client` red: state never `RefusedBusy`, audio after the refusal delivered (`client.state() == want`, `samplesDelivered() == 0`, `drain(client).empty()`); protocol test unaffected |
| no reconnect | a reconnect attempt added on a dropped or silent connection | red: `srv.accepted() == 1` fails in the stall test and in both drop tests |
| redirect not followed | the redirect target is dialled | red: `second.accepted() == 0` fails |
| frame size limit | the `kMaxFrameBytes` test removed from `parseFrame` | protocol test red (5 checks: the 64 KiB+1 frame and the 100 MB length accepted, the sample vector touched); client test red (the 200 KiB, 1 MiB and EXT frames no longer give `Malformed`) |
| header bounds | the SND `len < 10` test removed | `test_kiwisdr_protocol.exe` dies with 0xC0000409 (the unsigned length underflows, a huge allocation is attempted) instead of printing a summary |
| bounded close | `finish()` no longer shuts the socket | red: `close() with a stalled server took 2507 ms`, `close() during a stalled upgrade took 29695 ms`, `took < 2500` fails twice and `threadsAbandoned()` rises |

The "no reconnect" and "redirect not followed" rules are the absence of behaviour, so the break-it adds the behaviour and shows the test catches it. All six files were restored before the commit (`git status` clean).

**Gate** (`ctest --test-dir D:/foxsdr-build/feat-kiwi -C Release -L "headless|slint" -j4 --timeout 300` after a whole-tree build): `99% tests passed, 9 tests failed out of 761`, 1471 s. The 9:

- Known false failures, not chased: `test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`.
- `test_engine_api_transmit`: failed in the gate (112 s); re-run alone it PASSED (106 s).
- `test_pass_control`: failed in the gate; re-run alone it PASSED (46.9 s).
- `test_shutdown_budget_free`: failed in the gate and failed again alone (`test_shutdown_budget: 202 checks, 1 failed`, after "hang reports from a wedged close: 1"). It runs the application with a wedged shutdown and does not touch these files; not investigated.
- **`test_thread_inventory`: failed because of this lane's files, and still fails on this branch.** It holds every file under `src/` that creates a `std::thread` to a table in an existing file, and `src/source/kiwisdr_client.cpp` (two threads) has no row: `UNACCOUNTED THREADS: src/source/kiwisdr_client.cpp creates 2 std::thread ...`. Rule 3 forbids editing that file here, so the fix is in the WIRING HAND-OFF. The row was tried and then reverted: with `{"src/source/kiwisdr_client.cpp", 2, 0, 0, 0, "", R_DRIVER},` added to `tests/thread_inventory_rows.inc` (after the `hackrf_source.cpp` / `mirisdr_source.cpp` rows, before `pluto_source.cpp`) the test `Passed 3.15 sec`.

## ASSUMPTIONS

Each sits behind one named constant or function (`kiwi::assumed::` in `kiwisdr_protocol.hpp`,
unless stated). The tests mark the expectation "ASSUMED".

1. Audio rate 12000 Hz: `kAudioRateHz` (the outline gives no rate; the design notes say 12 kHz).
2. `SET AR OK in=12000 out=12000`: `kArOkIn`, `kArOkOut` (the outline gives the command and field names, not their meaning; no resampling is done here).
3. `freq=` is in kilohertz with three decimals: `kFreqUnitHz` (formatted with integer arithmetic so the C locale cannot change the point).
4. 16-bit PCM samples are big-endian: `kPcmBigEndian`.
5. ADPCM: low nibble is the earlier sample (`kAdpcmLowNibbleFirst`); decoder starts at predictor 0 / index 0 and its state carries across frames (the per-connection `AdpcmState` in `readerBody`); no per-frame header; sample count is two per payload byte.
6. `badp=0` means "accepted", codes 1 to 7 refuse; a non-numeric `badp` value, or `badp` with no value, is a refusal with code -1: `kBadpAcceptedCode`, `classify()`.
7. A MSG frame is `MSG` + optional one space + `key=value` words: `kMsgSeparator`, `parseKeyValues`. `too_busy`, `down` and `redirect` are recognised by key alone (any value); their numeric value, when it is a number, is reported as `code`.
8. The redirect target is reported as received, control characters replaced and cut at 512: `sanitiseRedirectTarget` (not decoded; the outline does not say if it is URL-encoded).
9. Password and user name are percent-encoded on the wire: `encodeValue` (an alphanumeric value is unchanged either way). An empty password sends `p=` with nothing after it: `buildAuth`.
10. Opening order is auth, ident_user, AR OK, compression, then (from the caller) mod/freq; none waits for an answer: `KiwiSdrClient::open`.
11. `<timestamp>` in the path is the Unix time in seconds: `Options::timestamp`, `buildSoundPath`.
12. `keepalive` is the text frame `SET keepalive`, first one a full interval after open, then every interval: `keeperBody`.
13. A malformed frame (too short, over 64 KiB, half a PCM sample) ends the connection (`Malformed`); an unknown tag is ignored and counted.
14. Mode names are 1 to 8 lower-case letters or digits (anything else could carry a second command); passband edges within plus or minus 100 kHz: `buildTune`.
15. `Playing` begins at the first audio frame, not at the upgrade, so a busy refusal is never shown as playing.
16. No WebSocket close frame is sent on `close()`: the socket is shut (`shutdown`), which is what makes the bound possible (see UNVERIFIED). The server sees a dropped TCP connection.
17. Only `ws://`. The vendored httplib is built without OpenSSL on Windows, so `wss://` (any receiver behind HTTPS) cannot be reached by this client.
18. A text frame and a binary frame are treated alike by the parser.

## PROTOCOL QUESTIONS

For a later research step to answer in prose. Items 1 to 12 are tied to the assumptions above.

1. What does `freq=` carry: kHz, Hz, or the centre of the passband? Decimals allowed?
2. What are the valid `mod=` names, and what do `low_cut` / `high_cut` mean for each (signed edges relative to the carrier)?
3. What do `in=` and `out=` of `SET AR OK` mean, what values does a server accept, and is the command required before audio starts?
4. What is the audio sample rate of the SND stream? Fixed 12 kHz? Does the server announce it in a MSG?
5. Byte order of 16-bit PCM samples.
6. IMA ADPCM details: nibble order, whether the state resets per frame or per connection, whether a frame carries a state header, the exact rounding of the reference decoder.
7. What do the other flag bits of the SND flags byte mean (0x10 is ADPCM)? Is there a flag for I/Q or stereo or for "squelch/mute"?
8. I/Q mode: the outline does not say that it exists. What is the `mod=` value, what does the SND payload look like (interleaved? rate? header?), and what does the flags byte say? Not built until this is answered.
9. Authentication flow: does the server answer `auth` (with `badp=0`?), may the client send the rest without waiting, and what does an exemption-password flow look like? What are the meanings of `badp` codes 1 to 7 (the client reports the number only)?
10. Format of the MSG text: are values URL-encoded (a redirect target, a name)? Are there other keys a polite client must honour (a per-user time limit, a "kicked" message, an inactivity timeout)? The outline says the client must "respect time-limit replies" but names no key for one.
11. `down`: does it have a value (reason, minutes)? `redirect`: what form is the target (a URL, a host:port, a path)?
12. Keep-alive: is `SET keepalive` the whole command? What is the server's inactivity limit?
13. How does a client find the receiver's real name, owner and contact, and the user count, for the "polite" line in the screen? (From the public directory, not from this protocol: the outline does not cover it.)
14. Is a client allowed to open `/SND` alone, or does a receiver expect a paired `/W/F` or `/EXT` connection first?
15. May the user supply a name other than `FoxSDR`? What are the terms about identification (the facts report: UNVERIFIED; the author's written OK is still the owner's action).

## WIRING HAND-OFF

For the engine lane that wraps this client in a `DeviceSource`-like object.

**Construct, tune, feed, close.**
1. `KiwiSdrClient c; Options o; o.host/o.port/o.password = user input; c.open(o)`. `open()` BLOCKS up to `connectWait` (default 5 s) plus the upgrade read, so call it from the open-worker (`launchDeviceOpen`), never from the engine pass. It returns true when the socket is up, NOT when audio flows; poll `state()` (`Connecting` to `Playing`, or a refusal).
2. `c.tune("usb", 300, 2700, hz)` straight after `open()` and on every retune; it is a network send of about 60 bytes and returns at once; the effect is a server round trip later. Tuning is not a local NCO move: the engine's retune coalescer should absorb bursts (the client does not coalesce).
3. Audio: from the consumer (the source's `read()` or a small pump thread) call `c.read(dst, n, wait)`. Output is mono `int16_t` at `KiwiSdrClient::audioRateHz()` (12000 Hz, assumed). The ring holds 65536 samples (about 5.5 s); `droppedSamples()` counts what a slow consumer lost. One consumer thread only.
4. `c.close()` from any thread; bounded 2.5 s; idempotent; also run by the destructor. If a thread has to be abandoned, `KiwiSdrClient::threadsAbandoned()` rises; the wrapper should then raise the engine's `Recovered::ReaderLeak` notice (as `RtlTcpSource` does) and treat the object as dead.

**Threads and allocation.** Two threads per open client (reader, keep-alive), both owning the shared link by value so an abandoned thread never touches freed memory. The reader allocates once per frame (the `std::string` httplib hands it for each message) and reuses its decode buffers; nothing allocates on the caller's `read()`. No file or disk access. The caller never needs a lock.

**What a `DeviceSource` wrapper must do.**
- It is an AUDIO source, not an I/Q source. The client's output is demodulated mono audio (the receiver demodulates), not complex samples. The pipeline for an I/Q radio (`Pipeline`, VFO, demod) cannot sit on it. The design's "12 kS/s I/Q source" is NOT what this client provides (I/Q mode is Protocol Question 8). The wrapper has to be an audio-only radio: no waterfall, the audio goes to the sink at 12 kHz (the audio-sink resampling to the device rate is the engine's business), and the frequency, mode and passband controls are sent with `tune()`. A radio-pane variant with no spectrum is a different screen object.
- Report faults from `state()` / `status()`: `RefusedBusy` / `RefusedPassword` / `ServerDown` / `Redirected` / `ConnectFailed` / `Dropped` / `Malformed` are all terminal; show `status().detail` and, for a redirect, `status().redirect`; the user presses Open again (a NEW object). Never reconnect automatically and never follow a redirect: that is the politeness contract.
- Do not add a second connection from the same user (no scanning, no "second receiver" in the same radio object).
- A password is never saved in the config file (the design says it is asked again). The address is a normal saved-radio key.
- `rssiDbm()` and `smeterRaw()` give an S-meter for the screen (`haveSmeter()` is false until the first frame).
- 12 kHz question: the rate is assumed (Protocol Question 4). If the real stream is not 12 kHz, change `kAudioRateHz` (it is both reported and sent in `AR OK`).
- Telemetry: `kiwisdr` is a new radio kind word; the Worker must learn it BEFORE the app; never report the server address.

**An existing file that must change (rule 3 forbade it here): `tests/thread_inventory_rows.inc`.** Add, in file order between `mirisdr_source.cpp` and `pluto_source.cpp`:
`{"src/source/kiwisdr_client.cpp", 2, 0, 0, 0, "", R_DRIVER},`
Without it `test_thread_inventory` is red on this branch (verified, and verified green with the row; see TESTS). If the wrapper registers a heartbeat for the reader instead, the row becomes `{..., 2, 0, 1, 1, "<spec>", ""}` style with the covered count its own. No CMake edit is needed: sources and tests are globbed and every test is `headless` by default.

**Known defect to decide on before shipping (vendored httplib).** `third_party/cpp_httplib/include/httplib.h`, `ws::impl::read_websocket_frame`, reads each frame header with `strm.read(buf, 2)` (and 2 / 8 / 4 bytes for the extended length and mask key) and treats a short read as a dead connection. `SocketStream::read` returns whatever a single `recv` gave it, so a TCP segment boundary that falls inside those few bytes drops the connection. On loopback it never happens by itself (the test cuts there on purpose: `testHeaderSplitKnownDefect`); on a real network, with ADPCM frames of a few hundred bytes, boundaries land anywhere and this is a real chance of a drop every so often (UNVERIFIED how often, never measured). The client cannot work around it from its own files. Recommended fix, a patch to the vendored header (not made here, rule 3): replace the four `strm.read(...) != N` header reads with a small loop helper

```cpp
inline bool read_exact(Stream &strm, char *buf, size_t n) {
  size_t got = 0;
  while (got < n) { auto r = strm.read(buf + got, n - got); if (r <= 0) return false; got += r; }
  return true;
}
```

When it is applied `testHeaderSplitKnownDefect` goes red on purpose: flip its expectation to "all samples arrive". The same function also allocates `payload.resize(payload_len)` for a declared length up to 16 MiB BEFORE reading the payload (`CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH`), so a hostile receiver can make this client allocate and zero 16 MiB per frame header; the client's own limit (64 KiB) is applied only after httplib has delivered the message. Lowering the macro is not safe (it is used by inline functions in other translation units), so the same patch should take a smaller limit for the client read path.

## UNVERIFIED

- This client has NEVER connected to a real KiwiSDR. The stand-in server was written from the same outline as the client, so the tests prove self-consistency, not compatibility. Every item under ASSUMPTIONS is a guess a real receiver may contradict.
- The politeness terms of the public receivers (facts report section 3) are unverified, and the KiwiSDR author's written OK has not been asked for (the owner's action).
- The frame-header short-read defect in httplib above: the failure is reproduced on loopback by a deliberate one-byte cut; how often it happens on a real network is not measured.
- `close()` sends no WebSocket close frame (httplib's `close()` reads from the stream the reader thread is reading, so it cannot be called from another thread); a real receiver sees an abrupt TCP shutdown. Whether any receiver minds is unknown.
- A tiny window remains between httplib closing a socket inside a failed `connect()` and this client forgetting the handle (`Link::sockValid`); a `close()` landing in exactly that window would call `shutdown` on a closed descriptor. Not reproduced.
- Not run: any GUI test, any test over a real network, any ASan / TSan build of these files.
- The thread-abandon path (the reader does not return within 2.5 s) is not exercised by a test: the stand-in cannot make the socket shutdown fail.
