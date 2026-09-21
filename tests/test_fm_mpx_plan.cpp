// The FM multiplex plan: what is in it, where it lands on the glass, and the
// rule that makes the whole thing appear in WFM and nowhere else.
//
// THE REQUEST (owner, 2026-09-21, with a picture of SDR Console V3): a view of
// the broadcast FM spectrum "where it shows what each frequency group is used
// for" - "I am just not sure how you can have this only show up for certain
// use cases". The answer is scopeSignalAvailable: the multiplex is a thing
// only WFM has, so the scope position that shows it only exists in WFM, and
// the labels come with the position rather than with a setting.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/fm_mpx_plan.hpp"

#include <cmath>
#include <cstring>
#include <string>

#include "gui/demod_scope.hpp"
#include "test_check.hpp"

using cascade::gui::kMpxBandCount;
using cascade::gui::kMpxBands;
using cascade::gui::kMpxSpanMaxHz;
using cascade::gui::MpxBand;
using cascade::gui::mpxBandInSpan;
using cascade::gui::mpxFraction;
using cascade::gui::mpxLabelRoomPx;
using cascade::gui::mpxSpanHz;
using cascade::gui::ScopeSignal;

namespace {

const MpxBand* bandNamed(const char* label, const char* note) {
    for (std::size_t i = 0; i < kMpxBandCount; ++i) {
        if (std::strcmp(kMpxBands[i].label, label) == 0 &&
            std::strcmp(kMpxBands[i].note, note) == 0) {
            return &kMpxBands[i];
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    // --- THE TABLE IS THE STANDARD'S, not a guess -------------------------
    //
    // These numbers also appear in core/stereo_fm.hpp, where the DECODER uses
    // them. The two must not drift apart: a label at 19 kHz over a decoder
    // locking at something else would be a drawing that lies about the
    // machine underneath it.
    {
        const MpxBand* sum = bandNamed("Mono L+R", "0-15 kHz");
        CHECK(sum != nullptr);
        CHECK_NEAR(sum->loHz, 0.0, 1e-9);
        CHECK_NEAR(sum->hiHz, 15000.0, 1e-9);
        CHECK(!sum->optional);  // every FM station transmits it

        const MpxBand* pilot = bandNamed("Pilot", "19 kHz");
        CHECK(pilot != nullptr);
        CHECK_NEAR(pilot->loHz, 19000.0, 1e-9);
        // A TONE, NOT A BAND: lo == hi is what tells the drawing to put one
        // rule there instead of a shaded extent.
        CHECK(pilot->loHz == pilot->hiHz);

        const MpxBand* lower = bandNamed("Stereo L-R", "lower");
        const MpxBand* upper = bandNamed("Stereo L-R", "upper");
        CHECK(lower != nullptr);
        CHECK(upper != nullptr);
        // 38 +/- 15 kHz, meeting at the suppressed carrier - which is why the
        // difference channel reads as two sidebands with a hole between them.
        CHECK_NEAR(lower->loHz, 23000.0, 1e-9);
        CHECK_NEAR(lower->hiHz, 38000.0, 1e-9);
        CHECK_NEAR(upper->loHz, 38000.0, 1e-9);
        CHECK_NEAR(upper->hiHz, 53000.0, 1e-9);
        CHECK(lower->hiHz == upper->loHz);

        const MpxBand* rds = bandNamed("RDS", "57 kHz");
        CHECK(rds != nullptr);
        // Centred on three times the pilot, with its own sidebands.
        CHECK_NEAR((rds->loHz + rds->hiHz) * 0.5, 57000.0, 1e-9);
        CHECK(rds->hiHz > rds->loHz);

        // The two SCA slots in common use, both tones, both optional.
        CHECK(bandNamed("SCA", "67.65 kHz") != nullptr);
        CHECK(bandNamed("SCA", "92 kHz") != nullptr);

        // Everything except the sum and the pilot is something a station may
        // simply not send, and is drawn fainter for it.
        for (std::size_t i = 0; i < kMpxBandCount; ++i) {
            const bool alwaysThere = std::strcmp(kMpxBands[i].label, "Mono L+R") == 0 ||
                                     std::strcmp(kMpxBands[i].label, "Pilot") == 0;
            CHECK(kMpxBands[i].optional == !alwaysThere);
        }

        // Ordered up the axis, because that is the order a reader meets them.
        for (std::size_t i = 1; i < kMpxBandCount; ++i) {
            CHECK(kMpxBands[i].loHz >= kMpxBands[i - 1].loHz);
        }
    }

    // --- THE SPAN: 100 kHz, or the composite's Nyquist when that is lower --
    {
        // The channel rate this application targets.
        CHECK_NEAR(mpxSpanHz(200000.0), kMpxSpanMaxHz, 1e-9);
        // A wider chain does not get a wider axis: there is nothing up there.
        CHECK_NEAR(mpxSpanHz(400000.0), kMpxSpanMaxHz, 1e-9);
        // A narrower one gets a SHORTER axis rather than a ruled lie.
        CHECK_NEAR(mpxSpanHz(120000.0), 60000.0, 1e-9);
        CHECK_NEAR(mpxSpanHz(0.0), 0.0, 1e-9);
        CHECK_NEAR(mpxSpanHz(-1.0), 0.0, 1e-9);
    }

    // --- WHERE A FREQUENCY LANDS ------------------------------------------
    {
        CHECK_NEAR(mpxFraction(0.0, 100000.0), 0.0, 1e-12);
        CHECK_NEAR(mpxFraction(19000.0, 100000.0), 0.19, 1e-12);
        CHECK_NEAR(mpxFraction(100000.0, 100000.0), 1.0, 1e-12);
        // Past the edge answers past 1, and does NOT clamp - the culling
        // below is what turns that into a decision.
        CHECK(mpxFraction(92000.0, 60000.0) > 1.0);
        CHECK_NEAR(mpxFraction(19000.0, 0.0), 0.0, 1e-12);
    }

    // --- WHAT IS DRAWN ON A SHORT AXIS ------------------------------------
    //
    // A 60 kHz composite can show the sum, the pilot and both difference
    // sidebands; RDS straddles the edge and is still worth its rule; the SCA
    // slots are outside what this receiver is looking at, and saying nothing
    // is honest where a label would imply the station is not sending one.
    {
        const double span = 60000.0;
        CHECK(mpxBandInSpan(*bandNamed("Mono L+R", "0-15 kHz"), span));
        CHECK(mpxBandInSpan(*bandNamed("Pilot", "19 kHz"), span));
        CHECK(mpxBandInSpan(*bandNamed("Stereo L-R", "upper"), span));
        CHECK(mpxBandInSpan(*bandNamed("RDS", "57 kHz"), span));
        CHECK(!mpxBandInSpan(*bandNamed("SCA", "67.65 kHz"), span));
        CHECK(!mpxBandInSpan(*bandNamed("SCA", "92 kHz"), span));

        // On the full span everything is in.
        for (std::size_t i = 0; i < kMpxBandCount; ++i) {
            CHECK(mpxBandInSpan(kMpxBands[i], kMpxSpanMaxHz));
        }
        // And on no span at all, nothing is.
        for (std::size_t i = 0; i < kMpxBandCount; ++i) {
            CHECK(!mpxBandInSpan(kMpxBands[i], 0.0));
        }
    }

    // --- HOW MUCH ROOM A LABEL HAS ----------------------------------------
    {
        const double span = 100000.0;
        const float w = 1000.0f;
        // The sum channel owns 15% of the axis: 150 px of a 1000 px tube.
        CHECK_NEAR(mpxLabelRoomPx(*bandNamed("Mono L+R", "0-15 kHz"), span, w), 150.0f, 0.01f);
        // A difference sideband owns 15 kHz too.
        CHECK_NEAR(mpxLabelRoomPx(*bandNamed("Stereo L-R", "upper"), span, w), 150.0f, 0.01f);
        // A tone is not zero wide for labelling purposes - the pilot's name
        // has to go somewhere - but it is the tightest thing on the glass.
        const float pilotRoom = mpxLabelRoomPx(*bandNamed("Pilot", "19 kHz"), span, w);
        CHECK(pilotRoom > 0.0f);
        CHECK(pilotRoom < 200.0f);
        // On a narrow tube everything shrinks in proportion, which is what
        // makes "does this text fit" the whole of the crowding rule.
        CHECK_NEAR(mpxLabelRoomPx(*bandNamed("Mono L+R", "0-15 kHz"), span, 200.0f), 30.0f, 0.01f);
        // A band clipped by the edge is measured to the edge, not past it.
        const float rdsOnShort = mpxLabelRoomPx(*bandNamed("RDS", "57 kHz"), 58000.0, 1000.0f);
        const float rdsOnFull = mpxLabelRoomPx(*bandNamed("RDS", "57 kHz"), 58000.0, 1000.0f);
        CHECK(rdsOnShort == rdsOnFull);
        CHECK(rdsOnShort <= (59400.0f - 54600.0f) / 58000.0f * 1000.0f + 0.01f);
        // Degenerate inputs draw nothing rather than dividing by zero.
        CHECK_NEAR(mpxLabelRoomPx(kMpxBands[0], 0.0, 100.0f), 0.0f, 1e-6f);
        CHECK_NEAR(mpxLabelRoomPx(kMpxBands[0], 100000.0, 0.0f), 0.0f, 1e-6f);
    }

    // --- THE GATE, WHICH IS THE OWNER'S QUESTION ---------------------------
    {
        using cascade::gui::scopeSignalAvailable;
        using cascade::gui::scopeSignalForMode;

        // In WFM every position exists.
        for (int i = 0; i < cascade::gui::kScopeSignalCount; ++i) {
            CHECK(scopeSignalAvailable(static_cast<ScopeSignal>(i), true));
        }
        // Outside it, the multiplex position does not - and nothing else is
        // affected.
        CHECK(!scopeSignalAvailable(ScopeSignal::Mpx, false));
        CHECK(scopeSignalAvailable(ScopeSignal::Audio, false));
        CHECK(scopeSignalAvailable(ScopeSignal::Spectrum, false));
        CHECK(scopeSignalAvailable(ScopeSignal::Baseband, false));
        CHECK(scopeSignalAvailable(ScopeSignal::Vector, false));

        // A scope left on MPX and reopened on AM shows the audio spectrum -
        // the nearest thing that exists - rather than an empty tube captioned
        // with a signal there is none of.
        CHECK(scopeSignalForMode(ScopeSignal::Mpx, false) == ScopeSignal::Spectrum);
        CHECK(scopeSignalForMode(ScopeSignal::Mpx, true) == ScopeSignal::Mpx);
        // And nothing else is touched by the mode at all.
        CHECK(scopeSignalForMode(ScopeSignal::Vector, false) == ScopeSignal::Vector);
        CHECK(scopeSignalForMode(ScopeSignal::Audio, false) == ScopeSignal::Audio);

        // The key and the caption exist for it, because a position with no
        // engraving is a position nobody can choose on purpose.
        CHECK(std::strlen(cascade::gui::scopeSignalKey(ScopeSignal::Mpx)) > 0);
        CHECK(std::strcmp(cascade::gui::scopeSignalKey(ScopeSignal::Mpx), "MPX") == 0);
        CHECK(std::strstr(cascade::gui::scopeSignalCaption(ScopeSignal::Mpx), "MULTIPLEX") !=
              nullptr);

        // A saved index still clamps into range, MPX included - the config
        // carries an int and a hand edit can say anything.
        CHECK(cascade::gui::scopeSignalFromIndex(4) == ScopeSignal::Mpx);
        CHECK(cascade::gui::scopeSignalFromIndex(5) == ScopeSignal::Vector);
        CHECK(cascade::gui::scopeSignalFromIndex(-1) == ScopeSignal::Audio);
    }

    // --- THE LABELS MUST LAND ON THE BINS --------------------------------
    //
    // The one thing a picture of this cannot be trusted to show. The face
    // draws bin k at k/(n-1) across the glass and puts a label at
    // mpxFraction(hz, span); if those two mappings disagree, every rule is
    // drawn beside its signal instead of on it, and at a glance that looks
    // fine. So they are compared here, in the numbers the application really
    // uses: a 200 kHz composite, the 8192-point transform the page runs for
    // this position, and the 100 kHz span that falls out of them.
    //
    // (Measured against the running application once, on a synthesised
    // multiplex: the pilot spike sat on the pilot rule and the 67.65 kHz tone
    // on its own. This is that check made permanent and exact.)
    {
        const double rate = 200000.0;
        const std::size_t fft = 8192;
        const double binHz = rate / static_cast<double>(fft);
        const double asked = mpxSpanHz(rate);
        const std::size_t bins = cascade::gui::scopeSpectrumBins(fft, rate, asked);
        CHECK(bins > 1);

        // THE AXIS IS THE BINS, and this is where that was learned. The last
        // bin drawn IS the right-hand edge of the glass, and it lands up to
        // one bin BELOW the span that was asked for, because the bin count is
        // capped at the transform's positive half. The page therefore
        // publishes this number - not the nominal one - and the labels are
        // placed against it, so the two agree exactly rather than by a
        // fraction of a pixel. Caught by this check failing the first time it
        // was written, against a 24 Hz discrepancy no screenshot would ever
        // have shown.
        const double span = static_cast<double>(bins - 1) * binHz;
        CHECK(span <= asked);
        // AT MOST ONE BIN, and at 200 kHz into an 8192-point transform it is
        // EXACTLY one: the span asks for bin 4096, which is the Nyquist bin,
        // and the positive half stops at 4095. Inclusive, therefore - the
        // first draft wrote "<" and failed on precisely that equality.
        CHECK(asked - span <= binHz);

        for (std::size_t i = 0; i < kMpxBandCount; ++i) {
            const MpxBand& band = kMpxBands[i];
            const double marks[2] = {band.loHz, band.hiHz};
            for (const double hz : marks) {
                if (hz <= 0.0 || hz > span) { continue; }
                // Where the face puts the bin nearest this frequency...
                const std::size_t k = static_cast<std::size_t>(hz / binHz + 0.5);
                const double binFrac = static_cast<double>(k) /
                                       static_cast<double>(bins - 1);
                // ...and where the label goes.
                const double labelFrac = mpxFraction(hz, span);
                // Half a bin is the most they may differ - that is the
                // resolution of the picture itself - and in practice they
                // agree to floating-point noise, because both are now
                // fractions of the SAME axis.
                const double halfBin = 0.5 / static_cast<double>(bins - 1);
                CHECK(std::fabs(binFrac - labelFrac) <= halfBin);
            }
        }
    }

    return testSummary("test_fm_mpx_plan");
}
