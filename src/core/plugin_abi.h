/*
 * cascade plugin ABI - the binary contract between the host application and a
 * separately distributed plugin binary (.dll / .so / .dylib).
 *
 * SPDX-License-Identifier: MIT
 *
 * LICENCE EXCEPTION - READ THIS IF YOU ARE WRITING A PLUGIN.
 * The application itself is licensed under PolyForm Noncommercial 1.0.0 (see
 * LICENSE at the repository root), which requires a paid licence for
 * commercial use. THIS HEADER IS DELIBERATELY EXEMPT and is MIT-licensed.
 * Every plugin must include it, so licensing it restrictively would mean no
 * one could write a commercial plugin without our permission - which would
 * kill the plugin ecosystem before it began. Copy this file into your project
 * and build against it freely, for any purpose, commercial or not. Doing so
 * grants you no rights to the rest of the application, and imposes none of
 * its terms on your plugin: your plugin is yours, under whatever licence you
 * choose, and that licence is displayed to users before they install it.
 *
 * THIS FILE IS THE WHOLE CONTRACT. It is deliberately a *pure C* header with
 * no includes beyond <stddef.h>/<stdint.h>, no C++ types, no STL, no
 * templates and no inline code, so a third-party author can copy this single
 * file into their own project - in C or C++, built with any compiler - and
 * have everything they need. Nothing else from the cascade tree is required,
 * and a plugin never links against the host.
 *
 * Why a C ABI at all: the host ships as a commercial binary built with one
 * specific MSVC toolset and one set of runtime flags. A C++ interface would
 * drag std::string layout, iostream state, the exception machinery and the
 * allocator across the boundary, all of which differ between compilers and
 * even between debug/release CRTs of the same compiler. Plain C structs of
 * fixed-width scalars and function pointers have one layout that every
 * toolchain on a given platform agrees on.
 *
 * ============================ RULES FOR PLUGINS ============================
 *
 * 1. Export exactly ONE symbol: cascade_plugin_query (see below). Everything
 *    else the plugin needs is reached through the function pointers it hands
 *    back. Exporting nothing else keeps future host versions free to change
 *    how plugins are discovered without breaking anybody.
 *
 * 2. NO EXCEPTION MAY CROSS THE BOUNDARY, ever, through any callback in this
 *    header, including cascade_plugin_query itself. C++ exceptions are not
 *    part of the C ABI: throwing through a C function pointer into a host
 *    frame is undefined behaviour, and in practice terminates the process.
 *    Wrap every callback body in `try { ... } catch (...) { ... }` and report
 *    the failure through the return value instead. The host cannot protect
 *    you from a throw across the boundary and does not try.
 *
 * 3. NO LONGJMP, no thread that outlives destroy(), no exit()/abort(), no
 *    modification of the host's process-wide state (locale, FPU control word,
 *    SEH handlers, working directory, std handles).
 *
 * 4. Every pointer the plugin returns in CascadePluginDesc must have STATIC
 *    storage duration: the host reads the descriptor immediately, but the
 *    strings must remain valid for as long as the module is loaded.
 *
 * 5. The plugin must state its licence truthfully (see `licence`). The host
 *    displays it verbatim. cascade itself is MIT and is sold commercially;
 *    what a user chooses to install into their own copy is their business,
 *    but the host must never hide what it loaded.
 *
 * ====================== THREADING AND LIFETIME CONTRACT ====================
 *
 * cascade_plugin_query is called once per module, from the host thread that
 * performs the scan, before any other call. It must be reentrant-safe only
 * in the trivial sense of "returns the same static pointer every time".
 *
 * A decoder instance (the void* from create) is owned by exactly one host
 * thread at a time and is never used concurrently from two threads. create
 * and destroy are called from the host's control thread; process, retune and
 * poll_text are called from a real-time audio/DSP thread, so they must not
 * block, allocate unboundedly, or perform I/O. destroy happens-after the last
 * process/retune/poll_text for that instance.
 *
 * ============================ ABI 2 - WHAT CHANGED =========================
 *
 * Version 2 adds a SECOND capability, CASCADE_CAP_IQ_DECODER, for decoders
 * that need complex baseband rather than demodulated audio. ADS-B is the
 * driver: 1090 MHz, 2 MS/s minimum, pulse-position modulation on the raw
 * envelope. There is no audio stream in that problem at all, so the version-1
 * "float audio in, text out" shape could not express it, and neither could
 * DAB, digital-voice framing, or anything else that lives below the
 * demodulator.
 *
 * A version-2 plugin may declare CASCADE_CAP_DECODER, CASCADE_CAP_IQ_DECODER,
 * or both; each declared bit requires its own function-pointer table.
 *
 * MOVING A VERSION-1 PLUGIN TO VERSION 2 - the complete list:
 *   1. Recompile against this header (the version constant changed, so a
 *      plugin that hard-codes 1 is refused; use CASCADE_PLUGIN_ABI_VERSION).
 *   2. CascadePluginDesc gained ONE trailing member, `iqDecoder`. A
 *      brace-initialised static descriptor that lists every member by
 *      position must add a trailing NULL (C++ value-initialises the missing
 *      member, but relying on that makes the next addition silent).
 *      structSize is sizeof(CascadePluginDesc), so it changes on its own.
 *   3. Nothing else. CascadeDecoderApi - the audio table - is byte-for-byte
 *      unchanged in layout and semantics, so an audio decoder needs no code
 *      changes beyond the recompile and the trailing NULL.
 * There is deliberately no compatibility shim: see the versioning policy at
 * CASCADE_PLUGIN_ABI_VERSION below.
 */

