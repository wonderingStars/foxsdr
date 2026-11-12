# Third-party notices for the radio sources in src/source

Facts, tables and message layouts in the files named below were ported (written afresh in this repository's
style, not pasted) from the permissively licensed projects listed here. Files read on 2026-11-12 over HTTPS
(`gh api` raw contents); nothing was cloned or built from them. Each project's LICENSE file was opened first.

## SoapySDRPlay3 (pothosware) - used by sdrplay_rate_map.{hpp,cpp}

* Project: https://github.com/pothosware/SoapySDRPlay3 , commit read: 48bd8b41072534018de1d74deb3dea5874d9e0e0
* File read: Settings.cpp (getInputSampleRateAndDecimation, listSampleRates, getSampleRate, getBwEnumForRate,
  the RSPduo mode strings). Header of Settings.cpp: "Copyright (c) 2015 Charles J. Cliffe, Copyright (c) 2020
  Franco Venturi - changes for SDRplay API version 3 and Dual Tuner for RSPduo".
* Licence (LICENSE.txt, MIT):

> The MIT License (MIT)
>
> Copyright (c) 2015 Charles J. Cliffe
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
> documentation files (the "Software"), to deal in the Software without restriction, including without
> limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
> Software, and to permit persons to whom the Software is furnished to do so, subject to the following
> conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions
> of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
> TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
> THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
> CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
> DEALINGS IN THE SOFTWARE.

## RFspace / MoeTronix interface specifications - used by sdriq_protocol.hpp, sdriq_source.{hpp,cpp} and the checks in tests/test_ascp_maker_spec.cpp

* Documents (published by the maker for implementers at https://moetronix.com/svdownload.htm; opened and read
  2026-11-12; none states a licence, so no text is reproduced, only the item numbers, byte layouts and numeric
  limits that an interface consists of): "SDR-IQ Interface Specification" Ver. 1.04 (Dec 18, 2013),
  "NetSDR Interface Specification" Rev. 1.05 (Sept 8, 2014), "SDR-IP Interface Specification" Ver. 1.03.
* CuteSDR (Simplified BSD, Copyright 2010-2011 Moe Wheatley) is the reference implementation named in the brief
  and the source of the earlier ascp_protocol.hpp numbers; it was NOT read again for the SDR-IQ (its siqs/
  folder is GPL-3 and stays closed).

## ic-r8600-usb-iq (ibelinp) - used by icr8600_protocol.{hpp,cpp}, icr8600_source.{hpp,cpp}

* Project: https://github.com/ibelinp/ic-r8600-usb-iq , commit read: 4b1be8a3fe2e510023438b3f5a57df8ae342d443
* Files read: README.md, LICENSE, firmware/README.md, tools/spt_to_json.py, examples/linux/r8600_iq.c,
  examples/python/icr8600_load.py. Facts ported: USB ids and endpoints, the CI-V start-up order and rate codes, the
  I/Q sync-marker rule, the Cypress CSPT record layout. None of Icom's firmware is in that repository, none is in
  this one, none is fetched.
* Licence (LICENSE, MIT):

> MIT License
>
> Copyright (c) 2026 Pieter Ibelings
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
> documentation files (the "Software"), to deal in the Software without restriction, including without limitation
> the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
> to permit persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of
> the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
> THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
> CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
> IN THE SOFTWARE.
>
> NOTE: This MIT license covers the original code and documentation in this repository ONLY. It does NOT cover any
> Icom firmware you supply (e.g. a `firmware/spt_seq.json` you generate) - that is Icom's IC-R8600 FX2 firmware,
> Icom's copyrighted work, not part of this repository, all rights reserved by Icom. Not affiliated with or
> endorsed by Icom.

## SoapyICR8600 (dmitryelj) - cross-check for icr8600_protocol.hpp

* Project: https://github.com/dmitryelj/SoapyICR8600 , commit read: 0e0ebff20ed01abe9b2c49a4598f45ebcecca4f9
* Files read: WinUSBDevice.cpp, WinUSBDevice.h, Settings.cpp, Streaming.cpp, SoapyICR8600.hpp (each header says
  "Made in 2018 by D.Eliuseev"; none names a GPL or other origin). Facts used: the answer frames, the antenna and
  pre-amp commands, the HF-only antenna rule, the RF-gain code scale (observation, not specified).
* Licence (LICENSE, MIT): "Copyright (c) 2018 D. Eliuseev", otherwise the same MIT text as above (permission
  notice and warranty disclaimer identical).

## SoapyFCDPP (pothosware) - used by fcd_protocol.{hpp,cpp}, fcd_control.{hpp,cpp}

