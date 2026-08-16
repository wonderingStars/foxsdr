@echo off
REM Launches FoxSDR from the build tree with the PATH the radio needs.
REM
REM WHY THIS EXISTS RATHER THAN A SHORTCUT STRAIGHT TO cascade.exe.
REM radioconda's SoapyUHD module (uhdSupport.dll) is built against UHD 4.8 and
REM has to resolve radioconda's own uhd.dll. If the standalone UHD 4.10 is
REM found first, opening a B200 fails with "ABI compatibility mismatch" - the
REM application starts perfectly, shows the generator, and simply cannot see
REM the radio. That is a confusing way to discover a PATH problem, so the PATH
REM is set here instead of being left to chance.
REM
REM SoapySDR.dll sits next to cascade.exe already and needs nothing added.

setlocal
REM Guarded, so this still launches on a machine with no radioconda install -
REM there the app simply runs without UHD hardware support rather than failing
REM to start.
set "RADIOCONDA=C:\Users\steve\radioconda\Library\bin"
if exist "%RADIOCONDA%" set "PATH=%RADIOCONDA%;%PATH%"

REM `start` so this console closes immediately instead of sitting around for
REM as long as the application runs.
start "FoxSDR" "%~dp0..\build\Release\cascade.exe"
