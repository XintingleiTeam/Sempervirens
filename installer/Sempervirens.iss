; Public Sempervirens installer. The native app is isolated from the legacy .NET AppId.
#define AppName "Sempervirens"
#define AppVersion "0.1.0.0"
#define AppExeName "Sempervirens.exe"

[Setup]
AppId={{695688F7-ADF4-4F7C-BE0F-8DA61D74C576}
AppName={#AppName}
AppVersion={#AppVersion}
MinVersion=10.0.10240
AppPublisher=XintingleiTeam
AppPublisherURL=https://github.com/XintingleiTeam/sempervirens
AppSupportURL=https://github.com/XintingleiTeam/sempervirens/issues
AppUpdatesURL=https://github.com/XintingleiTeam/sempervirens/releases
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\artifacts\release
OutputBaseFilename=Sempervirens-0.1.0.0-Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\sempervirens.ico
WizardImageFile=..\assets\branding\installer-welcome.png
WizardSmallImageFile=..\assets\branding\installer-header-light.png
DisableWelcomePage=no
ShowLanguageDialog=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExeName}
UsePreviousAppDir=yes
UsePreviousLanguage=yes
UsePreviousTasks=yes
CloseApplications=yes
RestartApplications=no
ChangesAssociations=yes

[Languages]
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
chinesesimplified.WelcomeLabel1=欢迎安装 Sempervirens
english.WelcomeLabel1=Welcome to Sempervirens Setup
chinesesimplified.FinishedHeadingLabel=Sempervirens 安装完成
english.FinishedHeadingLabel=Sempervirens Setup Complete

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\release\Sempervirens.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\release\SempervirensUpdater.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\release\current.json"; DestDir: "{app}"; Flags: onlyifdoesntexist
Source: "..\build\release\versions\0.1.0.0\SempervirensApp.exe"; DestDir: "{app}\versions\0.1.0.0"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\XintingleiTeam\Sempervirens"; ValueType: string; ValueName: "InstallerLanguage"; ValueData: "{language}"; Flags: uninsdeletevalue uninsdeletekeyifempty

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[Code]
procedure SHChangeNotify(EventId: Integer; Flags: Cardinal; Item1, Item2: Integer);
  external 'SHChangeNotify@shell32.dll stdcall';

procedure SHChangeNotifyPath(EventId: Integer; Flags: Cardinal; const Item1: String; Item2: Integer);
  external 'SHChangeNotify@shell32.dll stdcall';

procedure RefreshFileIcon(const Path: String);
begin
  if FileExists(Path) then
    SHChangeNotifyPath($00002000, $00002005, Path, 0);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    { 覆盖 EXE 后刷新当前用户的图标缓存，并通知资源管理器重新读取资源。 }
    RefreshFileIcon(ExpandConstant('{app}\{#AppExeName}'));
    RefreshFileIcon(ExpandConstant('{app}\unins000.exe'));
    RefreshFileIcon(ExpandConstant('{autoprograms}\{#AppName}.lnk'));
    RefreshFileIcon(ExpandConstant('{autodesktop}\{#AppName}.lnk'));
    Exec(ExpandConstant('{sys}\ie4uinit.exe'), '-show', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
    SHChangeNotify($08000000, 0, 0, 0);
  end;
end;