* Project: https://github.com/pothosware/SoapyFCDPP , commit read: 96c31ed20ceac1b9024fde3bf0a386767edb7ad6
* Files read: SoapyFCDPP/fcdcmd.h, fcd.c, fcd.h, SoapyFCDPP.cpp, SoapyFCDPP.hpp, README.md, LICENSE_1_0.txt and
  fcddirect.cpp (the last is a Soapy test program with no command in it). Headers: SoapyFCDPP.cpp "Copyright (c)
  2018 Albin Stigo, SPDX-License-Identifier: BSL-1.0"; fcd.c and fcd.h "Created by Albin Stigo ... Copyright 2016 /
  2018 Albin Stigo. All rights reserved" (no statement of another origin); fcdcmd.h has no header. The repository
  README says: "Boost Software License 1.0 (BSL-1.0)". Facts used: HID command numbers, the 64-byte report layout,
  the Pro gain tables and cutoffs, gain ranges, frequency bands, USB ids, the 192 / 96 kS/s rates.
  PROVENANCE CAVEAT: the macro names in fcdcmd.h match those of the GPL libfcd / qthid; this lane did not open any
  GPL source to compare, and the file carries no GPL notice.
* Licence (LICENSE_1_0.txt, Boost Software License 1.0):

> Boost Software License - Version 1.0 - August 17th, 2003
>
> Permission is hereby granted, free of charge, to any person or organization obtaining a copy of the software and
> accompanying documentation covered by this license (the "Software") to use, reproduce, display, distribute,
> execute, and transmit the Software, and to prepare derivative works of the Software, and to permit third-parties
> to whom the Software is furnished to do so, all subject to the following:
>
> The copyright notices in the Software and this entire statement, including the above license grant, this
> restriction and the following disclaimer, must be included in all copies of the Software, in whole or in part,
> and all derivative works of the Software, unless such copies or derivative works are solely in the form of
> machine-executable object code generated by a source language processor.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
> THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
> SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE FOR ANY DAMAGES OR OTHER LIABILITY,
> WHETHER IN CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
> OTHER DEALINGS IN THE SOFTWARE.

## libhackrf (Great Scott Gadgets) - used by hackrf_sweep.{hpp,cpp}

* Project: https://github.com/greatscottgadgets/hackrf , commit read: 3a63b29953a3b763f428821e02110def9fb1cade
* THE REPOSITORY AS A WHOLE IS GPL-2.0 (COPYING): its firmware and its command-line tools (including
  hackrf_sweep.c) are GPL and were NOT opened. Only two files were read, and the header of EACH was opened and read
  first: host/libhackrf/src/hackrf.c and host/libhackrf/src/hackrf.h. Both carry the BSD 3-clause licence below and
  neither names another origin. Facts used: HACKRF_VENDOR_REQUEST_INIT_SWEEP (26), the layout of
  hackrf_init_sweep's request, TRANSCEIVER_MODE_RX_SWEEP (5) and the USB API versions it needs, the sweep block
  header "0x7F - 0x7F - uint64_t frequency (LSBFIRST, in Hz)", BYTES_PER_BLOCK / SAMPLES_PER_BLOCK / MAX_SWEEP_RANGES,
  the sweep styles.
* Licence (the header of both files):

> Copyright (c) 2012-2026 Great Scott Gadgets <info@greatscottgadgets.com>
> Copyright (c) 2012, Jared Boone <jared@sharebrained.com>
> Copyright (c) 2013, Benjamin Vernoux <titanmkd@gmail.com>
>
> All rights reserved.
>
> Redistribution and use in source and binary forms, with or without modification, are permitted provided that
> the following conditions are met:
>
>     Redistributions of source code must retain the above copyright notice, this list of conditions and the
>     following disclaimer.
>     Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
>     following disclaimer in the documentation and/or other materials provided with the distribution.
>     Neither the name of Great Scott Gadgets nor the names of its contributors may be used to endorse or promote
>     products derived from this software without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
> WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
> PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
> INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
> SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
> THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
> ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## Hermes-Lite 2 wiki page "Protocol" - FACTS ONLY - used by hl2_protocol.hpp, the hl2 profile of hpsdr1_source.{hpp,cpp}

* Page: https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol (raw wiki markdown read on 2026-11-12).
* NO LICENCE: the repository (softerhardware/Hermes-Lite2) has no licence file, the page states none, and the gateware
  files carry "GNU GPL v2 or later" headers. Nothing of the repository was read or used - no gateware, no software, no
  file - and no code was copied from the page (it holds none). What was taken is the content of an interface
  description (which bit means what, which address holds what), re-expressed in this repository's own words and
  code. The page says of itself that it is "based on the original protocol1 from openHPSDR" (the USB-protocol and
  Metis documents the earlier lanes used).
