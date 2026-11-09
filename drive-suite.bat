@echo off
setlocal EnableDelayedExpansion
rem usage: drive-suite.bat [pattern]   (through ~/.claude/scripts/run_on_hidden_desktop.ps1): every script of src\slint\tests\drive\<pattern>.txt
rem through test_slint_drive_run, one after another, verbose (the dumps are read from the logs); one log per script,
rem drive-suite\<name>.log beside this file, and a summary line per script in drive-suite\summary.txt.
set PATTERN=%1
if "%PATTERN%"=="" set PATTERN=*
set ROOT=C:\Users\steve\foxsdr-wt\engine-stage4
set FOXSDR_DRIVE_VERBOSE=1
if not exist "%ROOT%\drive-suite" mkdir "%ROOT%\drive-suite"
del /q "%ROOT%\drive-suite\summary.txt" 2>nul
cd /d "%ROOT%\build\src\slint\Release"
for %%f in ("%ROOT%\src\slint\tests\drive\%PATTERN%.txt") do (
    .\test_slint_drive_run.exe "%%f" > "%ROOT%\drive-suite\%%~nf.log" 2>&1
    echo %%~nf exit !ERRORLEVEL! >> "%ROOT%\drive-suite\summary.txt"
)
echo done > "%ROOT%\drive-suite\done"
