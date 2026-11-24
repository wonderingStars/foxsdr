// radio_refusal.hpp - A REFUSED RADIO CHOICE SAYS WHY (0.99.113): the Source-line sentence, the data that travels with the refusal, and the popup's words.
//
// THE FAULT. Choosing a radio the device ledger says somebody else holds (the owner's case: his B200, named by a Radio part of a saved, stopped patch) is
// refused by the engine with FOXAPI_BUSY "in use by patch radio N" - and neither window showed anything and nothing was logged. The refusal is still right
// (WHO may hold a radio is the ledger's rule and is not changed here); the person is now told why and what to do.
//
// TWO THINGS LIVE HERE, all plain functions so a test holds them with no window:
//   1. sourceSentence(): the sentence the engine puts on the Source section's error line (Receiver::sourceError_, read by both windows as fact.status.sourceError
//      and by remote clients), for each kind of holder;
//   2. the DATA a refusal carries: a refused SELECT_SOURCE or RADIO_ADD answers FOXAPI_BUSY with applied[0] = the holder's kind (kHolder*) and applied[1] =
//      the number of the holder (a receiver's number from 1, a patch Radio part's node id - not shown on the page - and 0 for the transmitter). A window learns WHO holds the radio
//      from these two numbers and never by reading a sentence, translated or not;
//   (The popup a window raises for the patch-part holder is core/radio_refusal_popup.hpp.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_RADIO_REFUSAL_HPP
#define CASCADE_CORE_RADIO_REFUSAL_HPP

#include <string>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"

namespace cascade::core::refusal {

// Who holds the radio the user asked for: the value of applied[0] on a FOXAPI_BUSY answer to SELECT_SOURCE or RADIO_ADD.
enum : int {
    kHolderNone = 0,
    kHolderReceiver = 1,      // applied[1]: the radio's number, from 1 ("Radio 2")
    kHolderPatchPart = 2,     // applied[1]: the Radio part's number (the patch node's id, the same number DeviceHolder::describe() gives: "patch radio 3")
    kHolderTransmitter = 3,   // applied[1]: 0
};

// HOW A RADIO PART IS NAMED TO A PERSON. The Patch page shows a part by its CAPTION (Node::name, "Radio" when it was added; the Name box of either window renames it; two Radio parts may carry the same
// caption; no number is drawn on the part). So the sentences name the part by that caption in quotation marks and never by its node number. A part whose caption was cleared still shows its kind
// ("RADIO"), so an empty caption is named "Radio".
inline std::string partCaptionFor(const std::string& caption) {
    std::size_t a = 0, b = caption.size();
    while (a < b && (caption[a] == ' ' || caption[a] == '	')) { ++a; }
    while (b > a && (caption[b - 1] == ' ' || caption[b - 1] == '	')) { --b; }
    return a == b ? std::string(i18n::tr("Radio")) : caption.substr(a, b - a);
}

// The sentence for the Source error line. "" for kHolderNone. `number` is the holder's number (a receiver's from 1); `partCaption` is the Radio part's caption, for kHolderPatchPart only.
inline std::string sourceSentence(int holderKind, int number, const std::string& partCaption = std::string()) {
    switch (holderKind) {
        case kHolderPatchPart:
            return formatText(i18n::tr("This radio is set as the radio of the part \"%s\" on the Patch page. Change or remove that part, or start the patch to use the radio there."),
                              partCaptionFor(partCaption).c_str());
        case kHolderReceiver:
            return formatText(i18n::tr("This radio is in use by Radio %d. Choose another radio there first."), number);
        case kHolderTransmitter:
            return std::string(i18n::tr("This radio is in use by the transmitter."));
        default:
            break;
    }
    return std::string();
}

}  // namespace cascade::core::refusal

#endif  // CASCADE_CORE_RADIO_REFUSAL_HPP
