// event_trail.hpp - the last few things the application DID, kept in memory so a crash or a
// freeze report can say what led up to it (field-early-warning).
//
// WHY THIS EXISTS. A fault report says where the process was (a stack) and what the log ring
// held, and the log ring is a few hundred lines of everything - a retune storm or a chatty plugin
// pushes the one line that mattered out of it. "It froze three seconds after the radio was closed
// and a rescan began" is the kind of sentence that tells an engineer which bug this is before any
// symbol is resolved, and it needs a record that only state TRANSITIONS write to: a bounded ring
// of the last thirty of them.
//
// WHAT AN ENTRY IS. A moment (whole seconds before the report was written) and a word from a
// CLOSED list, optionally qualified by a word from that event's own closed list:
//
//     src-open.<driver>   src-close.<driver>   a radio opened / the receiver stopped
//     mode.<demod>        the demodulator changed (nfm wfm am dsb usb cw lsb raw)
//     rescan.<step>       a plugin reload entered a step (the PhaseClock names)
//     rec-start rec-stop  a recording began / ended
//     plug-fit plug-unfit a plugin was installed / removed by the person
//
// NEVER free text. A driver is the health vocabulary's driver word, never a device name or a
// serial; a mode is one of eight; a step is one of the reload's own step names. No frequency, no
// file, no path, no plugin name, no address can be written, because the writers take enums and
// small indexes and the report's text is built from the tables below - and the uploader checks
// every entry against the same tables again before a byte leaves the machine
// (sanitise()), so a hand-edited report cannot make the field carry anything else.
//
// ON THE WIRE (the contract with the receiving site, PRIVACY.md): `context.trail`, one string,
// at most 30 entries separated by `;`, OLDEST FIRST, each `<secs>:<event>[.<qualifier>]`, secs
// 0..999999. Example: `412:src-open.rtlsdr;405:mode.wfm;3:rescan.unload`.
//
// SAFE FOR A FAULT HANDLER. note() is two relaxed atomic operations on a fixed array - no lock,
// no allocation, any thread. format() allocates nothing and takes no lock either, so the crash
// handler (which runs in a signal or an exception filter) and the freeze watchdog can both call
// it. The ring is lossy on purpose: a writer that laps a reader replaces an entry, and an entry
// read half-written is simply refused by the validity check.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_EVENT_TRAIL_HPP
#define CASCADE_CORE_EVENT_TRAIL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cascade::core::trail {

enum class Event : std::uint8_t {
    None = 0,
    SrcOpen = 1,
    SrcClose = 2,
    Mode = 3,
    Rescan = 4,
    RecStart = 5,
    RecStop = 6,
    PlugFit = 7,
    PlugUnfit = 8,
};
inline constexpr unsigned kEventCount = 8;

// How many entries the ring holds, and so the most a report carries.
inline constexpr std::size_t kEntries = 30;
// The longest an entry's word can be on the wire (the receiving site's own bound).
inline constexpr std::size_t kMaxEventChars = 32;
// A buffer that holds any trail format() can write: kEntries entries of at most
// 6 digits + ':' + 32 characters + ';', and the NUL.
inline constexpr std::size_t kMaxFormatted = kEntries * (6 + 1 + kMaxEventChars + 1) + 1;
// The largest `secs` a report carries.
inline constexpr std::uint64_t kMaxSecs = 999999;

// The event's word ("src-open"), or "" for None / out of range.
const char* eventWord(Event e) noexcept;
// How many qualifier words the event has (0 for an event with none).
unsigned qualifierCount(Event e) noexcept;
// The qualifier with 1-based index `q`, or nullptr (q == 0 is "no qualifier").
const char* qualifierWord(Event e, unsigned q) noexcept;
// The 1-based index of `word` among the event's qualifiers, or 0 when it is not one.
unsigned qualifierIndex(Event e, std::string_view word) noexcept;

// ---------------------------------------------------------------------------
// Writing - any thread, any time, cheap. Nothing is kept when `q` is not a qualifier of the event
// (an unknown driver is the word "other", so a source always has one).
// ---------------------------------------------------------------------------
void note(Event e, unsigned q = 0) noexcept;
// The same with the moment given (steady-clock milliseconds), for the tests.
void noteAt(Event e, unsigned q, std::uint64_t steadyMs) noexcept;

// The call sites' helpers. Each reduces what it is given to the vocabulary and drops the rest.
// `driverWord` is health::driverWord()'s answer ("rtlsdr", ..., "other"); a word that is not on
// the list is recorded as "other".
void noteSource(bool opened, std::string_view driverWord) noexcept;
// `index` is the application's mode index (NFM, WFM, AM, DSB, USB, CW, LSB, RAW); out of range
// writes nothing.
void noteMode(int index) noexcept;
// `phaseName` is one of core/plugin_reload_phases.hpp's names, as the PhaseClock spells it
// ("panels and map" included); anything else writes nothing.
void noteRescanStep(const char* phaseName) noexcept;

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// Writes the trail into buf[0..cap) as the wire string - oldest first, `;` between entries -
// NUL-terminated, and returns its length (0 and an empty string when there is nothing). `nowMs`
// is the steady-clock millisecond the report is being written at. Allocation-free and lock-free.
// `cap` should be kMaxFormatted; a smaller buffer holds only the entries that fit, oldest first, cut
// on an entry boundary, and is never overrun.
std::size_t format(char* buf, std::size_t cap, std::uint64_t nowMs) noexcept;
// The same at the current moment: what the fault writers call.
std::size_t format(char* buf, std::size_t cap) noexcept;
// The same for the normal paths, as a std::string.
std::string formatNow();

// ---------------------------------------------------------------------------
// The uploader's check
// ---------------------------------------------------------------------------

// True when `entry` is exactly `<secs>:<event>[.<qualifier>]` with secs 0..999999 and the word a
// member of the tables above (an event with qualifiers REQUIRES one; one without refuses one).
bool validEntry(std::string_view entry) noexcept;
// The entries of `text` (`;`-separated) that are valid, the newest kEntries of them, joined the
// same way. Empty when none is.
std::string sanitise(std::string_view text);

// Forgets everything (the tests share one process).
void resetForTest() noexcept;

}  // namespace cascade::core::trail

#endif  // CASCADE_CORE_EVENT_TRAIL_HPP
