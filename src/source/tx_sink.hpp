// tx_sink.hpp - the other direction. IqSink is to a transmitter what
// IqSource is to a receiver: the one interface everything above the radio
// talks to, so the modulator, the TX thread and the panel never know which
// board is on the end of it.
//
// WHY IT IS NOT JUST IqSource WITH write() INSTEAD OF read(). Three of the
// differences are structural rather than cosmetic, and every one of them is a
// safety property:
//
//   1. GAIN IS AN ATTENUATION HERE, and its "more" direction is DOWN. An
//      AD9361's transmit gain is published in negative decibels - 0 dB is
//      full output and -89.75 dB is as quiet as the part goes - so a control
//      that treated it like a receive gain and clamped a stray 40 into range
//      would clamp it to MAXIMUM POWER. gainRangeDb() reports the board's own
//      pair and set/get talk in exactly those numbers, so there is no place
//      in this interface where a larger number quietly means more RF.
//
//   2. A SINK THAT IS NOT RUNNING IS NOT TRANSMITTING, and that is a promise
//      rather than a description. start() is the only thing that may key a
//      radio; stop() and ~IqSink() must leave it quiet even if the caller
//      never called stop(), and must leave it quiet even if the board stopped
//      answering halfway through being told to. The implementation comment on
//      PlutoTx::stop() is where that is spelled out for a real board.
//
//   3. write() MAY NOT BLOCK THE CALLER INDEFINITELY. The TX thread feeding
//      it is also the thread that notices the PTT has been released, so a
//      write parked for ever on a board that has gone is a transmitter that
//      cannot be unkeyed. Every implementation bounds it and returns what it
//      accepted, exactly as IqSource::read returns what it produced.
//
// THREADING, and it is the mirror image of IqSource's. start/stop and the
// setters are called from the GUI or control thread; write() only ever from
// the transmitter's own thread. running() and faulted() are polled from both,
// so they are answered without taking anything the other side holds across a
// network round trip.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>

namespace cascade::source {

class IqSink {
public:
    virtual ~IqSink() = default;

    // --- keying ------------------------------------------------------------

    // THE ONLY THING THAT MAY PUT RF OUT OF A RADIO. Opening a sink, setting
    // its frequency and setting its gain are all silent; this is the step
    // that is not, and it is the step a PTT is wired to. Idempotent when
    // already running. False with lastError() set on failure, and a failed
    // start must leave the radio as quiet as a successful stop would.
    virtual bool start() = 0;

    // Quiet, in that order: attenuation to its maximum FIRST, and only then
    // whatever else the board offers. Idempotent, safe before start(), safe
    // after a fault, and safe from a destructor - which is where it is called
    // from when a caller forgets.
    virtual void stop() = 0;

    // THE ORDINARY END OF A TRANSMISSION, as opposed to an emergency one.
    // Called by the transmitter only once the modulation it has written has
    // ALREADY been ramped down to zero (the modulator's raised-cosine
    // key-up): everything write() accepted is let through to the air, then
    // the radio is made quiet exactly as stop() makes it. stop() instead
    // silences at once and throws away whatever is still queued - which is
    // right for a fault, a frozen window or a destructor, and wrong here,
    // because on a board with a queue between write() and the DAC it drops
    // the very ramp that exists to stop a transmission ending in a step.
    // BOUNDED like stop(); a sink with nothing queued has nothing to let
    // through, so the default is simply stop().
    virtual void finish() { stop(); }

    virtual bool running() const = 0;

    // --- what the board will accept ----------------------------------------

    virtual double sampleRateHz() const = 0;
    virtual bool setSampleRateHz(double hz) = 0;

    virtual double centerFrequencyHz() const = 0;
    virtual bool setCenterFrequencyHz(double hz) = 0;

    // THE BOARD'S OWN LIMITS, not a table. False means "this sink does not
    // know", which is an answer; a made-up range is not. The panel shows the
    // pair when there is one and says so when there is not, the same rule
    // PlutoSource::frequencyRangeHz follows on the receive side.
    virtual bool frequencyRangeHz(double& loHz, double& hiHz) const = 0;
    virtual bool sampleRateRangeHz(double& loHz, double& hiHz) const = 0;

    // The transmit gain, in the board's own units - negative decibels of
    // ATTENUATION on everything this interface currently has behind it. See
    // point 1 in the file header for why that is stated so loudly.
    virtual bool gainRangeDb(double& loDb, double& hiDb) const = 0;
    virtual double gainDb() const = 0;
    virtual bool setGainDb(double db) = 0;

    // --- the samples -------------------------------------------------------

    // Hand over up to n complex samples at sampleRateHz(), each in [-1, 1) on
    // both axes. Returns how many were accepted; less than n means the board
    // is not taking them as fast as they are being made and the caller should
    // drop the rest rather than queue them, because stale modulation is worse
    // than a gap. BOUNDED: see point 3 in the file header.
    virtual std::size_t write(const std::complex<float>* samples, std::size_t n) = 0;

    // --- state -------------------------------------------------------------

    // True once the sink has hit something it cannot recover from by being
    // written to again. Polled by the transmitter after every write, exactly
    // as the pipeline's source loop polls IqSource::faulted() - and with the
    // same consequence, which here is that the radio is unkeyed.
    virtual bool faulted() const = 0;

    virtual const char* name() const = 0;
    virtual const char* lastError() const = 0;
};

}  // namespace cascade::source
