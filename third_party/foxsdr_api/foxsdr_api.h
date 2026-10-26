/*
 * foxsdr_api.h - the FoxSDR engine API, in-process C ABI (API 0.3, draft).
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 Steven Fox
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this header file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * ===========================================================================
 * WHAT THIS IS
 * ===========================================================================
 *
 * FoxSDR split in two: an ENGINE (radio, DSP, decoders, plugins, recorder,
 * patch page, transmitter, crash reporting) that runs without a window, and
 * INTERFACES that drive it through this API and nothing else. The standard
 * FoxSDR window is the first interface; others can be downloaded.
 *
 * This header is the in-process form: a table of C function pointers
 * (FoxEngineApi) that an engine module exports and an interface calls. The
 * out-of-process form (docs/TRANSPORTS.md) is a client library that
 * implements THE SAME TABLE over a socket, so an interface plugin never knows
 * or cares which one it was handed. The full specification, with the object
 * model and the coverage table, is docs/API.md; this header must match it.
 *
 * ===========================================================================
 * THE FOUR RULES EVERYTHING BELOW FOLLOWS
 * ===========================================================================
 *
 *  1. READS ARE SNAPSHOTS AND NEVER WAIT. read_state and read_spectrum copy
 *     the newest consistent value the engine published, through a sequence
 *     lock, with no mutex. A read that keeps overlapping a publish answers
 *     FOXAPI_BUSY rather than wait: ask again next frame.
 *
 *  2. CHANGES ARE COMMANDS IN A QUEUE, AND EVERY ANSWER IS A RESULT.
 *     submit() never waits for the engine to judge a command: FOXAPI_OK for
 *     a command means only "the transport has it" (queued, with a ticket).
 *     Every command that was given a ticket produces exactly one
 *     FoxCommandResult through poll_results, and none is ever dropped: a
 *     command counts against the session's FOXAPI_MAX_PENDING (256) until
 *     its result has been READ, so an interface that stops polling is
 *     answered BUSY rather than losing answers - except the commands that
 *     can only make the transmitter safer, which are NEVER answered BUSY:
 *     each of their five kinds has at most one ticket outstanding per
 *     session, and a newer one of the kind is MERGED with it (submit answers
 *     FOXAPI_NO_CHANGE and that ticket; its one result answers both).
 *     Results arrive in ticket order, merged ones included.
 *     FOXAPI_RESULT_REFUSED is set
 *     on a result EXACTLY when its status is not FOXAPI_OK, and then the
 *     command changed NOTHING - whether it was refused at submit (grant,
 *     range, unknown op, no transmitter), refused when applied (another
 *     session holds the key, a latch mark, the re-arm time, consent
 *     withdrawn meanwhile, the session closed or detached before it was
 *     applied), failed, or found things already so (FOXAPI_NO_CHANGE). A
 *     result with FOXAPI_OK carries what LANDED (the clamped value). The state
 *     sequence counters say the same, and a result is never readable before
 *     the snapshot that shows it has been published: read "OK", then
 *     read_state, and the change is there. So an interface on a network submits
 *     its frame's batch and draws on - it never waits a round trip per
 *     click - and a per-frame PTT re-assertion costs nothing.
 *
 *  3. THE DSP THREAD NEVER WAITS ON AN INTERFACE. Nothing an interface does
 *     can block sample processing: queues are bounded (a full one answers
 *     FOXAPI_BUSY), snapshots are published without waiting for readers, and
 *     no engine thread ever calls interface code. There are NO CALLBACKS in
 *     this API; an interface polls (events, results, counters).
 *
 *  4. EVERYTHING GROWS AT THE END. Every struct starts with structSize, set by
 *     whoever allocated it to sizeof() as THEY compiled it. The other side
 *     reads or writes only min(its sizeof, structSize) bytes and, for a
 *     struct it FILLS, writes the number of bytes it filled back into
 *     structSize. Members are only ever appended: never reordered, removed or
 *     retyped. The table itself is versioned the same way, and
 *     FOXAPI_HAS(api, fn) is the only correct test for a function. EVERY
 *     struct has a FOXAPI_MIN_* size, by ONE rule: its whole layout in the
 *     oldest API version this header's engines accept (during 0.x an engine
 *     serves only its own minor, so today: the whole 0.3 layout). A struct
 *     smaller than that cannot come from any build allowed to call, and is
 *     FOXAPI_BAD_ARGUMENT. Nothing ever compares a caller's structSize with
 *     its OWN sizeof, which grows. (The same rules as CascadeHostApi in the
 *     FoxSDR plugin ABI, deliberately.)
 *
 * THREADING. Every function may be called from any thread, except that one
 * FoxSession must not be used from two threads AT ONCE (sessions are cheap:
 * open one per thread that needs one). An interface normally owns one session
 * and uses it from its frame loop.
 *
 * MEMORY. The engine never keeps a pointer the caller passed in; strings and
 * structs are copied before the call returns. Strings are NUL-terminated
 * UTF-8. Every buffer the engine fills is caller-allocated with an explicit
 * capacity.
 *
 * PURE C AT THE BOUNDARY. No C++ exception, longjmp or signal crosses a table
 * function or an interface's run(): an implementation catches everything at
 * every entry and answers FOXAPI_FAILED (or, for a void function, does
 * nothing more). Every function pointer and entry point is FOXAPI_CALL, which
 * pins the calling convention on 32-bit Windows (cdecl) where compilers could
 * otherwise disagree; elsewhere it is empty.
 */
#ifndef FOXSDR_API_H
#define FOXSDR_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* API version. MAJOR changes only when an existing member changes meaning
 * (never, if the rules above hold); MINOR advances when members are appended.
 * 0.x is a DRAFT: until 1.0 the owner may still reshape it, so a 0.x engine
 * and a 0.x interface talk only when their minors are EQUAL (see
 * foxsdr_engine_query). 0.2 changed what submit() answers (rule 2), which is
 * exactly why a 0.1 interface must not be handed a 0.2 engine. */
#define FOXAPI_VERSION_MAJOR 0u
#define FOXAPI_VERSION_MINOR 3u

#if defined(_WIN32)
#define FOXAPI_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define FOXAPI_EXPORT __attribute__((visibility("default")))
#else
#define FOXAPI_EXPORT
#endif

/* The calling convention of every table function and entry point. */
#if defined(_WIN32) && !defined(_WIN64)
#define FOXAPI_CALL __cdecl
#else
#define FOXAPI_CALL
#endif

/* ---------------------------------------------------------------------------
 * Answers. The first eleven have the SAME values as CASCADE_API_* in the
 * FoxSDR plugin ABI, so code that already understands one understands both.
 * ------------------------------------------------------------------------- */
#define FOXAPI_OK 0
#define FOXAPI_NO_CHANGE 1              /* a read found nothing newer; a command found it already so */
#define FOXAPI_DENIED (-1)              /* the session lacks the grant */
#define FOXAPI_OUT_OF_RANGE (-2)        /* a value outside what the engine accepts */
#define FOXAPI_NO_DEVICE (-3)           /* needs an open radio / transmitter */
#define FOXAPI_FAILED (-4)              /* the engine could not do it */
#define FOXAPI_BAD_ARGUMENT (-5)        /* NULL, NaN, a short struct, a malformed string */
#define FOXAPI_UNSUPPORTED (-6)         /* this engine (or radio) has no such thing */
#define FOXAPI_BUSY (-7)                /* a bounded queue is full, or a read kept overlapping */
#define FOXAPI_DETACHED (-8)            /* the session or engine has been closed */
#define FOXAPI_NOT_FOUND (-9)           /* no such id, key or list entry */
#define FOXAPI_LIMIT (-10)              /* a per-session quota is full */
#define FOXAPI_WRONG_THREAD (-11)       /* not allowed on this thread */
#define FOXAPI_UNAUTHENTICATED (-12)    /* remote: no valid login token */
#define FOXAPI_VERSION_MISMATCH (-13)   /* the two sides cannot talk */

/* ---------------------------------------------------------------------------
 * Capability bits: what an ENGINE implements (FoxEngineApi::capabilities).
 * Whether a thing is available RIGHT NOW (is a transmitter open?) is state,
 * and lives in FoxReceiverState::flags instead. An interface greys out what
 * the engine lacks rather than failing on it.
 * ------------------------------------------------------------------------- */
#define FOXAPI_CAP_RECEIVER      (1ull << 0)  /* run/stop, tune, mode, bandwidth, squelch, volume */
#define FOXAPI_CAP_SPECTRUM      (1ull << 1)  /* read_spectrum (the waterfall is its history) */
#define FOXAPI_CAP_AUDIO_LEVELS  (1ull << 2)  /* audioLevelDb, underruns, sink name in the state */
#define FOXAPI_CAP_AUDIO_STREAM  (1ull << 3)  /* read_audio */
#define FOXAPI_CAP_IQ_STREAM     (1ull << 4)  /* read_iq */
#define FOXAPI_CAP_SOURCES       (1ull << 5)  /* device list, select, rate, antenna */
#define FOXAPI_CAP_GAINS         (1ull << 6)  /* gain stages, device AGC */
#define FOXAPI_CAP_AUDIO_DSP     (1ull << 7)  /* de-emphasis, stereo, NR, notch, auto-notch */
#define FOXAPI_CAP_RDS           (1ull << 8)  /* RDS fields in the state */
#define FOXAPI_CAP_DECODERS      (1ull << 9)  /* decoder list, start/stop, text + image events */
#define FOXAPI_CAP_PATCH         (1ull << 10) /* the patch page graph */
#define FOXAPI_CAP_RECORDER      (1ull << 11) /* I/Q and audio recording */
#define FOXAPI_CAP_TRANSMIT      (1ull << 12) /* the transmitter and its key */
#define FOXAPI_CAP_BOOKMARKS     (1ull << 13) /* bookmarks */
/*      (1ull << 14) retired in 0.2: imported frequency lists ARE bookmarks with a group, as in the app */
#define FOXAPI_CAP_SCANNER       (1ull << 15) /* the scanner */
#define FOXAPI_CAP_SETTINGS      (1ull << 16) /* get_setting / SETTING_SET */
#define FOXAPI_CAP_PLUGIN_STORE  (1ull << 17) /* catalogue fetch / install / remove */
#define FOXAPI_CAP_REPORTS       (1ull << 18) /* problem reports, feature requests, crash uploads */
#define FOXAPI_CAP_TELEMETRY     (1ull << 19) /* the usage-report switch */
#define FOXAPI_CAP_EVENTS        (1ull << 20) /* subscribe / poll_events */
#define FOXAPI_CAP_BAND_PLAN     (1ull << 21) /* band-plan list and selection */
#define FOXAPI_CAP_TRACKS        (1ull << 22) /* map tracks from track-source plugins */
#define FOXAPI_CAP_SCOPE         (1ull << 23) /* demod-scope taps (read_scope; a REMOTE session is held to FOXAPI_SCOPE_REMOTE_FRAMES a read) */
#define FOXAPI_CAP_POSITION      (1ull << 24) /* receiver position, GPS port */
#define FOXAPI_CAP_IMAGES        (1ull << 25) /* read_image: decoded pictures */
/* 0.3 */
#define FOXAPI_CAP_FACTS         (1ull << 26) /* read-only "fact.*" keys through get_setting */
#define FOXAPI_CAP_LONG_TEXT     (1ull << 27) /* submit_text: one command with a text longer than text[] */
#define FOXAPI_CAP_NODE_SPECTRUM (1ull << 28) /* read_node_spectrum: a patch radio's own spectrum */

/* ---------------------------------------------------------------------------
 * Session grants. Asked for in FoxSessionParams::grants; the engine grants
 * the subset policy allows and reports it in FoxReceiverState::grants.
 * ------------------------------------------------------------------------- */
