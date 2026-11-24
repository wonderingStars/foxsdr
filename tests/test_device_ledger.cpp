// test_device_ledger.cpp - the device ledger (engine/device_ledger.hpp, multi-radio stage 2): WHO HOLDS THIS RADIO?
//
// The rule it carries is the old one - one physical radio, one holder, the generator exempt, two keys naming one dongle by its serial across
// driver stacks are one radio (core::patch::sameDevice, unchanged) - with the two new things: a refusal NAMES THE HOLDER, and there is ONE LIMIT
// on radios open in total (receivers and patch radios together).
//
//   A  a free radio is granted; a second claim for it is refused and the sentence names the first holder ("in use by Radio 1", "patch radio 3");
//   B  the same radio spelled two ways (a native key and a SoapySDR key with the one serial) is one radio;
//   C  the generator and an empty key are never held against anyone;
//   D  a holder holds ONE device: asking again replaces what it held, and the old one is free; release frees it too;
//   E  THE LIMIT: the claim that would be the limit-plus-first open radio is refused, the sentence states the limit and names every open holder;
//      a holder that merely NAMES a device (not open) does not count (and, if it is a patch node, does not hold the device against a receiver: H);
//   F  THE ORDER IS THE PLAN'S: a claim that was refused still names its device (the plan's DeviceTwice rule, which looks at every earlier radio),
//      and a refusal names the holder behind it, never the refused claimant;
//   G  THE LOAN: a receiver's loan is recorded, and the radio is free for a patch radio to claim;
//   H  A PATCH NODE THAT IS NOT OPEN DOES NOT HOLD A RADIO AGAINST A RECEIVER (0.99.114): a stopped part names the device and no more; node against node ("named twice"),
//      an open node, a receiver and the transmitter hold as they did.
//
// Each test was shown red against a mutant of the ledger (see docs/multi-radio-records/engine-stages-1-3.md).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdio>
#include <string>

#include "core/patch_devices.hpp"
#include "engine/device_ledger.hpp"
#include "engine/radio_refusal_data.hpp"
#include "test_check.hpp"

// test_check.hpp has no CHECK_EQ: a mismatch prints the expression, as CHECK does.
#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace {

using cascade::engine::DeviceHolder;
using cascade::engine::DeviceLedger;
namespace pc = cascade::core::patch;

const std::string kRtl = pc::makeDeviceKey("rtlsdr", "serial=00000001");
const std::string kRtlViaSoapy = pc::makeDeviceKey("soapy", "driver=rtlsdr,serial=00000001");
const std::string kB200 = pc::makeDeviceKey("soapy", "driver=uhd,serial=30A1B2C");
const std::string kHackrf = pc::makeDeviceKey("hackrf", "serial=0000000000000000a06063c8234e925f");

void testHoldersSpeak() {
    CHECK_EQ(DeviceHolder::receiver(0).describe(), std::string("Radio 1"));
    CHECK_EQ(DeviceHolder::receiver(7).describe(), std::string("Radio 8"));
    CHECK_EQ(DeviceHolder::patchNode(3).describe(), std::string("patch radio 3"));
    CHECK_EQ(DeviceHolder::transmitter().describe(), std::string("the transmitter"));
    CHECK(DeviceHolder{}.none());
    CHECK(!DeviceHolder::receiver(0).none());
    CHECK(DeviceHolder::receiver(1) == DeviceHolder::receiver(1));
    CHECK(DeviceHolder::receiver(1) != DeviceHolder::patchNode(1));
}

