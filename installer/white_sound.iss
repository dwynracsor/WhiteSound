; Inno Setup script for White Sound (Windows system tray keep-alive).
;
; Per-user install (no admin / no UAC): copies the executable into
; %LOCALAPPDATA%\Programs\White Sound, adds a Start Menu shortcut, an optional
; desktop shortcut, an optional "start with Windows" entry, and an uninstaller.
; The user's configuration (%APPDATA%\white-sound) is left untouched.
;
; Build with the helper, which builds the app first:
;     installer\build_installer.ps1
; or directly (the app must already be built):
;     ISCC.exe installer\white_sound.iss
; Output:  dist\WhiteSoundSetup-<version>.exe
;
; The version comes from CMake (build\generated\version.iss, generated from the
; project(... VERSION ...) line), so the installer, the executable metadata and
; the About dialog all agree.

#include "..\build\generated\version.iss"

#define MyAppName "White Sound"
#define MyAppExeName "white_sound_tray.exe"
#define MyAppPublisher "White Sound Project"

[Setup]
; A stable AppId keeps upgrades and the uninstall entry tied to one product.
; Do not change it between releases.
AppId={{3870C687-7128-4828-92A6-72B455777F54}}
AppName={#MyAppName}
AppVersion={#WS_VERSION}
AppVerName={#MyAppName} {#WS_VERSION}
AppPublisher={#MyAppPublisher}
VersionInfoVersion={#WS_VERSION}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} Setup
VersionInfoProductName={#MyAppName}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=WhiteSoundSetup-{#WS_VERSION}
SetupIconFile=..\assets\white_sound.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Per-user install, no elevation.
PrivilegesRequired=lowest
; 64-bit only (the executable is x86_64).
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Ask to close a running instance before replacing files. The mutex name comes
; from tray_app.cpp (WinMain).
AppMutex=WhiteSoundTray_SingleInstance
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[CustomMessages]
english.StartWithWindows=Start %1 when I sign in
spanish.StartWithWindows=Iniciar %1 al iniciar sesión

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "{cm:StartWithWindows,{#MyAppName}}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md";            DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";              DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Optional "start with Windows" entry, removed again on uninstall.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "{#MyAppName}"; \
    ValueData: """{app}\{#MyAppExeName}"""; \
    Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#MyAppExeName}"; \
    Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; \
    Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
