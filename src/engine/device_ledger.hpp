// THE DEVICE LEDGER (multi-radio engine, stage 2; docs/multi-radio-records/engine-stages-1-3.md).
//
// One question, asked in one place: WHO HOLDS THIS RADIO? Before this, three places answered it for themselves - the patch plan's DeviceTwice rule
// (core/patch_plan.hpp), Engine::deviceTakenEarlier, and the receiver's loan to the patch (gui::receiverSourceForPatch) - and none of them could say
// WHO. A holder is a receiver slot (Radio 1, Radio 2, ...), a patch radio node, or the transmitter. The rule does not change: one physical radio,
// one holder; the signal generator is exempt (any number of holders may sit on it). Whether two device keys name one radio is
// core::patch::sameDevice's, unchanged - including the serial match across driver stacks.
//
// THE LIMIT. At most `limit` radios open in total, receivers and patch radios together (the owner's rule: eight, kMaxReceivers). A holder counts
// toward it when it is OPEN; a patch node that merely names a device (its switch is off, or the patch is stopped) names the device but does not
// count. Past the limit a claim is refused with a sentence that states the limit and names every holder.
//
// WHO MAY BE REFUSED BY A PATCH NODE (0.99.114). A patch node that is not OPEN only NAMES a device: it does not hold it against a RECEIVER, so a stopped patch (or
// a part switched off) never takes a radio from the person who chose it as Source or added it as another radio. Against another PATCH NODE it holds as it always
// did - the plan's DeviceTwice rule, "named twice", and its words, are not touched - and an OPEN node, a receiver, the transmitter and an open still in flight
// (see InFlight) hold against everyone. The rule is ONE: DeviceLedger::claim() applies it to a receiver that asks, and blockerOf() answers the same question for a
// choice that is judged before it is made (FOXAPI_OP_SELECT_SOURCE).
//
// THE LOAN. A receiver that lends its radio to the patch page (the page takes it while the patch runs, the receiver runs on the generator meanwhile)
// is recorded with lend(): the radio is then free for a patch node to claim, and lent() says the receiver is waiting to get it back.
//
// PURE DATA, NO THREADS, NO LOCKS: the engine rebuilds it on the pass, from the state it holds, so it cannot drift from it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/patch_devices.hpp"

namespace cascade::engine {

struct DeviceHolder {
    // InFlight: a driver open that is still running on a worker after the one who asked for it let go of it (a patch open handed to a detached thread by STOP, an open
    // the first radio set aside for another). The radio is being opened all the same, and nobody may open it a second time beside that open; it is named as whoever
    // asked for it (describe() and owner()), and it is held against every asker, a receiver included.
    enum class Kind : std::uint8_t { None, Receiver, PatchNode, Transmitter, InFlight };
    Kind kind = Kind::None;
    // Receiver: the slot, from 0 (it is spelled Radio <slot + 1>). PatchNode: the node's id. Transmitter: 0. InFlight: a number that tells this open from the others.
    std::uint32_t id = 0;
    // InFlight only: who asked for the open (a Receiver or a PatchNode).
    Kind ofKind = Kind::None;
    std::uint32_t ofId = 0;

    static DeviceHolder receiver(std::size_t slot) { return {Kind::Receiver, static_cast<std::uint32_t>(slot)}; }
    static DeviceHolder patchNode(std::uint32_t node) { return {Kind::PatchNode, node}; }
    static DeviceHolder transmitter() { return {Kind::Transmitter, 0}; }
    static DeviceHolder inFlight(const DeviceHolder& of, std::uint32_t seq) { return {Kind::InFlight, seq, of.kind, of.id}; }

    bool operator==(const DeviceHolder& o) const { return kind == o.kind && id == o.id; }
    bool operator!=(const DeviceHolder& o) const { return !(*this == o); }
    bool none() const { return kind == Kind::None; }

    // WHO IT IS TO A PERSON: the holder itself, or for an open still in flight the one that asked for it.
    DeviceHolder owner() const { return kind == Kind::InFlight ? DeviceHolder{ofKind, ofId} : *this; }