void testAFreeRadioIsGrantedAndASecondClaimNamesTheHolder() {
    DeviceLedger l(8);
    DeviceLedger::Claim c = l.claim(kRtl, DeviceHolder::receiver(0));
    CHECK(c.granted());
    CHECK(c.refusal.empty());
    CHECK(l.holderOf(kRtl) == DeviceHolder::receiver(0));
    CHECK_EQ(l.keyOf(DeviceHolder::receiver(0)), kRtl);

    c = l.claim(kRtl, DeviceHolder::patchNode(3));
    CHECK(!c.granted());
    CHECK(c.verdict == DeviceLedger::Verdict::Held);
    CHECK(c.holder == DeviceHolder::receiver(0));
    CHECK_EQ(c.refusal, std::string("in use by Radio 1"));
    // The refused one holds nothing.
    CHECK(l.holderOf(kRtl) == DeviceHolder::receiver(0));

    // The transmitter is a holder too, and is named in the same words.
    CHECK(l.claim(kB200, DeviceHolder::transmitter()).granted());
    const DeviceLedger::Claim t = l.claim(kB200, DeviceHolder::receiver(1));
    CHECK(!t.granted());
    CHECK_EQ(t.refusal, std::string("in use by the transmitter"));

    // A holder asking for what it holds is not refused by itself.
    CHECK(l.claim(kRtl, DeviceHolder::receiver(0)).granted());
}

void testTwoSpellingsAreOneRadio() {
    DeviceLedger l(8);
    CHECK(l.claim(kRtl, DeviceHolder::receiver(0)).granted());
    const DeviceLedger::Claim c = l.claim(kRtlViaSoapy, DeviceHolder::patchNode(2));
    CHECK(!c.granted());
    CHECK_EQ(c.refusal, std::string("in use by Radio 1"));
    CHECK(l.holderOf(kRtlViaSoapy) == DeviceHolder::receiver(0));
    // Another dongle is another radio.
    CHECK(l.claim(kHackrf, DeviceHolder::patchNode(2)).granted());
    CHECK(l.holderOf(kHackrf) == DeviceHolder::patchNode(2));
}

void testTheGeneratorAndNoDeviceAreNeverHeld() {
    DeviceLedger l(8);
    CHECK(l.claim(pc::kGeneratorKey, DeviceHolder::receiver(0)).granted());
    CHECK(l.claim(pc::kGeneratorKey, DeviceHolder::receiver(1)).granted());
    CHECK(l.claim(pc::kGeneratorKey, DeviceHolder::patchNode(4)).granted());
    CHECK(l.claim(std::string(), DeviceHolder::patchNode(5)).granted());
    CHECK(l.holderOf(pc::kGeneratorKey) == DeviceHolder{});
    CHECK(l.holderOf(std::string()) == DeviceHolder{});
    // ...but each is an open radio all the same.
    CHECK_EQ(l.countOpen(), static_cast<std::size_t>(4));
}

void testAHolderHoldsOneDevice() {
    DeviceLedger l(8);
    CHECK(l.claim(kRtl, DeviceHolder::receiver(0)).granted());
    CHECK(l.claim(kB200, DeviceHolder::receiver(0)).granted());  // replaces
    CHECK(l.holderOf(kRtl) == DeviceHolder{});
    CHECK(l.holderOf(kB200) == DeviceHolder::receiver(0));
    CHECK_EQ(l.countOpen(), static_cast<std::size_t>(1));
    CHECK(l.claim(kRtl, DeviceHolder::patchNode(1)).granted());  // free again
    l.release(DeviceHolder::patchNode(1));
    CHECK(l.holderOf(kRtl) == DeviceHolder{});
    CHECK(l.claim(kRtl, DeviceHolder::patchNode(2)).granted());
}