* Not built, by decision: every word that writes to the radio's own hardware or stored settings (I2C, AD9866 SPI,
  PA bias, EEPROM with its fixed IP and MAC, clock/synchronisation/master commands, reset on disconnect), transmit,
  PureSignal, CW, the wide-band stream.

## openHPSDR documents (earlier lane feat/hpsdrduo; re-read in part by this lane)

* "openHPSDR Ethernet Protocol v4.4" (Phil Harman VK6PH and contributors, March 2019,
  https://github.com/TAPR/OpenHPSDR-Firmware/tree/master/Protocol%202/Documentation - the repository as a whole is
  GPL-3.0, no firmware or gateware was opened; the PDF states no licence): used for src/source/hpsdr2_protocol.*.
  This lane re-checked the Board Type list (0 Atlas, 1 and 2 Hermes, 3 Angela, 4 Orion, 5 Orion Mk II, 6 Hermes Lite,
  10 Saturn) and Appendix A for board 1 (DSP clock 122.88 MHz, one RF ADC, 48 to 1536 ksps, 238 I&Q samples per 24-bit
  packet) against the existing tables: they agree.
* "Metis - How it works" V1.33 and the USB data protocol V1.58/V1.60: src/source/hpsdr_protocol.* and hpsdr1_frames.hpp.

# Third-party notices for src/source (lane ossnet, 2026-11-12)

Each entry: project, URL, the commit read, what was taken, and the licence text in full. Code here is this repository's own;
protocol facts, numbers and layouts were ported from the projects below, with attribution. Nothing was copied whole.

## spy2go (SpyServer client, Go) - Apache-2.0
- URL: https://github.com/racerxdl/spy2go , commit 14102c047be58ea987be26a4d74639f7a506b2ff (read 2026-11-12)
- Files read: spyserver/protocol.go, spyserver/SpyServer.go, spytypes/spytypes.go, README.md, LICENSE
- Used in: spyserver_protocol.hpp, spyserver_source.hpp, spyserver_source.cpp (constants, message layouts, session order, decimation table)
- The repository's LICENSE file is the standard Apache License 2.0 text with the appendix template unfilled (no copyright holder named); it is reproduced below exactly as read.

```text
                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS

   APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "[]"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

   Copyright [yyyy] [name of copyright owner]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

```

## spyclient (SpyServer client, Python) - MIT
- URL: https://github.com/Tubbz-alt/spyclient (mirror of https://github.com/sam210723/spyclient), commit ace7f8ac0b4d9287a454835c9cb655115c2a2f89 (read 2026-11-12)
- Files read: spyclient/spyclient.py, enums.py, tuples.py, README.md, LICENSE (and the test and docs folder listings; the bundled third-party binaries in test/ were NOT downloaded or opened)
- Used in: spyserver_protocol.hpp (protocol version, HELLO layout, DEVICE_INFO and CLIENT_SYNC layouts, message and setting numbers)

```text
MIT License

Copyright (c) 2020 Sam

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## gr-aaronia_rtsa (GNU Radio source block for the RTSA-Suite HTTP server) - MIT
- URL: https://github.com/hb9fxq/gr-aaronia_rtsa , commit 72b291b09c503fa38ce7a7805d1e4e7b860e73ba (read 2026-11-12)
- Files read: lib/rtsa_http_server_source_impl.{cc,h}, include/gnuradio/aaronia_rtsa/rtsa_http_server_source.h, grc/aaronia_rtsa_rtsa_http_server_source.block.yml, MANIFEST.md, docs/README.aaronia_rtsa, LICENSE. Every source file read carries the same MIT header; none is under the GPL (the GNU Radio runtime it links is not part of the files read and was not opened).
- Used in: aaronia_protocol.hpp, aaronia_source.{hpp,cpp} (the block names Block_IQDemodulator_1 / Block_Spectran_V6B_1, the demodulator-plus-SPECTRAN control scheme, the default endpoint 127.0.0.1:54664)

## libspectranstream (HTTP stream client library the block calls) - MIT
- URL: https://github.com/hb9fxq/libspectranstream , commit d50791e5de02d49132e62314909dd18c8d871b57 (read 2026-11-12)
- Files read: lib/spectranstream.cpp, lib/spectranstream.h, app/test_req.http, README.md, LICENSE
- Used in: aaronia_protocol.hpp, aaronia_source.{hpp,cpp} (raw record delimiting by the 0x1E byte, /stream?format=float32 and /stream?format=int16&scale=32767, the PUT /remoteconfig request bodies, the /info probe)

```text
MIT License

Copyright (c) 2021 Frank Werner-Krippendorf

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

(gr-aaronia_rtsa's LICENSE file is the identical MIT text, "Copyright (c) 2021 Frank Werner-Krippendorf".)