#ifndef CASCADE_PLUGIN_ABI_H
#define CASCADE_PLUGIN_ABI_H

#include <stddef.h> /* size_t  */
#include <stdint.h> /* uint32_t, int32_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ABI version. The host REFUSES, with a message, any plugin whose
 * abiVersion is not EXACTLY equal to the host's value - not "less than or
 * equal", not "same major". A best-effort match is how a struct layout change
 * turns into a memory-corruption bug that surfaces days later in unrelated
 * code; refusing to load is the only outcome that is always safe.
 *
 * Bump this for ANY change to the structs, the callback signatures, or the
 * documented semantics of either.
 *
 * VERSIONING POLICY, stated once so nobody has to infer it. Every plugin
 * built against version 1 is refused by a version-2 host, including one that
 * only ever used facilities version 2 still has. That is deliberate, not an
 * oversight and not a temporary state:
 *
 *   - The descriptor GREW in version 2. A version-1 plugin's descriptor is
 *     shorter than this header describes, so reading the new trailing member
 *     out of it is an out-of-bounds read of the plugin's image. The host
 *     cannot know from the bytes alone which layout it is holding; it knows
 *     only what abiVersion claims, which is precisely why abiVersion is
 *     checked first and why the check is exact.
 *   - "Load it anyway if it only uses the old fields" means every future
 *     change has to be audited against every past layout, forever. Refusing
 *     is one rule with no matrix.
 *   - The cost is a recompile of the plugin against this header, which is a
 *     few seconds and which the author has to do anyway to build against a
 *     new host. The cost of the alternative is a memory-corruption bug in a
 *     paying customer's receiver.
 *
 * The refusal is reported with BOTH numbers ("host requires exactly 2, plugin
 * reports 1"), so an author never has to guess what happened.
 */
#define CASCADE_PLUGIN_ABI_VERSION 2

/* The one exported symbol's name, as the host looks it up. */
#define CASCADE_PLUGIN_ENTRY_NAME "cascade_plugin_query"

/* Export decoration for the plugin's definition of the entry point. */
#if defined(_WIN32)
#define CASCADE_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define CASCADE_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define CASCADE_PLUGIN_EXPORT
#endif

/*
 * Capability bits. A plugin sets one bit per facility it provides and must
 * populate the matching pointer in CascadePluginDesc.
 *
 * The host REFUSES a plugin that sets a bit it does not know, even alongside
 * known bits. That is consistent with the exact-version rule above: the set
 * of legal bits is frozen by CASCADE_PLUGIN_ABI_VERSION, so an unknown bit at
 * a matching ABI version means the descriptor is not what it claims to be.
 */
#define CASCADE_CAP_DECODER 0x00000001u
#define CASCADE_CAP_IQ_DECODER 0x00000002u
#define CASCADE_CAP_ALL_KNOWN 0x00000003u /* OR of every bit above */

/*
 * Rate bounds the host range-checks the tables against. A plugin that asks
 * for something outside these is refused at load time rather than at the
 * first block, because "your decoder wants 3 GS/s" is a message an author can
 * act on and a silent no-audio is not.
 *
 * Audio (CascadeDecoderApi.requiredRateHz): integer Hz, 0 or [1e3, 1e6].
 *
 * IQ (CascadeIqDecoderApi.requiredRateHz / preferredRateHz): 0 or
 * [8e3, 61.44e6]. Those are exactly the input rates the host's own DSP chain
 * accepts, so the upper bound is not arbitrary - it is the fastest stream the
 * application will ever have to hand out. 2.4 MS/s (ADS-B, RTL-SDR) and
 * 2.048 MS/s (DAB) sit comfortably inside it.
 */
#define CASCADE_AUDIO_RATE_MIN_HZ 1000u
#define CASCADE_AUDIO_RATE_MAX_HZ 1000000u
#define CASCADE_IQ_RATE_MIN_HZ 8000.0
#define CASCADE_IQ_RATE_MAX_HZ 61440000.0

/*
 * CASCADE_CAP_DECODER - a text decoder.
 *
 * Shape: demodulated real audio in, UTF-8 text lines out. This deliberately
 * carries no DSP objects across the boundary (no filters, no complex types,
 * no buffers the host owns), which is what keeps the ABI small enough to
 * freeze. It covers the decoders worth shipping separately - POCSAG/FLEX
 * pagers, RDS-style data, M17 metadata, CTCSS/DCS reporting - because all of
 * them are "audio in, text out" once the analog demodulator has run.
 *
 * The host feeds the post-demodulation, post-squelch audio stream: mono,
 * 32-bit float, nominally in [-1, +1] but NOT hard-clipped, at the rate the
 * decoder asked for in requiredRateHz (the host resamples). Samples are
 * contiguous and consecutive; block sizes vary between calls.
 *
 * EVERY function pointer here must be non-NULL, and none of them may throw.
 */
typedef struct CascadeDecoderApi {
    /* sizeof(CascadeDecoderApi) as the PLUGIN compiled it. Checked. */
    uint32_t structSize;

    /*
     * Audio sample rate the decoder needs, in Hz; the host resamples to it
     * and passes it to create(). 0 means "any rate", in which case the host
     * passes whatever it already has. Must be 0 or in
     * [CASCADE_AUDIO_RATE_MIN_HZ, CASCADE_AUDIO_RATE_MAX_HZ].
     */
    uint32_t requiredRateHz;

    /*
     * Creates one decoder instance for the given rate in Hz. Returns an
     * opaque handle, or NULL on failure (the host then reports the plugin as
     * unusable rather than calling anything else on it). Never throws.
     */
    void *(*create)(uint32_t rateHz);

    /*
     * Consumes `count` consecutive audio samples from `samples`. The pointer
     * is borrowed and valid only for the duration of the call - copy what you
     * need. `count` may be 0. Called from a real-time thread: no blocking, no
     * I/O, no unbounded allocation. Never throws.
     */
    void (*process)(void *handle, const float *samples, size_t count);

    /*
     * Retrieves decoded text. Writes at most `cap` bytes of UTF-8 into `buf`
     * and returns the number of bytes written:
     *     > 0  that many bytes were written
     *     = 0  nothing pending (the normal case; must be cheap)
     *     < 0  the decoder has failed permanently; the host stops polling it
     * No NUL terminator is written and none is expected - the host uses the
     * returned length. Output is text with '\n' as the line separator; a
     * partial line at the end of a poll is allowed and is continued by the
     * next one. Must never write more than `cap` bytes, and must split only
     * on a UTF-8 code point boundary. The host always passes cap >= 256.
     * Never throws.
     */
    int32_t (*poll_text)(void *handle, char *buf, size_t cap);

    /*
     * Destroys an instance created by create(). Called exactly once per
     * handle, after the last process/poll_text on it. Never throws.
     */
    void (*destroy)(void *handle);
} CascadeDecoderApi;

/*
 * CASCADE_CAP_IQ_DECODER - a text decoder fed COMPLEX BASEBAND (new in v2).
 *
 * Shape: interleaved float32 I/Q in, UTF-8 text lines out. Same output
 * contract as the audio decoder, entirely different input: this stream is the
 * receiver's raw baseband, before any demodulator, at the device's own sample
 * rate. It is what a mode needs when "the audio" does not exist as a concept:
 * ADS-B (2 MS/s, pulse-position modulation on the magnitude envelope), DAB
 * (2.048 MS/s OFDM), burst modes whose framing lives below the demodulator.
 *
 *   ***********************************************************************
 *   *  THE INTERLEAVING RULE - the single thing authors get wrong.        *
 *   *                                                                     *
 *   *  process(h, interleavedIq, frames) passes ONE array of              *
 *   *      2 * frames  floats,  laid out  I0, Q0, I1, Q1, I2, Q2, ...     *
 *   *                                                                     *
 *   *  `frames` counts COMPLEX SAMPLES, not floats and not bytes. The     *
 *   *  buffer is frames * 2 * sizeof(float) bytes long. Sample n is       *
 *   *  ( interleavedIq[2*n], interleavedIq[2*n + 1] ).                    *
 *   *                                                                     *
 *   *  Reading `frames` floats instead of 2*frames processes half the     *
 *   *  signal; treating it as `frames` bytes walks off the end. Both      *
 *   *  mistakes are silent. The layout is bit-compatible with a           *
 *   *  C99 `float _Complex[]` and with C++ `std::complex<float>[]`, so    *
 *   *  a reinterpret_cast of the pointer to either is legitimate and is   *
 *   *  the intended way to consume it in C++.                             *
 *   ***********************************************************************
 *
 * Scaling and spectrum: nominally in [-1, +1] per component but NOT
 * hard-clipped, and with no AGC applied - a decoder that needs amplitude
 * (ADS-B does) sees the true relative envelope. DC (bin zero) corresponds to
 * the RF frequency passed as centerHz; positive frequencies are above it.
 * Samples are contiguous and consecutive with no gaps between calls; block
 * sizes vary between calls and may be zero.
 *
 * Rates: the host delivers the stream at its input rate, which it passes to
 * create() and which is guaranteed to equal requiredRateHz when that is
 * non-zero. Set requiredRateHz to 0 to accept whatever the device is running
 * at (and then honour the rate create() was given); set preferredRateHz to
 * advertise what you would rather have - it is a hint the host may ignore
 * entirely, and it never changes what create() is told.
 *
 * THREADING - identical rules to the audio table, and they matter more here
 * because the block rate is the DEVICE rate, not the audio rate. create() and
 * destroy() come from the host's control thread. process(), retune() and
 * poll_text() are called from the real-time DSP thread and are serialised
 * with respect to each other (never concurrent), so they need no internal
 * locking - and must not block, must not allocate, must not perform I/O, and
 * must not throw. At 2 MS/s a 1024-frame block is roughly 500 microseconds of
 * budget; a mutex, a malloc or a log line will be heard as an audio dropout
 * in the rest of the application.
 *
 * EVERY function pointer here must be non-NULL EXCEPT retune, which may be
 * NULL if the decoder does not care where the receiver is tuned. The host
 * checks for NULL before every retune call.
 */
typedef struct CascadeIqDecoderApi {
    /* sizeof(CascadeIqDecoderApi) as the PLUGIN compiled it. Checked.
     * (Four bytes of padding follow on every 64-bit target, before the first
     * double; the host never reads them and they need not be initialised.) */
    uint32_t structSize;

    /*
     * Baseband sample rate the decoder REQUIRES, in Hz. 0 means "any rate":
     * the host passes whatever the device is running at and the decoder must
     * cope. Non-zero means the host feeds this decoder only when the input
     * rate equals it exactly. Must be 0 or in
     * [CASCADE_IQ_RATE_MIN_HZ, CASCADE_IQ_RATE_MAX_HZ].
     *
     * A double, not an integer, because SDR rates are not all integers:
     * 2.4e6 is, but a resampled 1.2288e6/1.5 is not, and rounding a rate is
     * how a bit clock drifts.
     */
    double requiredRateHz;

    /*
     * The rate the decoder would PREFER, in Hz - purely advisory. Meaningful
     * mainly alongside requiredRateHz == 0: "I work at anything, but I decode
     * best at 2.4 MS/s". The host may surface it, may retune the device to
     * it, and may ignore it completely; it is never what create() is told.
     * 0 means "no preference". Must be 0 or in the same range as above.
     */
    double preferredRateHz;

    /*
     * Creates one decoder instance. rateHz is the baseband sample rate the
     * stream will actually arrive at (equal to requiredRateHz when that is
     * non-zero) and centerHz is the RF frequency currently at DC. Returns an
     * opaque handle, or NULL on failure (the host then reports the plugin as
     * unusable rather than calling anything else on it). Never throws.
     */
    void *(*create)(double rateHz, double centerHz);

    /*
     * Consumes `frames` complex samples from `interleavedIq`, which holds
     * 2 * frames floats as I,Q,I,Q,... - see THE INTERLEAVING RULE above. The
     * pointer is borrowed and valid only for the duration of the call - copy
     * what you need. `frames` may be 0. Real-time thread: no blocking, no
     * I/O, no allocation. Never throws.
     */
    void (*process)(void *handle, const float *interleavedIq, size_t frames);

    /*
     * Tells the decoder the receiver moved: centerHz is the new RF frequency
     * at DC, effective from the next process() call. MAY BE NULL if the
     * decoder does not care (a decoder that only looks at the envelope
     * usually does not); the host checks before calling. Called on the same
     * thread as process(), never concurrently with it, under the same
     * real-time rules. Never throws.
     */
    void (*retune)(void *handle, double centerHz);

    /*
     * Retrieves decoded text. Byte-for-byte the same contract as
     * CascadeDecoderApi::poll_text: at most `cap` bytes of UTF-8 written,
     * return > 0 = bytes written, 0 = nothing pending (must be cheap), < 0 =
     * failed permanently and the host stops polling. No NUL is written or
     * expected; '\n' separates lines; split only on a code point boundary;
     * the host always passes cap >= 256. Never throws.
     */
    int32_t (*poll_text)(void *handle, char *buf, size_t cap);

    /*
     * Destroys an instance created by create(). Called exactly once per
     * handle, after the last process/retune/poll_text on it. Never throws.
     */
    void (*destroy)(void *handle);
} CascadeIqDecoderApi;

/*
 * The plugin descriptor: static, immutable, returned by the entry point.
 *
 * LAYOUT RULE: structSize and abiVersion are the first two fields, in that
 * order, and MUST NEVER MOVE in any future ABI version. They are the only
 * fields the host may read before it has established that it understands the
 * layout, so they are the fixed point the whole compatibility check stands
 * on. Everything after them is fair game to change (with a version bump).
 *
 * Version 2 is what that rule is FOR: the struct grew a trailing member and
 * the two frozen fields at the front are what let the host detect a
 * version-1 descriptor safely, without reading a single byte whose meaning
 * depends on the layout it has not yet confirmed.
 */
typedef struct CascadePluginDesc {
    /* sizeof(CascadePluginDesc) as the PLUGIN compiled it. Second guard,
     * after abiVersion: it catches a plugin built against an edited or
     * mismatched copy of this header that forgot to bump the version, and it
     * catches a 32/64-bit mixup. */
    uint32_t structSize;

    /* Must equal the host's CASCADE_PLUGIN_ABI_VERSION exactly. */
    uint32_t abiVersion;

    /* Short identifier shown in the UI, e.g. "POCSAG". Non-NULL, non-empty,
     * ASCII or UTF-8, no newlines. Static storage. */
    const char *name;

    /* The PLUGIN's own version string, e.g. "1.2.0". Non-NULL, non-empty. */
    const char *version;

    /* Author or organisation. Non-NULL (may be empty). */
    const char *author;

    /* SPDX identifier where possible ("MIT", "GPL-3.0-only", "Proprietary").
     * Non-NULL, non-empty; displayed verbatim by the host. */
    const char *licence;

    /* OR of CASCADE_CAP_* bits. Must be non-zero and contain no unknown
     * bits. Declaring BOTH decoder bits is legal and expected of a plugin
     * that ships, say, an audio POCSAG decoder and an IQ ADS-B decoder in one
     * module: each declared bit simply needs its table. */
    uint32_t capabilities;

    /* Must be 0. Reserved so a future ABI can add a small scalar without
     * changing the struct's size on 64-bit (it currently occupies the
     * padding that would otherwise sit before `decoder`). */
    uint32_t reserved;

    /* Non-NULL if and only if CASCADE_CAP_DECODER is set. Static storage.
     * The host refuses the plugin if the bit is set and this is NULL; if the
     * bit is NOT set it ignores this pointer entirely and never calls
     * through it, so leave it NULL. */
    const CascadeDecoderApi *decoder;

    /* Added in ABI 2, and the reason ABI 2 exists. Non-NULL if and only if
     * CASCADE_CAP_IQ_DECODER is set; same host treatment as `decoder` above.
     * A version-1 plugin has no such member, which is exactly why a version-1
     * descriptor cannot be read with this layout and is refused outright. */
    const CascadeIqDecoderApi *iqDecoder;
} CascadePluginDesc;

/*
 * The entry point. The host calls:
 *
 *     const CascadePluginDesc *d = cascade_plugin_query(CASCADE_PLUGIN_ABI_VERSION);
 *
 * Return a pointer to a static descriptor, or NULL if the plugin cannot
 * support this host (typically hostAbiVersion != the version it was built
 * against). Returning NULL is the polite refusal; the host records it as
 * "plugin declined" and moves on. Returning a descriptor whose abiVersion
 * disagrees with the host is refused too - the host does not trust the
 * plugin's own compatibility judgement, it checks.
 *
 * Must not throw. Must not have side effects beyond initialising the
 * plugin's own static data; in particular do not start threads or open
 * devices here - that belongs in create().
 */
typedef const CascadePluginDesc *(*CascadePluginQueryFn)(uint32_t hostAbiVersion);

/* The prototype is declared only when BUILDING a plugin, so that including
 * this header in the host (or in a tool) never declares a dllexport symbol
 * the includer does not define. */
#if defined(CASCADE_PLUGIN_BUILD)
CASCADE_PLUGIN_EXPORT const CascadePluginDesc *cascade_plugin_query(uint32_t hostAbiVersion);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CASCADE_PLUGIN_ABI_H */
