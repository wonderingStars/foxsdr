@echo off
rem the B8 matrix: {1, 4 radios} x {default renderer, software renderer} x {FOXSDR_FRAME_CAP_HZ unset, 60}; three runs each
set EXE=D:\foxsdr-build\mr-screen1\src\slint\Release\test_slint_drive_run.exe
set DIR=C:\Users\steve\foxsdr-wt\mr-screen1\src\slint\tests\drive\measure
set OUT=D:\foxsdr-build\mr-screen1-meter
mkdir %OUT% 2>nul
set FOXSDR_DRIVE_VERBOSE=1
for %%R in (1 2 3) do (
 for %%S in (meter-one meter-four) do (
  for %%B in (default software) do (
   for %%C in (unset 60) do (
    setlocal
    if "%%B"=="software" set SLINT_BACKEND=winit-software
    if "%%C"=="60" set FOXSDR_FRAME_CAP_HZ=60
    %EXE% %DIR%\%%S.txt > %OUT%\%%S-%%B-%%C-%%R.log 2>&1
    endlocal
   )
  )
 )
)
