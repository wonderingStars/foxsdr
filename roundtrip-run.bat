@echo off
rem usage: roundtrip-run.bat <tag>  (through run_on_hidden_desktop.ps1): the settings round-trip test, log roundtrip-<tag>.log beside this file
cd /d C:\Users\steve\foxsdr-wt\engine-stage4\build\src\slint\Release
.\test_slint_config_roundtrip.exe > ..\..\..\..\roundtrip-%1.log 2>&1
echo test exit %ERRORLEVEL% >> ..\..\..\..\roundtrip-%1.log
