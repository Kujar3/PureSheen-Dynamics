; PureSheen Dynamics installer (Inno Setup 6)
; Build everything with:  scripts\build-installer.bat   -> dist\PureSheen-Dynamics-Setup-<version>.exe
; (compiles a static-runtime Release build in build-release, copies it to installer\payload, then runs ISCC on this file
;  with /DAppVersion=<the project() version in CMakeLists.txt>)
; Payload: installer\payload\{VST3\PureSheen Dynamics.vst3, CLAP\PureSheen Dynamics.clap,
;          Standalone\PureSheen Dynamics.exe}, built with the static MSVC runtime (-DCMAKE_MSVC_RUNTIME_LIBRARY=
;          MultiThreaded), so no VC++ redistributable is needed.
; Testing: ISCC /DTestRoot=<folder> installer\PureSheen.iss builds a per-user variant that installs everything below
;          <folder> instead of Program Files (no admin rights, never touches a real installation).

#define AppName "PureSheen Dynamics"
#ifndef AppVersion
  #define AppVersion "0.3.4"
#endif
#define AppPublisher "Kujar3"

#ifdef TestRoot
  #define PluginRoot TestRoot + "\CommonFiles"
  #define AppRoot TestRoot + "\App"
#else
  #define PluginRoot "{commoncf64}"
  #define AppRoot "{autopf}\" + AppName
#endif

[Setup]
#ifdef TestRoot
; Own AppId, never the previous install's folder: a test install can never take over or move the real one.
AppId={{207BE6F2-A7C0-4787-BE16-EDF2C29DDAC8}
UsePreviousAppDir=no
#else
AppId={{5F91530F-2554-42B2-968A-E638C0518C12}
#endif
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={#AppRoot}
DisableDirPage=yes
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
#ifdef TestRoot
PrivilegesRequired=lowest
OutputBaseFilename=PureSheen-Dynamics-Setup-{#AppVersion}-TEST
#else
PrivilegesRequired=admin
OutputBaseFilename=PureSheen-Dynamics-Setup-{#AppVersion}
#endif
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
SetupIconFile=puresheen.ico
UninstallDisplayIcon={app}\puresheen.ico
UninstallDisplayName={#AppName} {#AppVersion}
InfoAfterFile=QuickStart.txt
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes
; The default filter (*.exe,*.dll,*.chm) would not notice a DAW that has the plugin binaries loaded.
CloseApplicationsFilter=*.exe,*.dll,*.chm,*.vst3,*.clap

[Messages]
WelcomeLabel2=This will install [name/ver] - a complete mastering suite for FL Studio and other DAWs: EQ, multiband dynamics, stereo imaging, harmonic exciter, maximizer and dither in one plugin.%n%nPlease close FL Studio before continuing.

[Types]
Name: "full"; Description: "Recommended (VST3 + CLAP)"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin (use this one in FL Studio)"; Types: full custom; Flags: fixed
Name: "clap"; Description: "CLAP plugin"; Types: full
Name: "standalone"; Description: "Standalone app (master an audio input without a DAW)"

[Files]
Source: "payload\VST3\PureSheen Dynamics.vst3\*"; DestDir: "{#PluginRoot}\VST3\PureSheen Dynamics.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "payload\CLAP\PureSheen Dynamics.clap"; DestDir: "{#PluginRoot}\CLAP"; Components: clap; Flags: ignoreversion
Source: "payload\Standalone\PureSheen Dynamics.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "QuickStart.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "puresheen.ico"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; A clean bundle every time (an older build may have had other files inside it).
Type: filesandordirs; Name: "{#PluginRoot}\VST3\PureSheen Dynamics.vst3"

[UninstallDelete]
Type: dirifempty; Name: "{#PluginRoot}\VST3\PureSheen Dynamics.vst3"

[Icons]
Name: "{group}\PureSheen Dynamics"; Filename: "{app}\PureSheen Dynamics.exe"; Components: standalone
Name: "{group}\PureSheen Dynamics Quick Start"; Filename: "{app}\QuickStart.txt"
Name: "{group}\Uninstall PureSheen Dynamics"; Filename: "{uninstallexe}"
