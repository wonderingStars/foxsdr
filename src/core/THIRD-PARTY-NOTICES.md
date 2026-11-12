# Third-party notices for src/core (lane ossnet, 2026-11-12)

## SDRunoPlugin_Fran (frequency annotate plugin for SDRuno) - BSD 3-clause wording in its README
- URL: https://github.com/LinuxSheeple-E/SDRunoPlugin_Fran , commit bdee270f9c8887493f7e7db47d9157fe309005f4 (read 2026-11-12)
- Files read: Readme.md (the licence text below), 00Readme_Fran.txt, SDRunoPlugin_Fran.cpp (function loadS1bCsvFile), SDRunoPlugin_Fran.h (struct SWSKEDSRecord). The repository has no LICENSE file and GitHub shows no licence; the licence is the permission text in the README, reproduced below exactly as read.
- Used in: core/freq_import_sdruno.hpp - the COLUMN LIST of an .s1b (Frequency, Scan, M, Station, UTC, SubM, Filter, Port, Threshold), the 8-or-9-column rule and the double-quote escape; no code was copied or paraphrased, and the plugin's behaviour is not reproduced.
- The plugin bundles ben-strasser's fast-cpp-csv-parser (csvlib/, 3-clause BSD, "Copyright (c) 2015, ben-strasser"; text in the README below). Nothing of it was read or used.

### Readme.md, section Copyrights, verbatim

```text
## Copyrights
Portions are Copyright (C) 2020 Eric A. Cottrell
Contact: eric.c.boston@gmail.com

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

CSV reading library:
Copyright (c) 2015, ben-strasser
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of fast-cpp-csv-parser nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Vocabulary of the mode column
- Public sample banks in https://github.com/CaptDaags/SDRUno_S1B (no licence file) were read ONLY to learn which mode, sub-mode, filter and title-line forms occur; nothing of them was copied or shipped. The codes seen: AM, FM, USB, LSB, CW; sub-mode NFM; filters in hertz or with a K suffix.