void testTheLimitStatesItselfAndNamesTheHolders() {
    DeviceLedger l(3);
    CHECK_EQ(l.limit(), static_cast<std::size_t>(3));
    CHECK(l.claim(kRtl, DeviceHolder::receiver(0)).granted());
    CHECK(l.claim(kB200, DeviceHolder::patchNode(7)).granted());
    CHECK(l.claim(pc::kGeneratorKey, DeviceHolder::patchNode(8)).granted());
    CHECK_EQ(l.countOpen(), static_cast<std::size_t>(3));
    const DeviceLedger::Claim c = l.claim(kHackrf, DeviceHolder::patchNode(9));
    CHECK(!c.granted());
    CHECK(c.verdict == DeviceLedger::Verdict::LimitReached);
    CHECK_EQ(c.refusal, std::string("at most 3 radios can be open at once; open now: Radio 1, patch radio 7, patch radio 8"));
    CHECK_EQ(l.countOpen(), static_cast<std::size_t>(3));   // the refused one is not open
    CHECK(l.holderOf(kHackrf) == DeviceHolder{});
    // A holder that is already one of them is not refused by the limit.
    CHECK(l.claim(kRtl, DeviceHolder::receiver(0)).granted());
    // A node that only NAMES a device (switch off, patch stopped) does not count (against another NODE it still holds the device: F; against a receiver it does not: H).
    const std::string kSdrplay = pc::makeDeviceKey("sdrplay", "serial=1234567890");
    CHECK(l.claim(kSdrplay, DeviceHolder::patchNode(10), /*open=*/false).granted());
    CHECK_EQ(l.countOpen(), static_cast<std::size_t>(3));
    CHECK(l.holderOf(kSdrplay) == DeviceHolder::patchNode(10));
    // The claim the limit refused holds nothing, but the plan counts a device named twice all the same: a later node naming it is refused, and the
    // sentence is the limit's, not "in use by" a node that holds nothing.
    const DeviceLedger::Claim again = l.claim(kHackrf, DeviceHolder::patchNode(11), /*open=*/false);
    CHECK(!again.granted());
    CHECK(again.refusal.find("at most 3 radios") == 0u);
    CHECK(l.holderOf(kHackrf) == DeviceHolder{});
    // Eight is the engine's limit: nothing here is four.
    DeviceLedger eight(8);
    for (std::size_t i = 0; i < 8; ++i) { CHECK(eight.claim(pc::kGeneratorKey, DeviceHolder::receiver(i)).granted()); }
    CHECK(!eight.claim(pc::kGeneratorKey, DeviceHolder::patchNode(1)).granted());
}

void testARefusedClaimStillNamesItsDevice() {
    // The plan's DeviceTwice rule looks at every EARLIER radio, refused or not, and sameDevice is not transitive (a serial match across stacks, and a
    // family match when one side has no serial): the ledger must give the plan's answer, naming the holder behind the refused one.
    const std::string withSerial = pc::makeDeviceKey("rtlsdr", "serial=00000001");
    const std::string soapyNoSerial = pc::makeDeviceKey("soapy", "driver=rtlsdr");
    const std::string otherSerial = pc::makeDeviceKey("rtlsdr", "serial=00000002");
    // A and C differ in serial, so they are two radios; B (no serial, same family, other stack) matches whichever it is compared to first.
    CHECK(!pc::sameDevice(withSerial, otherSerial));
    CHECK(pc::sameDevice(withSerial, soapyNoSerial));
    CHECK(pc::sameDevice(otherSerial, soapyNoSerial));

    DeviceLedger l(8);
    CHECK(l.claim(withSerial, DeviceHolder::patchNode(1), false).granted());
    const DeviceLedger::Claim b = l.claim(soapyNoSerial, DeviceHolder::patchNode(2), false);
    CHECK(!b.granted());
    CHECK(b.holder == DeviceHolder::patchNode(1));
    // C is a different radio from A, and the plan says "twice" because B (earlier) names a radio it matches: the ledger says it too, and names A, who
    // actually holds the radio B was refused for.
    const DeviceLedger::Claim c = l.claim(otherSerial, DeviceHolder::patchNode(3), false);
    CHECK(!c.granted());
    CHECK(c.holder == DeviceHolder::patchNode(1));
    CHECK_EQ(c.refusal, std::string("in use by patch radio 1"));
    // The holder of a radio is the one that was granted, never a refused claimant.
    CHECK(l.holderOf(soapyNoSerial) == DeviceHolder::patchNode(1));
}

