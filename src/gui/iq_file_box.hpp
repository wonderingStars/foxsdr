// gui/iq_file_box.hpp - what the Source section's I/Q-file row does with the text in its box: see core/iq_file_box.hpp, where the pure
// functions live (the engine logs a refused open and includes nothing of the interface). Re-exported here under the names the window uses.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_IQ_FILE_BOX_HPP
#define CASCADE_GUI_IQ_FILE_BOX_HPP

#include "core/iq_file_box.hpp"

namespace cascade::gui {

using cascade::core::iqOpenReasonForLog;
using cascade::core::isWavName;
using cascade::core::typedIqPath;

}  // namespace cascade::gui

#endif  // CASCADE_GUI_IQ_FILE_BOX_HPP
