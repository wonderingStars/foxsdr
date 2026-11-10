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
//      a holder that merely NAMES a device (not open) reserves it without counting;
//   F  THE ORDER IS THE PLAN'S: a claim that was refused still names its device (the plan's DeviceTwice rule, which looks at every earlier radio),
//      and a refusal names the holder behind it, never the refused claimant;
//   G  THE LOAN: a receiver's loan is recorded, and the radio is free for a patch radio to claim.
//
// Each test was shown red against a mutant of the ledger (see docs/multi-radio-records/engine-stages-1-3.md).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdio>
#include <string>

#include "core/patch_devices.hpp"
#include "engine/device_ledger.hpp"
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
    // A node that only NAMES a device (switch off, patch stopped) reserves it and does not count.
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
    return testSummary("test_device_ledger");
}
