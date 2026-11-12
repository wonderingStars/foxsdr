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
