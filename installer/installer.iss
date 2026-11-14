; FanControl Inno Setup 安装器
; 单个安装包内嵌两套负载：
;   FullSource 自带 .NET 8 运行时（体积大，任何机器都能跑）
;   SlimSource 需系统已装 .NET 8 桌面运行时（体积小）
; 安装时检测系统运行时：没有 → 强制自带；有 → 允许用户在「运行环境」页二选一。
; 覆盖升级：安装前清空安装目录，旧的运行时文件不会残留。
;
; 命令行（都支持 /SILENT 与 /VERYSILENT）：
;   /FULL   强制自带运行时        /SLIM   强制使用系统运行时
;   /DIR="..."  指定安装目录
;
; 编译：ISCC.exe installer.iss /DAppVersion=1.4.0 ^
;         /DFullSource="..\..\artifacts\release\selfcontained" ^
;         /DSlimSource="..\..\artifacts\release\framework" ^
;         /DOutputName="FanControl-Setup-1.4.0" /DOutputDir="..\..\artifacts\installer"

#define MyAppName "FanControl"
#ifndef AppVersion
  #define AppVersion "1.4.0"
#endif
#define MyAppVersion AppVersion
#define MyAppPublisher "anlerway"
#define MyAppExeName "FanControl.exe"

#ifndef FullSource
  #define FullSource "..\..\artifacts\release\selfcontained"
#endif
#ifndef SlimSource
  #define SlimSource "..\..\artifacts\release\framework"
#endif
#ifndef OutputName
  #define OutputName "FanControl-Setup"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\artifacts\installer"
#endif
; 本地测试时可覆盖：/DPrivileges=lowest /DDefaultDir={localappdata}\FanControlTest
#ifndef Privileges
  #define Privileges "admin"
#endif
#ifndef DefaultDir
  #define DefaultDir "{autopf}\FanControl"
#endif

