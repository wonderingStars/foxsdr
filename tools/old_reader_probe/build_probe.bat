@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set R=C:\Users\steve\foxsdr-wt\mr-engine1
set B=D:\foxsdr-build\mr-engine1
set INC=/I%R%\src /I%B%\generated /I%R%\third_party\foxsdr_api /I%R%\third_party\portaudio\include /I%R%\third_party\nlohmann_json\include /I%R%\third_party /I%R%\third_party\imgui /I%R%\third_party\cpp_httplib\include /IC:\vcpkg\installed\x64-windows\include
set DEFS=/DWIN32 /D_WINDOWS /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS
set LIBS=%B%\Release\cascade_engine.lib %B%\third_party\portaudio\Release\portaudio_static_x64.lib uuid.lib %B%\Release\cascade_pffft.lib ws2_32.lib C:\vcpkg\installed\x64-windows\lib\SoapySDR.lib winusb.lib setupapi.lib cfgmgr32.lib newdev.lib winmm.lib mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib runtimeobject.lib comdlg32.lib kernel32.lib user32.lib gdi32.lib winspool.lib shell32.lib oleaut32.lib advapi32.lib
cd /d %~dp0
echo == new writer
cl /nologo /std:c++20 /EHsc /MD /O1 /utf-8 /Zc:__cplusplus %DEFS% %INC% ..\newwriter\new_main.cpp /Fe:new_writer.exe /link %LIBS%
echo == old reader
cl /nologo /std:c++20 /EHsc /MD /O1 /utf-8 /Zc:__cplusplus %DEFS% /I%~dp0. %INC% old_main.cpp old_config.cpp /Fe:old_reader.exe /link %LIBS%
echo BAT_DONE