#define FOXAPI_GRANT_VIEW      (1ull << 0) /* read state, spectrum, lists (always granted) */
#define FOXAPI_GRANT_TUNE      (1ull << 1) /* frequency, VFO offset, bookmarks-tune, scanner */
#define FOXAPI_GRANT_SETTINGS  (1ull << 2) /* mode, bandwidth, squelch, volume, source, gains, run */
#define FOXAPI_GRANT_TRANSMIT  (1ull << 3) /* the transmitter and its key */
#define FOXAPI_GRANT_ADMIN     (1ull << 4) /* plugin store, servers, reports, telemetry, settings */
#define FOXAPI_GRANT_AUDIO     (1ull << 5) /* read_audio */
#define FOXAPI_GRANT_IQ        (1ull << 6) /* read_iq */

/* FoxSessionParams::flags */
#define FOXAPI_SESSION_LOCAL   0x00000001u /* in-process, on this machine: may LATCH the key */
#define FOXAPI_SESSION_REMOTE  0x00000002u /* over a transport: can never latch */

/* ---------------------------------------------------------------------------
 * Demodulators. The SAME values as CASCADE_DEMOD_* in the plugin ABI.
 * ------------------------------------------------------------------------- */
#define FOXAPI_DEMOD_NFM 1u
#define FOXAPI_DEMOD_WFM 2u
#define FOXAPI_DEMOD_AM  3u
#define FOXAPI_DEMOD_DSB 4u
#define FOXAPI_DEMOD_USB 5u
#define FOXAPI_DEMOD_CW  6u
#define FOXAPI_DEMOD_LSB 7u
#define FOXAPI_DEMOD_RAW 8u

/* ---------------------------------------------------------------------------
 * The receiver's state: everything an interface draws every frame, as ONE
 * consistent copy.
 *
 * CHANGE NOTIFICATION IS A COUNTER, NOT A CALLBACK. `seq` advances whenever
 * anything except the measurements (signalDb, sMeter, audioLevelDb, the
 * underrun and hold counters) changes; the group counters say WHICH. They
 * never go backwards within an engine session and start above zero, so 0
 * means "never read".
 * ------------------------------------------------------------------------- */
#define FOXAPI_RX_RUNNING          0x00000001u /* the receiver is streaming */
#define FOXAPI_RX_DEVICE_OPEN      0x00000002u /* a radio (not the built-in generator) */
#define FOXAPI_RX_FAULTED          0x00000004u /* a worker died; faultMessage says why */
#define FOXAPI_RX_MUTED            0x00000008u /* the user's mute */
#define FOXAPI_RX_SQUELCH_OPEN     0x00000010u
#define FOXAPI_RX_STEREO_ENABLED   0x00000020u
#define FOXAPI_RX_STEREO_ACTIVE    0x00000040u /* WFM, enabled, pilot locked: the ST lamp */
#define FOXAPI_RX_NR               0x00000080u
#define FOXAPI_RX_NOTCH            0x00000100u
#define FOXAPI_RX_AUTO_NOTCH       0x00000200u
#define FOXAPI_RX_DEVICE_AGC       0x00000400u
#define FOXAPI_RX_AGC_SUPPORTED    0x00000800u
#define FOXAPI_RX_RECORDING_IQ     0x00001000u
#define FOXAPI_RX_RECORDING_AUDIO  0x00002000u
#define FOXAPI_RX_SCANNER_ACTIVE   0x00004000u
#define FOXAPI_RX_DECODER_ACTIVE   0x00008000u /* the DEC lamp: a decoder is being fed */
#define FOXAPI_RX_TX_AVAILABLE     0x00010000u /* a transmitter is open */
#define FOXAPI_RX_TX_KEYED         0x00020000u /* RF is going out NOW */
#define FOXAPI_RX_TX_LATCHED       0x00040000u
#define FOXAPI_RX_TX_KEY_MINE      0x00080000u /* ...and THIS session holds the key */
#define FOXAPI_RX_SINK_OPEN        0x00100000u /* the engine's audio output is open */
#define FOXAPI_RX_WEB_LISTENING    0x00200000u /* a network transport is accepting */
#define FOXAPI_RX_TX_REMOTE_ARMED  0x00400000u /* a LOCAL session has consented to remote PTT (TX_REMOTE_ARM) */
/* THIS session must send TX_LATCH 0 before its TX_LATCH 1 can be taken: its
 * principal's last latch ended by a marking end and this session has not
 * released since (see THE KEY). Per session, like TX_KEY_MINE; it may trail
 * the rest of the snapshot by one control pass. Show it on the latch key. */
#define FOXAPI_RX_TX_LATCH_RELEASE_FIRST 0x00800000u
/* 0.3 */
#define FOXAPI_RX_BIAS_TEE_PRESENT 0x01000000u /* the open radio has a bias tee this engine can switch */
#define FOXAPI_RX_BIAS_TEE_ON      0x02000000u /* ...and it is on (the radio's readback) */
#define FOXAPI_RX_PATCH_RUNNING    0x04000000u /* the patch page's graph is running */
#define FOXAPI_RX_DEVICE_OPENING   0x08000000u /* a radio is being opened: the open runs on a worker and has not finished */
#define FOXAPI_RX_AUTO_NOTCH_ENGAGED 0x10000000u /* the auto-notch has found a tone and is notching it (FOXAPI_RX_AUTO_NOTCH is "enabled"; autoNotchHz is where) */
#define FOXAPI_RX_PILOT_LOCKED     0x20000000u /* WFM: the stereo decoder has locked the 19 kHz pilot, enabled or not (STEREO_ACTIVE is enabled AND locked) */
#define FOXAPI_RX_POSITION_SET     0x40000000u /* the receiver's position is known (rxLatDeg and rxLonDeg are valid; 8.11) */

#define FOXAPI_NAME_CHARS 64
#define FOXAPI_MESSAGE_CHARS 128
#define FOXAPI_RDS_PS_CHARS 9      /* 8 characters and a NUL */
#define FOXAPI_RDS_TEXT_CHARS 65   /* 64 characters and a NUL */

typedef struct FoxReceiverState {
    uint32_t structSize;
    uint32_t flags;             /* FOXAPI_RX_* */
    uint64_t seq;               /* anything below except the measurements */
    uint64_t tuneSeq;           /* centreHz, vfoOffsetHz */
    uint64_t modeSeq;           /* demodMode, bandwidthHz, squelchDb, audio DSP */
    uint64_t deviceSeq;         /* the source, its rate, gains, AGC, running */
    uint64_t audioSeq;          /* volume, mute, sink */
    uint64_t displaySeq;        /* dbMin, dbMax */
    uint64_t txSeq;             /* the transmitter and its key */
    uint64_t listSeq;           /* any read_list list changed */
    uint64_t grants;            /* FOXAPI_GRANT_* this session holds */
    double centreHz;            /* the radio's centre frequency (DC) */
    double vfoOffsetHz;         /* the tuned channel's offset from centre */
    double tunedHz;             /* centreHz + vfoOffsetHz: what is heard */
    double sampleRateHz;        /* the active source's rate */
    double channelRateHz;       /* the VFO's output rate */
    double bandwidthHz;         /* the tuned channel's width */
    double squelchDb;           /* threshold on signalDb's scale; -200 = always open */
    double volume;              /* 0..1 */
    double signalDb;            /* channel power, dB full scale - measured */
    double sMeter;              /* 0..1 over [-120, 0] dB, the bar the user sees */
    double audioLevelDb;        /* finished audio level, dBFS - the VOLUME meter */
    double dbMin;               /* display range, spectrum axis and waterfall palette */
    double dbMax;
    double nrStrength;          /* 0..1 */
    double notchHz;
    double notchQ;
    double pilotLevel;          /* WFM stereo pilot, 0..1 */
    uint32_t demodMode;         /* FOXAPI_DEMOD_* */
    uint32_t deemphasis;        /* 0 = 50 us, 1 = 75 us, 2 = off */
    uint32_t gainCount;         /* entries in FOXAPI_LIST_GAINS */
    uint32_t decodersRunning;
    uint32_t decodersFitted;
    uint32_t txMode;            /* FOXAPI_TX_MODE_* */
    uint64_t audioUnderruns;    /* sink callbacks that starved, since start */
    double txFrequencyHz;
    double txPowerDb;           /* the board's own negative attenuation */
    int64_t txHoldRemainingMs;  /* remote-style hold left on the key; 0 = none */
    int64_t txLatchRemainingMs; /* latch left before it opens itself; 0 = none */
    char deviceName[FOXAPI_NAME_CHARS];   /* "Signal generator" with no radio */
    char sinkName[FOXAPI_NAME_CHARS];     /* the audio output device */
    char faultMessage[FOXAPI_MESSAGE_CHARS];
    char txUnkeyReason[FOXAPI_MESSAGE_CHARS]; /* why the key last opened ON ITS OWN */
    /* ---- 0.3 (docs/api-0.3-additions.md 8.1): the transmitter as a page
     * draws it. All move txSeq but txInputPeak, a measurement. ---- */
    uint32_t txFlags;           /* FOXAPI_TX_* */
    uint32_t txInput;           /* TX_SET_INPUT's value: 0 microphone, 1 tone */
    double txSplitHz;           /* where a SPLIT transmitter goes (TX_SET_SPLIT, TX_SET_FREQUENCY) */
    double txToneHz;            /* the test tone (TX_SET_TONE) */
    double txPowerMinDb;        /* the board's range for TX_SET_POWER, valid with FOXAPI_TX_POWER_RANGE */
    double txPowerMaxDb;
    double txInputPeak;         /* the input's peak since the last publish, 0..1 - measured */
    char txDeviceName[FOXAPI_NAME_CHARS];     /* the open transmitter; "" with none */
    char txError[FOXAPI_MESSAGE_CHARS];       /* the transmitter's last error; "" none */
    /* ---- 0.3 (docs/api-0.3-additions.md 8.2): the status column as a page
     * draws it. audioFlags, audioRateHz and the two texts move audioSeq;
     * decodersMatched moves seq; the counters after audioRateHz are
     * MEASUREMENTS and move nothing. ---- */
    uint32_t audioFlags;               /* FOXAPI_AUDIO_* */
    uint32_t decodersMatched;          /* decoder modules built and matched to the receiver's rate, running or not */
    double audioRateHz;                /* the finished audio's rate (what the ring's frames are counted at) */
    uint64_t audioRingFrames;          /* frames waiting in the output ring - measured */
    uint64_t audioRingCapacityFrames;  /* the ring's size in frames - measured */
    uint64_t audioPluginGaps;          /* blocks the playing plugin came up short in, since the plugins were last rebuilt - measured */
    uint64_t audioPluginGapFrames;     /* ...and the frames of silence that cost - measured */
    uint64_t decoderLinesTotal;        /* every decoder output line since the engine started - measured */
    char audioMutedBy[FOXAPI_MESSAGE_CHARS];       /* the plugins muting the audio, "A and B"; "" when none is */
    char audioPlayingPlugin[FOXAPI_MESSAGE_CHARS]; /* the plugin holding the speakers; "" when the receiver's own audio plays */
    /* ---- 0.3 (docs/api-0.3-additions.md 8.4): the receiver rail as its page
     * draws it, the RDS decoder and the auto-notch's readout. rdsFlags, rdsPi,
     * rdsPty, rdsPs and rdsRadioText move rdsSeq (and so seq); rdsGroups,
     * rdsErrors and autoNotchHz are MEASUREMENTS and move nothing. ---- */
    uint64_t rdsSeq;                   /* any of rdsFlags, rdsPi, rdsPty, rdsPs, rdsRadioText changed */
    uint64_t rdsGroups;                /* RDS groups decoded since the decoder was last reset (it is, on every retune) - measured */
    uint64_t rdsErrors;                /* blocks that failed their checkword while synchronised, since then - measured */
    double autoNotchHz;                /* where the auto-notch sits; 0 when it is not engaged - measured */
    uint32_t rdsFlags;                 /* FOXAPI_RDS_* */
    uint32_t rdsPi;                    /* programme identification, valid with FOXAPI_RDS_PI_VALID */
    uint32_t rdsPty;                   /* programme type, the raw 5-bit code (RDS and RBDS name it differently) */
    char rdsPs[FOXAPI_RDS_PS_CHARS];           /* programme service name: 8 characters as transmitted, space padded; "" until all four fragments are in */
    char rdsRadioText[FOXAPI_RDS_TEXT_CHARS];  /* RadioText, up to 64 characters; "" until some has arrived */
    /* ---- 0.3 (docs/api-0.3-additions.md 8.6): the scanner as its section draws it.
     * The seven settings (the range and the four timings) move scanSeq, and seq with it;
     * scannerState and scanCurrentHz are live state and move seq alone. The timings are
     * whole milliseconds: rounded to the nearest, 0 for a negative or NaN, saturating
     * at 4294967295. ---- */
    uint64_t scanSeq;                  /* any of scanStartHz, scanStopHz, scanStepHz, scanDwellMs, scanHoldMs, scanResumeMs, scanListenMs changed */
    double scanCurrentHz;              /* the scanner's frequency: where it is, or where the last scan stopped */
    double scanStartHz;                /* the range a Start would scan, as stored */
    double scanStopHz;
    double scanStepHz;
    uint32_t scannerState;             /* FOXAPI_SCANNER_* */
    uint32_t scanDwellMs;              /* time on each step with the squelch closed */
    uint32_t scanHoldMs;               /* how long a quiet signal is waited on before moving on */
    uint32_t scanResumeMs;             /* the grace after the hold before the next step */
    uint32_t scanListenMs;             /* the longest stay on one signal; 0 = until it goes quiet */
    /* ---- 0.3 (docs/api-0.3-additions.md 8.10): the Source section as it is
     * drawn. One flag word, in the device group (deviceSeq, and seq with it). ---- */
    uint32_t sourceFlags;              /* FOXAPI_SRC_* */
    /* ---- 0.3 (docs/api-0.3-additions.md 8.11): where the receiver is, for the
     * radar scope. Valid with FOXAPI_RX_POSITION_SET; 0 otherwise. They move
     * seq and nothing else (the position is set by FOXAPI_OP_SET_POSITION or
     * restored from the saved configuration). ---- */
    double rxLatDeg;                   /* the receiver's latitude, degrees north */
    double rxLonDeg;                   /* the receiver's longitude, degrees east */
} FoxReceiverState;


