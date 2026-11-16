@echo off
rem lane S2: the tick's work with lane S2 left out and with everything per-frame on, three runs each (hidden desktop)
set EXE=D:\foxsdr-build\en-screen-k\src\slint\Release\test_slint_drive_run.exe
set DIR=D:\foxsdr-wt\en-screen-k\src\slint\tests\drive\measure
set OUT=D:\foxsdr-build\s2-scratch\cost
mkdir %OUT% 2>nul
set FOXSDR_DRIVE_VERBOSE=1
for %%R in (1 2 3) do (
 for %%N in (off on) do (
  %EXE% %DIR%\s2-%%N.txt > %OUT%\s2-%%N-%%R.log 2>&1
 )
)
echo done > %OUT%\done