void testALoanIsRecordedAndTheRadioIsFree() {
    DeviceLedger l(8);
    CHECK(!l.lent(DeviceHolder::receiver(0)).has_value());
    // The receiver has lent its radio to the patch: it holds the generator meanwhile, the radio is free, and the loan is on the books.
    CHECK(l.claim(pc::kGeneratorKey, DeviceHolder::receiver(0)).granted());
    l.lend(DeviceHolder::receiver(0), kRtl);
    CHECK(l.lent(DeviceHolder::receiver(0)).has_value());
    CHECK_EQ(*l.lent(DeviceHolder::receiver(0)), kRtl);
    CHECK(l.claim(kRtl, DeviceHolder::patchNode(1)).granted());
    CHECK(l.holderOf(kRtl) == DeviceHolder::patchNode(1));
    // Lending again replaces; releasing the lender ends the loan; clearing forgets everything.
    l.lend(DeviceHolder::receiver(0), kB200);
    CHECK_EQ(*l.lent(DeviceHolder::receiver(0)), kB200);
    l.release(DeviceHolder::receiver(0));
    CHECK(!l.lent(DeviceHolder::receiver(0)).has_value());
    l.lend(DeviceHolder::receiver(1), kRtl);
    l.clear();
    CHECK(!l.lent(DeviceHolder::receiver(1)).has_value());
    CHECK_EQ(l.countOpen(), static_cast<std::size_t>(0));
    CHECK(l.holderOf(kRtl) == DeviceHolder{});
}

void testARecordingIsDecidedByItsFile() {
    DeviceLedger l(8);
    const std::string a = std::string(pc::kIqFilePrefix) + "C:/rec/one.wav";
    const std::string b = std::string(pc::kIqFilePrefix) + "C:/rec/two.wav";
    CHECK(l.claim(a, DeviceHolder::patchNode(1)).granted());
    CHECK(l.claim(b, DeviceHolder::patchNode(2)).granted());
    const DeviceLedger::Claim c = l.claim(a, DeviceHolder::patchNode(3));
    CHECK(!c.granted());
    CHECK_EQ(c.refusal, std::string("in use by patch radio 1"));
}

// ---- H: WHO MAY BE REFUSED BY A PATCH NODE (radio holder fix, 0.99.114) -------------------------------------------------------------------------------
//
// A patch Radio node that is not OPEN (the patch is stopped, or its switch is off) only NAMES a device: it does not hold it against a RECEIVER. Node against node
// (the plan's DeviceTwice rule, "named twice") is unchanged, and so is everything else that is open: a receiver, the transmitter, an open node.

void testStoppedNodeDoesNotBlockAReceiver() {
    DeviceLedger l(8);
    CHECK(l.claim(kB200, DeviceHolder::patchNode(1), /*open=*/false).granted());
    // the owner's case: a stopped part names the B200, and Radio 1 asks for it
    const DeviceLedger::Claim c = l.claim(kB200, DeviceHolder::receiver(0));
    CHECK(c.granted());
    CHECK(c.verdict == DeviceLedger::Verdict::Granted);
    CHECK(c.refusal.empty());
    CHECK(l.holderOf(kB200) == DeviceHolder::receiver(0));
    CHECK_EQ(l.countOpen(), static_cast<std::size_t>(1));   // the node never counted; the receiver does
    // the node, asking again, now reads who has the radio
    const DeviceLedger::Claim node = l.claim(kB200, DeviceHolder::patchNode(1), /*open=*/false);
    CHECK(!node.granted());
    CHECK(node.verdict == DeviceLedger::Verdict::Held);
    CHECK_EQ(node.refusal, std::string("in use by Radio 1"));
    CHECK(node.holder == DeviceHolder::receiver(0));
}

void testOpenNodeStillBlocksAReceiver() {
    DeviceLedger l(8);
    CHECK(l.claim(kB200, DeviceHolder::patchNode(7), /*open=*/true).granted());
    const DeviceLedger::Claim c = l.claim(kB200, DeviceHolder::receiver(0));
    CHECK(!c.granted());
    CHECK(c.verdict == DeviceLedger::Verdict::Held);
    CHECK_EQ(c.refusal, std::string("in use by patch radio 7"));
    CHECK(l.holderOf(kB200) == DeviceHolder::patchNode(7));
    // the same through the other driver stack's spelling
    CHECK(l.claim(kRtl, DeviceHolder::patchNode(8), true).granted());
    CHECK_EQ(l.claim(kRtlViaSoapy, DeviceHolder::receiver(1)).refusal, std::string("in use by patch radio 8"));
}

