; FoxSDR (cascade) Windows installer (Inno Setup 6).
;
; Builds a self-contained setup.exe: cascade.exe + SoapySDR.dll (the hardware
; ABI boundary — the ONLY runtime DLL the build produces, enforced below) plus
; app-local MSVC runtime DLLs (msvcp140/vcruntime140/vcruntime140_1, sourced
; from the VS Redist folder — mirrors mayhem-b200's app-local-CRT approach; the
; UCRT is inbox on Windows 10+). Hardware vendor modules (SoapyUHD etc.) are
; deliberately NOT bundled — see POSTINSTALL.txt; the app runs with zero
; hardware (signal generator + IQ file playback).
;
; Compile:  "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer\cascade.iss
;   (or "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" on a machine-wide install)
; Override the VC CRT source dir if your VS version differs:
;   ISCC.exe /DVcCrtDir="<...>\VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT" installer\cascade.iss
;
; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#define AppName "FoxSDR"
#define AppVersion "0.27.0"
#define AppPublisher "Steven Fairclough"
#define AppExe "cascade.exe"
; Install-dir leaf avoids '+' (some tools mishandle it in paths); display name keeps it.
#define InstallLeaf "FoxSDR"
#define BuildDir AddBackslash(SourcePath) + "..\build\Release"
#ifndef VcCrtDir
  #define VcCrtDir "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT"
#endif

; ---------------------------------------------------------------------------
; Compile-time payload contract checks — fail loudly, never ship silently.
; The build's runtime payload is cascade.exe + SoapySDR.dll ONLY (everything
; else is statically linked). If another *.dll ever appears in build\Release,
; the compile aborts so the new DLL is added here DELIBERATELY or removed.
; ---------------------------------------------------------------------------
#if !FileExists(BuildDir + "\cascade.exe")
  #pragma error "cascade.exe not found in build\Release — build the Release configuration first (see README.md)"
#endif
#if !FileExists(VcCrtDir + "\msvcp140.dll")
  #pragma error "VC CRT redist dir not found: " + VcCrtDir + " — pass /DVcCrtDir=<...\Microsoft.VC143.CRT>"
#endif
#define DllCount 0
#define FindHandle
#define FindResult
#sub CheckRuntimeDll
  #define DllName FindGetFileName(FindHandle)
  #if LowerCase(DllName) != "soapysdr.dll"
    #pragma error "Unexpected runtime DLL in build\Release: " + DllName + " — payload contract is cascade.exe + SoapySDR.dll only; update installer\cascade.iss deliberately"
  #endif
  #expr DllCount = DllCount + 1
#endsub
#for {FindHandle = FindResult = FindFirst(BuildDir + "\*.dll", 0); FindResult; FindResult = FindNext(FindHandle)} CheckRuntimeDll
#if FindHandle
  #expr FindClose(FindHandle)
#endif
#if DllCount == 0
  #pragma error "SoapySDR.dll not found in build\Release — the build is incomplete"
#endif

[Setup]
AppId={{B3D4A7E2-6C51-4F8B-9A0D-2E7F5C8B1A64}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#InstallLeaf}
DefaultGroupName={#AppName}
DisableProgramGroupPage=auto
LicenseFile=..\LICENSE
InfoAfterFile=POSTINSTALL.txt
OutputDir=Output
OutputBaseFilename=foxsdr-setup-{#AppVersion}
; The setup.exe's own icon (Explorer, the download bar, the UAC prompt) and the
; wizard's title-bar icon. Same .ico the executable embeds via
; resources\icon\foxsdr.rc, so setup and app are visually one product.
SetupIconFile=..\resources\icon\foxsdr.ico
; Add/Remove Programs icon. Resolves to the installed cascade.exe, which now
; carries the icon as its lowest-numbered RT_GROUP_ICON — no separate .ico is
; installed, and nothing here needed to change for the icon to appear.
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName} {#AppVersion}
WizardStyle=modern
Compression=lzma2/ultra
SolidCompression=yes
; x64 only — the payload is an x64 build.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Default machine-wide (Program Files); /CURRENTUSER on the command line gives
; an unelevated per-user install ({autopf} then resolves under %LOCALAPPDATA%).
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline
VersionInfoVersion={#AppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; Core payload — the two runtime binaries the build produces.
Source: "{#BuildDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\SoapySDR.dll"; DestDir: "{app}"; Flags: ignoreversion
; App-local MSVC runtime (from the VS Redist folder — NEVER from C:\Windows).
; cascade.exe and SoapySDR.dll import exactly these three; the UCRT they also
; need is an OS component on Windows 10+.
Source: "{#VcCrtDir}\msvcp140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#VcCrtDir}\vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#VcCrtDir}\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion
; License + post-install notes (also shown as wizard pages).
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
; Third-party notices ship with the binary because MIT, BSD-3 and Zlib all
; require their copyright and permission notices to accompany distribution --
; a hard condition of the licences, not a courtesy, and one that applies to a
; commercial release exactly as it does to a free one.
Source: "THIRD-PARTY-LICENSES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "POSTINSTALL.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Comment: "Start {#AppName}"
Name: "{group}\Hardware setup notes"; Filename: "{app}\POSTINSTALL.txt"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Comment: "Start {#AppName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName} now"; WorkingDir: "{app}"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
; The app never writes into {app} at runtime (config lives under the user
; profile), so nothing to purge beyond what the installer placed.
