; Inno Setup script. scripts\package.bat runs it with Version and Source, and a build folder holding the files.
; Installs for the current user without asking for administrator rights: nothing here needs them, and the
; catalogue and settings in %LOCALAPPDATA%\Arpegio stay when the program is removed.

#ifndef Version
  #error Run this through scripts\package.bat
#endif

; Must match kAppUserModelId in src\app\main.cpp.
#define AppUserModelId "OrbitDR.Arpegio"
#define WebView2Key "Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"

[Setup]
AppId={{6F6B3E2A-7C51-4E58-9B0D-2D6A4F3C1E77}
AppName=Arpegio
AppVersion={#Version}
AppPublisher=OrbitDR
AppPublisherURL=https://github.com/OrbitDR/Arpegio
AppSupportURL=https://github.com/OrbitDR/Arpegio/issues
DefaultDirName={autopf}\Arpegio
DefaultGroupName=Arpegio
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#Source}\..\installer
OutputBaseFilename=Arpegio-{#Version}-x64
SetupIconFile=..\src\app\arpegio.ico
UninstallDisplayIcon={app}\arpegio.exe
UninstallDisplayName=Arpegio
LicenseFile=..\LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ShowLanguageDialog=no

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#Source}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{autoprograms}\Arpegio"; Filename: "{app}\arpegio.exe"; AppUserModelID: "{#AppUserModelId}"
Name: "{autodesktop}\Arpegio"; Filename: "{app}\arpegio.exe"; AppUserModelID: "{#AppUserModelId}"; Tasks: desktopicon

[Run]
Filename: "https://developer.microsoft.com/microsoft-edge/webview2/"; Description: "Descargar WebView2 Runtime, que Arpegio necesita y no está en este equipo"; Flags: shellexec postinstall skipifsilent; Check: not HasWebView2
Filename: "{app}\arpegio.exe"; Description: "{cm:LaunchProgram,Arpegio}"; Flags: nowait postinstall skipifsilent; Check: HasWebView2

[Code]
function RuntimeVersionAt(Root: Integer; Key: String): Boolean;
var
  Version: String;
begin
  Result := RegQueryStringValue(Root, Key, 'pv', Version) and (Version <> '') and (Version <> '0.0.0.0');
end;

// Where Microsoft says the Evergreen Runtime leaves its version. Windows 11 always has it.
function HasWebView2: Boolean;
begin
  Result := RuntimeVersionAt(HKLM, 'SOFTWARE\WOW6432Node\{#WebView2Key}')
    or RuntimeVersionAt(HKCU, 'Software\{#WebView2Key}');
end;
