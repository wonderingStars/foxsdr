@echo off
rem foxsdr-slint launcher for THIS worktree: a SCRATCH profile, as the GUI test gate uses, so the product's own
rem %APPDATA%\foxsdr\config.json is never read or written. foxsdr-slint boots as the application now (bootProcess, as the
rem ImGui window does): an interactive run reads and writes %APPDATA%\foxsdr\config.json, and NOTHING refuses the real file
rem any more - the APPDATA, LOCALAPPDATA and USERPROFILE redirect below IS the protection. Never run the exe without it.
rem (slint-spike-run.bat is the spike's launcher and points at the spike worktree's build.)
set SCRATCH=C:\Users\steve\foxsdr-wt\scratch-slint-spike
if not exist "%SCRATCH%\appdata" mkdir "%SCRATCH%\appdata"
if not exist "%SCRATCH%\local" mkdir "%SCRATCH%\local"
if not exist "%SCRATCH%\profile" mkdir "%SCRATCH%\profile"
set APPDATA=%SCRATCH%\appdata
set LOCALAPPDATA=%SCRATCH%\local
set USERPROFILE=%SCRATCH%\profile
set HOME=%SCRATCH%\profile
rem the GUI test gate's switch too: it names the settings file of a BOUNDED run (slint-run.bat --frames 30) and is ignored by an
rem interactive one, whose file is the scratch profile's own %APPDATA%\foxsdr\config.json
set CASCADE_CONFIG_TEST=%SCRATCH%\cfg.json
set PATH=C:\Users\steve\slint-cpp-1.18.1\lib;%PATH%
set EXE=%~dp0build\src\slint\Release\foxsdr-slint.exe
cd /d "%~dp0build\src\slint\Release"
rem the executable is named by its full path: a bare name failed under cmd /c with 9009 (docs 6.34)
"%EXE%" %*
echo exit %ERRORLEVEL%
