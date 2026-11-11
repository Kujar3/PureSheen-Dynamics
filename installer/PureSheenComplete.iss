; All-in-one installer (Inno Setup 6): the suite and every single-module plugin, with a checklist of what to install.
; scripts\package-all.ps1 compiles this file after writing the list of plugins, from installer\products.csv, to
; installer\payload\complete-products.iss:
;   /DAppVersion=<x.y.z> /DProductsFile=<that file>
; The binaries come straight from the static-runtime build, so no VC++ redistributable is needed.
; Testing: /DTestRoot=<folder> builds a per-user variant that installs below <folder>. Its registry entries are per-user
;          too, so it can never find, remove or take over a real (per-machine) installation.
;
; The separate installers keep their own AppIds, so a plugin could end up owned by two uninstall entries. This installer
; removes the separate install of each plugin it is about to put in; see TakeOver below.

#define AppName "PureSheen Complete"
#ifndef AppVersion
  #define AppVersion "1.2.0"
#endif
#define AppPublisher "Kujar3"

#ifdef TestRoot
  #define AppGuid "D865A95C-4430-41A0-977D-4DEAF021F781"
  #define PluginRoot TestRoot + "\CommonFiles"
  #define AppRoot TestRoot + "\App"
#else
  #define AppGuid "AA4CEAFF-8498-415D-AA45-618872D35F60"
  #define PluginRoot "{commoncf64}"
  ; The suite's folder: the plugin looks for the Claude connector there.
  #define AppRoot "{autopf}\PureSheen Dynamics"
#endif

[Setup]
AppId={{{#AppGuid}}
#ifdef TestRoot
UsePreviousAppDir=no
PrivilegesRequired=lowest
OutputBaseFilename=PureSheen-Complete-Setup-{#AppVersion}-TEST
#else
PrivilegesRequired=admin
OutputBaseFilename=PureSheen-Complete-Setup-{#AppVersion}
#endif
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={#AppRoot}
DisableDirPage=yes
DefaultGroupName=PureSheen Dynamics
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
SetupIconFile=puresheen.ico
UninstallDisplayIcon={app}\puresheen.ico
UninstallDisplayName={#AppName} {#AppVersion}
InfoAfterFile=QuickStartComplete.txt
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes
; The default filter (*.exe,*.dll,*.chm) would not notice a DAW that has the plugin binaries loaded.
CloseApplicationsFilter=*.exe,*.dll,*.chm,*.vst3,*.clap

[Messages]
WelcomeLabel2=This will install [name/ver] - the PureSheen Dynamics mastering suite and its modules as plugins of their own. You choose which ones on the next page.%n%nPlease close FL Studio before continuing.
SelectComponentsLabel2=Untick anything you don't want. VST3 versions are always installed. You can run this installer again later to add or remove plugins.

[Types]
Name: "full"; Description: "Recommended (all plugins, VST3 + CLAP)"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "clap"; Description: "CLAP versions of the plugins ticked below"; Types: full
Name: "suite"; Description: "PureSheen Dynamics - a complete mastering suite"; Types: full; Flags: checkablealone
Name: "suite\standalone"; Description: "Standalone app (master an audio input without a DAW)"; Flags: dontinheritcheck
Name: "suite\claude"; Description: "Claude connector (lets Claude Desktop control the plugin, off until you turn it on in the plugin)"; Types: full; Flags: dontinheritcheck
Name: "modules"; Description: "Modules as plugins of their own"; Types: full

[Files]
Source: "QuickStartComplete.txt"; DestDir: "{app}"; DestName: "QuickStart.txt"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "puresheen.ico"; DestDir: "{app}"; Flags: ignoreversion

#ifndef TestRoot
[Icons]
Name: "{group}\PureSheen Dynamics"; Filename: "{app}\PureSheen Dynamics.exe"; Components: suite\standalone
Name: "{group}\PureSheen Quick Start"; Filename: "{app}\QuickStart.txt"
Name: "{group}\Uninstall PureSheen Complete"; Filename: "{uninstallexe}"
#endif

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\';

var
  // What the previous run of this installer left in place, from before this run changes it.
  PreviousSelection: String;
  SuiteHadStandalone: Boolean;

function HasItem(const List, Item: String): Boolean;
begin
  Result := Pos(',' + Item + ',', ',' + List + ',') > 0;
end;

function SelectedComponentsOf(const Guid: String): String;
begin
  if not RegQueryStringValue(HKA64, UninstallKey + '{' + Guid + '}_is1', 'Inno Setup: Selected Components', Result) then
    Result := '';
end;

function InitializeSetup(): Boolean;
begin
  PreviousSelection := SelectedComponentsOf('{#AppGuid}');
  SuiteHadStandalone := HasItem(SelectedComponentsOf('5F91530F-2554-42B2-968A-E638C0518C12'), 'standalone');
  Result := True;
end;

// Taking over the suite must not lose a Standalone app the separate installer put in.
procedure InitializeWizard();
begin
  if SuiteHadStandalone then
    WizardSelectComponents('suite\standalone');
end;

// [InstallDelete]: a clean bundle for a plugin going in, and removal of one that a previous run of this installer put in
// and that is now unticked. An unticked plugin this installer never owned (a separate install) stays where it is.
function CleanVst3(const Component: String): Boolean;
begin
  Result := WizardIsComponentSelected(Component) or HasItem(PreviousSelection, Component);
end;

function DropClap(const Component: String): Boolean;
begin
  Result := HasItem(PreviousSelection, Component) and HasItem(PreviousSelection, 'clap')
            and not (WizardIsComponentSelected(Component) and WizardIsComponentSelected('clap'));
end;

function DropApp(const Component: String): Boolean;
begin
  Result := HasItem(PreviousSelection, Component) and not WizardIsComponentSelected(Component);
end;

// Removes the separate install of a plugin this installer is about to put in, with its uninstall entry. Presets stay:
// they live in the user's own folder, which no uninstaller touches. Failure is not fatal: the new files go over the old.
procedure TakeOver(const Component, Guid: String);
var
  Uninstaller: String;
  ResultCode, Waited: Integer;
begin
  if not WizardIsComponentSelected(Component) then
    Exit;
  if not RegQueryStringValue(HKA64, UninstallKey + '{' + Guid + '}_is1', 'UninstallString', Uninstaller) then
    Exit;
  Uninstaller := RemoveQuotes(Uninstaller);
  Log('Removing the separate install: ' + Uninstaller);
  if not Exec(Uninstaller, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    Log('Could not start it: ' + SysErrorMessage(ResultCode));
    Exit;
  end;
  Log('Exit code ' + IntToStr(ResultCode));
  // It deletes itself last. Installing before it finishes would let it delete the new files.
  Waited := 0;
  while FileExists(Uninstaller) and (Waited < 30000) do
  begin
    Sleep(100);
    Waited := Waited + 100;
  end;
  if FileExists(Uninstaller) then
    Log('It is still there after 30 s');
end;

#include ProductsFile

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    WizardForm.StatusLabel.Caption := 'Removing older separate installs...';
    TakeOverAll();
  end;
end;