void testTwoStoppedNodesStillNameTwice() {
    DeviceLedger l(8);
    CHECK(l.claim(kB200, DeviceHolder::patchNode(1), false).granted());
    const DeviceLedger::Claim second = l.claim(kB200, DeviceHolder::patchNode(2), false);
    CHECK(!second.granted());
    CHECK(second.verdict == DeviceLedger::Verdict::Held);
    CHECK_EQ(second.refusal, std::string("in use by patch radio 1"));   // the node's words, never the limit's
    CHECK(second.refusal.find("at most") == std::string::npos);
    // and a receiver arriving later takes the radio from the first of them, which the second still depends on only by name
    CHECK(l.claim(kB200, DeviceHolder::receiver(0)).granted());
    CHECK(l.holderOf(kB200) == DeviceHolder::receiver(0));
}

void testStoppedFamilyNodeBlocksNoDongle() {
    // a part that names "any RTL-SDR" through SoapySDR (no serial) matches every dongle of the family; stopped, it must hold none of them
    DeviceLedger l(8);
    const std::string family = pc::makeDeviceKey("soapy", "driver=rtlsdr");
    const std::string a = pc::makeDeviceKey("rtlsdr", "serial=A0000001");
    const std::string b = pc::makeDeviceKey("rtlsdr", "serial=B0000002");
    CHECK(l.claim(family, DeviceHolder::patchNode(1), false).granted());
    CHECK(l.claim(a, DeviceHolder::receiver(0)).granted());
    CHECK(l.claim(b, DeviceHolder::receiver(1)).granted());
    CHECK(l.holderOf(a) == DeviceHolder::receiver(0));
    CHECK(l.holderOf(b) == DeviceHolder::receiver(1));
    // ...and the same family node, OPEN, holds either dongle against a receiver
    DeviceLedger open(8);
    CHECK(open.claim(family, DeviceHolder::patchNode(1), true).granted());
    CHECK(!open.claim(a, DeviceHolder::receiver(0)).granted());
    CHECK(!open.claim(b, DeviceHolder::receiver(1)).granted());
}

void testReceiverStillBlocksReceiverAcrossStacks() {
    DeviceLedger l(8);
    CHECK(l.claim(kRtl, DeviceHolder::receiver(0)).granted());
    const DeviceLedger::Claim c = l.claim(kRtlViaSoapy, DeviceHolder::receiver(1));
    CHECK(!c.granted());
    CHECK_EQ(c.refusal, std::string("in use by Radio 1"));
    // a stopped node naming it changes nothing for the second receiver
    CHECK(!l.claim(kRtl, DeviceHolder::patchNode(3), false).granted());
    CHECK_EQ(l.claim(kRtlViaSoapy, DeviceHolder::receiver(1)).refusal, std::string("in use by Radio 1"));
}

void testSameSerialInTwoFamiliesIsOneRadio() {
    // PINS TODAY'S ANSWER (review F9, for the owner to decide): sameDevice compares serial strings without the driver family, so two radios of two families that
    // happen to carry one serial (a default, an all-zero one) are one radio to the ledger. Not changed by the holder fix.
    const std::string rtl = pc::makeDeviceKey("rtlsdr", "serial=00000001");
    const std::string hack = pc::makeDeviceKey("hackrf", "serial=00000001");
    CHECK(pc::sameDevice(rtl, hack));
    DeviceLedger l(8);
    CHECK(l.claim(rtl, DeviceHolder::receiver(0)).granted());
    const DeviceLedger::Claim c = l.claim(hack, DeviceHolder::receiver(1));
    CHECK(!c.granted());
    CHECK_EQ(c.refusal, std::string("in use by Radio 1"));
}

