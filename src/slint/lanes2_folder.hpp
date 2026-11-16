// lanes2_folder.hpp - the folder chooser of screen lane S2 (docs/multi-radio-records/screen-s2.md), behind a plain function so a test never raises the real one: the host hands the publisher either this
// function (a person at the window) or a stand-in. On Windows it is the system's own folder picker (IFileOpenDialog with FOS_PICKFOLDERS), owned by the window with the keyboard focus; elsewhere it
// answers "" (nothing chosen). The answer is the folder's full path in UTF-8, "" when the person cancelled or the picker could not open.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

namespace foxsdr::slints2 {

std::string chooseFolderDialog();

}  // namespace foxsdr::slints2