[Setup]
AppId={{8F2B6C41-7E5A-4D2F-9B31-2A8C6E1D5F90}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL=https://github.com/anlerway/Bluetooth-FanControl
AppSupportURL=https://github.com/anlerway/Bluetooth-FanControl
DefaultDirName={#DefaultDir}
DefaultGroupName=FanControl
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired={#Privileges}
CloseApplications=yes
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
VersionInfoVersion={#MyAppVersion}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} {#MyAppVersion} Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\src\FanControl.UI.Cpp\app.ico
UninstallDisplayIcon={app}\FanControl.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"

[CustomMessages]
english.RuntimeCaption=Runtime
english.RuntimeDesc=Choose which .NET runtime this installation should use.
english.RtSlim=Use the .NET 8 Desktop Runtime already on this PC (smaller)
english.RtFull=Install the bundled .NET 8 Desktop Runtime (works everywhere)
english.RuntimeHint=Your system already has the .NET 8 Desktop Runtime. The bundled copy is independent of it, but takes about 70 MB more disk space.
english.MemoRuntimeRuntime=Runtime: bundled .NET 8 Desktop Runtime
english.MemoRuntimeSystem=Runtime: system .NET 8 Desktop Runtime
english.TaskDesktopIcon=Create a desktop shortcut
english.TaskGroup=Additional tasks:
english.RunApp=Launch FanControl
english.UninstallKeepData=Keep the configuration and log files?
english.UninstallKeepDataDesc=Choose No to delete them as well. They are stored in %APPDATA%\FanControl.

chinesesimplified.RuntimeCaption=运行环境
chinesesimplified.RuntimeDesc=请选择本次安装要使用的 .NET 运行环境。
chinesesimplified.RtSlim=使用系统已安装的 .NET 8 桌面运行时（体积小）
chinesesimplified.RtFull=使用安装包自带的 .NET 8 桌面运行时（任何电脑都能用）
chinesesimplified.RuntimeHint=检测到本机已安装 .NET 8 桌面运行时。自带的一份与它互不影响，但会多占约 70 MB 磁盘空间。
chinesesimplified.MemoRuntimeRuntime=运行环境：自带的 .NET 8 桌面运行时
chinesesimplified.MemoRuntimeSystem=运行环境：系统已安装的 .NET 8 桌面运行时
chinesesimplified.TaskDesktopIcon=创建桌面快捷方式
chinesesimplified.TaskGroup=附加任务：
chinesesimplified.RunApp=启动 FanControl
chinesesimplified.UninstallKeepData=保留配置与日志文件吗？
chinesesimplified.UninstallKeepDataDesc=选择「否」会一并删除，它们保存在 %APPDATA%\FanControl。

[Tasks]
Name: "desktopicon"; Description: "{cm:TaskDesktopIcon}"; GroupDescription: "{cm:TaskGroup}"

; 覆盖升级前清空安装目录：避免「自带运行时」与「需环境」两种负载混装后残留 coreclr/hostfxr，
; 导致使用系统运行时的版本反而找不到匹配的运行时。
[InstallDelete]
Type: filesandordirs; Name: "{app}\*"

[Files]
Source: "{#SlimSource}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: IsSlimPayload
Source: "{#FullSource}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: IsFullPayload

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:RunApp}"; Flags: nowait postinstall skipifsilent

[Code]
var
  RuntimePage: TWizardPage;
  RadioSlim: TNewRadioButton;
  RadioFull: TNewRadioButton;
  HintLabel: TNewStaticText;
  HasDesktopRuntime: Boolean;
  WantBundled: Boolean;
  ForceBundled: Boolean;
  ForceSlim: Boolean;
  UserDataStashed: Boolean;
  KeepUserData: Boolean;

// 系统是否已装 .NET 8 桌面运行时：先查官方注册表记录，再兜底查安装目录
function DesktopRuntime8Installed(): Boolean;
var
  Names: TArrayOfString;
  I: Integer;
  Root: String;
begin
  Result := False;
  Root := 'SOFTWARE\dotnet\Setup\InstalledVersions\x64\sharedfx\Microsoft.WindowsDesktop.App';
  if RegGetValueNames(HKLM64, Root, Names) then
    for I := 0 to GetArrayLength(Names) - 1 do
      if Pos('8.', Names[I]) = 1 then
      begin
        Result := True;
        Exit;
      end;
  if RegGetValueNames(HKLM32, Root, Names) then
    for I := 0 to GetArrayLength(Names) - 1 do
      if Pos('8.', Names[I]) = 1 then
      begin
        Result := True;
        Exit;
      end;
end;

function HasSwitch(const Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Name) = 0 then
    begin
      Result := True;
      Exit;
    end;
end;

function InitializeSetup(): Boolean;
begin
  ForceBundled := HasSwitch('/FULL');
  ForceSlim := HasSwitch('/SLIM');
  HasDesktopRuntime := DesktopRuntime8Installed();
  if ForceSlim then
    WantBundled := False
  else if ForceBundled then
    WantBundled := True
  else
    WantBundled := not HasDesktopRuntime;
  Result := True;
end;

procedure InitializeWizard();
begin
  RuntimePage := CreateCustomPage(wpSelectDir, ExpandConstant('{cm:RuntimeCaption}'), ExpandConstant('{cm:RuntimeDesc}'));

  RadioSlim := TNewRadioButton.Create(RuntimePage);
  RadioSlim.Parent := RuntimePage.Surface;
  RadioSlim.Left := 0;
  RadioSlim.Top := 0;
  RadioSlim.Width := RuntimePage.SurfaceWidth;
  RadioSlim.Caption := ExpandConstant('{cm:RtSlim}');
  RadioSlim.Checked := not WantBundled;

  RadioFull := TNewRadioButton.Create(RuntimePage);
  RadioFull.Parent := RuntimePage.Surface;
  RadioFull.Left := 0;
  RadioFull.Top := ScaleY(30);
  RadioFull.Width := RuntimePage.SurfaceWidth;
  RadioFull.Caption := ExpandConstant('{cm:RtFull}');
  RadioFull.Checked := WantBundled;

  HintLabel := TNewStaticText.Create(RuntimePage);
  HintLabel.Parent := RuntimePage.Surface;
  HintLabel.Left := 0;
  HintLabel.Top := ScaleY(64);
  HintLabel.Width := RuntimePage.SurfaceWidth;
  HintLabel.AutoSize := False;
  HintLabel.Height := ScaleY(56);
  HintLabel.WordWrap := True;
  HintLabel.Caption := ExpandConstant('{cm:RuntimeHint}');
end;

// 没有系统运行时（或命令行强制）时不显示这一页
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  if PageID = RuntimePage.ID then
    Result := (not HasDesktopRuntime) or ForceBundled or ForceSlim
  else
    Result := False;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  if CurPageID = RuntimePage.ID then
    WantBundled := RadioFull.Checked;
  Result := True;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine;
  if WantBundled then
    Result := Result + ExpandConstant('{cm:MemoRuntimeRuntime}')
  else
    Result := Result + ExpandConstant('{cm:MemoRuntimeSystem}');
  if MemoTasksInfo <> '' then
    Result := Result + NewLine + NewLine + MemoTasksInfo;
end;

function IsSlimPayload(): Boolean;
begin
  Result := not WantBundled;
end;

function IsFullPayload(): Boolean;
begin
  Result := WantBundled;
end;

// 安装/覆盖/卸载前先关闭正在运行的进程，避免 exe 与 dll 被占用
procedure KillFanControl();
var
  ResultCode: Integer;
begin
  Exec('taskkill.exe', '/IM FanControl.exe /T /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

// cmd 的 move：同卷是改名，跨卷是复制后删除。目标已存在会失败，先清掉目标。
function MoveDir(const FromDir, ToDir: String): Boolean;
var
  ResultCode: Integer;
begin
  Result := False;
  if not DirExists(FromDir) then
    Exit;
  DelTree(ToDir, True, True, True);
  if Exec(ExpandConstant('{cmd}'), '/c move /y "' + FromDir + '" "' + ToDir + '"', '',
          SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Result := (ResultCode = 0) and DirExists(ToDir);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  KillFanControl();
  // 覆盖升级：先把安装目录里的 Userdata（「程序目录」数据模式的配置与日志）挪到临时目录，
  // 等 [InstallDelete] 清空安装目录后再挪回来，配置不会因为升级丢掉。
  UserDataStashed := MoveDir(ExpandConstant('{app}\Userdata'), ExpandConstant('{tmp}\FanControl-Userdata-upgrade'));
  Result := '';
end;

procedure DeinitializeSetup();
begin
  // 安装失败中途退出时也要把配置挪回去，不能留在临时目录里被清掉
  if UserDataStashed then
  begin
    ForceDirectories(ExpandConstant('{app}'));
    MoveDir(ExpandConstant('{tmp}\FanControl-Userdata-upgrade'), ExpandConstant('{app}\Userdata'));
    UserDataStashed := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and UserDataStashed then
  begin
    ForceDirectories(ExpandConstant('{app}'));
    MoveDir(ExpandConstant('{tmp}\FanControl-Userdata-upgrade'), ExpandConstant('{app}\Userdata'));
    UserDataStashed := False;
  end;
end;

function PrepareToUninstall(var Msg: String): Boolean;
begin
  KillFanControl();
  Result := True;
end;

// 卸载：先删掉开机自启的计划任务，再问是否保留配置，最后按选择处理数据目录
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
  AppDataDir: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    Exec('schtasks.exe', '/Delete /TN FanControl /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    AppDataDir := ExpandConstant('{userappdata}\FanControl');
    // 默认保留：卸载器不该顺手把用户配置删了，只有明确选「否」才删
    KeepUserData := True;
    if (not UninstallSilent) and (DirExists(AppDataDir) or DirExists(ExpandConstant('{app}\Userdata'))) then
      KeepUserData := MsgBox(ExpandConstant('{cm:UninstallKeepData}') + #13#10 + #13#10 +
                             ExpandConstant('{cm:UninstallKeepDataDesc}'), mbConfirmation, MB_YESNO) = IDYES;

    if not KeepUserData then
      DelTree(AppDataDir, True, True, True);
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    // 选「保留」时不动安装目录里的 Userdata；选「删除」时连安装目录一起清干净
    if not KeepUserData then
      DelTree(ExpandConstant('{app}'), True, True, True);
  end;
end;
