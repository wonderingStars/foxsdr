@echo off
rem lane K: the tick's work with lane K left out and with everything per-frame on, three runs each (hidden desktop, FOXSDR_FRAME_CAP_HZ unset)
set EXE=D:\foxsdr-build\en-screen-k\src\slint\Release\test_slint_drive_run.exe
set DIR=D:\foxsdr-wt\en-screen-k\src\slint\tests\drive\measure
set OUT=D:\foxsdr-build\sk-cost
mkdir %OUT% 2>nul
set FOXSDR_DRIVE_VERBOSE=1
for %%R in (1 2 3) do (
 for %%N in (off on) do (
  %EXE% %DIR%\lanek-%%N.txt > %OUT%\lanek-%%N-%%R.log 2>&1
 )
)
echo done > %OUT%\done