/* FoxReceiverState::txFlags (0.3) */
#define FOXAPI_TX_SPLIT         0x00000001u /* TX_SET_SPLIT on: not following the receiver */
#define FOXAPI_TX_MONITOR       0x00000002u /* TX_SET_MONITOR on: the receiver is heard while transmitting */
#define FOXAPI_TX_FAULTED       0x00000004u /* the open transmitter has faulted */
#define FOXAPI_TX_INPUT_OPENING 0x00000008u /* the microphone chosen is still opening */
#define FOXAPI_TX_POWER_RANGE   0x00000010u /* the board published a power range (txPowerMin/MaxDb) */

/* FoxReceiverState::audioFlags (0.3) */
#define FOXAPI_AUDIO_OPENING        0x00000001u /* a worker is inside the output device's open */
#define FOXAPI_AUDIO_EVER_OPENED    0x00000002u /* an open of the output device has succeeded since the engine started */
#define FOXAPI_AUDIO_STREAM_ALIVE   0x00000004u /* the watchdog's view: the stream's callback is still being served (clear while OPENING) */
#define FOXAPI_AUDIO_PLUGIN_PLAYING 0x00000008u /* a plugin holding CASCADE_CAP_AUDIO_OUT replaces the demodulated audio (audioPlayingPlugin names it) */

/* FoxReceiverState::sourceFlags (0.3, 8.10) */
#define FOXAPI_SRC_SCAN_PENDING   0x00000001u /* a SoapySDR device scan is running on its worker */
#define FOXAPI_SRC_SCANNED        0x00000002u /* a SoapySDR scan has been started this session (a deferred one does not count) */
#define FOXAPI_SRC_SCAN_PARTIAL   0x00000004u /* the last scan left the open radios' drivers out; a whole one is still owed */
#define FOXAPI_SRC_SCAN_GATED     0x00000008u /* a radio is open, opening or held by the patch, so a SoapySDR scan is deferred or leaves that radio's driver out (fact.source.scanGate and .scanSkip say which) */
#define FOXAPI_SRC_NETWORK_USRPS  0x00000010u /* "Look for network USRPs" is on */
#define FOXAPI_SRC_UHD_LEFT_OUT   0x00000020u /* the last scan that landed left UHD out (nothing could be a USRP) */
#define FOXAPI_SRC_SDRPLAY_ROWS   0x00000040u /* the last native scan found an SDRplay API row */
#define FOXAPI_SRC_CARD_LISTED    0x00000080u /* the sound card inputs have been listed (SOUND_CARDS is that list, which may be empty) */
#define FOXAPI_SRC_CARD_SCANNING  0x00000100u /* the sound card inputs are being listed on a worker */
#define FOXAPI_SRC_CARD_OPENING   0x00000200u /* a sound card is being opened on a worker */

/* FoxReceiverState::scannerState (0.3, 8.6) */
#define FOXAPI_SCANNER_IDLE     0u /* not scanning (scanCurrentHz is where the last scan stopped) */
#define FOXAPI_SCANNER_SCANNING 1u /* stepping along the range, one point each dwell */
#define FOXAPI_SCANNER_PAUSED   2u /* a signal was found: held on it, listening */
#define FOXAPI_SCANNER_HOLDING  3u /* the signal went quiet: waiting for it to come back before moving on */

/* FoxReceiverState::rdsFlags (0.3) */
#define FOXAPI_RDS_SYNCED   0x00000001u /* the block decoder has found the 1187.5 bit/s stream: listening, with no field complete yet */
#define FOXAPI_RDS_PI_VALID 0x00000002u /* rdsPi holds a programme identification that passed its checkword */
#define FOXAPI_RDS_PS_VALID 0x00000004u /* all four fragments of the programme service name have arrived (rdsPs) */
#define FOXAPI_RDS_TP       0x00000008u /* the station carries traffic information at all */
#define FOXAPI_RDS_TA       0x00000010u /* a traffic announcement is on the air now */
#define FOXAPI_RDS_RT_VALID 0x00000020u /* rdsRadioText holds some text */

/* Every FOXAPI_MIN_* is the whole layout of the oldest accepted version
 * (rule 4), written as the end of its last member so it never moves when
 * members are appended. */
#define FOXAPI_MIN_RECEIVER_STATE (offsetof(FoxReceiverState, rxLonDeg) + sizeof(double))

/* ---------------------------------------------------------------------------
 * The spectrum stream. The waterfall is NOT a separate stream: it is the
 * history of this one, kept by the interface (docs/API.md, "Streams").
 * ------------------------------------------------------------------------- */
typedef struct FoxSpectrumInfo {
    uint32_t structSize;
    uint32_t binCount;          /* bins the ENGINE has; bins copied = min(cap, binCount) */
    uint64_t seq;               /* strictly increasing per published frame */
    double centreHz;            /* RF at the middle bin */
    double spanHz;              /* width covered by all bins */
    int64_t unixMs;             /* engine clock when the frame was published */
    uint32_t copied;            /* bins written to the caller's buffer */
    uint32_t flags;             /* reserved, 0 */
} FoxSpectrumInfo;

#define FOXAPI_MIN_SPECTRUM_INFO (offsetof(FoxSpectrumInfo, flags) + sizeof(uint32_t))

/* ---------------------------------------------------------------------------
 * Commands. One struct for every operation: `op` says which, and each op
 * documents the slots it reads (docs/API.md, "Command reference"). Unused
 * slots must be zero. In an ARRAY passed to submit(), every element has the
 * same structSize and the engine steps by structSize, not by its own sizeof -
 * that is what lets an older interface pass an older, shorter command.
 * ------------------------------------------------------------------------- */
#define FOXAPI_TEXT_CHARS 256

typedef struct FoxCommand {
    uint32_t structSize;
    uint32_t op;                /* FOXAPI_OP_* */
    uint32_t target;            /* object id where an op has several (VFO, decoder); 0 = default */
    uint32_t flags;             /* reserved, 0 */
    double num[4];
    int64_t ival[2];
    char text[FOXAPI_TEXT_CHARS];
} FoxCommand;

#define FOXAPI_MIN_COMMAND (offsetof(FoxCommand, text) + FOXAPI_TEXT_CHARS)

/* The most commands one submit() may carry; more is FOXAPI_LIMIT for the
 * whole call (a transport's message cap in the same units). */
#define FOXAPI_MAX_BATCH 1024u

/* The most ORDINARY commands one session may have OUTSTANDING: queued, being
 * applied, or answered but not yet read by poll_results. Past it submit
 * answers FOXAPI_BUSY for the command (never for the safer commands below).
 * Because a command counts until its result has been POLLED, a session can
 * never have more unread results than FOXAPI_MAX_PENDING +
 * FOXAPI_SAFETY_RESERVE, and a result is never dropped: every ticket gets its
 * result. */
#define FOXAPI_MAX_PENDING 256u

/* The commands that can only make the transmitter SAFER - TX_PTT 0,
 * TX_LATCH 0, RUN 0, TX_CLOSE and TX_REMOTE_ARM 0, five KINDS - are never
 * answered BUSY and never pile up: a session has at most ONE ticket of each
 * kind outstanding (queued, or answered but not yet read). A safer command
 * of a kind that already has one is MERGED with it: submit answers
 * FOXAPI_NO_CHANGE with THAT ticket (no new ticket; not counted in submit's
 * return), and the ticket's one result reports the latest application. (It
 * may report the one before, when the result was read while a merged
 * application was still queued.) Merging touches only tickets and results,
 * never WHERE a command takes effect: every send is applied in its own place
 * in the queue, exactly as if it had not been merged - except a send that
 * cannot change anything, which the engine may drop (the same session's same
 * kind is already queued with nothing but safer commands after it). So a
 * command sent once or N times, merged or not, leaves the same state. A
 * command is never merged into a ticket whose result poll_results has
 * already returned: that send takes a NEW ticket. A safer command REFUSED at
 * submit (malformed, no grant, a remote session's LATCH 0) changes nothing,
 * so it is not one of these: it is never merged, takes its own ticket and
 * refusal as an ordinary command, and counts toward FOXAPI_MAX_PENDING (so
 * it can be BUSY). So an interface that stopped polling can always open the
 * key, and one sending PTT 0 every frame uses one ticket, not one a frame.
 * FOXAPI_SAFETY_RESERVE is how many kinds there are: at most that many
 * unread results beyond FOXAPI_MAX_PENDING. */
#define FOXAPI_SAFETY_RESERVE 5u

