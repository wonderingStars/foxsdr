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
 * ====================== ABI 3 - WHAT CHANGED, AND WHY ======================
 *
 * Version 3 exists to be THE LAST BREAKING VERSION. Versions 1 and 2 each
 * added a capability as a trailing pointer in CascadePluginDesc, which grew
 * the struct, which meant an older host reading a newer descriptor would read
 * past the end of it - so the only safe rule was to refuse any version that
 * was not an exact match, and every new capability became a flag day for every
 * plugin ever built.
 *
 * That does not scale, and the third capability is what forced the issue.
 * Version 3 therefore moves the tables OUT of the descriptor:
 *
 *   - CascadePluginDesc no longer has `decoder` and `iqDecoder` members. It
 *     carries `capabilityCount` and `capabilityTables`, a counted array of
 *     CascadeCapabilityEntry - each one a {capability bit, table size, table
 *     pointer}. The struct is now a fixed 56 bytes on 64-bit and stays that
 *     size however many capabilities are added later.
 *   - Unknown capability bits are IGNORED rather than refused. The host uses
 *     what it recognises and never dereferences a table it does not
 *     understand, so a plugin built for a later host still works here for
 *     whatever it has in common.
 *   - Each entry carries its own tableSize. If one table ever has to grow,
 *     only plugins using THAT table lose THAT capability; everything else
 *     keeps working.
 *
 * The consequence worth stating plainly: after this, adding a capability needs
 * a new bit and a new table type, and needs NO new ABI version. Nobody's
 * plugin stops loading. If a table's shape turns out to be wrong, it gets a
 * new bit rather than a bigger struct, and old and new can be declared side by
 * side by the same plugin.
 *
 * Version 3 also adds CASCADE_CAP_IMAGE_DECODER, for decoders whose output is
 * a picture rather than text - SSTV, Meteor-M LRPT, HRPT, the GOES products.
 *
 * MOVING A VERSION-2 PLUGIN TO VERSION 3 - the complete list:
 *   1. Recompile against this header.
 *   2. Replace the trailing `&decoderApi` / `&iqApi` members of the static
 *      descriptor with a capability table:
 *
 *        static const CascadeCapabilityEntry kCaps[] = {
 *            {CASCADE_CAP_DECODER, (uint32_t)sizeof(CascadeDecoderApi), &kAudioApi},
 *        };
 *        ... , CASCADE_CAP_DECODER, 1u, kCaps };
 *
 *      structSize is sizeof(CascadePluginDesc) and changes on its own.
 *   3. Nothing else. CascadeDecoderApi and CascadeIqDecoderApi are
 *      byte-for-byte unchanged in layout and semantics, so the decoder code
 *      itself needs no changes at all.
 *
 * There is deliberately no compatibility shim for versions 1 and 2: see the
 * versioning policy at CASCADE_PLUGIN_ABI_VERSION below.
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
 * VERSIONING POLICY, stated once so nobody has to infer it. Every plugin built
 * against version 1 or 2 is refused by a version-3 host, including one that
 * only ever used facilities version 3 still has. That is deliberate:
 *
 *   - The descriptor CHANGED SHAPE in each of those versions - it grew in 2,
 *     and in 3 the trailing table pointers were replaced by a counted array.
 *     An older plugin's descriptor is therefore not the layout this header
 *     describes, and reading it as if it were is an out-of-bounds read of the
 *     plugin's image. The host cannot know from the bytes alone which layout
 *     it is holding; it knows only what abiVersion claims, which is precisely
 *     why abiVersion is checked FIRST and why the check is exact.
 *   - "Load it anyway if it only uses the old fields" means every future
 *     change has to be audited against every past layout, forever. Refusing
 *     is one rule with no matrix.
 *   - The cost is a recompile against this header - seconds of work, and the
 *     catalogue's plugins are all maintained in-tree precisely so that cost
 *     falls on the maintainers rather than on users. The cost of the
 *     alternative is a memory-corruption bug in a paying customer's receiver.
 *
 * WHY THIS SHOULD BE THE LAST TIME. The rule above is safe but expensive, and
 * version 3 removes the thing that kept triggering it. The descriptor is now
 * fixed-size, capabilities live behind a counted array, unknown bits are
 * ignored, and each table carries its own size. A future capability is
 * additive: new bit, new table type, NO version bump, nothing stops loading.
 * If you find yourself about to increment this constant, check first whether
 * a new capability bit would do the job instead - it almost certainly will.
 *
 * The refusal is reported with BOTH numbers ("host requires exactly 3, plugin
 * reports 2"), so an author never has to guess what happened.
 */
#define CASCADE_PLUGIN_ABI_VERSION 3

/* The one exported symbol's name, as the host looks it up. */
#define CASCADE_PLUGIN_ENTRY_NAME "cascade_plugin_query"

/*
 * Canonical way to get one capability's table out of a descriptor, defined
 * here so the host, the plugins and their tests all read the array the same
 * way instead of each hand-rolling the loop and each getting the malformed
 * cases subtly differently.
 *
 * Returns NULL unless the entry names EXACTLY this one capability and carries
 * a non-NULL table. The caller must still check the table's own structSize
 * before casting - that is per-capability and cannot be done here.
 *
 * Declared below CascadePluginDesc; see the definition after the struct.
 */
#define CASCADE_HAVE_CAPABILITY_LOOKUP 1

/* Export decoration for the plugin's definition of the entry point. */
#if defined(_WIN32)
#define CASCADE_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define CASCADE_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define CASCADE_PLUGIN_EXPORT
#endif

/*
 * Capability bits. A plugin sets one bit per facility it provides and supplies
 * a matching entry in the descriptor's capability table.
 *
 * UNKNOWN BITS ARE IGNORED, NOT REFUSED - changed in version 3, and the reason
 * version 3 exists. In versions 1 and 2 each capability was a trailing pointer
 * in the descriptor, so adding one GREW the struct, so an older host reading a
 * newer descriptor would read off the end of it. Refusing outright was then the
 * only safe answer, and every new capability meant a new ABI version and a
 * flag day for every plugin in existence.
 *
 * Version 3 moves the tables out of the struct and behind a counted array (see
 * CascadeCapabilityEntry). The descriptor is now a FIXED size for ever, so a
 * plugin declaring a capability this host has never heard of is no longer a
 * malformed descriptor - it is simply a plugin that does more than this host
 * can use. The host uses the bits it knows, ignores the rest, and never
 * dereferences a table it does not understand.
 *
 * The practical consequence: adding a capability after this is ADDITIVE. It
 * needs a new bit and a new table type, and it does NOT need a new ABI version
 * and does NOT stop any existing plugin from loading. This should be the last
 * breaking change to this header.
 */
#define CASCADE_CAP_DECODER 0x00000001u
#define CASCADE_CAP_IQ_DECODER 0x00000002u
#define CASCADE_CAP_IMAGE_DECODER 0x00000004u
#define CASCADE_CAP_TRACK_SOURCE 0x00000008u
#define CASCADE_CAP_PANEL 0x00000010u
#define CASCADE_CAP_HOST_CLIENT 0x00000020u
#define CASCADE_CAP_PRESET 0x00000040u
#define CASCADE_CAP_BASEMAP 0x00000080u
#define CASCADE_CAP_ALL_KNOWN 0x000000FFu /* OR of every bit THIS host knows */

/*
 * The four bits above 0x04 were added WITHOUT an ABI bump, which is the whole
 * claim ABI 3 made and this is the proof of it: they are new entries in the
 * capability table, the descriptor did not change size, and every plugin built
 * before they existed still loads and runs untouched.
 *
 * They also change what a plugin IS. Up to here a plugin was a decoder - bytes
 * in, text or pictures out. TRACK_SOURCE and PANEL let a plugin contribute to
 * the user interface, and HOST_CLIENT lets it act on the receiver. A satellite
 * tracker is the case that forced all three: it consumes no signal at all, it
 * needs a map and a table of passes, and it has to Doppler-correct the VFO.
 *
 * A plugin declaring ONLY these and no decoder is therefore legitimate and
 * must load.
 */

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
 * ONE CAPABILITY, ONE ENTRY. The descriptor carries a counted array of these
 * instead of a pointer per capability, which is what makes the descriptor a
 * fixed size and every future capability additive.
 *
 * `tableSize` is sizeof the pointed-to table AS THE PLUGIN COMPILED IT. The
 * host compares it against its own sizeof for that capability and, on a
 * mismatch, SKIPS THAT ONE CAPABILITY while still using the others. That is
 * the second half of the forward-compatibility story: if a table ever has to
 * grow, only the plugins using that table lose that facility, instead of every
 * plugin in the catalogue failing to load.
 *
 * A table that needs to change incompatibly should get a NEW capability bit
 * rather than a bigger struct, and then old and new can be declared side by
 * side by the same plugin.
 */
typedef struct CascadeCapabilityEntry {
    /* Exactly ONE CASCADE_CAP_* bit. An entry naming two capabilities, or
     * none, is malformed and the host skips it. */
    uint32_t capability;

    /* sizeof(the table `table` points at), as the plugin compiled it. */
    uint32_t tableSize;

    /* The capability's API table: CascadeDecoderApi for CASCADE_CAP_DECODER,
     * CascadeIqDecoderApi for CASCADE_CAP_IQ_DECODER, and so on. Static
     * storage, non-NULL. Typed as void* because the array is heterogeneous;
     * the host casts only after matching BOTH the bit and tableSize. */
    const void *table;
} CascadeCapabilityEntry;

/*
 * Pixel formats an image decoder may produce. Deliberately only two, both
 * trivially uploadable as a texture with no conversion table and no palette:
 * a weather-satellite or SSTV frame is either luminance or RGB.
 */
#define CASCADE_IMAGE_GRAY8 1u  /* 1 byte per pixel */
#define CASCADE_IMAGE_RGB24 2u  /* 3 bytes per pixel, R,G,B */

/* Sanity bounds the host range-checks a produced image against, so a corrupt
 * width/height cannot make the host allocate or read absurd amounts. An LRPT
 * frame is ~1568 px wide and an HRPT line ~2048; 16384 leaves generous room. */
#define CASCADE_IMAGE_MAX_DIM 16384u

/*
 * An image handed from plugin to host. The PIXELS ARE OWNED BY THE PLUGIN and
 * borrowed by the host only until release_image() is called for that image, so
 * nothing is copied on the boundary and neither side frees the other's memory.
 */
typedef struct CascadeImage {
    /* sizeof(CascadeImage) as the HOST compiled it; the host fills this in
     * before calling poll_image so the plugin can tell what it is filling. */
    uint32_t structSize;

    uint32_t width;   /* 1..CASCADE_IMAGE_MAX_DIM */
    uint32_t height;  /* 1..CASCADE_IMAGE_MAX_DIM */
    uint32_t format;  /* CASCADE_IMAGE_GRAY8 or CASCADE_IMAGE_RGB24 */

    /* Bytes per row. May exceed width * bytesPerPixel for alignment; must not
     * be less. The host reads `height` rows of width*bpp bytes at `stride`
     * intervals from `pixels`. */
    uint32_t stride;

    /* 0 while the image is still being received, 1 when finished. A slow-scan
     * mode takes tens of seconds per frame, and showing it building is the
     * difference between "working" and "frozen", so partial frames are
     * expected and are not an error. */
    int32_t complete;

    /* Increments per image. Lets the host tell a new frame from an update to
     * the one it is already displaying. */
    uint64_t sequence;

    /* width*height*bpp bytes readable at `stride` per row. Valid until
     * release_image() for this image, or until destroy(). Never NULL. */
    const uint8_t *pixels;
} CascadeImage;

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
/*
 * CASCADE_CAP_IMAGE_DECODER - a decoder whose output is a picture.
 *
 * Added in version 3, and the reason the image types above exist. SSTV,
 * Meteor-M LRPT, HRPT and the GOES products all end in a bitmap, and the
 * text-out shape could not express that at all.
 *
 * `inputKind` says what stream this wants: CASCADE_INPUT_AUDIO for
 * demodulated audio (SSTV) or CASCADE_INPUT_IQ for complex baseband (LRPT).
 * One table serves both because everything after the input differs not at all
 * - the host feeds one buffer and polls for pictures either way.
 *
 * poll_text is still here, and is not optional: a decoder that is receiving
 * but has produced no picture yet must be able to say so ("VIS 60, 320x256,
 * 34% received"), or a slow mode is indistinguishable from a broken one.
 *
 * EVERY function pointer here must be non-NULL, and none of them may throw.
 */
#define CASCADE_INPUT_AUDIO 1u
#define CASCADE_INPUT_IQ 2u

/* ==========================================================================
 * CASCADE_CAP_TRACK_SOURCE - things at a place, drawn on the host's map.
 *
 * Deliberately ONE type for aircraft, ships, stations and satellites. They
 * differ in almost nothing that a map cares about: an identifier, a position,
 * how fast it is going and which way, and how long ago it was heard. Giving
 * each its own capability would have meant four near-identical tables and four
 * renderers, and a user who wants aircraft and ships on one map would have got
 * two maps.
 *
 * Fixed-size character arrays rather than pointers, and the host COPIES what
 * it is given. Tracks are small, there are hundreds at most, and the
 * alternative - a borrow with a release call, as the image API needs - would
 * put a lifetime contract around data that is cheaper to copy than to reason
 * about. The image API borrows because a frame is megabytes; a track is bytes.
 * ==========================================================================
 */

#define CASCADE_TRACK_ID_CHARS 24
#define CASCADE_TRACK_LABEL_CHARS 40

/* What the thing is. The host uses it to pick a symbol and a colour, and
 * nothing else - an unknown kind draws as a plain dot rather than being
 * rejected, so a future kind needs no ABI change here either. */
#define CASCADE_TRACK_UNKNOWN 0u
#define CASCADE_TRACK_AIRCRAFT 1u
#define CASCADE_TRACK_VESSEL 2u
#define CASCADE_TRACK_STATION 3u   /* fixed: an APRS digipeater, a base */
#define CASCADE_TRACK_SATELLITE 4u

/* Optional per-track hints. */
#define CASCADE_TRACK_FLAG_EMERGENCY 0x00000001u
#define CASCADE_TRACK_FLAG_SELECTED 0x00000002u

typedef struct CascadeTrack {
    /* Stable across updates - it is how the host knows this is the same thing
     * moving rather than a new thing appearing. An ICAO address, an MMSI, a
     * callsign, a NORAD number. Non-empty, NUL-terminated. */
    char id[CASCADE_TRACK_ID_CHARS];

    /* What to draw beside it. May be empty, in which case the host shows the
     * id - a target with no label must never render as an unlabelled dot the
     * user cannot identify. NUL-terminated. */
    char label[CASCADE_TRACK_LABEL_CHARS];

    double latDeg;  /* -90..90   */
    double lonDeg;  /* -180..180 */

    /* Metres above mean sea level. NaN when the source does not report it -
     * NOT 0, which is a real altitude and would put every ship at sea level
     * by accident and every aircraft with unknown altitude on the ground. */
    double altM;

    /* Degrees true, 0..360, and metres per second. NaN when unknown, for the
     * same reason: 0 is a real course and a real speed. */
    double courseDeg;
    double speedMps;

    /* Milliseconds since this track was last UPDATED by the plugin. The host
     * fades and eventually drops stale targets, and it cannot compute this
     * itself - only the plugin knows when it last actually heard from the
     * thing, as opposed to when it last happened to be polled. */
    uint64_t ageMs;

    uint32_t kind;   /* CASCADE_TRACK_* */
    uint32_t flags;  /* CASCADE_TRACK_FLAG_* */
} CascadeTrack;

/*
 * A polyline: a satellite's ground track, an aircraft's trail, a footprint
 * circle. Separate from CascadeTrack because a path is a shape rather than a
 * position, and squeezing one into the other would have meant either a
 * pointer inside CascadeTrack (a lifetime contract) or one track per vertex
 * (hundreds of phantom targets on the map).
 */
#define CASCADE_PATH_FLAG_CLOSED 0x00000001u /* join last vertex to first */
#define CASCADE_PATH_FLAG_DASHED 0x00000002u

typedef struct CascadePathPoint {
    double latDeg;
    double lonDeg;
} CascadePathPoint;

typedef struct CascadePath {
    char id[CASCADE_TRACK_ID_CHARS];
    uint32_t kind;   /* CASCADE_TRACK_*, so a path can be coloured like its owner */
    uint32_t flags;  /* CASCADE_PATH_FLAG_* */
    /* Vertices, owned by the PLUGIN and valid until the next poll_paths call
     * or destroy - whichever comes first. A path can be hundreds of points
     * (a full orbit), which is the one place in this API where copying every
     * poll would be wasteful enough to matter. */
    const CascadePathPoint *points;
    uint32_t count;
} CascadePath;

typedef struct CascadeTrackSourceApi {
    uint32_t structSize;

    /* Creates the source. Takes no rate: a track source is not fed a signal.
     * Something that decodes AND plots declares both capabilities and gets two
     * instances, which is deliberate - the decoder half is real-time and the
     * plotting half is not, and they should not share a lifetime. */
    void *(*create)(void);

    /* Fills up to `cap` tracks and returns how many were written, or negative
     * on permanent failure. Called from the GUI thread at frame rate, so it
     * must be cheap and must not block - it is a read of state the plugin has
     * already computed, never the place to compute it. */
    int32_t (*poll_tracks)(void *handle, CascadeTrack *out, uint32_t cap);

    /* Same contract for polylines. MAY BE NULL - most sources have no paths -
     * and the host checks before calling. */
    int32_t (*poll_paths)(void *handle, CascadePath *out, uint32_t cap);

    void (*destroy)(void *handle);
} CascadeTrackSourceApi;

/* ==========================================================================
 * CASCADE_CAP_PANEL - a window of the plugin's own, drawn by the host.
 *
 * The plugin describes WHAT to show; the host decides how. That boundary is
 * the point. The alternatives were to hand plugins the host's ImGui context -
 * which welds every plugin to one exact ImGui build, the precise class of
 * coupling ABI 3 exists to end - or to let plugins open real OS windows, which
 * means every plugin ships a GUI stack, runs its own event loop, and can take
 * the display down with it.
 *
 * The cost is that a plugin cannot draw anything the host has no widget for.
 * That is accepted: a pass table, a status readout and a progress bar covers
 * what these plugins actually need, and a genuinely novel visual can come
 * later as another capability without disturbing this one.
 * ==========================================================================
 */

#define CASCADE_PANEL_TITLE_CHARS 40
#define CASCADE_PANEL_MAX_COLUMNS 8
#define CASCADE_PANEL_CELL_CHARS 32

/* Row kinds. An unknown kind is skipped, not refused. */
#define CASCADE_ROW_CELLS 0u      /* a table row: cells[0..columns-1] */
#define CASCADE_ROW_HEADING 1u    /* cells[0] as a section heading */
#define CASCADE_ROW_SEPARATOR 2u  /* a rule; cells ignored */

/* Per-row emphasis. Advisory - the host may render these however it likes. */
#define CASCADE_ROW_FLAG_GOOD 0x00000001u
#define CASCADE_ROW_FLAG_WARN 0x00000002u
#define CASCADE_ROW_FLAG_MUTED 0x00000004u

typedef struct CascadePanelRow {
    uint32_t kind;
    uint32_t flags;
    char cells[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS];
} CascadePanelRow;

typedef struct CascadePanelApi {
    uint32_t structSize;

    /* Window title, e.g. "Satellite passes". Static storage, non-NULL. */
    const char *title;

    void *(*create)(void);

    /* Number of columns, 1..CASCADE_PANEL_MAX_COLUMNS, and their headings.
     * Called once after create() - the shape of a panel is fixed for its
     * lifetime, so the host can build the table once instead of rediscovering
     * it every frame. `headings` is filled by the plugin. */
    uint32_t (*columns)(void *handle,
                        char headings[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS]);

    /* Fills up to `cap` rows, returns how many, negative on permanent
     * failure. GUI thread, at frame rate: cheap, non-blocking, no I/O. */
    int32_t (*poll_rows)(void *handle, CascadePanelRow *out, uint32_t cap);

    void (*destroy)(void *handle);
} CascadePanelApi;

/* ==========================================================================
 * CASCADE_CAP_HOST_CLIENT - the only capability that points the other way.
 *
 * Everything else is the plugin producing something. This one lets a plugin
 * ASK THE HOST for things: what the receiver is doing, what the time is, and -
 * the reason it exists - to please retune. A satellite tracker that cannot
 * Doppler-correct is not a satellite tracker.
 *
 * THE SAFETY RULE, because a plugin that can move the VFO can also fight the
 * user for it: a tune request is a REQUEST. The host refuses unless the user
 * has explicitly given that plugin control, and may refuse at any time for any
 * reason. request_tune returns 0 when it was honoured and a negative
 * CASCADE_TUNE_* code when it was not, so a plugin can tell "you are not
 * allowed" from "that frequency is out of range" and say something useful
 * instead of silently failing to track.
 * ==========================================================================
 */

#define CASCADE_TUNE_OK 0
#define CASCADE_TUNE_DENIED (-1)     /* the user has not granted control */
#define CASCADE_TUNE_OUT_OF_RANGE (-2)
#define CASCADE_TUNE_NO_DEVICE (-3)
#define CASCADE_TUNE_FAILED (-4)     /* the device refused or errored */

typedef struct CascadeHostApi {
    uint32_t structSize;

    /* Opaque host context. Pass it back unchanged as the first argument of
     * every call below; it is not a pointer the plugin may inspect. */
    void *ctx;

    /* Where the receiver is tuned, in Hz, and what it is sampling at. 0 when
     * no device is open. */
    double (*centre_hz)(void *ctx);
    double (*sample_rate_hz)(void *ctx);

    /* Asks the receiver to tune. See the safety rule above. */
    int32_t (*request_tune)(void *ctx, double centreHz);

    /* Milliseconds since the Unix epoch, UTC. Supplied by the host rather
     * than taken from the plugin's own clock so that everything in the
     * application agrees on the time - and so a future recorded-playback mode
     * can hand a plugin the timestamp of the SAMPLES rather than the wall
     * clock, which is exactly what a pass predictor replaying a capture
     * needs. */
    int64_t (*unix_time_ms)(void *ctx);
} CascadeHostApi;

/*
 * ==========================================================================
 * CASCADE_CAP_PRESET - "where do I listen for this?"
 * ==========================================================================
 *
 * A decoder knows the frequency it is for and the user usually does not.
 * Installing an ADS-B decoder and then having to find out for yourself that
 * ADS-B lives at 1090 MHz, and that it wants 2 MS/s of raw bandwidth rather
 * than a narrow channel, is the difference between a plugin that works when
 * you click it and one that appears to do nothing.
 *
 * So a plugin may declare the places it expects to be used. The host offers
 * them as one click each: tune there, put the receiver into the right mode,
 * and open whatever windows that plugin contributes.
 *
 * THIS IS A SUGGESTION, NOT A TUNE. Nothing here lets a plugin move the
 * receiver - only the USER clicking one of these does, and CASCADE_CAP_
 * HOST_CLIENT with its separate per-plugin permission remains the only way a
 * plugin can retune anything on its own. A plugin declaring a preset is
 * publishing a fact about the mode it decodes, in the same spirit as its name
 * and its licence.
 *
 * WHY A NEW CAPABILITY BIT AND NOT AN ABI BUMP. This is exactly the case
 * version 3 restructured the descriptor for: a new bit plus a new table type,
 * costing nothing to plugins that do not declare it and refusing nothing on a
 * host that does not know it. Plugins built before this bit existed keep
 * loading unchanged.
 */

#define CASCADE_PRESET_LABEL_CHARS 48

/* Demodulator, matching what the host offers. A preset naming a mode the host
 * does not implement is ignored rather than refused - the tune still
 * happens, which is the part that matters. */
#define CASCADE_DEMOD_UNCHANGED 0u /* leave the receiver's mode alone */
#define CASCADE_DEMOD_NFM 1u
#define CASCADE_DEMOD_WFM 2u
#define CASCADE_DEMOD_AM 3u
#define CASCADE_DEMOD_DSB 4u
#define CASCADE_DEMOD_USB 5u
#define CASCADE_DEMOD_CW 6u
#define CASCADE_DEMOD_LSB 7u
#define CASCADE_DEMOD_RAW 8u

/*
 * Tune the DEVICE's centre frequency to `frequencyHz` rather than putting the
 * VFO there.
 *
 * The distinction is not cosmetic and gets one of the two kinds of decoder
 * wrong if ignored. An audio decoder wants its signal in the tuned channel, so
 * the VFO goes to the frequency. An I/Q decoder is handed the whole raw device
 * band and does its own tuning inside it, so what it needs is the BAND to
 * contain its signal - and ADS-B at 1090 MHz with a VFO offset of a few
 * hundred kHz would otherwise sit the device slightly off, which is legal but
 * pointless.
 */
#define CASCADE_PRESET_DEVICE_CENTRE 0x00000001u

typedef struct CascadePreset {
    /* sizeof(CascadePreset) as the HOST compiled it; the host fills this in
     * before the call so the plugin can tell what it is filling. */
    uint32_t structSize;

    /* Shown on the button, e.g. "ADS-B 1090 MHz" or "APRS (Europe)". A plugin
     * with one preset may repeat its own name; a plugin with several must
     * distinguish them, because the label is all the user sees. NUL-terminated
     * UTF-8, no newlines. */
    char label[CASCADE_PRESET_LABEL_CHARS];

    /* Where to listen, in Hz. Must be > 0. */
    double frequencyHz;

    /* CASCADE_DEMOD_*. */
    uint32_t demodMode;

    /* Channel bandwidth in Hz, or 0 to leave the receiver's alone. */
    double bandwidthHz;

    /* Device sample rate the decoder needs, in Hz, or 0 for "no preference".
     * ADS-B needs at least 2 MS/s and simply cannot work below it, which is
     * the sort of thing a user should not have to discover from a decoder that
     * silently produces nothing. Advisory: the host applies it only if the
     * open device supports it. */
    double sampleRateHz;

    /* OR of CASCADE_PRESET_* flags. */
    uint32_t flags;
} CascadePreset;

typedef struct CascadePresetApi {
    /* sizeof(CascadePresetApi) as the PLUGIN compiled it. Checked. */
    uint32_t structSize;

    /* How many presets this plugin offers. Bounded by the host at a small
     * number; a plugin claiming hundreds is not one whose list belongs in a
     * menu. Must not throw. */
    uint32_t (*count)(void);

    /* Fills `out` for 0 <= index < count(). Returns 1 on success and 0 if the
     * index is out of range or the plugin declines to describe it. Called on
     * the GUI thread, never in real time, and not tied to any instance - a
     * preset is a property of the PLUGIN, not of a running decoder, so this
     * takes no handle and may be called before anything is created. */
    int32_t (*get)(uint32_t index, CascadePreset *out);
} CascadePresetApi;

/*
 * ==========================================================================
 * CASCADE_CAP_BASEMAP - map imagery, supplied by a plugin
 * ==========================================================================
 *
 * The host ships a coastline drawn from public-domain vector data, and that is
 * a deliberate floor rather than an ambition: it is small, offline, and free of
 * conditions a sold product would have to answer for. Real street-level map
 * imagery is a different matter - OpenStreetMap-derived tiles carry ODbL, and
 * the public tile servers exclude commercial use - so it cannot ship IN the
 * application.
 *
 * It can, however, arrive as a plugin the user installs and points at a tile
 * server they run themselves. That keeps the licence question where it
 * belongs: with the person who chose to serve the tiles. Install the plugin and
 * the map gets imagery; remove it and the map is exactly what it was.
 *
 * TILE SCHEME. The standard slippy-map XYZ arrangement: Web Mercator, zoom z,
 * with x increasing east from the antimeridian and y increasing SOUTH from the
 * north edge, 2^z tiles each way. This is what every raster tile server speaks,
 * so a plugin is a fetch and a decode rather than a coordinate exercise.
 *
 * THE HOST DRAWS IN MERCATOR WHILE A BASEMAP IS ACTIVE and reverts to
 * equirectangular without one. Tiles are Mercator by construction and
 * reprojecting them per frame would cost sharpness and time to preserve a
 * projection whose advantage - not misplacing polar orbits - matters precisely
 * when a satellite is being tracked, which is when street imagery is least
 * useful anyway.
 *
 * get_tile MUST NOT BLOCK. It is called for every visible tile of every
 * rendered frame, on the GUI thread. A plugin that fetches over a network must
 * start the request, return CASCADE_TILE_PENDING immediately, and answer
 * CASCADE_TILE_READY on some later call - a plugin that waits for the network
 * here freezes the whole application, radio included.
 */

/* A tile's pixels are OWNED BY THE PLUGIN and borrowed by the host between
 * get_tile and release_tile, exactly as an image decoder's are. */
typedef struct CascadeTile {
    /* sizeof(CascadeTile) as the HOST compiled it; filled before the call. */
    uint32_t structSize;

    uint32_t width;  /* normally 256; must equal the declared tileSize */
    uint32_t height;
    uint32_t format; /* CASCADE_IMAGE_RGB24 - the only format for now */
    uint32_t stride; /* bytes per row; >= width * 3 */

    const uint8_t *pixels; /* valid until release_tile, never NULL */
} CascadeTile;

#define CASCADE_TILE_READY 1
#define CASCADE_TILE_PENDING 0  /* being fetched; ask again on a later frame */
#define CASCADE_TILE_MISSING (-1) /* will never exist - do not ask again */

typedef struct CascadeBasemapApi {
    /* sizeof(CascadeBasemapApi) as the PLUGIN compiled it. Checked. */
    uint32_t structSize;

    /*
     * Attribution the host displays whenever this basemap is on screen.
     * Non-NULL and non-empty, and the host REFUSES the capability without it.
     *
     * That is not politeness. OpenStreetMap-derived data is ODbL and requires
     * attribution; a basemap plugin that could omit it would quietly put the
     * user in breach, and the whole reason imagery lives in a plugin is to keep
     * that obligation visible and attached to whoever supplies the tiles.
     */
    const char *attribution;

    uint32_t minZoom;
    uint32_t maxZoom;
    uint32_t tileSize; /* edge length in pixels; 64..CASCADE_TILE_SIZE_MAX */

    /* One instance. NULL on failure. */
    void *(*create)(void);

    /*
     * Offers the tile at (z, x, y). Returns CASCADE_TILE_READY with `out`
     * filled, CASCADE_TILE_PENDING if it is not available yet, or
     * CASCADE_TILE_MISSING if it never will be. MUST NOT BLOCK - see above.
     *
     * On READY the host owns a borrow of out->pixels until release_tile.
     */
    int32_t (*get_tile)(void *handle, uint32_t z, uint32_t x, uint32_t y,
                        CascadeTile *out);

    /* Returns a borrow taken by get_tile. Called once per READY. */
    void (*release_tile)(void *handle, const CascadeTile *tile);

    /* Status text: same contract as CascadeDecoderApi::poll_text. Where a
     * plugin says it cannot reach its server, which is the failure a user will
     * actually hit. */
    int32_t (*poll_text)(void *handle, char *buf, size_t cap);

    void (*destroy)(void *handle);
} CascadeBasemapApi;

#define CASCADE_TILE_SIZE_MAX 1024u

typedef struct CascadeHostClientApi {
    uint32_t structSize;

    /* Called ONCE, before any other capability's create(), with a table that
     * remains valid for as long as the plugin is loaded. The plugin should
     * store the pointer. A plugin that declares this capability but does not
     * need the table may ignore it; the host still calls. */
    void (*attach)(const CascadeHostApi *host);
} CascadeHostClientApi;

typedef struct CascadeImageDecoderApi {
    /* sizeof(CascadeImageDecoderApi) as the PLUGIN compiled it. Checked
     * against the host's own sizeof; a mismatch disables THIS capability
     * only, not the whole plugin. */
    uint32_t structSize;

    /* CASCADE_INPUT_AUDIO or CASCADE_INPUT_IQ. */
    uint32_t inputKind;

    /* As the other tables: 0 means "any rate". Range-checked against the
     * AUDIO bounds when inputKind is CASCADE_INPUT_AUDIO and the IQ bounds
     * when it is CASCADE_INPUT_IQ. */
    double requiredRateHz;
    double preferredRateHz;

    /* Creates one instance. centerHz is the RF frequency at DC, and is 0 for
     * an audio-input decoder. NULL on failure. */
    void *(*create)(double rateHz, double centerHz);

    /* Consumes `frames` samples. For CASCADE_INPUT_AUDIO that is `frames`
     * real floats; for CASCADE_INPUT_IQ it is 2*frames floats interleaved
     * I,Q,I,Q,... - the same rule as CascadeIqDecoderApi. Borrowed pointer,
     * valid only for the call. Real-time thread: no blocking, no I/O, no
     * unbounded allocation. */
    void (*process)(void *handle, const float *samples, size_t frames);

    /* The receiver moved. MAY BE NULL if the decoder does not care; the host
     * checks before calling. */
    void (*retune)(void *handle, double centerHz);

    /*
     * Offers the newest image, if any. The host fills out->structSize before
     * the call and the plugin fills the rest. Returns 1 if an image was
     * written, 0 if none is pending (must be cheap - it is polled per frame),
     * negative if the decoder has failed permanently.
     *
     * On 1, the host owns a BORROW of out->pixels until it calls
     * release_image with the same image. The plugin must not free or rewrite
     * those bytes in the meantime; a decoder building the next frame into the
     * same buffer needs two buffers.
     */
    int32_t (*poll_image)(void *handle, CascadeImage *out);

    /* Returns a borrow taken by poll_image. Called exactly once per successful
     * poll_image, before destroy(). */
    void (*release_image)(void *handle, const CascadeImage *img);

    /* Status text: same contract as CascadeDecoderApi::poll_text. */
    int32_t (*poll_text)(void *handle, char *buf, size_t cap);

    /* Destroys an instance. Called once, after the last call of any other
     * function on that handle and after every image has been released. */
    void (*destroy)(void *handle);
} CascadeImageDecoderApi;

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

    /* OR of CASCADE_CAP_* bits. Must be non-zero. Declaring several is legal
     * and expected of a plugin that ships, say, an audio POCSAG decoder and
     * an IQ ADS-B decoder in one module: each declared bit simply needs its
     * entry in the table below.
     *
     * Bits the host does not know are IGNORED rather than refused - see the
     * capability-bit comment above. This field is a fast filter; the table is
     * the authority, and a bit with no matching entry provides nothing. */
    uint32_t capabilities;

    /* Number of entries in `capabilityTables`. Must be non-zero, and is
     * bounded by the host at a small number - a descriptor claiming thousands
     * of tables is not a plugin this host should be walking. */
    uint32_t capabilityCount;

    /*
     * `capabilityCount` entries, static storage, non-NULL.
     *
     * REPLACES the per-capability pointers that versions 1 and 2 carried as
     * trailing struct members. Because the array is out of line, this struct
     * is now a fixed 56 bytes on every 64-bit target and stays that size no
     * matter how many capabilities are added later. That is the entire reason
     * version 3 is a breaking change: it is the change that makes further
     * breaking changes unnecessary.
     *
     * Duplicate entries for one capability are malformed; the host uses the
     * first and ignores the rest.
     */
    const CascadeCapabilityEntry *capabilityTables;
} CascadePluginDesc;

