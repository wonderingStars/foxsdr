@echo off
rem foxsdr-slint (spike) launcher: a SCRATCH profile, as the GUI test gate uses, so the product's own
rem %APPDATA%\foxsdr\config.json is never read or written. main.cpp refuses the real file regardless.
set SCRATCH=C:\Users\steve\foxsdr-wt\scratch-slint-spike
if not exist "%SCRATCH%\appdata" mkdir "%SCRATCH%\appdata"
if not exist "%SCRATCH%\local" mkdir "%SCRATCH%\local"
if not exist "%SCRATCH%\profile" mkdir "%SCRATCH%\profile"
set APPDATA=%SCRATCH%\appdata
set LOCALAPPDATA=%SCRATCH%\local
set USERPROFILE=%SCRATCH%\profile
set HOME=%SCRATCH%\profile
set FOXSDR_SLINT_CONFIG=%SCRATCH%\slint-spike-config.json
rem the GUI test gate's switch too, so this throwaway profile behaves exactly as a test run does
set CASCADE_CONFIG_TEST=%SCRATCH%\cfg.json
set PATH=C:\Users\steve\slint-cpp-1.18.1\lib;%PATH%
cd /d C:\Users\steve\foxsdr-wt\slint-spike\build\src\slint\Release
"C:\Users\steve\foxsdr-wt\slint-spike\build\src\slint\Release\foxsdr-slint.exe" %*
echo exit %ERRORLEVEL%