void testDisplacedNodeDoesNotCountTowardTheLimit() {
    DeviceLedger l(2);
    CHECK(l.claim(kRtl, DeviceHolder::receiver(0)).granted());
    CHECK(l.claim(kB200, DeviceHolder::patchNode(5), false).granted());
    CHECK_EQ(l.countOpen(), static_cast<std::size_t>(1));
    // the receiver takes the node's device: the node is displaced, and the count is the receivers' alone
    CHECK(l.claim(kB200, DeviceHolder::receiver(1)).granted());
    CHECK_EQ(l.countOpen(), static_cast<std::size_t>(2));
    CHECK(l.holderOf(kB200) == DeviceHolder::receiver(1));
    // the limit is judged on open radios only: a third receiver is refused with the limit's words naming the two, not the stopped node
    const DeviceLedger::Claim third = l.claim(kHackrf, DeviceHolder::receiver(2));
    CHECK(third.verdict == DeviceLedger::Verdict::LimitReached);
    CHECK_EQ(third.refusal, std::string("at most 2 radios can be open at once; open now: Radio 1, Radio 2"));
    CHECK_EQ(l.countOpen(), static_cast<std::size_t>(2));
}

void testTransmitterEntryAlwaysBlocks() {
    // the transmitter is no patch node: open or not, it is never skipped by the holder fix
    for (const bool open : {true, false}) {
        DeviceLedger l(8);
        CHECK(l.claim(kB200, DeviceHolder::transmitter(), open).granted());
        const DeviceLedger::Claim c = l.claim(kB200, DeviceHolder::receiver(0));
        CHECK(!c.granted());
        CHECK_EQ(c.refusal, std::string("in use by the transmitter"));
        CHECK(l.holderOf(kB200) == DeviceHolder::transmitter());
    }
}

void testPlutoStoppedNodeDoesNotBlockAReceiverAndARunningOneDoes() {
    // a Pluto is held by the address as typed; the holder fix is the same for it as for a scanned radio
    const std::string pluto = pc::makeDeviceKey("pluto", "uri=ip:192.168.2.1");
    DeviceLedger stopped(8);
    CHECK(stopped.claim(pluto, DeviceHolder::patchNode(1), false).granted());
    CHECK(stopped.claim(pluto, DeviceHolder::receiver(1)).granted());
    DeviceLedger running(8);
    CHECK(running.claim(pluto, DeviceHolder::patchNode(1), true).granted());
    CHECK_EQ(running.claim(pluto, DeviceHolder::receiver(1)).refusal, std::string("in use by patch radio 1"));
}

void testBlockerOfAnswersWhatAReceiversClaimWouldBeTold() {
    // blockerOf (a source choice, asked before the claim) and claim (RADIO_ADD) must agree for a receiver, whoever holds the radio (review of the holder fix: no unit test of it)
    DeviceLedger l(8);
    CHECK(l.blockerOf(kB200).none());                                            // nobody
    CHECK(l.claim(kB200, DeviceHolder::patchNode(1), /*open=*/false).granted());
    CHECK(l.blockerOf(kB200).none());                                            // a stopped part only names it
    CHECK(l.holderOf(kB200) == DeviceHolder::patchNode(1));                       // (holderOf is the old, strict answer)
    DeviceLedger open(8);
    CHECK(open.claim(kB200, DeviceHolder::patchNode(2), true).granted());
    CHECK(open.blockerOf(kB200) == DeviceHolder::patchNode(2));                  // an open part holds
    CHECK(open.blockerOf(kRtl).none());
    DeviceLedger rx(8);
    CHECK(rx.claim(kRtl, DeviceHolder::receiver(0)).granted());
    CHECK(rx.blockerOf(kRtlViaSoapy) == DeviceHolder::receiver(0));              // a receiver holds, through the other stack's spelling too
    CHECK(rx.blockerOf("").none());
    CHECK(rx.blockerOf(pc::kGeneratorKey).none());                               // the generator is exempt
    // a refused (never granted) node entry only names the device: it blocks nobody
    DeviceLedger twice(8);
    CHECK(twice.claim(kB200, DeviceHolder::patchNode(1), true).granted());
    CHECK(!twice.claim(kB200, DeviceHolder::patchNode(2), true).granted());
    CHECK(twice.blockerOf(kB200) == DeviceHolder::patchNode(1));
}

