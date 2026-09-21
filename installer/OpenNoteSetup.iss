; OpenNote installer (Inno Setup)
; https://jrsoftware.org/isinfo.php
;
; Version comes from the build so it cannot drift from the tag that shipped:
;   ISCC.exe /DMyAppVersion=0.2.0 installer\OpenNoteSetup.iss

; Lower case: the product is "opennote". AppId is untouched, so an existing
; installation still upgrades in place.
#define MyAppName "opennote"
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#define MyAppPublisher "sp00nz"
#define MyAppURL "https://github.com/sp00nznet/opennote"
#define MyAppExeName "OpenNote.exe"

[Setup]
; NOTE: The value of AppId uniquely identifies this application.
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
; Output settings
OutputDir=..\build\installer
OutputBaseFilename=OpenNoteSetup
SetupIconFile=..\res\icon.ico
Compression=lzma
SolidCompression=yes
WizardStyle=modern
; Per-user by default: an editor does not need administrator rights to install.
; Choosing to elevate at the prompt installs for all users instead, and the {auto*}
; constants below follow that choice.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
; Uninstall settings
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "associate"; Description: "Offer opennote as a handler for .txt, .log, .md and .ini"; GroupDescription: "File associations:"

[Files]
Source: "..\build\bin\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Registry]
; HKA writes to HKCU for a per-user install and HKLM for an all-users install, matching
; whatever the user chose at the elevation prompt.

; "Open with OpenNote" on the context menu for any file.
Root: HKA; Subkey: "Software\Classes\*\shell\OpenWithOpenNote"; ValueType: string; ValueName: ""; ValueData: "Open with opennote"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\*\shell\OpenWithOpenNote"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#MyAppExeName},0"
Root: HKA; Subkey: "Software\Classes\*\shell\OpenWithOpenNote\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

; A ProgID for OpenNote, then OpenWithProgids entries for the extensions it handles.
; OpenWithProgids *adds* OpenNote to the "Open with" list. It deliberately does not write
; the extension's default value, so installing this does not steal an association from
; whatever the user already uses.
Root: HKA; Subkey: "Software\Classes\OpenNote.Document"; ValueType: string; ValueName: ""; ValueData: "Text Document"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\OpenNote.Document\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\OpenNote.Document\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: associate

Root: HKA; Subkey: "Software\Classes\.txt\OpenWithProgids"; ValueType: string; ValueName: "OpenNote.Document"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.log\OpenWithProgids"; ValueType: string; ValueName: "OpenNote.Document"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.md\OpenWithProgids";  ValueType: string; ValueName: "OpenNote.Document"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.ini\OpenWithProgids"; ValueType: string; ValueName: "OpenNote.Document"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associate

; Registered application, so OpenNote appears in Settings > Default apps.
Root: HKA; Subkey: "Software\Classes\Applications\{#MyAppExeName}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".txt"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".log"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".md"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".ini"; ValueData: ""

; Notes live in %APPDATA%\OpenNote\opennote.db and are deliberately left behind by the
; uninstaller. Removing someone's notes because they uninstalled the reader would be a
; hostile thing to do; the directory is one they can delete themselves.