/*
 * Look up one capability's table. See CASCADE_HAVE_CAPABILITY_LOOKUP above.
 *
 * Header-only and static so it costs nothing and adds no link dependency
 * between a plugin and the host. Both sides use this, so "what counts as a
 * malformed entry" has exactly one definition:
 *
 *   - an entry whose capability field is not EXACTLY the requested bit is
 *     skipped (including an entry with several bits set, which would leave
 *     the reader unable to know which table type it holds);
 *   - a NULL table is skipped;
 *   - the first match wins, so a duplicate is ignored rather than being an
 *     error - listing a capability twice is odd, but refusing to load over it
 *     would help nobody.
 *
 * The count is clamped so a corrupt capabilityCount cannot walk off the array.
 */
#define CASCADE_MAX_CAPABILITY_ENTRIES 32u

static inline const void *cascade_plugin_capability(const CascadePluginDesc *desc,
                                                    uint32_t capability) {
    uint32_t n;
    uint32_t i;
    if (desc == NULL || desc->capabilityTables == NULL || capability == 0u) {
        return NULL;
    }
    /* Exactly one bit, or the request itself is meaningless. */
    if ((capability & (capability - 1u)) != 0u) {
        return NULL;
    }
    n = desc->capabilityCount;
    if (n > CASCADE_MAX_CAPABILITY_ENTRIES) {
        n = CASCADE_MAX_CAPABILITY_ENTRIES;
    }
    for (i = 0; i < n; ++i) {
        if (desc->capabilityTables[i].capability == capability &&
            desc->capabilityTables[i].table != NULL) {
            return desc->capabilityTables[i].table;
        }
    }
    return NULL;
}

