; Installer of one single-module plugin (Inno Setup 6). scripts\package-all.ps1 compiles this file once per row of
; installer\products.csv, with:
;   /DAppName=<PureSheen EQ> /DAppVersion=<x.y.z> /DAppGuid=<guid> /DPayload=<payload\eq> /DOutputName=<file name>
;   /DDescription=<what it is, for the welcome page>
; Payload: <Payload>\VST3\<AppName>.vst3 and <Payload>\CLAP\<AppName>.clap, from the static-runtime build, so no VC++
; redistributable is needed.
; Testing: /DTestRoot=<folder> builds a variant that installs below <folder> for the current user, with no admin rights,
;          no registry entries and no uninstaller, so it never touches a real installation.

#define AppPublisher "Kujar3"

#ifdef TestRoot
  #define PluginRoot TestRoot + "\CommonFiles"
  #define AppRoot TestRoot + "\App\" + AppName
#else
  #define PluginRoot "{commoncf64}"
  #define AppRoot "{autopf}\PureSheen\" + AppName
#endif

[Setup]
#ifdef TestRoot
AppId=PureSheenInstallerTest {#AppName}
UsePreviousAppDir=no
Uninstallable=no
CreateUninstallRegKey=no
PrivilegesRequired=lowest
OutputBaseFilename={#OutputName}-TEST
#else
AppId={{{#AppGuid}}
PrivilegesRequired=admin
OutputBaseFilename={#OutputName}
UninstallDisplayIcon={app}\puresheen.ico
UninstallDisplayName={#AppName} {#AppVersion}
#endif
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={#AppRoot}
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
SetupIconFile=puresheen.ico
InfoAfterFile={#Payload}\QuickStart.txt
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes
; The default filter (*.exe,*.dll,*.chm) would not notice a DAW that has the plugin binaries loaded.
CloseApplicationsFilter=*.exe,*.dll,*.chm,*.vst3,*.clap

[Messages]
WelcomeLabel2=This will install [name/ver] - {#Description}.%n%nPlease close FL Studio before continuing.

[Types]
Name: "full"; Description: "Recommended (VST3 + CLAP)"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin (use this one in FL Studio)"; Types: full custom; Flags: fixed
Name: "clap"; Description: "CLAP plugin"; Types: full

[Files]
Source: "{#Payload}\VST3\{#AppName}.vst3\*"; DestDir: "{#PluginRoot}\VST3\{#AppName}.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Payload}\CLAP\{#AppName}.clap"; DestDir: "{#PluginRoot}\CLAP"; Components: clap; Flags: ignoreversion
Source: "{#Payload}\QuickStart.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "puresheen.ico"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; A clean bundle every time (an older build may have had other files inside it).
Type: filesandordirs; Name: "{#PluginRoot}\VST3\{#AppName}.vst3"

[UninstallDelete]
Type: dirifempty; Name: "{#PluginRoot}\VST3\{#AppName}.vst3"
