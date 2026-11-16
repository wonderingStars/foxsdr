; install-report.iss - THE INSTALLER REPORTS A FAILED, CANCELLED OR BLOCKED INSTALL (owner decision 15, 2026-11-16; engine lane E1).
;
; OFF. Nothing here is compiled into the setup program unless cascade.iss has `#define ReportInstallOutcome` (it is a comment there) - and it stays a
; comment until the Worker accepts the new kind (docs/multi-radio-records/engine-e1.md, CONTRACT ISSUES). With the define off the setup program is the
; same bytes it was.
;
; WHAT IS SENT: one POST of the radio-reports contract's envelope (schema 2), kind "install_outcome", and nothing else:
;
;   {"schema":2,"kind":"install_outcome","messageId":"<16 random hex>","t":0,
;    "body":{"outcome":"failed"|"cancelled"|"blocked","appVersion":"0.99.109","windows":"10.0.22631"}}
;
; The outcome, the FoxSDR version being installed and the Windows version (major.minor.build). No install id, no session id, no user name, no machine name, no
; path, no address, no error text, no folder. The request carries no cookie. The Worker sees the sender's network address as it sees every request's: the
; Worker does not keep it (the contract's section 10).
;
; HOW, WITHOUT A HELPER PROGRAM: Inno Setup's [Code] can create the COM object that ships with Windows, WinHttp.WinHttpRequest.5.1, and make one HTTPS request
; with it. No file is dropped, nothing is downloaded, nothing is started.
;
; WHEN IT IS SENT: at the end of Setup (DeinitializeSetup), only when the install did not finish, only for an interactive setup (not /SILENT, /VERYSILENT, a
; /DATAROOTS= test run or /NOREPORT), once, with a total wait of at most five seconds so a closed network never keeps the person looking at a window.
;   cancelled  the person pressed Cancel (or closed the window) before the install finished.
;   blocked    Setup stopped before it began copying files: it refused (PrepareToInstall) or the other-scope uninstall left the install unable to start.
;   failed     Setup began copying files and did not reach the end (a file in use, no space, a copy that failed).
; WHAT CANNOT BE REPORTED AT ALL: an installer that Windows blocks BEFORE it runs (SmartScreen, an antivirus that deletes or quarantines the file, a
; policy that forbids the program, a download that never finished, a person who never opens it) runs none of this code, so nothing is sent and nothing
; can be. The count of installs that reach the first run, against the count of downloads, is the only trace of those.
;
; UNVERIFIED: this file has not been compiled or run (no Inno Setup compiler is on the build machine and none was downloaded). The behaviour of
; CancelButtonClick when the person answers "No" to "Exit Setup?" is the documented one (the flag is only a candidate; it counts only when Setup then ends
; without finishing).

// (This file is included INSIDE cascade.iss's own [Code] section.)
var
  ReportReachedInstall: Boolean;
  ReportFinished: Boolean;
  ReportCancelClicked: Boolean;
  ReportRan: Boolean;

function ReportRandomHex(Len: Integer): String;
var
  I: Integer;
  Digits: String;
begin
  Digits := '0123456789abcdef';
  Result := '';
  for I := 1 to Len do
    Result := Result + Digits[Random(16) + 1];
end;

function ReportAllowed(): Boolean;
begin
  // Interactive installs only: a scripted, silent or test install never reports.
  Result := (not WizardSilent) and (ExpandConstant('{param:noreport|0}') <> '1') and (ExpandConstant('{param:dataroots|}') = '');
end;

function ReportOutcomeWord(): String;
begin
  if ReportFinished then
    Result := ''
  else if ReportCancelClicked then
    Result := 'cancelled'
  else if ReportReachedInstall then
    Result := 'failed'
  else
    Result := 'blocked';
end;

function ReportBody(const Outcome: String): String;
var
  V: TWindowsVersion;
begin
  GetWindowsVersionEx(V);
  Result := '{"schema":2,"kind":"install_outcome","messageId":"' + ReportRandomHex(16) + '","t":0,"body":{"outcome":"' + Outcome +
            '","appVersion":"{#AppVersion}","windows":"' + IntToStr(V.Major) + '.' + IntToStr(V.Minor) + '.' + IntToStr(V.Build) + '"}}';
end;

procedure ReportSend(const Outcome: String);
var
  Req: Variant;
begin
  try
    Req := CreateOleObject('WinHttp.WinHttpRequest.5.1');
    // resolve, connect, send, receive: at most about five seconds in all
    Req.SetTimeouts(1000, 1500, 1500, 1500);
    Req.Open('POST', '{#ReportInstallUrl}', False);
    Req.SetRequestHeader('Content-Type', 'application/json');
    Req.Send(ReportBody(Outcome));
    Log('Install report sent: ' + Outcome + ' (status ' + IntToStr(Req.Status) + ').');
  except
    // A failure to report is not a failure to install: it is logged and nothing more.
    Log('Install report not sent: ' + GetExceptionMessage);
  end;
end;

procedure ReportNoteStep(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    ReportReachedInstall := True;
  if CurStep = ssDone then
    ReportFinished := True;
end;

procedure ReportNoteCancel();
begin
  ReportCancelClicked := True;
end;

procedure ReportAtEnd();
var
  Word: String;
begin
  if ReportRan then
    exit;
  ReportRan := True;
  if not ReportAllowed() then
    exit;
  Word := ReportOutcomeWord();
  if Word = '' then
    exit;
  ReportSend(Word);
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  // Only a candidate: the person may answer No to "Exit Setup?". It counts only if Setup then ends without finishing.
  ReportNoteCancel();
end;

procedure DeinitializeSetup();
begin
  ReportAtEnd();
end;
