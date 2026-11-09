@echo off
rem usage: drive-run.bat <tag> [script]   (run through ~/.claude/scripts/run_on_hidden_desktop.ps1): one driven run of the Slint
rem window through test_slint_drive_run, on a scratch profile the test makes itself; the log lands beside this file.
set TAG=%1
set SCRIPT=%2
cd /d C:\Users\steve\foxsdr-wt\engine-stage4\build\src\slint\Release
.\test_slint_drive_run.exe %SCRIPT% > ..\..\..\..\drive-run-%TAG%.log 2>&1
echo test exit %ERRORLEVEL% >> ..\..\..\..\drive-run-%TAG%.log
echo done > ..\..\..\..\drive-run-%TAG%.done
