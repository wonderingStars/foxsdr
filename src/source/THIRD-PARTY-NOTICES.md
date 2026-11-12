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
