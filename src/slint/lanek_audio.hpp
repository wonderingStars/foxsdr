// lanek_audio.hpp - SCREEN LANE K's TWO SOUND PATHS (docs/multi-radio-records/screen-k.md): the sonification tone and the spoken readout, each on the Windows default output and nowhere else.
//
//   ToneOut        a waveOut device fed 50 ms blocks of the access lane's ToneRenderer from the window's tick (no thread of its own: the tick refills the blocks the device has finished).
//   makeSpeaker    SAPI 5 on the default voice and the default output, asynchronous (the call returns at once; a new utterance may purge the one still speaking).
//
// Neither is ever opened in a bounded run or a test (the publisher's `bounded`), and neither is a radio: a screen reader or a tone is the user's own choice, off until switched on.
// NOBODY HAS LISTENED to either (docs/multi-radio-records/screen-k.md): they are written from the Windows documentation and proven only up to the call that opens the device.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <memory>
#include <string>

#include "core/access_sonify.hpp"
#include "core/access_speech.hpp"

namespace foxsdr::slintlanek {

class ToneOut {
public:
    ToneOut();
    ~ToneOut();
    ToneOut(const ToneOut&) = delete;
    ToneOut& operator=(const ToneOut&) = delete;
    // Opens the device on the first use; false when there is none or it refused (the reason in lastError()).
    bool start();
    void stop();
    bool running() const;
    // Refills any finished block with the tone for `params` (call once a tick while running).
    void feed(const cascade::core::access::ToneParams& params);
    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// nullptr off Windows or when SAPI cannot be created.
std::unique_ptr<cascade::core::access::Speaker> makeDeviceSpeaker();

}  // namespace foxsdr::slintlanek
