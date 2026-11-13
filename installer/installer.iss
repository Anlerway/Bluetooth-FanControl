; FanControl Inno Setup installer script
; 两个发行版本共用此脚本（仅 AppSource / OutputName 不同）：
;   ISCC.exe installer.iss /DAppSource="...\selfcontained" /DOutputName="FanControl-Setup-Full-1.4.0"
;   ISCC.exe installer.iss /DAppSource="...\framework"     /DOutputName="FanControl-Setup-Slim-1.4.0"
;
; 安装内容是两个 exe：FanControl.Host.exe（后台守护，随开机自启）与 FanControl.UI.exe（界面，按需拉起）。
;
; 注意：安装阶段不检测 .NET 运行环境。
; “需环境版”装到未安装 .NET 8 的机器上依然可以正常安装，由程序首次启动时自行提示。

#define MyAppName "FanControl"
#ifndef AppVersion
  #define AppVersion "1.4.0"
#endif
#define MyAppVersion AppVersion
#define MyAppPublisher "anlerway"
#define MyAppExeName "FanControl.Host.exe"

#ifndef AppSource
  #define AppSource "..\..\artifacts\release\selfcontained"
#endif
#ifndef OutputName
  #define OutputName "FanControl-Setup-Full"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\artifacts\installer"
#endif

[Setup]
AppId={{8F2B6C41-7E5A-4D2F-9B31-2A8C6E1D5F90}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL=https://github.com/anlerway/Bluetooth-FanControl
AppSupportURL=https://github.com/anlerway/Bluetooth-FanControl
DefaultDirName={autopf}\FanControl
DefaultGroupName=FanControl
DisableProgramGroupPage=yes
PrivilegesRequired=admin
CloseApplications=yes
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
VersionInfoVersion={#MyAppVersion}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} {#MyAppVersion} Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\src\FanControl.UI.Cpp\app.ico
UninstallDisplayIcon={app}\FanControl.Host.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional tasks:"

; 安装前清空旧目录：避免自带运行库版与无环境版混装后残留 coreclr/hostfxr 等运行时文件，
; 导致无环境版启动时报“需要安装 .NET 8”。
[InstallDelete]
Type: filesandordirs; Name: "{app}\*"

[Files]
Source: "{#AppSource}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[Code]
// 安装/覆盖/卸载前先关闭正在运行的进程：界面与后台各一个 exe，避免文件被占用
procedure KillFanControl();
var
  ResultCode: Integer;
begin
  Exec('taskkill.exe', '/IM FanControl.UI.exe /T /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec('taskkill.exe', '/IM FanControl.Host.exe /T /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  KillFanControl();
  Result := '';
end;

function PrepareToUninstall(var Msg: String): Boolean;
begin
  KillFanControl();
  Result := True;
end;
