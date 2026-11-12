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
