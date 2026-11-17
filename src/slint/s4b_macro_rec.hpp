// s4b_macro_rec.hpp - screen lane S4b, G3-34: the macro recorder's tap on the table of engine calls the window sends its commands through.
//
// Every command the window sends goes through the API table its publishers are handed (the same table copy the frequency lock and the delta lock sit in). recordingApi() is another such copy: its
// submit and submit_text call the real ones, and for each command the engine ACCEPTED while the person has recording on, hands it to the recorder (s4b_logic.hpp), which keeps only the
// receiver's own tuning and listening settings. With recording off the wrapper does nothing but pass the call on.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>

#include "foxsdr_api.h"

namespace foxsdr::slints4b {

const FoxEngineApi* recordingApi(const FoxEngineApi* real);

// The recorder's lock: the window's thread starts and stops it, the wrapper notes from whichever thread submits.
void recorderStart();
void recorderStop();
void recorderClear();
bool recorderActive();
std::size_t recorderSteps();
std::size_t recorderSkipped();
std::string recorderSaveJson(const std::string& name);

}  // namespace foxsdr::slints4b