    // The words a person is told: "Radio 2", "patch radio 3" (the node's number), "the transmitter".
    std::string describe() const {
        switch (kind) {
            case Kind::Receiver: return "Radio " + std::to_string(id + 1u);
            case Kind::PatchNode: return "patch radio " + std::to_string(id);
            case Kind::Transmitter: return "the transmitter";
            case Kind::InFlight: return owner().describe();
            case Kind::None: break;
        }
        return "nobody";
    }
};

class DeviceLedger {
public:
    enum class Verdict : std::uint8_t {
        Granted,       // the holder now holds the device (or held it already)
        Held,          // another holder has it: `holder` and `refusal` say who
        LimitReached,  // the device is free but `limit` radios are open already: `refusal` says so and names them
    };

    struct Claim {
        Verdict verdict = Verdict::Granted;
        DeviceHolder holder;   // Held: the one that has it
        std::string refusal;   // not Granted: a sentence naming the holder(s), written to be shown
        bool granted() const { return verdict == Verdict::Granted; }
    };

    explicit DeviceLedger(std::size_t limit) : limit_(limit) {}

    std::size_t limit() const { return limit_; }

    void clear() {
        entries_.clear();
        lent_.clear();
    }

    // `who` asks to hold `key`. `open`: the holder is a radio that is open now, counted toward the limit (false: it only names the device, as a patch
    // node with its switch off does). A holder holds ONE device: asking again replaces what it held. An empty key and the generator are never held
    // against anyone (the generator is exempt; an empty key is no device at all) but the holder is still recorded, and still counts if open.
    //
    // A RECEIVER that asks is not refused by a patch node that is not open (see the head of this file): it is granted the radio, and each such node that held it
    // is then named as blocked by the receiver ("in use by Radio 1" when it asks again). Everyone else is judged as before.
    Claim claim(const std::string& key, DeviceHolder who, bool open = true) {
        remove(who);
        Claim c;
        const bool exempt = key.empty() || cascade::core::patch::isGeneratorKey(key);
        std::vector<std::size_t> displaced;   // the not-open nodes that held this radio and let a receiver have it
        if (!exempt) {
            // The first entry, in the order the claims were made, that names this radio: a node that was itself refused still counts as naming it
            // (the plan's rule, so the plan and the engine always agree), and a refusal names the holder behind it.
            for (std::size_t i = 0; i < entries_.size(); ++i) {
                const Entry& e = entries_[i];
                if (!e.exempt && cascade::core::patch::sameDevice(e.key, key)) {
                    if (who.kind == DeviceHolder::Kind::Receiver && !blocksReceiver(e)) {
                        if (e.held) { displaced.push_back(i); }
                        continue;
                    }
                    if (e.held || !e.blockedBy.none()) {
                        c.verdict = Verdict::Held;
                        c.holder = e.held ? e.who : e.blockedBy;
                        c.refusal = "in use by " + c.holder.describe();
                    } else {
                        // Named by a claim that the LIMIT refused: it holds nothing, but the plan counts the device as named twice all the same.
                        c.verdict = Verdict::LimitReached;
                        c.refusal = limitSentence();
                    }
                    break;
                }
            }
        }
        if (c.verdict == Verdict::Granted && open && countOpen() >= limit_) {
            c.verdict = Verdict::LimitReached;
            c.refusal = limitSentence();
        }
        if (c.verdict == Verdict::Granted) {
            for (const std::size_t i : displaced) {
                entries_[i].held = false;
                entries_[i].blockedBy = who;
            }
        }
        Entry e;
        e.key = key;
        e.who = who;
        e.exempt = exempt;
        e.open = open && c.verdict != Verdict::LimitReached;
        e.blockedBy = c.verdict == Verdict::Held ? c.holder : DeviceHolder{};
        e.held = c.verdict == Verdict::Granted;
        entries_.push_back(std::move(e));
        return c;
    }

