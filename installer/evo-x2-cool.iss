; Installer for EVO-X2-COOL (Inno Setup 6). Build: installer\bauen.cmd (fetches the PawnIO files first).
; Installs both programs to Program Files, the PawnIO driver if it is missing, the controller as a service and
; optionally the window in the tray at logon. Uninstall puts the BIOS fan curve back and may keep the learned data.

#define AppVer "0.6.6"

[Setup]
AppId={{7464B3D3-23BC-4840-B095-EC3434E506D4}
AppName=EVO-X2-COOL
AppVersion={#AppVer}
AppVerName=EVO-X2-COOL {#AppVer}
AppPublisher=Fabian Storck (inoffiziell / unofficial)
AppPublisherURL=https://github.com/D3rPapts/evo-x2-cool
AppSupportURL=https://github.com/D3rPapts/evo-x2-cool/issues
DefaultDirName={autopf}\EVO-X2-COOL
DefaultGroupName=EVO-X2-COOL
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=Output
OutputBaseFilename=evo-x2-cool-setup-{#AppVer}
SetupIconFile=..\evo.ico
UninstallDisplayIcon={app}\evo-x2-cool-front.exe
UninstallDisplayName=EVO-X2-COOL
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
ShowLanguageDialog=auto

[Languages]
Name: "de"; MessagesFile: "compiler:Languages\German.isl"; InfoBeforeFile: "hinweis-de.txt"
Name: "en"; MessagesFile: "compiler:Default.isl"; InfoBeforeFile: "hinweis-en.txt"

[CustomMessages]
de.Dienst=Regler als Windows-Dienst einrichten und starten (empfohlen)
en.Dienst=Set up and start the controller as a Windows service (recommended)
de.Autostart=Fenster beim Anmelden versteckt im Infobereich starten
en.Autostart=Start the window hidden in the tray at logon
de.StatusPawnIO=PawnIO-Treiber wird installiert ...
en.StatusPawnIO=Installing the PawnIO driver ...
de.StatusDienst=Regler-Dienst wird eingerichtet ...
en.StatusDienst=Setting up the controller service ...
de.Oeffnen=EVO-X2-COOL jetzt öffnen
en.Oeffnen=Open EVO-X2-COOL now
de.DatenLoeschen=Gelerntes Modell, Einstellungen und Protokolle in %1 auch löschen?
en.DatenLoeschen=Also delete the learned model, settings and logs in %1?

[Tasks]
Name: "dienst"; Description: "{cm:Dienst}"
Name: "autostart"; Description: "{cm:Autostart}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[Files]
Source: "..\evo-x2-cool.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\evo-x2-cool-front.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\sprache-de.ini"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\sprache-en.ini"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"
Source: "..\README.md"; DestDir: "{app}"
Source: "DRITTANBIETER.txt"; DestDir: "{app}"
Source: "drittanbieter\modules\AMDFamily17.bin"; DestDir: "{app}\pawnio"; Flags: ignoreversion
Source: "drittanbieter\modules\LpcACPIEC.bin"; DestDir: "{app}\pawnio"; Flags: ignoreversion
Source: "drittanbieter\modules\COPYING"; DestDir: "{app}\pawnio"; DestName: "COPYING-LGPL-2.1.txt"
Source: "drittanbieter\PawnIO_setup.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: not PawnIOInstalled

[Icons]
Name: "{autoprograms}\EVO-X2-COOL"; Filename: "{app}\evo-x2-cool-front.exe"
Name: "{autodesktop}\EVO-X2-COOL"; Filename: "{app}\evo-x2-cool-front.exe"; Tasks: desktopicon
Name: "{commonstartup}\EVO-X2-COOL"; Filename: "{app}\evo-x2-cool-front.exe"; Parameters: "--leiste"; Tasks: autostart

[Run]
Filename: "{tmp}\PawnIO_setup.exe"; Parameters: "-install"; StatusMsg: "{cm:StatusPawnIO}"; Check: not PawnIOInstalled
Filename: "{app}\evo-x2-cool.exe"; Parameters: "--installieren"; StatusMsg: "{cm:StatusDienst}"; Flags: runhidden waituntilterminated; Tasks: dienst
Filename: "{app}\evo-x2-cool-front.exe"; Description: "{cm:Oeffnen}"; Flags: postinstall nowait skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/im evo-x2-cool-front.exe /f"; Flags: runhidden; RunOnceId: "front"
Filename: "{app}\evo-x2-cool.exe"; Parameters: "--entfernen"; Flags: runhidden waituntilterminated; RunOnceId: "dienst"
Filename: "{app}\evo-x2-cool.exe"; Parameters: "--stopp"; Flags: runhidden waituntilterminated; RunOnceId: "stopp"

[Code]
function PawnIOInstalled: Boolean;
begin
  Result := RegKeyExists(HKLM64, 'SYSTEM\CurrentControlSet\Services\PawnIO');
end;

// update or reinstall: stop what is running (service, console run, window) before files are replaced;
// every stop puts the BIOS fan curve back
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  rc: Integer;
  exe: String;
  wnd: HWND;
begin
  ExtractTemporaryFile('evo-x2-cool.exe');
  exe := ExpandConstant('{tmp}\evo-x2-cool.exe');
  // ask the window to end itself first (WM_APP + 4): a killed one leaves a dead icon next to the clock
  wnd := FindWindowByClassName('evo-x2-cool-front');
  if wnd <> 0 then begin PostMessage(wnd, $8004, 0, 0); Sleep(1500); end;
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/im evo-x2-cool-front.exe /f', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Exec(exe, '--entfernen', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Exec(exe, '--stopp', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  data: String;
begin
  if CurUninstallStep = usPostUninstall then begin
    data := ExpandConstant('{commonappdata}\evo-x2-cool');
    if DirExists(data) and not UninstallSilent and
       (MsgBox(FmtMessage(CustomMessage('DatenLoeschen'), [data]), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES) then
      DelTree(data, True, True, True);
  end;
end;