void testAnOpenInFlightHoldsAgainstEveryoneAndNamesItsAsker() {
    // an open still on its worker after its asker let go (a patch open STOP handed to a thread, an open a receiver set aside): held against a receiver, a node and a second
    // in-flight open alike, described as whoever asked, and never counted toward the limit
    DeviceLedger l(2);
    const DeviceHolder fly = DeviceHolder::inFlight(DeviceHolder::patchNode(4), 1);
    CHECK(l.claim(kB200, fly, /*open=*/false).granted());
    CHECK(fly.kind == DeviceHolder::Kind::InFlight);
    CHECK(fly.owner() == DeviceHolder::patchNode(4));
    CHECK_EQ(fly.describe(), std::string("patch radio 4"));
    CHECK_EQ(l.countOpen(), static_cast<std::size_t>(0));                          // not an open radio: no part of the limit
    CHECK(l.holderOf(kB200) == fly);
    CHECK(l.blockerOf(kB200) == fly);                                              // holds against a receiver (blockerOf) ...
    const DeviceLedger::Claim rxClaim = l.claim(kB200, DeviceHolder::receiver(0));
    CHECK(!rxClaim.granted());                                                     // ... and a receiver's claim
    CHECK_EQ(rxClaim.refusal, std::string("in use by patch radio 4"));
    CHECK(rxClaim.holder == fly);
    const DeviceLedger::Claim nodeClaim = l.claim(kB200, DeviceHolder::patchNode(9), false);
    CHECK(!nodeClaim.granted());                                                   // ... and a node's
    CHECK_EQ(nodeClaim.refusal, std::string("in use by patch radio 4"));
    // the words and the data a refusal carries name the one that asked, never the open
    const cascade::engine::RefusalHolder who = cascade::engine::refusalHolderOf(fly);
    CHECK_EQ(who.kind, cascade::engine::refusalHolderOf(DeviceHolder::patchNode(4)).kind);
    CHECK_EQ(who.number, 4);
    const cascade::engine::RefusalHolder rxWho = cascade::engine::refusalHolderOf(DeviceHolder::inFlight(DeviceHolder::receiver(2), 2));
    CHECK_EQ(rxWho.kind, cascade::engine::refusalHolderOf(DeviceHolder::receiver(2)).kind);
    CHECK_EQ(rxWho.number, 3);                                                     // "Radio 3"
}

}  // namespace

int main() {
    std::printf("test_device_ledger\n");
    testHoldersSpeak();
    testAFreeRadioIsGrantedAndASecondClaimNamesTheHolder();
    testTwoSpellingsAreOneRadio();
    testTheGeneratorAndNoDeviceAreNeverHeld();
    testAHolderHoldsOneDevice();
    testTheLimitStatesItselfAndNamesTheHolders();
    testARefusedClaimStillNamesItsDevice();
    testALoanIsRecordedAndTheRadioIsFree();
    testARecordingIsDecidedByItsFile();
    testStoppedNodeDoesNotBlockAReceiver();
    testOpenNodeStillBlocksAReceiver();
    testTwoStoppedNodesStillNameTwice();
    testStoppedFamilyNodeBlocksNoDongle();
    testReceiverStillBlocksReceiverAcrossStacks();
    testSameSerialInTwoFamiliesIsOneRadio();
    testDisplacedNodeDoesNotCountTowardTheLimit();
    testTransmitterEntryAlwaysBlocks();
    testPlutoStoppedNodeDoesNotBlockAReceiverAndARunningOneDoes();
    testBlockerOfAnswersWhatAReceiversClaimWouldBeTold();
    testAnOpenInFlightHoldsAgainstEveryoneAndNamesItsAsker();
    return testSummary("test_device_ledger");
}