    // Everything `who` held is free again (and a loan it had made is over).
    void release(DeviceHolder who) {
        remove(who);
        for (std::size_t i = 0; i < lent_.size();) {
            if (lent_[i].from == who) {
                lent_.erase(lent_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    // `from` has handed the radio `key` over to the patch page; it takes it back when the patch stops.
    void lend(DeviceHolder from, const std::string& key) {
        for (Loan& l : lent_) {
            if (l.from == from) {
                l.key = key;
                return;
            }
        }
        lent_.push_back({from, key});
    }
    // The radio `from` has lent, or nothing.
    std::optional<std::string> lent(DeviceHolder from) const {
        for (const Loan& l : lent_) {
            if (l.from == from) { return l.key; }
        }
        return std::nullopt;
    }

    // Who holds the radio `key` (the first claim that names it, behind a refused one), or nobody. The generator and an empty key are never held.
    DeviceHolder holderOf(const std::string& key) const {
        if (key.empty() || cascade::core::patch::isGeneratorKey(key)) { return {}; }
        for (const Entry& e : entries_) {
            if (!e.exempt && e.held && cascade::core::patch::sameDevice(e.key, key)) { return e.who; }
        }
        return {};
    }

    // Who holds the radio `key` AGAINST A RECEIVER, or nobody: holderOf without the patch nodes that are not open (the question of a source choice, asked before the
    // claim is made - claim() gives a receiver the same answer).
    DeviceHolder blockerOf(const std::string& key) const {
        if (key.empty() || cascade::core::patch::isGeneratorKey(key)) { return {}; }
        for (const Entry& e : entries_) {
            if (!e.exempt && e.held && blocksReceiver(e) && cascade::core::patch::sameDevice(e.key, key)) { return e.who; }
        }
        return {};
    }

    // The device `who` holds ("" for none, the generator or a holder that is not there).
    std::string keyOf(DeviceHolder who) const {
        for (const Entry& e : entries_) {
            if (e.who == who) { return e.key; }
        }
        return {};
    }

    // Radios open now: receivers and patch radios together, what the limit is judged against.
    std::size_t countOpen() const {
        std::size_t n = 0;
        for (const Entry& e : entries_) {
            if (e.open) { ++n; }
        }
        return n;
    }

    // Every open holder, in the order they claimed.
    std::vector<DeviceHolder> openHolders() const {
        std::vector<DeviceHolder> out;
        for (const Entry& e : entries_) {
            if (e.open) { out.push_back(e.who); }
        }
        return out;
    }

    // The sentence for a claim past the limit: the limit, and who has the radios.
    std::string limitSentence() const {
        std::string s = "at most " + std::to_string(limit_) + " radios can be open at once; open now: ";
        bool first = true;
        for (const DeviceHolder& h : openHolders()) {
            if (!first) { s += ", "; }
            s += h.describe();
            first = false;
        }
        return s;
    }

private:
    struct Entry {
        std::string key;
        DeviceHolder who;
        DeviceHolder blockedBy;  // not none: this claim was refused, and this is who held the radio
        bool exempt = false;
        bool open = false;
        bool held = false;       // granted: this is the holder of the radio (a refused claim only names it)
    };
    // A patch node holds a radio against a receiver only while it holds it AND is open: not-open (the patch stopped, the part switched off) and refused nodes only
    // name it. Every other holder - a receiver, the transmitter, an open in flight - holds against a receiver as against anyone.
    static bool blocksReceiver(const Entry& e) { return e.who.kind != DeviceHolder::Kind::PatchNode || (e.held && e.open); }
    struct Loan {
        DeviceHolder from;
        std::string key;
    };

    // A holder's entry goes, and so do the claims that were refused only because of it (they must ask again to be judged).
    void remove(DeviceHolder who) {
        for (std::size_t i = 0; i < entries_.size();) {
            if (entries_[i].who == who || (!entries_[i].held && entries_[i].blockedBy == who)) {
                entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    std::size_t limit_;
    std::vector<Entry> entries_;
    std::vector<Loan> lent_;
};

}  // namespace cascade::engine
