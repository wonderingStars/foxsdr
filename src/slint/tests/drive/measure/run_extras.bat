@echo off
rem lane B: the tick's work with the extras off and on, default renderer, three runs each (hidden desktop, FOXSDR_FRAME_CAP_HZ unset)
set EXE=D:\foxsdr-build\mr-screen1\src\slint\Release\test_slint_drive_run.exe
set DIR=C:\Users\steve\foxsdr-wt\mr-screen1\src\slint\tests\drive\measure
set OUT=D:\foxsdr-build\mr-screen1-meter-b
mkdir %OUT% 2>nul
set FOXSDR_DRIVE_VERBOSE=1
for %%R in (1 2 3) do (
 for %%N in (off on) do (
  %EXE% %DIR%\extras-%%N.txt > %OUT%\extras-%%N-%%R.log 2>&1
 )
)
echo done > %OUT%\done