/* Receiver (grant: TUNE for tuning ops, SETTINGS for the rest) */
#define FOXAPI_OP_RUN               0x0101u /* ival[0]: 1 start, 0 stop */
#define FOXAPI_OP_SET_CENTRE        0x0102u /* num[0]: centre Hz (the radio moves) */
#define FOXAPI_OP_SET_FREQUENCY     0x0103u /* num[0]: tuned Hz; VFO kept, centre follows */
#define FOXAPI_OP_SET_VFO_OFFSET    0x0104u /* num[0]: offset Hz, clamped into the band */
#define FOXAPI_OP_STEP_TUNE         0x0105u /* ival[0]: signed steps; num[0]: step Hz */
#define FOXAPI_OP_SET_MODE          0x0106u /* ival[0]: FOXAPI_DEMOD_*; bandwidth goes to the mode default */
#define FOXAPI_OP_SET_BANDWIDTH     0x0107u /* num[0]: Hz, clamped to the channel */
#define FOXAPI_OP_SET_SQUELCH       0x0108u /* num[0]: dB, -200..20 */
#define FOXAPI_OP_SET_VOLUME        0x0109u /* num[0]: 0..1 */
#define FOXAPI_OP_SET_MUTED         0x010Au /* ival[0]: 0/1 */
#define FOXAPI_OP_SET_DEEMPHASIS    0x010Bu /* ival[0]: 0 = 50 us, 1 = 75 us, 2 = off */
#define FOXAPI_OP_SET_STEREO        0x010Cu /* ival[0]: 0/1 */
#define FOXAPI_OP_SET_NR            0x010Du /* ival[0]: 0/1; num[0]: strength 0..1 (ival[1]=1 to set it) */
#define FOXAPI_OP_SET_NOTCH         0x010Eu /* ival[0]: 0/1; num[0]: Hz; num[1]: Q (ival[1]=1 to set them) */
#define FOXAPI_OP_SET_AUTO_NOTCH    0x010Fu /* ival[0]: 0/1 */
/* Display */
#define FOXAPI_OP_SET_DISPLAY_RANGE 0x0201u /* num[0]: dbMin, num[1]: dbMax (min span enforced) */
#define FOXAPI_OP_SET_BAND_PLAN     0x0202u /* text: selection name */
/* Source (grant: SETTINGS) */
#define FOXAPI_OP_SCAN_DEVICES      0x0301u /* result arrives as FOXAPI_LIST_DEVICES changing */
#define FOXAPI_OP_SELECT_SOURCE     0x0302u /* text: a device id from FOXAPI_LIST_DEVICES, verbatim; "file:" and "pluto:" ids LOCAL sessions only */
#define FOXAPI_OP_SET_SAMPLE_RATE   0x0303u /* num[0]: Hz */
#define FOXAPI_OP_SET_GAIN          0x0304u /* text: stage name as listed; num[0]: dB */
#define FOXAPI_OP_SET_DEVICE_AGC    0x0305u /* ival[0]: 0/1 */
#define FOXAPI_OP_SET_ANTENNA       0x0306u /* text */
#define FOXAPI_OP_SET_BIAS_TEE      0x0307u /* ival[0]: 0/1; text (0.3, optional): the radio ("fact.biasTee.radioKey") the sender asked about - FOXAPI_NOT_FOUND if another is open now */
#define FOXAPI_OP_SET_DEVICE_OPTION 0x0308u /* text: option name from FOXAPI_LIST_DEVICE_OPTIONS; ival[0] / num[0]: value */
/* Recorder (grant: SETTINGS; the directory is NOT settable remotely) */
#define FOXAPI_OP_RECORD_IQ         0x0401u /* ival[0]: 0/1 */
#define FOXAPI_OP_RECORD_AUDIO      0x0402u /* ival[0]: 0/1 */
/* Bookmarks and frequency lists (grant: TUNE to tune, SETTINGS to edit) */
#define FOXAPI_OP_BOOKMARK_ADD      0x0501u /* text: name; the CURRENT frequency, mode and bandwidth, ungrouped */
#define FOXAPI_OP_BOOKMARK_TUNE     0x0502u /* ival[0]: bookmark id; its frequency, mode and bandwidth */
#define FOXAPI_OP_BOOKMARK_REMOVE   0x0503u /* ival[0]: bookmark id */
/*      0x0504u retired in 0.2 (FREQ_LIST_TUNE): an imported list is a bookmark group */
#define FOXAPI_OP_BOOKMARK_FAVOURITE 0x0505u /* ival[0]: bookmark id; ival[1]: 0/1 */
#define FOXAPI_OP_BOOKMARK_IMPORT   0x0506u /* text: file CONTENT (SDR# XML / CSV), never a path */
#define FOXAPI_OP_BOOKMARK_REMOVE_GROUP 0x0507u /* text: group name; every bookmark in it */
/* Scanner (grant: TUNE) */
#define FOXAPI_OP_SCANNER_RUN       0x0601u /* ival[0]: 0/1; num[0..2]: start, stop, step Hz */
#define FOXAPI_OP_SCANNER_SKIP      0x0602u
#define FOXAPI_OP_SCANNER_CONFIG    0x0603u /* num[0..3]: dwell, hold, resume, listen ms */
/* Decoders and plugins (grant: SETTINGS; store ops ADMIN) */
#define FOXAPI_OP_DECODER_START     0x0701u /* text: plugin name */
#define FOXAPI_OP_DECODER_STOP      0x0702u /* text: plugin name */
#define FOXAPI_OP_DECODER_STOP_ALL  0x0703u
#define FOXAPI_OP_PLUGIN_PRESET     0x0704u /* text: plugin name; ival[0]: preset index */
#define FOXAPI_OP_PLUGIN_GRANT      0x0705u /* text: plugin name; ival[0]: 1 tune, 2 settings; ival[1]: 0/1 */
#define FOXAPI_OP_PLUGIN_COMMAND    0x0706u /* text: plugin name; ival[0]: command id (a plate key) */
#define FOXAPI_OP_PLUGIN_MUTE       0x0707u /* text: plugin name; ival[0]: mute speakers while it runs 0/1 */
#define FOXAPI_OP_PLUGIN_RESCAN     0x0708u
#define FOXAPI_OP_USER_PRESET_SAVE  0x0709u /* text: plugin name; the CURRENT frequency and mode */
#define FOXAPI_OP_USER_PRESET_FORGET 0x070Au /* ival[0]: user preset id */
#define FOXAPI_OP_STORE_FETCH       0x0801u
#define FOXAPI_OP_STORE_INSTALL     0x0802u /* text: catalogue id; ival[0]: 1 = legal notice acknowledged */
#define FOXAPI_OP_STORE_REMOVE      0x0803u /* text: installed file name */
#define FOXAPI_OP_STORE_CANCEL      0x0804u
#define FOXAPI_OP_STORE_UPDATE_ALL  0x0805u /* ival[0]: 1 = legal notices acknowledged */
/* Patch page (grant: SETTINGS) */
#define FOXAPI_OP_PATCH_LOAD        0x0901u /* text: a patch document (see docs/API.md) */
#define FOXAPI_OP_PATCH_RUN         0x0902u /* ival[0]: 0/1 */
#define FOXAPI_OP_PATCH_ALL_OFF     0x0903u
/* Transmitter (grant: TRANSMIT) - see "THE KEY" below */
#define FOXAPI_OP_TX_OPEN           0x0A01u /* text: transmitter device id */
#define FOXAPI_OP_TX_CLOSE          0x0A02u
#define FOXAPI_OP_TX_PTT            0x0A03u /* ival[0]: 1 = assert / extend the hold, 0 = release now */
#define FOXAPI_OP_TX_LATCH          0x0A04u /* ival[0]: 0/1; LOCAL sessions only */
#define FOXAPI_OP_TX_SET_MODE       0x0A05u /* ival[0]: FOXAPI_TX_MODE_* */
#define FOXAPI_OP_TX_SET_FREQUENCY  0x0A06u /* num[0]: Hz */
#define FOXAPI_OP_TX_SET_POWER      0x0A07u /* num[0]: attenuation dB (<= 0) */
#define FOXAPI_OP_TX_SET_INPUT      0x0A08u /* ival[0]: 0 microphone, 1 tone */
#define FOXAPI_OP_TX_SET_SPLIT      0x0A09u /* ival[0]: 0/1; num[0]: split Hz */
#define FOXAPI_OP_TX_SET_TONE       0x0A0Au /* num[0]: Hz */
#define FOXAPI_OP_TX_SET_MONITOR    0x0A0Bu /* ival[0]: 0/1 */
#define FOXAPI_OP_TX_REMOTE_ARM     0x0A0Cu /* ival[0]: 0/1; LOCAL sessions only: the local operator's consent to remote PTT */
/* Settings, reports, telemetry, audio output, position (grant: ADMIN unless noted) */
#define FOXAPI_OP_SETTING_SET       0x0B01u /* text: "key=value" */
#define FOXAPI_OP_PROBLEM_REPORT    0x0C01u /* ival[0]: 0 bug, 1 dislike; text: the words (grant: VIEW; 255 bytes in 0.2) */
#define FOXAPI_OP_FEATURE_REQUEST   0x0C02u /* text: the words (grant: VIEW; 255 bytes in 0.2 - see docs/API.md gaps) */
#define FOXAPI_OP_TELEMETRY_ENABLE  0x0D01u /* ival[0]: 0/1 */
#define FOXAPI_OP_AUDIO_DEVICE      0x0E01u /* text: output device id (grant: SETTINGS) */
#define FOXAPI_OP_SET_POSITION      0x0F01u /* num[0]: lat, num[1]: lon (grant: SETTINGS) */
#define FOXAPI_OP_GPS               0x0F02u /* ival[0]: 0/1; text: serial port; num[0]: baud (grant: SETTINGS) */
#define FOXAPI_OP_SCAN_SERIAL_PORTS 0x0F03u /* 0.3; LOCAL sessions only; result arrives as FOXAPI_LIST_SERIAL_PORTS changing */
#define FOXAPI_OP_SERVER_CONFIG     0x1001u /* text: JSON (web + CAT listeners); LOCAL sessions only */
#define FOXAPI_OP_UPDATE_CHECK      0x1101u /* ival[0]: 1 = check now, 2 = enable at start, 3 = disable */

#define FOXAPI_TX_MODE_CW  0u
#define FOXAPI_TX_MODE_AM  1u
#define FOXAPI_TX_MODE_NFM 2u
#define FOXAPI_TX_MODE_USB 3u
#define FOXAPI_TX_MODE_LSB 4u