/*
 * Typed conveniences over the lookup above - one per capability this header
 * defines. These are what calling code should use: they read as well as the
 * old `desc->iqDecoder` did, and they put the cast in exactly one place
 * instead of at every call site, where a wrong one would compile silently and
 * reinterpret an unrelated table.
 *
 * Each returns NULL when the plugin does not provide that capability, so the
 * null check callers already had around the old member still works unchanged.
 * NOTE they do NOT check the table's structSize; the host does that during
 * validation, and a plugin's own test should do it explicitly so a mismatch is
 * reported as itself rather than as a missing table.
 */
static inline const CascadeDecoderApi *cascade_plugin_audio_decoder(
    const CascadePluginDesc *desc) {
    return (const CascadeDecoderApi *)cascade_plugin_capability(desc, CASCADE_CAP_DECODER);
}

static inline const CascadeIqDecoderApi *cascade_plugin_iq_decoder(
    const CascadePluginDesc *desc) {
    return (const CascadeIqDecoderApi *)cascade_plugin_capability(desc, CASCADE_CAP_IQ_DECODER);
}

static inline const CascadeImageDecoderApi *cascade_plugin_image_decoder(
    const CascadePluginDesc *desc) {
    return (const CascadeImageDecoderApi *)cascade_plugin_capability(desc,
                                                                     CASCADE_CAP_IMAGE_DECODER);
}

static inline const CascadeTrackSourceApi *cascade_plugin_track_source(
    const CascadePluginDesc *desc) {
    return (const CascadeTrackSourceApi *)cascade_plugin_capability(desc,
                                                                    CASCADE_CAP_TRACK_SOURCE);
}

static inline const CascadeBasemapApi *cascade_plugin_basemap(
    const CascadePluginDesc *desc) {
    return (const CascadeBasemapApi *)cascade_plugin_capability(desc, CASCADE_CAP_BASEMAP);
}

static inline const CascadePresetApi *cascade_plugin_preset(
    const CascadePluginDesc *desc) {
    return (const CascadePresetApi *)cascade_plugin_capability(desc, CASCADE_CAP_PRESET);
}

static inline const CascadePanelApi *cascade_plugin_panel(const CascadePluginDesc *desc) {
    return (const CascadePanelApi *)cascade_plugin_capability(desc, CASCADE_CAP_PANEL);
}

static inline const CascadeHostClientApi *cascade_plugin_host_client(
    const CascadePluginDesc *desc) {
    return (const CascadeHostClientApi *)cascade_plugin_capability(desc,
                                                                   CASCADE_CAP_HOST_CLIENT);
}

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
