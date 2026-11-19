// radio_message.hpp - A MESSAGE FROM THE TEAM FOR ONE RADIO (0.99.110; docs/multi-radio-records/radio-message.md).
//
// THE OWNER'S REQUEST (2026-11-19): "in the next build i want to be able to send messages to a specific radio it should show in a popup window on the app".
//
// HOW IT REACHES THE APPLICATION WITHOUT NEW TRACKING. The application does NOT send its radios anywhere to fetch a message. The update check it already
// makes (GET /api/update, one per launch) is answered with a NEW list, `radioMessages`, beside the existing `messages`. Every entry names its radio only by
// a KEYED HASH, and the application hashes the radio keys it has itself found or opened on this PC and shows an entry only when one of them matches:
//
//     {"id":"rm4k2p9x1a","date":"2026-11-19","text":"...","salt":"<16 to 64 lower-case hex>","hash":"<64 lower-case hex>"}
//     hash = lower-case hex SHA-256 over the bytes  salt (as the 16 to 64 hex characters, ASCII)  followed by  the radio key (UTF-8)
//     the radio key is radioreport's radioKey(): "<model slug>:<SERIAL upper-cased>", e.g. "nesdr-smart-v5:00A1B2C3"
//
// A copy that has not got that radio learns only that some message exists. The hash is a keyed LOCK, not a secret: the key space (a model and a serial) is
// small, so someone who already has a guess of a radio key can test it. It stops the answer from naming the radios, which is what "no new tracking" needs.
//
// OLD COPIES. 0.99.109 and every release before it never read the key `radioMessages` (the update reader takes the keys it knows and ignores the rest;
// tests/test_radio_message.cpp feeds the real 0.99.109 reader's own parser shape a manifest with the key and checks nothing else changes), so none of them
// can show one of these to anybody, addressed or not.
//
// WHAT IS SHOWN. Plain text, only: the text is cut to kMaxRadioMessageChars characters, control characters and the invisible direction controls are
// dropped, and the window draws it as a string (no link is made clickable, nothing is interpreted).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_RADIO_MESSAGE_HPP
#define CASCADE_CORE_RADIO_MESSAGE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::core {

// One entry of the answer's `radioMessages` list, after the reader has bounded it.
struct RadioMessage {
    std::string id;     // the service's id (wellFormedMessageId); what Seen remembers, in the same list as the team messages' ids
    std::string date;   // as published, informational
    std::string text;   // plain text, sanitised, at most kMaxRadioMessageChars characters
    std::string salt;   // 16 to 64 lower-case hex characters
    std::string hash;   // 64 lower-case hex characters: the keyed hash of the radio key
};

inline constexpr std::size_t kMaxRadioMessages = 20;          // the first twenty usable entries are kept
inline constexpr std::size_t kMaxRadioMessageChars = 600;     // the site's own limit, applied again on receipt
inline constexpr std::size_t kMinRadioMessageSaltChars = 16;
inline constexpr std::size_t kMaxRadioMessageSaltChars = 64;

// The keyed hash of a radio key under a salt (see the header comment): the one function both the application and the contract fixture use.
std::string radioMessageHash(const std::string& salt, const std::string& radioKey);

// Does this message address this radio key?
bool radioMessageIsFor(const RadioMessage& m, const std::string& radioKey);

// The text a message may show: invalid UTF-8, control characters (a new line stays, a tab becomes a space), the direction and zero-width controls and the
// line and paragraph separators dropped; a run of blank lines cut to one; trimmed; at most `maxChars` characters, cut on a character boundary.
std::string sanitiseRadioMessageText(const std::string& raw, std::size_t maxChars = kMaxRadioMessageChars);

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The radios this copy has found or opened since it started. Filled by the radio reporter's one funnel (radioreport::Reporter::event: radio_found and
// radio_open), read by the window. Process-wide and thread-safe. A radio with no serial is not recorded: it cannot be addressed.
// ---------------------------------------------------------------------------------------------------------------------------------------------
struct KnownRadio {
    std::string key;      // radioKey()
    std::string model;    // the radio's model as the reports name it
    std::string serial;   // upper-case
};

namespace radiomessage {
void noteRadio(const std::string& key, const std::string& model, const std::string& serial);
std::vector<KnownRadio> knownRadios();
// Counts every NEW radio recorded, so a window polling it knows when to look again.
std::uint64_t generation();
void resetForTest();
}  // namespace radiomessage

// ---------------------------------------------------------------------------------------------------------------------------------------------
// The popup the window shows.
// ---------------------------------------------------------------------------------------------------------------------------------------------
struct RadioMessagePopup {
    bool shown = false;
    std::string id;
    std::string date;
    std::string text;
    std::string model;       // the radio's model, as found on this PC
    std::string serialTail;  // the LAST 4 characters of its serial (all of it when shorter): never the whole serial
    int more = 0;            // matched, unseen messages behind this one
    bool operator==(const RadioMessagePopup&) const = default;
};

// The first matched message the Seen list does not cover, oldest first (the list arrives newest first; an older message is read before a newer).
// Nothing is shown when the Updates tick is off (no check was made, so none arrived) or for a Microsoft Store copy (it never asks the service).
RadioMessagePopup radioMessagePopupFor(bool updateCheckEnabled, bool packaged, const std::vector<RadioMessage>& messages, const std::vector<KnownRadio>& radios,
                                       const std::vector<std::string>& seen);

}  // namespace cascade::core

#endif  // CASCADE_CORE_RADIO_MESSAGE_HPP