/*
 * THE KEY. The engine, not the interface, enforces these; an interface
 * cannot opt out of them (docs/API.md, "Transmitter"):
 *   - TX_PTT with ival[0]=1 is a HOLD, not a switch: it keys (or extends) for
 *     FOXAPI_PTT_HOLD_MS and no more. The interface re-asserts while the
 *     operator holds the key; silence opens it.
 *   - TX_LATCH is refused (DENIED) to any session without
 *     FOXAPI_SESSION_LOCAL, and releases itself after FOXAPI_LATCH_TIMEOUT_MS
 *     from when it CLOSED: a LATCH 1 while latched changes nothing (result
 *     FOXAPI_NO_CHANGE).
 *   - ONE LATCH, NOT A CHAIN. A latch belongs to a PRINCIPAL: "local" for
 *     EVERY LOCAL session (in-process, or out-of-process through the local
 *     token file - the operator at the machine), the login token (or the
 *     configured token) for a remote one. When a latch ends by ANY means
 *     other than its OWNING session's LATCH 0 - the timeout, a stop (RUN 0,
 *     from any session, local or remote), a fault, a transmitter change or
 *     close, a lost keep-alive, its session closing or being detached, a
 *     LATCH 0 from any session that did not hold it (the operator's other
 *     window included: pressing RELEASE there because this one is stuck) -
 *     the owner's principal is MARKED. While marked, LATCH 1 from ANY
 *     session of that principal - including sessions opened later - is
 *     refused (DENIED, the message naming how the latch ended) until THAT
 *     session itself sends LATCH 0. A session's own LATCH 0, whether or not
 *     a latch is active, exempts that session and no other: the owner's
 *     LATCH 0 on its active latch ends it (no mark) and exempts the owner; a
 *     LATCH 0 from any other session ends an active latch, marks the owner's
 *     principal and exempts the sender. Every new marking end re-marks and
 *     withdraws all exemptions. The mark is never cleared; closing sessions
 *     clears nothing and exempts nobody (a closed session's queued LATCH 0
 *     still releases, and exempts no one). So LATCH 0 is idempotent. When in
 *     doubt the transmitter stays unkeyed. An interface that re-sends
 *     LATCH 1 every frame gets ONE latch - however often it is closed and
 *     reconnects, however many local sessions do the same, and however often
 *     the operator presses RELEASE in another window - because it never
 *     sends LATCH 0 itself; the deliberate release-then-press works in the
 *     window where it is pressed.
 *   - THE LATCH KEY PROTOCOL. An interface's latch key is EDGE-triggered: it
 *     sends only on a press. A press on a key shown RELEASED sends TX_LATCH 0
 *     and TX_LATCH 1 in ONE submit batch (release first, then press), and a
 *     press on a key shown LATCHED sends TX_LATCH 0. So a deliberate press
 *     always latches (after the re-arm time) even when the last latch ended
 *     by a timeout, a stop or another window - while a stuck window that
 *     only re-sends LATCH 1 stays refused. Show
 *     FOXAPI_RX_TX_LATCH_RELEASE_FIRST on the key. A LEVEL-triggered latch
 *     key (sending the key's state every frame) is not supported: two such
 *     windows fight - one's per-frame LATCH 0 ends the latch the operator
 *     just pressed in the other - and a stuck one gets one latch and then
 *     only refusals.
 *   - After ANY latch ends - released, or opened in any of the ways above -
 *     no session can close the latch again for FOXAPI_LATCH_REARM_MS
 *     (DENIED; applied[0] = milliseconds left), so the key is off for at
 *     least that long and toggling LATCH 0 + LATCH 1 cannot hold it.
 *   - A REMOTE session may only press and release the PTT: every other
 *     transmitter op is DENIED to it whatever its grants (the app's web remote
 *     can send nothing but its PTT), and pressing needs the local operator's
 *     consent - TX_REMOTE_ARM 1 from a LOCAL session, shown as
 *     FOXAPI_RX_TX_REMOTE_ARMED - which lapses when that session disarms,
 *     closes or stops answering, opening any remote key with it.
 *   - A session whose heartbeat() is older than its keep-alive
 *     (FOXAPI_KEEPALIVE_MS local, FOXAPI_REMOTE_KEEPALIVE_MS remote) loses any
 *     key it holds, and so does a closed session. Only heartbeat() counts:
 *     re-asserting the PTT is not a heartbeat.
 *   - The key is never restored from configuration and never survives a
 *     transmitter change, a stop (RUN 0) or a fault.
 */
#define FOXAPI_PTT_HOLD_MS      2000
#define FOXAPI_LATCH_TIMEOUT_MS 60000
#define FOXAPI_LATCH_REARM_MS   1000  /* after ANY latch ends, none closes again for this long */
#define FOXAPI_KEEPALIVE_MS     1000
#define FOXAPI_REMOTE_KEEPALIVE_MS 2000

/* One per command submitted. status is FOXAPI_OK (the transport has it, with
 * a NEW ticket; a FoxCommandResult with that ticket WILL arrive),
 * FOXAPI_NO_CHANGE (a safer command MERGED with the one of its kind this
 * session already has outstanding - see FOXAPI_SAFETY_RESERVE: it will be
 * applied, and `ticket` is that command's, whose one result answers both),
 * or FOXAPI_BUSY (an ordinary command, and the session already has its
 * maximum pending; not taken, no ticket, no result). Nothing else: every
 * judgement of the command itself is a result. NEW tickets are per session,
 * start at 1 and only increase. Only the ENGINE assigns tickets and decides
 * what is merged, and this answer is where it says so; a remote client
 * library never predicts either - it maps its own request ids to the
 * engine's tickets from the answers it receives (docs/TRANSPORTS.md 2.2). */
typedef struct FoxSubmitResult {
    uint32_t structSize;
    int32_t status;             /* FOXAPI_OK, FOXAPI_NO_CHANGE or FOXAPI_BUSY - see above */
    uint64_t ticket;            /* non-zero when taken or merged; matches FoxCommandResult::ticket */
} FoxSubmitResult;

#define FOXAPI_MIN_SUBMIT_RESULT (offsetof(FoxSubmitResult, ticket) + sizeof(uint64_t))

#define FOXAPI_RESULT_CLAMPED 0x00000001u /* applied, but not to the value asked for */
#define FOXAPI_RESULT_REFUSED 0x00000002u /* status != FOXAPI_OK: NOTHING changed (see rule 2) */

typedef struct FoxCommandResult {
    uint32_t structSize;
    int32_t status;             /* what APPLYING it produced */
    uint64_t ticket;
    uint32_t op;
    uint32_t flags;             /* FOXAPI_RESULT_* */
    double applied[2];          /* the value(s) actually in force afterwards */
    char message[FOXAPI_MESSAGE_CHARS]; /* a sentence for the user when status != OK */
} FoxCommandResult;

#define FOXAPI_MIN_COMMAND_RESULT (offsetof(FoxCommandResult, message) + FOXAPI_MESSAGE_CHARS)

/* ---------------------------------------------------------------------------
 * Events: things that HAPPEN (as against state, which IS). Polled, never
 * delivered by callback. Each session has a bounded ring; when it overflows
 * the oldest are dropped and a FOXAPI_EVENT_OVERFLOW (value[0] = how many)
 * takes their place, so a slow interface learns it missed something.
 * ------------------------------------------------------------------------- */
#define FOXAPI_EVENT_STATE        1u  /* value[0]: which seq groups moved (bit per group) */
#define FOXAPI_EVENT_FAULT        2u  /* text: fault message */
#define FOXAPI_EVENT_TX_UNKEYED   3u  /* text: why the key opened on its own */
#define FOXAPI_EVENT_NOTICE       4u  /* value[0]: level 1 info 2 warn 3 error; text */
#define FOXAPI_EVENT_DECODER_TEXT 5u  /* target: decoder id; text: one output line */
#define FOXAPI_EVENT_DECODER_IMAGE 6u /* target: decoder id; value[0]: image id for read_image. 0.3 (8.12): a PATCH decoder's picture - target: the patch node, value[1]: 1, text: the node's name; once a pass in which the node's newest picture changed */
#define FOXAPI_EVENT_LIST_CHANGED 7u  /* target: FOXAPI_LIST_* */
#define FOXAPI_EVENT_SESSION      8u  /* text: e.g. "keep-alive late" */
#define FOXAPI_EVENT_OVERFLOW     9u  /* value[0]: events dropped */
#define FOXAPI_EVENT_PATCH        10u /* 0.3; value[0]: graph epoch; value[1]: 1 running */
#define FOXAPI_EVENT_MUTE_TUNE_AWAY 11u /* 0.3 (8.7); value[0]: how many plugins were muting the audio when the receiver tuned off their preset (FOXAPI_LIST_MUTE_CAUSES names them); once per tune-away */
#define FOXAPI_EVENT_PRESET_APPLIED 12u /* 0.3 (8.14); text: the plugin's module file name (PLUGINS' detail); once for each preset applied, raised by the endPass of the pass that applied it */
#define FOXAPI_EVENT_MASK(kind) (1ull << (kind))

typedef struct FoxEvent {
    uint32_t structSize;
    uint32_t kind;              /* FOXAPI_EVENT_* */
    uint64_t seq;               /* per session, strictly increasing */
    int64_t unixMs;
    uint32_t target;
    uint32_t reserved;
    double value[2];
    char text[FOXAPI_TEXT_CHARS];
} FoxEvent;

#define FOXAPI_MIN_EVENT (offsetof(FoxEvent, text) + FOXAPI_TEXT_CHARS)

/* ---------------------------------------------------------------------------
 * Lists: the variable-length collections, read an entry at a time.
 * read_list returns HOW MANY entries there are (>= 0) and fills `out` when
 * index < that count. FOXAPI_EVENT_LIST_CHANGED / listSeq say when to re-read.
 * The meaning of value[] per list is in docs/API.md, "Lists".
 * ------------------------------------------------------------------------- */
