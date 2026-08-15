/*
 * cascade plugin ABI - the binary contract between the host application and a
 * separately distributed plugin binary (.dll / .so / .dylib).
 *
 * SPDX-License-Identifier: MIT
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
 * and destroy are called from the host's control thread; process and
 * poll_text are called from a real-time audio/DSP thread, so they must not
 * block, allocate unboundedly, or perform I/O. destroy happens-after the last
 * process/poll_text for that instance.
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
 */
#define CASCADE_PLUGIN_ABI_VERSION 1

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
#define CASCADE_CAP_ALL_KNOWN 0x00000001u /* OR of every bit above */

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
     * passes whatever it already has. Must be 0 or in [1000, 1000000].
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
 * The plugin descriptor: static, immutable, returned by the entry point.
 *
 * LAYOUT RULE: structSize and abiVersion are the first two fields, in that
 * order, and MUST NEVER MOVE in any future ABI version. They are the only
 * fields the host may read before it has established that it understands the
 * layout, so they are the fixed point the whole compatibility check stands
 * on. Everything after them is fair game to change (with a version bump).
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
     * bits. */
    uint32_t capabilities;

    /* Must be 0. Reserved so a future ABI can add a small scalar without
     * changing the struct's size on 64-bit (it currently occupies the
     * padding that would otherwise sit before `decoder`). */
    uint32_t reserved;

    /* Non-NULL if and only if CASCADE_CAP_DECODER is set. Static storage. */
    const CascadeDecoderApi *decoder;
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
