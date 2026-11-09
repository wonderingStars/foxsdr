@echo off
rem usage: adsb-classic.bat <frames>  (through run_on_hidden_desktop.ps1): the CLASSIC window (cascade.exe) on the owner's B200 ADS-B profile
rem (src\slint\tests\drive\fixtures\adsb-b200.json) in a scratch profile with the adsb-decoder plugin alone, the receiver running
rem (CASCADE_DECODE_TEST), the plugin's frame log written to adsb-classic-frames.csv beside this file (docs 6.51). Nothing of the
rem real %APPDATA%\foxsdr is read or written: every folder and endpoint is redirected as the drive test does.
setlocal
set ROOT=C:\Users\steve\foxsdr-wt\engine-stage4
set SCRATCH=%TEMP%\foxsdr-adsb-classic-%RANDOM%
mkdir "%SCRATCH%\appdata" "%SCRATCH%\local\foxsdr\plugins" "%SCRATCH%\home" "%SCRATCH%\diag" 2>nul
copy /y "%LOCALAPPDATA%\FoxSDR\plugins\adsb-decoder-1.9.1-abi3-win-x64.dll" "%SCRATCH%\local\foxsdr\plugins\" >nul
copy /y "%LOCALAPPDATA%\FoxSDR\plugins\catalogue.json" "%SCRATCH%\local\foxsdr\plugins\" >nul
copy /y "%LOCALAPPDATA%\FoxSDR\plugins\installed.json" "%SCRATCH%\local\foxsdr\plugins\" >nul
copy /y "%ROOT%\src\slint\tests\drive\fixtures\adsb-b200.json" "%SCRATCH%\config.json" >nul
rem the owner's SoapyUHD module lives under the REAL user profile (radioconda); named before the profile is redirected, or the saved B200 is "absent"
if not defined SOAPY_SDR_ROOT if exist "%USERPROFILE%\radioconda\Library\lib\SoapySDR" set SOAPY_SDR_ROOT=%USERPROFILE%\radioconda\Library
rem 60 frames a second: the hidden desktop has no display to pace the swap, and an uncapped --frames 18000 ran out in 16 s
set FOXSDR_FRAME_CAP_HZ=60
set APPDATA=%SCRATCH%\appdata
set LOCALAPPDATA=%SCRATCH%\local
set USERPROFILE=%SCRATCH%\home
set HOME=%SCRATCH%\home
set FOXSDR_DIAG_DIR=%SCRATCH%\diag
set FOXSDR_TELEMETRY_URL=http://127.0.0.1:9/
set FOXSDR_CRASH_URL=http://127.0.0.1:9/
set FOXSDR_UPDATE_URL=http://127.0.0.1:9/
set FOXSDR_REPORTS_URL=http://127.0.0.1:9/
set FOXSDR_FEATURE_URL=http://127.0.0.1:9/
set FOXSDR_PROBLEM_URL=http://127.0.0.1:9/
set CASCADE_CONFIG_TEST=%SCRATCH%\config.json
set CASCADE_PLUGIN_DIR_TEST=%SCRATCH%\local\foxsdr\plugins
set CASCADE_DECODE_TEST=1
set FOXSDR_PASS=free
set FOXSDR_ADSB_LOG=%ROOT%\adsb-classic-frames.csv
del /q "%ROOT%\adsb-classic-frames.csv" 2>nul
cd /d "%ROOT%\build\Release"
echo scratch %SCRATCH% > "%ROOT%\adsb-classic.log"
.\cascade.exe --frames %1 >> "%ROOT%\adsb-classic.log" 2>&1
echo exit %ERRORLEVEL% >> "%ROOT%\adsb-classic.log"