#define FOXAPI_LIST_MODES         1u  /* value[0]: FOXAPI_DEMOD_*, value[1]: default bandwidth Hz */
#define FOXAPI_LIST_BANDWIDTHS    2u  /* value[0]: Hz (the stepped choices) */
#define FOXAPI_LIST_DEVICES       3u  /* name: label; detail: id to pass to SELECT_SOURCE (cut at 159 bytes: 0.3 (8.10) fact.source.id.<id> is the whole id); 0.3 (8.10): entries 1-3 are the generator, the IQ file and the sound card, then come the native and SoapySDR rows, so an entry's index is its row in the Source combo */
#define FOXAPI_LIST_SAMPLE_RATES  4u  /* value[0]: Hz; 0.3 (8.10): detail: the rate's caption ("2.400 MS/s") */
#define FOXAPI_LIST_GAINS         5u  /* name; detail: the unit token ("dB" or "step", 0.3); value[0..3]: min, max, step, current; FOXAPI_ITEM_AGC (0.3) */
#define FOXAPI_LIST_ANTENNAS      6u
#define FOXAPI_LIST_BOOKMARKS     7u  /* sorted by frequency; id; name; detail: group ("" = none); value[0]: Hz, [1]: mode, [2]: bandwidth Hz; FOXAPI_ITEM_FAVOURITE; 0.3 (9.6, stub campaign B6): value[3]: the seconds the Airband monitor has heard this row talking (kept with the list); FOXAPI_ITEM_TICKED */
#define FOXAPI_LIST_DECODERS      8u  /* id; name; flags: running/fed; detail: status text; 0.3 (8.7): MUTES */
#define FOXAPI_LIST_PLUGINS       9u  /* installed plugins; detail: file name; 0.3 (8.7): value[0]: the capability word (CASCADE_CAP_* of the plugin ABI); ACTIVE loaded; STOPPED, TUNE_ALLOWED, SETTINGS_ALLOWED; (8.8): value[1]: the module file's size in bytes, value[2]: its notice level (0 none, 2 warning, 3 error); MATCHED, ASKED_SETTINGS, DIGEST_MISMATCH; (9.3): value[3]: how it runs - bits 0-1 the run state (0 AUTO, 1 ALWAYS, 2 STOPPED), bit 2 (4) asleep: AUTO and nothing is using it */
#define FOXAPI_LIST_CATALOGUE     10u /* the store's entries; detail: the catalogue id; 0.3 (8.9): value[0]: the ABI version the entry's build declares, value[1]: the size in bytes of the build for this machine (0 = not stated or no build); INSTALLED, COMPATIBLE, BUILD_HERE */
#define FOXAPI_LIST_AUDIO_DEVICES 11u
#define FOXAPI_LIST_TX_DEVICES    12u
/*      13u retired in 0.2 (FREQ_LISTS): imported lists are bookmark groups */
#define FOXAPI_LIST_TRACKS        14u /* map tracks: value[0..2]: lat, lon, alt */
#define FOXAPI_LIST_MARKERS       15u /* plugin marks on the spectrum: value[0]: Hz, value[1]: width; 0.3 (8.7): value[2]: colour 0xRRGGBBAA, SPAN, DASHED, SPECTRUM_ONLY */
#define FOXAPI_LIST_BAND_PLAN     16u /* value[0], value[1]: start, stop Hz */
#define FOXAPI_LIST_PRESETS       17u /* plugin presets: name; detail: plugin; value[0]: Hz; 0.3 (8.7): DEVICE_CENTRE */
#define FOXAPI_LIST_DEVICE_OPTIONS 18u /* radio-specific switches (RSP notches, HDR, dither...) */
#define FOXAPI_LIST_SERIAL_PORTS  19u
#define FOXAPI_LIST_USER_PRESETS  20u
#define FOXAPI_LIST_BOOKMARK_GROUPS 21u /* name: group; value[0]: bookmarks in it */
/* 0.3 (docs/api-0.3-additions.md) */
#define FOXAPI_LIST_STATUS_LINES  22u /* name: key; detail: text (whole: "fact.status.<key>"); value[0]: level 1 info 2 warn 3 error; 0.3 (8.10): key deviceFault is the whole fault sentence the state's faultMessage cuts at 127 bytes */
#define FOXAPI_LIST_CONVERTERS    23u /* detail: radio key; value[0]: 0 off 1 up 2 down, [1]: LO Hz, [2]: 1 inverted; ACTIVE: the open radio's, whose value[3] is the radio's own centre readback in Hz, read live (0.3) */
#define FOXAPI_LIST_GAIN_MODES    24u /* name; value[0]: mode code (the op that takes it names it); ACTIVE */
#define FOXAPI_LIST_DECIMATIONS   25u /* value[0]: factor, value[1]: resulting rate Hz; ACTIVE */
#define FOXAPI_LIST_PATCH_NODES   26u /* id: node; name; detail: kind; value[0..1]: x, y; [2]: Hz; [3]: rate Hz; ACTIVE on, REFUSED, FAULT */
#define FOXAPI_LIST_PATCH_WIRES   27u /* value[0]: from node, [1]: its port, [2]: to node, [3]: its port */
#define FOXAPI_LIST_PATCH_PROBLEMS 28u /* id: node; name: problem; detail: what the page says */
#define FOXAPI_LIST_PATCH_PARTS   29u /* the parts a graph may use: name; detail: key; value[0]: feed port type, [1]: rate Hz it needs, [2]: 1 when the module also has a track source (0.3, 8.12) */
#define FOXAPI_LIST_RECORDINGS    30u /* name: file; detail: path (LOCAL only); value[0]: 1 I/Q 2 audio 3 patch, [1]: bytes, [2]: rate Hz, [3]: the take's seconds (0.3; live, like [1]); ACTIVE, STALLED, FULL */
#define FOXAPI_LIST_DECODER_LINES 31u /* the decoder-output backlog, oldest first: id: line number; name: plugin; detail: text; value[0]: the patch decoder node the line came from, 0 for the receiver's own decoders (0.3, 8.12) */
#define FOXAPI_LIST_STORE_BLOCKED 32u /* name: file; detail: why it was refused */
#define FOXAPI_LIST_STORE_UPDATES 33u /* name: catalogue id; detail: "from -> to" */
#define FOXAPI_LIST_BAND_PLANS    34u /* the plans SET_BAND_PLAN takes: name; detail: id; ACTIVE */
#define FOXAPI_LIST_PATCH_RADIOS  35u /* id: node; name: device; detail: error, or for a running radio the note its face prints (0.3, 8.12); value[0]: rate Hz, [1]: an open radio's AIR centre Hz (0.3, 8.12), [2]: 1 while its open is on its worker (0.3, 8.12); ACTIVE open, FAULT */
#define FOXAPI_LIST_MUTE_CAUSES   36u /* 0.3 (8.7): the plugins muting the receiver's audio now. name: plugin; detail: its module file name; value[0]: 1 when the mute is held after a tune-away; ACTIVE */
#define FOXAPI_LIST_STALE_GRANTS  37u /* 0.3 (8.7): the tune grants no loaded module holds. name: the module file name; REFUSED when the host holds a record of that file that did not load */
#define FOXAPI_LIST_PLUGIN_COMMANDS 38u /* 0.3 (8.8): the command keys the loaded modules have added. id: the command id (FOXAPI_OP_PLUGIN_COMMAND's ival[0]); name: the key's label; detail: the module file name */
#define FOXAPI_LIST_STORE_ACTIVITY 39u /* 0.3 (8.9): what the plugin store is doing now, at most two entries told apart by id. id 1: a catalogue fetch or a download is in flight (name: the module's name, "" for the catalogue; value[0]: the progress 0..1, read live; value[1]: 1 fetching the catalogue, 2 downloading); id 2: an ADD ALL run is active (name: the module moving now; value[0]: its position, 1-based; value[1]: the total); ACTIVE on both */

#define FOXAPI_LIST_UNBOUND_RADIOS 40u /* 0.3 (8.10): USB radios that are plugged in and cannot be opened (no WinUSB driver bound). name: the bus description ("" when it reports none); detail: the family, "rtl", "rx888" or "" for any other; value[0], value[1]: vendor id, product id */
#define FOXAPI_LIST_SCAN_EVIDENCE 41u /* 0.3 (8.10), LOCAL sessions only (DENIED to REMOTE): where the SoapySDR runtime looked, taken when a scan landed that found nothing. value[0]: 1 a vendor install (name: its vendor, detail: its root), 2 a module search path (detail), 3 a module loaded (detail) */
#define FOXAPI_LIST_SOUND_CARDS   42u /* 0.3 (8.10): the sound card inputs as the last listing found them. id: index + 1 (what FOXAPP_OP_SOUND_CARD_PICK takes); name: "device (host API)"; detail: the host API; ACTIVE: the entry the form's card is */
#define FOXAPI_LIST_SOUND_CARD_RATES 43u /* 0.3 (8.10): the rates the form's card offers in the form's format. value[0]: Hz of the card; EXCLUSIVE; ACTIVE: the form's rate. Empty when the card is not listed */
#define FOXAPI_LIST_SOUND_CARD_FORM 44u /* 0.3 (8.10): ONE entry, the sound card form as the engine holds it. name: the device and detail: its host API (both "" to a REMOTE session; whole in fact.soundcard.device and .hostApi); value[0]: the card's rate Hz, [1]: the I/Q centre Hz, [2]: the channel (0 left, 1 right); IQ_STEREO, SWAP_IQ, PICKED */
#define FOXAPI_LIST_PATCH_PLAN    45u /* 0.3 (8.12): the plan the patch page draws, one entry per planned channel and decoder. id: node; detail: "channel", or "decoder:radio" / "decoder:channel" / "decoder:audio" (what feeds it); name: a decoder's plugin key; value[0]: a channel's offset Hz from its radio's centre, a decoder's rate Hz it is created at; [1]: a channel's output rate Hz, a decoder's input rate Hz; [2]: a channel's decimation, an I/Q decoder's RF centre Hz at DC; [3]: the radio node it runs on */
#define FOXAPI_LIST_PATCH_DEVICES 46u /* 0.3 (8.12): the devices a patch Radio can be set to, in the order the Radio's Device combo lists them: the generator, the I/Q recordings, the native radios, the SoapySDR radios, the sound cards, the receiver's lent radio when no list shows it. id: index + 1; name: the label; detail: the device KEY a Radio node names it by (LOCAL sessions only, else empty; cut at 159 bytes: fact.patch.device.<id> is the whole key); value[0]: a recording's own rate Hz, else 0 */
#define FOXAPI_LIST_PATCH_LIVE    47u /* 0.3 (8.12): what the patch's running nodes are doing NOW, one entry per node that has something, taken once a pass (never moves listSeq; read it each time you draw). id: node; value[2]: the kind - 1 a demodulator's squelch (value[0]: the level it judges, dB; ACTIVE: the gate is open), 2 a speaker's output (name: what it plays to; value[0]: its newest peak 0..1; value[1]: samples written; detail: its error, "" none), 3 a channel's level (value[0]: dB), 4 a decoder's face (value[0]: lines said so far; detail: the newest line), 5 a speaker whose output could not be made (detail: why), 6 a picture decoder's newest picture (value[0]: the image id read_image takes, [1]: its bytes; name: its caption; ACTIVE: the decoder has finished it; the row stays while the node is in the graph, whether or not the patch runs) */
#define FOXAPI_LIST_GPS           48u /* 0.3 (8.13): ONE entry, the GPS reader as the position row draws it, read at the call - it never moves listSeq, read it each time you draw. name: the port given to the last start ("" when the reader is idle; LOCAL sessions only); detail: the status line ("" when idle; LOCAL sessions only, the sentence names the port; cut at 159 bytes); value[0]: the state (0 idle, 1 listening, 2 fixed, 3 failed, 4 timed out), value[1]: seconds since the start (frozen when it ends), value[2]: satellites in view (-1 none yet), value[3]: the baud; ACTIVE: listening; FAULT: failed or timed out. Never a coordinate: the position a fix gives is the state's (FOXAPI_RX_POSITION_SET) */
#define FOXAPI_LIST_SERVERS       49u /* 0.3 (8.16), LOCAL sessions only (DENIED to REMOTE): the two listeners the engine runs for FOXAPI_OP_SERVER_CONFIG, always two entries, the web server first. id 1: web, id 2: CAT; name: "web" or "cat"; detail: the sentence the last start was refused with ("" when none; written to be shown, never holds a password); value[0]: the port (the one it is listening on, else the configured one), value[1]: browser sessions (web) or connected clients (CAT), value[2]: browsers listening to the audio (web; 0 for CAT), value[3]: commands received; ACTIVE: listening; FAULT: enabled and refused to start. value[1..3] are read at the call and move nothing. The configuration itself is the facts fact.web.* and fact.cat.* */
#define FOXAPI_LIST_STORE_PICTURES 50u /* 0.3 (8.9, stub campaign B3): the plugin store's pictures, every one it was asked for and still knows (FOXAPP_OP_STORE_PICTURES_WANT), in sha256 order. id: the first 64 bits of the sha256; name: the sha256, 64 hex digits (the cache key; the catalogue entry names it in fact.store.<key>.pictures); detail: for a Ready one the cached file's path (LOCAL sessions only; the whole path is in fact.store.picture.<sha256>.path), for a Failed one the reason, cut at 159 bytes (whole in fact.store.picture.<sha256>.reason); value[0]: 1 pending (queued or in flight), 2 ready, 3 failed; ACTIVE: the one in flight. One transfer at a time, and none while a catalogue read, a download or an ADD ALL run is going */
#define FOXAPI_LIST_AIRBAND_CHANNELS 51u /* 0.3 (9.6, stub campaign B6): the channels the Airband monitor is playing, one entry per channel of the plan (not only the block on the air), read at the call - it never moves listSeq, read it each time you draw; empty while the monitor is not listening. id: the channel's number in the plan, from 1 (frequency order); name: the row that names it (the first ticked row on that frequency); value[0]: the frequency Hz, [1]: the channel width Hz, [2]: the level its squelch judges, dB (-200 when it has not reported one), [3]: the seconds heard since the last time the monitor wrote them to the list (add it to BOOKMARKS value[3] for the figure to show); ACTIVE: its squelch is open now; FOXAPI_ITEM_ON_AIR: it is in the block on the air now (none is before the first block has been tuned) */
#define FOXAPI_LIST_PLUGIN_SETTINGS 52u /* 0.3 (9.7, stub campaign B9), LOCAL sessions only (DENIED to REMOTE): the fields of the settings forms the loaded modules declare (CASCADE_CAP_SETTINGS_UI), one entry per field the host can store, a module's fields together and in its own order. id: the entry's number in this list, from 1 (what fact.plugin.setting.<id> is read by; it moves when the modules do); name: the field's label (the plugin's own text, as given); detail: the module file name (PLUGINS' detail); value[0]: the kind (CASCADE_SETTING_TEXT 0, NUMBER 1, BOOL 2, PATH 3), value[1]: the longest value, in UTF-8 bytes. The field's key and placeholder are the fact fact.plugin.setting.<id>.spec ("<key>", a unit separator (byte 0x1F), "<placeholder>"), its value - the stored one, else the plugin's default - the fact fact.plugin.setting.<id>. The list moves listSeq when a field's value changes, whoever changed it */

