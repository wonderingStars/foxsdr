@echo off
rem screen lane J: the frame cost of the map page with the new layers off and on, default renderer, three runs each (hidden desktop, FOXSDR_FRAME_CAP_HZ unset)
set EXE=D:\foxsdr-build\en-screen-j\src\slint\Release\test_slint_drive_run.exe
set DIR=D:\foxsdr-wt\en-screen-j\src\slint\tests\drive\measure
set OUT=D:\foxsdr-build\en-screen-j-meter
mkdir %OUT% 2>nul
set FOXSDR_DRIVE_VERBOSE=1
for %%R in (1 2 3) do (
 for %%N in (off on) do (
  %EXE% %DIR%\mapj-%%N.txt > %OUT%\mapj-%%N-%%R.log 2>&1
 )
)
echo done > %OUT%\done
