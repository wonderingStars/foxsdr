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
; The product's identity across every version and both install scopes. It is
; the AppId below AND the registry key the [Code] section looks for in the
; other hive, so it is written once here rather than twice.
#define AppGuid "B3D4A7E2-6C51-4F8B-9A0D-2E7F5C8B1A64"
; Overridable so a nightly can be stamped with its own version:
;   ISCC.exe /DAppVersion="0.56.0-nightly.20260819.58fe5a3" installer\cascade.iss
; tools/build-nightly.ps1 does exactly that, and passes the SAME string to
; CMake as CASCADE_VERSION_STRING so the binary agrees with its installer.
#ifndef AppVersion
  ; NOT a literal. The version lives in CMakeLists.txt and CMake writes it into
  ; generated-version.iss at configure time, so the installer cannot be stamped
  ; with a different number from the binary it packages.
  ;
  ; It was a literal until it drifted: the project moved to 0.57.0 and this file
  ; still said 0.56.0, so a build of the new code produced an installer named
  ; after the old release - the same class of mistake as the checksum the
  ; website published for a file it was not serving.
  #include "generated-version.iss"
#endif
#define AppPublisher "Steven Fairclough"
#define AppExe "cascade.exe"
; Install-dir leaf avoids '+' (some tools mishandle it in paths); display name keeps it.
#define InstallLeaf "FoxSDR"
; Overridable so a nightly can be packaged from its own build tree. That tree
; is kept separate from the release one deliberately: the reported version is a
; compile definition, and a shared build directory would let a stale object
; leave a release binary calling itself a nightly.
#ifndef BuildDir
  #define BuildDir AddBackslash(SourcePath) + "..\build\Release"
#endif
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
AppId={{{#AppGuid}}
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
; Windows' VERSIONINFO resource takes a NUMERIC x.y.z[.w] and nothing else, so
; a pre-release version like "0.56.0-nightly.20260819.abc1234" is rejected
; outright. The display version above keeps the full string - that is what the
; user, the About line and any bug report see - while this one carries just the
; numeric part for the file properties dialog. tools/build-nightly.ps1 passes
; it; a release build leaves it equal to AppVersion.
#ifndef AppVersionNumeric
  #define AppVersionNumeric AppVersion
#endif
VersionInfoVersion={#AppVersionNumeric}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; Core payload — the runtime binaries the build produces.
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

[InstallDelete]
; The WebView radar unit shipped as a second program, foxsdr-radar.exe, from
; 0.72.0 to 0.76.0 and was removed in 0.77.0 in favour of the native scope.
; Inno only ever ADDS on an upgrade: a file and a Start Menu key that a newer
; script no longer lists stay exactly where the older installer put them, so
; every upgraded install kept a dead "Radar unit" beside the real one (seen on
; the 0.77.0 upgrade of this machine: the .exe in {app} and the .lnk in the
; group, both dated from the old install). Removed here, at install time,
; before the new files go down.
Type: files; Name: "{app}\foxsdr-radar.exe"
Type: files; Name: "{group}\Radar unit.lnk"

[UninstallDelete]
; The app never writes into {app} at runtime (config lives under the user
; profile), so nothing to purge beyond what the installer placed.

[Code]
// ONE FoxSDR PER MACHINE, WHICHEVER SCOPE IT WAS INSTALLED IN.
//
// Inno keys an upgrade on AppId within ONE registry hive: a machine-wide
// install (HKLM, Program Files) and a per-user one (HKCU, %LOCALAPPDATA%)
// never see each other, so a per-user 0.64.0 survived every machine-wide
// upgrade after it - with its own Start Menu folder, also called "FoxSDR",
// merged into the same menu as the current one. The two entries look
// identical, and which version ran depended on which was clicked; the
// application's own log showed 0.64.0 starting between runs of 0.75.0 on the
// same afternoon, and every bug fixed in between came back with it (found
// 2026-09-04). So before installing, the OTHER hive is checked for the same
// AppId, and the install found there is uninstalled silently first. Its own
// uninstaller does the removal, so nothing here has to know what it placed.
//
// The user's data is untouched: config, plugins, logs and caches live under
// the profile, not under either install directory.

function OtherScopeUninstaller(): String;
var
  Root: Integer;
  Key: String;
  Value: String;
begin
  Result := '';
  // The preprocessor pastes the GUID; the braces round it are literal here,
  // because [Code] strings do not expand constants.
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#AppGuid}}_is1';
  // The hive THIS setup is not writing to.
  if IsAdminInstallMode then
    Root := HKEY_CURRENT_USER
  else
    Root := HKEY_LOCAL_MACHINE;
  if RegQueryStringValue(Root, Key, 'UninstallString', Value) then
    Result := RemoveQuotes(Value)
  else if RegQueryStringValue(Root, 'WOW6432Node\' + Key, 'UninstallString', Value) then
    Result := RemoveQuotes(Value);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Uninstaller: String;
  ResultCode: Integer;
begin
  Result := '';
  Uninstaller := OtherScopeUninstaller();
  if Uninstaller = '' then
    exit;
  Log('Removing the FoxSDR installed in the other scope: ' + Uninstaller);
  // /SILENT keeps its own progress window; /NORESTART because nothing here
  // needs one; /SUPPRESSMSGBOXES so a "still running" prompt cannot stall an
  // unattended install. A failure is logged and does not abort this install:
  // a stale copy is a nuisance, and refusing to install over it would be
  // worse than leaving it.
  if not Exec(Uninstaller, '/SILENT /NORESTART /SUPPRESSMSGBOXES', '', SW_SHOW,
              ewWaitUntilTerminated, ResultCode) then
    Log('The other-scope uninstaller could not be started (error ' +
        IntToStr(ResultCode) + '); continuing.')
  else
    Log('The other-scope uninstaller exited with code ' + IntToStr(ResultCode) + '.');
end;