#define FOXAPI_ITEM_ACTIVE    0x00000001u /* the selected / running / current entry */
#define FOXAPI_ITEM_FAVOURITE 0x00000002u /* a bookmark marked as a favourite */
/* 0.3 */
#define FOXAPI_ITEM_REFUSED    0x00000004u /* a patch node the engine refused to run */
#define FOXAPI_ITEM_FAULT      0x00000008u /* ...or whose radio faulted */
#define FOXAPI_ITEM_STALLED    0x00000010u /* a recording whose writes are failing */
#define FOXAPI_ITEM_FULL       0x00000020u /* a recording that reached its size limit */
#define FOXAPI_ITEM_FED        0x00000040u /* a decoder being fed signal now */
#define FOXAPI_ITEM_INSTALLED  0x00000080u /* a catalogue entry installed here */
#define FOXAPI_ITEM_COMPATIBLE 0x00000100u /* a catalogue entry this build can load */
#define FOXAPI_ITEM_AGC        0x00000200u /* a gain stage its own automatic gain control is driving now (an Airspy's LNA or MIXER) */
#define FOXAPI_ITEM_MUTES      0x00000400u /* a decoder whose "mute audio while running" setting is on (DECODERS, 8.7) */
#define FOXAPI_ITEM_TUNE_ALLOWED 0x00000800u /* a plugin granted control of the receiver's tuning (PLUGINS, 8.7) */
#define FOXAPI_ITEM_SETTINGS_ALLOWED 0x00001000u /* a plugin granted control of the radio's settings (PLUGINS, 8.7) */
#define FOXAPI_ITEM_STOPPED    0x00002000u /* a loaded plugin the user has stopped (PLUGINS, 8.7) */
#define FOXAPI_ITEM_DEVICE_CENTRE 0x00004000u /* a preset that moves the radio's centre, not the VFO (PRESETS, 8.7) */
#define FOXAPI_ITEM_SPAN       0x00008000u /* a mark that is a shaded band from value[0] to value[0] + value[1], not a line (MARKERS, 8.7) */
#define FOXAPI_ITEM_DASHED     0x00010000u /* a mark drawn dashed (MARKERS, 8.7) */
#define FOXAPI_ITEM_SPECTRUM_ONLY 0x00020000u /* a mark that is not drawn on the waterfall (MARKERS, 8.7) */
#define FOXAPI_ITEM_MATCHED    0x00040000u /* a plugin the runner is feeding, or would feed with the receiver running: matched to the rate the chain delivers, whatever its kind (PLUGINS, 8.8) */
#define FOXAPI_ITEM_ASKED_SETTINGS 0x00080000u /* a plugin that has asked to change a receiver setting, which only the settings grant allows (PLUGINS, 8.8) */
#define FOXAPI_ITEM_DIGEST_MISMATCH 0x00100000u /* a plugin file that is not the one installed: its bytes no longer match the sha256 recorded at install (PLUGINS, 8.8) */
#define FOXAPI_ITEM_BUILD_HERE 0x00200000u /* a catalogue entry that publishes a build for this machine's os and architecture (CATALOGUE, 8.9); "installable here" is COMPATIBLE and BUILD_HERE */
#define FOXAPI_ITEM_EXCLUSIVE  0x00400000u /* a sound card rate the card accepts only in WASAPI exclusive mode (SOUND_CARD_RATES, 8.10) */
#define FOXAPI_ITEM_IQ_STEREO  0x00800000u /* the sound card form is set to I/Q (stereo) and not real (mono) (SOUND_CARD_FORM, 8.10) */
#define FOXAPI_ITEM_SWAP_IQ    0x01000000u /* the sound card form swaps I and Q (SOUND_CARD_FORM, 8.10) */
#define FOXAPI_ITEM_PICKED     0x02000000u /* the sound card form's device was chosen from SOUND_CARDS, so it names exactly that entry (SOUND_CARD_FORM, 8.10) */
#define FOXAPI_ITEM_TICKED     0x04000000u /* a bookmark ticked: the scanner's list mode scans it and, when its mode is AM or NFM, the Airband monitor plays it (BOOKMARKS, 9.6) */
#define FOXAPI_ITEM_ON_AIR     0x08000000u /* an Airband channel that is in the block on the air now - once the first block has been tuned (AIRBAND_CHANNELS, 9.6) */

typedef struct FoxListItem {
    uint32_t structSize;
    uint32_t list;
    uint32_t index;
    uint32_t flags;             /* FOXAPI_ITEM_* */
    uint64_t id;                /* stable for the entry's lifetime; 0 when the list has none */
    double value[4];
    char name[96];
    char detail[160];
} FoxListItem;

#define FOXAPI_MIN_LIST_ITEM (offsetof(FoxListItem, detail) + 160)

/* ---------------------------------------------------------------------------
 * Audio (and, later, I/Q) streams. A cursor is an absolute frame count in the
 * stream; pass 0 to join LIVE. Returns frames copied (>= 0) and advances the
 * cursor; `dropped` says how many frames the reader fell behind by (the ring
 * overran it), which is the reader's problem, never the DSP thread's.
 * ------------------------------------------------------------------------- */
typedef struct FoxStreamInfo {
    uint32_t structSize;
    uint32_t channels;          /* interleaved */
    double rateHz;
    uint64_t dropped;           /* frames lost to overrun on this read */
    uint64_t written;           /* frames the engine has produced in total */
} FoxStreamInfo;

#define FOXAPI_MIN_STREAM_INFO (offsetof(FoxStreamInfo, written) + sizeof(uint64_t))

typedef struct FoxImageInfo {
    uint32_t structSize;
    uint32_t width;
    uint32_t height;
    uint32_t format;            /* 1 = GRAY8, 2 = RGB24 (as CASCADE_IMAGE_*) */
    uint64_t bytes;             /* size of the pixel data */
    char caption[FOXAPI_NAME_CHARS];
    /* 0.3 (8.12), APPENDED (rule 4: a caller that gives FOXAPI_MIN_IMAGE_INFO bytes gets the fields above and none of
     * these): */
    uint32_t complete;          /* 0 while the decoder is still receiving the picture, 1 when it is finished */
    uint32_t reserved;
    uint64_t sequence;          /* the decoder's own count of the picture's frames (a number that moves as it builds) */
} FoxImageInfo;

#define FOXAPI_MIN_IMAGE_INFO (offsetof(FoxImageInfo, caption) + FOXAPI_NAME_CHARS)

/* ---------------------------------------------------------------------------
 * 0.3: SCOPE TAPS (FOXAPI_CAP_SCOPE). The newest samples of a demod-scope tap,
 * oldest first: a SNAPSHOT, not a stream - an interface draws the last N and
 * asks again next frame. Never more than FOXAPI_SCOPE_REMOTE_FRAMES to a
 * REMOTE session ("reduced").
 * ------------------------------------------------------------------------- */
#define FOXAPI_SCOPE_AUDIO 1u   /* the finished audio, mono float */
#define FOXAPI_SCOPE_IQ    2u   /* the channel's I/Q, interleaved complex pairs */
#define FOXAPI_SCOPE_MPX   3u   /* the WFM multiplex (only while WFM runs) */
#define FOXAPI_SCOPE_REMOTE_FRAMES 4096u

typedef struct FoxScopeInfo {
    uint32_t structSize;
    uint32_t tap;               /* FOXAPI_SCOPE_* */
    uint32_t channels;          /* 1, or 2 for I/Q */
    uint32_t capacityFrames;    /* 0.3 (8.11): the most frames a read_scope on THIS session can return for this tap - the ring's size (65536 audio, 262144 I/Q, 65536 multiplex) for a LOCAL session, min(ring, FOXAPI_SCOPE_REMOTE_FRAMES) for a REMOTE one; 0 from an engine that predates it */
    double rateHz;              /* the tap's sample rate */
    uint64_t written;           /* frames the tap has taken in total: one that stops moving is stalled */
} FoxScopeInfo;

#define FOXAPI_MIN_SCOPE_INFO (offsetof(FoxScopeInfo, written) + sizeof(uint64_t))

/* 0.3: the most text submit_text carries with one command. */
#define FOXAPI_MAX_LONG_TEXT (1u << 20)

/* ---------------------------------------------------------------------------
 * Opening an engine and a session.
 * ------------------------------------------------------------------------- */
typedef struct FoxEngine FoxEngine;
typedef struct FoxSession FoxSession;

typedef struct FoxEngineParams {
    uint32_t structSize;
    uint32_t flags;             /* reserved, 0 */
    /* "key=value;key=value" engine options (the mock engine's are listed in
     * src/mock_engine/mock_engine.hpp). May be NULL. A remote client library
     * takes its endpoint here: "endpoint=wss://host:8073". */
    const char *options;
} FoxEngineParams;

#define FOXAPI_MIN_ENGINE_PARAMS (offsetof(FoxEngineParams, options) + sizeof(const char *))

typedef struct FoxSessionParams {
    uint32_t structSize;
    uint32_t flags;             /* FOXAPI_SESSION_* */
    uint64_t grants;            /* FOXAPI_GRANT_* asked for */
    const char *clientName;     /* "FoxSDR standard interface" */
    const char *clientVersion;
    const char *token;          /* remote: a token from login() (or the engine's configured one); local: NULL */
} FoxSessionParams;

#define FOXAPI_MIN_SESSION_PARAMS (offsetof(FoxSessionParams, token) + sizeof(const char *))

/* ---------------------------------------------------------------------------
 * THE TABLE. An engine module exports foxsdr_engine_query(); a remote client
 * library exports the same symbol and speaks the wire protocol behind it.
 * ------------------------------------------------------------------------- */
