@echo off
rem lane S1 item 4: the tick's work with 0, 64 and 256 made-up marks, default renderer, three runs each (hidden desktop, FOXSDR_FRAME_CAP_HZ unset)
set EXE=D:\foxsdr-build\mr-screen1\src\slint\Release\test_slint_drive_run.exe
set DIR=C:\Users\steve\foxsdr-wt\mr-screen1\src\slint\tests\drive\measure
set OUT=D:\foxsdr-build\mr-screen1-meter-s1
mkdir %OUT% 2>nul
set FOXSDR_DRIVE_VERBOSE=1
for %%R in (1 2 3) do (
 for %%N in (0 64 256 256-plain) do (
  %EXE% %DIR%\marks-%%N.txt > %OUT%\marks-%%N-%%R.log 2>&1
 )
)
echo done > %OUT%\done
