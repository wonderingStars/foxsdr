@echo off
rem the lane C frame-cost comparison: the same run with the lane's tick (on) and without it (FOXSDR_SLINT_LANEC_OFF=1, off), three each, alternating
set EXE=D:\foxsdr-build\feat-gaps\src\slint\Release\test_slint_drive_run.exe
set DIR=C:\Users\steve\foxsdr-wt\feat-gaps\src\slint\tests\drive\measure
set OUT=D:\foxsdr-build\feat-gaps-lanec-cost
mkdir %OUT% 2>nul
set FOXSDR_FRAME_CAP_HZ=60
set FOXSDR_DRIVE_VERBOSE=1
for %%R in (1 2 3) do (
 for %%M in (on off) do (
  setlocal
  if "%%M"=="off" set FOXSDR_SLINT_LANEC_OFF=1
  %EXE% %DIR%\lanec-cost.txt > %OUT%\lanec-cost-%%M-%%R.log 2>&1
  endlocal
 )
)
echo MEASURE-FINISHED > %OUT%\done.txt