typedef struct FoxEngineApi {
    uint32_t structSize;        /* sizeof(FoxEngineApi) as the ENGINE compiled it */
    uint32_t versionMajor;      /* FOXAPI_VERSION_MAJOR of the engine */
    uint32_t versionMinor;
    uint32_t reserved0;
    uint64_t capabilities;      /* FOXAPI_CAP_* this engine implements */
    const char *engineName;     /* "FoxSDR mock engine"; diagnostics only */
    const char *engineVersion;

    int32_t (FOXAPI_CALL *create)(const FoxEngineParams *params, FoxEngine **out);
    /* Closes every session (releasing any key), stops, frees. */
    void (FOXAPI_CALL *destroy)(FoxEngine *engine);

    /* LIMIT when the engine already has its maximum of sessions, or, for a
     * remote one, when only the places kept for LOCAL sessions are left (the
     * mock keeps 4 of its 64, so remote logins can never lock the local
     * window out) or it already has its maximum of ATTACHED remote sessions;
     * UNAUTHENTICATED for a remote session without a valid token. A session
     * DETACHED (its login revoked or expired) is closed by the engine itself
     * on its next control pass and holds no place from then on; its handle
     * stays valid, answering DETACHED, until close_session. */
    int32_t (FOXAPI_CALL *open_session)(FoxEngine *engine, const FoxSessionParams *params,
                                        FoxSession **out);
    /* Releases a key this session holds, then frees it (a detached session's
     * handle included). Any later call through it is undefined; calls
     * through OTHER sessions are unaffected. Of what it left queued, only
     * the commands that make the transmitter safer are still applied. */
    void (FOXAPI_CALL *close_session)(FoxSession *session);

    /* The session's dead-man handle: call it at least every keep-alive
     * (FOXAPI_KEEPALIVE_MS local, FOXAPI_REMOTE_KEEPALIVE_MS remote); an
     * interface calls it once a frame. The ONLY thing that counts as
     * liveness: a submit, a read or a transport's ping does not. */
    int32_t (FOXAPI_CALL *heartbeat)(FoxSession *session);

    /* Rule 1. Fills `out` (set out->structSize first). */
    int32_t (FOXAPI_CALL *read_state)(FoxSession *session, FoxReceiverState *out);

    /* The newest spectrum frame if its seq > sinceSeq (else FOXAPI_NO_CHANGE
     * and nothing written). Up to `cap` bins of dB into `bins`; when cap is
     * smaller than the engine's bin count the engine reduces by taking the
     * MAXIMUM of each group, so a narrow carrier never disappears. */
    int32_t (FOXAPI_CALL *read_spectrum)(FoxSession *session, uint64_t sinceSeq,
                                         FoxSpectrumInfo *info, float *bins, uint32_t cap);

    /* Rule 2. Hands `count` commands to the engine (or, remotely, to the
     * transport) and returns at once. `results` (may be NULL) receives one
     * FoxSubmitResult per command: OK and a new ticket, NO_CHANGE (a merged
     * safer command) and the ticket answering it, or BUSY. Returns how many
     * NEW tickets were given (each will produce exactly one
     * FoxCommandResult; merged ones are not counted), or a
     * negative FOXAPI_* for the whole call: BAD_ARGUMENT (NULL, a stride
     * below FOXAPI_MIN_COMMAND or FOXAPI_MIN_SUBMIT_RESULT), LIMIT (count >
     * FOXAPI_MAX_BATCH), DETACHED. Refusals of a command are RESULTS. */
    int32_t (FOXAPI_CALL *submit)(FoxSession *session, const FoxCommand *commands,
                                  uint32_t count, FoxSubmitResult *results);

    /* Results, oldest first: refusals (FOXAPI_RESULT_REFUSED) and what
     * landed. Returns how many were written (<= cap). */
    int32_t (FOXAPI_CALL *poll_results)(FoxSession *session, FoxCommandResult *out, uint32_t cap);

    /* Which FOXAPI_EVENT_* this session wants (FOXAPI_EVENT_MASK bits). */
    int32_t (FOXAPI_CALL *subscribe)(FoxSession *session, uint64_t eventMask);
    int32_t (FOXAPI_CALL *poll_events)(FoxSession *session, FoxEvent *out, uint32_t cap);

    /* Audio: the finished audio the engine plays, interleaved float. Needs
     * FOXAPI_GRANT_AUDIO. `info` may be NULL; one shorter than
     * FOXAPI_MIN_STREAM_INFO is BAD_ARGUMENT (never silently skipped). */
    int32_t (FOXAPI_CALL *read_audio)(FoxSession *session, uint64_t *cursor, float *out,
                                      uint32_t capFrames, FoxStreamInfo *info);
    /* I/Q: complex float pairs at a REDUCED rate the engine chooses (never
     * the full device rate to a remote session). Needs FOXAPI_GRANT_IQ. */
    int32_t (FOXAPI_CALL *read_iq)(FoxSession *session, uint64_t *cursor, float *out,
                                   uint32_t capFrames, FoxStreamInfo *info);

    int32_t (FOXAPI_CALL *read_list)(FoxSession *session, uint32_t list, uint32_t index,
                                     FoxListItem *out);

    /* Returns the value's length (>= 0) and copies what fits, always
     * NUL-terminated when cap > 0. NOT_FOUND for an unknown key. 0.3: keys
     * beginning "fact." are READ-ONLY facts the engine publishes
     * (FOXAPI_CAP_FACTS; the list is docs/api-0.3-additions.md) - no
     * SETTING_SET ever takes one - and the channel for texts longer than a
     * list item's detail[]. */
    int32_t (FOXAPI_CALL *get_setting)(FoxSession *session, const char *key, char *buf, size_t cap);

    /* A decoded picture by the id a FOXAPI_EVENT_DECODER_IMAGE carried.
     * Copies up to `cap` bytes of pixels; returns the full byte count.
     * 0.3 (8.12): a picture is IMMUTABLE under its id and an id names it for as long as it is the newest picture of
     * its decoder, so a read is whole or it is not made (NOT_FOUND once a newer picture has replaced it, or the
     * decoder has gone; the newest id is in the FOXAPI_LIST_PATCH_LIVE row). `info` and `pixels` may each be NULL
     * (`pixels` with cap 0): the call is then the size and the shape alone. */
    int32_t (FOXAPI_CALL *read_image)(FoxSession *session, uint64_t imageId, FoxImageInfo *info,
                                      uint8_t *pixels, size_t cap);

    /* LOGIN. Exchanges a user name and password for a token a REMOTE session
     * presents in FoxSessionParams::token. Writes the token (NUL-terminated;
     * at least 65 bytes of `cap` needed) and returns its length.
     * UNAUTHENTICATED for a wrong name or password, or when the engine has no
     * credential configured; LIMIT while failures from this engine's callers
     * are being throttled (try later). A token lasts at most 12 hours. The
     * remote client library calls this over the transport's login request;
     * in-process it exists so the same code path can be tested. */
    int32_t (FOXAPI_CALL *login)(FoxEngine *engine, const char *user, const char *password,
                                 char *token, size_t cap);
    /* Revokes a login token: no new session opens with it, and every session
     * opened with it is DETACHED at once (any key it held opens, and of what
     * it had queued only the commands that make the transmitter safer are
     * applied). OK for a live token; NOT_FOUND for one that is unknown,
     * already revoked or EXPIRED - an expired token's sessions were detached
     * when it expired, however that was noticed; DENIED for the engine's
     * CONFIGURED token, which is configuration, not a login: logout cannot
     * revoke it and detaches nothing (changing the configuration does). */
    int32_t (FOXAPI_CALL *logout)(FoxEngine *engine, const char *token);

    /* ---- 0.3 (docs/api-0.3-additions.md) ---------------------------------- */

    /* A scope tap's newest frames (FOXAPI_CAP_SCOPE): up to capFrames frames,
     * oldest first, `channels` floats each. Returns frames copied (>= 0);
     * UNSUPPORTED for an unknown tap; `info` may be NULL. */
    int32_t (FOXAPI_CALL *read_scope)(FoxSession *session, uint32_t tap, FoxScopeInfo *info, float *out,
                                      uint32_t capFrames);
    /* A patch radio's own spectrum (FOXAPI_CAP_NODE_SPECTRUM), exactly as
     * read_spectrum reads the main one: the node id from
     * FOXAPI_LIST_PATCH_RADIOS; NOT_FOUND for a node with no radio running. */
    int32_t (FOXAPI_CALL *read_node_spectrum)(FoxSession *session, uint64_t nodeId, uint64_t sinceSeq,
                                              FoxSpectrumInfo *info, float *bins, uint32_t cap);
    /* ONE command whose text is `text` (len bytes, UTF-8, no NUL needed;
     * FOXAPI_CAP_LONG_TEXT) instead of its text[] slot: a patch document, a
     * bookmark file, a report longer than 255 bytes. Otherwise exactly
     * submit() with a count of 1, and the same answer. LIMIT when len >
     * FOXAPI_MAX_LONG_TEXT. */
    int32_t (FOXAPI_CALL *submit_text)(FoxSession *session, const FoxCommand *command, const char *text,
                                       uint32_t len, FoxSubmitResult *result);
} FoxEngineApi;

/* True when the table covers member `m`: the engine was built with it. */
#define FOXAPI_COVERS(api, m)                                                  \
    ((api) != NULL &&                                                          \
     (api)->structSize >= (uint32_t)(offsetof(FoxEngineApi, m) + sizeof((api)->m)))
/* True when the table PROVIDES function `fn`: covered and non-NULL. */
#define FOXAPI_HAS(api, fn) (FOXAPI_COVERS(api, fn) && (api)->fn != NULL)

/* The oldest accepted table, whole (rule 4). */
#define FOXAPI_MIN_ENGINE_API (offsetof(FoxEngineApi, submit_text) + sizeof(void (*)(void)))

/*
 * The engine module's entry point. Returns the table, or NULL when the engine
 * cannot serve an interface built for (apiMajor, apiMinor): a different MAJOR
 * always, and during 0.x any MINOR other than the engine's own.
 */
#define FOXAPI_ENGINE_QUERY_NAME "foxsdr_engine_query"
typedef const FoxEngineApi *(FOXAPI_CALL *FoxEngineQueryFn)(uint32_t apiMajor, uint32_t apiMinor);

/* ---------------------------------------------------------------------------
 * INTERFACE PLUGINS. An interface module exports foxsdr_interface_query().
 * The HOST opens the session (so the host, not the interface, decides the
 * grants) and hands the interface the table and the session. run() owns the
 * calling thread (the process main thread, which GLFW and macOS require)
 * until the user closes the interface or maxFrames is reached.
 * ------------------------------------------------------------------------- */
#define FOXAPI_RUN_HIDDEN 0x00000001u /* no visible window: render offscreen (tests) */

typedef struct FoxInterfaceRunParams {
    uint32_t structSize;
    uint32_t flags;             /* FOXAPI_RUN_* */
    uint64_t maxFrames;         /* 0 = until closed */
    int32_t width;              /* 0 = the interface's default */
    int32_t height;
    const char *uiPackagePath;  /* a foxsdr-ui/1 package (.foxui); NULL = built-in */
    const char *screenshotPath; /* write the last frame here as PNG; NULL = none */
    const char *testScript;     /* scripted input for tests; NULL = none */
} FoxInterfaceRunParams;

#define FOXAPI_MIN_INTERFACE_RUN_PARAMS (offsetof(FoxInterfaceRunParams, testScript) + sizeof(const char *))

typedef struct FoxInterfaceResult {
    uint32_t structSize;
    int32_t exitCode;           /* 0 = closed normally */
    uint64_t frames;
    uint64_t stateReads;        /* read_state calls */
    uint64_t spectrumFrames;    /* new spectrum frames consumed */
    uint64_t submitCalls;       /* submit() calls (one per frame at most) */
    uint64_t commands;          /* commands submitted */
    double meanFrameMs;
    double maxFrameMs;
    double meanApiUs;           /* time per frame spent inside API calls */
    char message[256];
} FoxInterfaceResult;

#define FOXAPI_MIN_INTERFACE_RESULT (offsetof(FoxInterfaceResult, message) + 256)

typedef struct FoxInterfaceDesc {
    uint32_t structSize;
    uint32_t apiMajor;          /* FOXAPI_VERSION_MAJOR it was built against */
    uint32_t apiMinor;
    uint32_t reserved0;
    uint64_t requiredCaps;      /* the engine must have ALL of these */
    const char *name;
    const char *version;
    const char *licence;        /* SPDX */
    int32_t (FOXAPI_CALL *run)(const FoxEngineApi *api, FoxSession *session,
                               const FoxInterfaceRunParams *params, FoxInterfaceResult *result);
} FoxInterfaceDesc;

#define FOXAPI_MIN_INTERFACE_DESC (offsetof(FoxInterfaceDesc, run) + sizeof(void (*)(void)))

/* An interface module's entry point: NULL when it cannot run on a host of
 * (apiMajor, apiMinor) - a different MAJOR always, during 0.x any other MINOR. */
#define FOXAPI_INTERFACE_QUERY_NAME "foxsdr_interface_query"
typedef const FoxInterfaceDesc *(FOXAPI_CALL *FoxInterfaceQueryFn)(uint32_t apiMajor, uint32_t apiMinor);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FOXSDR_API_H */
