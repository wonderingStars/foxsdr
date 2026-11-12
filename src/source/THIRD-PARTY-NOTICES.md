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
