<#
FanControl 发布打包脚本（单一进程 FanControl.exe + 进程内界面 FanControl.UI.dll）
-------------------------------------------------------------------------------
产物（文件名来自 package-names.txt，UTF-8 保存，{0} 为版本号）：
  zip   自带运行时 : <ReleaseRoot>\FanControl-v1.4.0-<label>.zip
  zip   需 .NET 8  : <ReleaseRoot>\FanControl-v1.4.0-<label>.zip
  安装包 自带运行时 : <InstallerRoot>\FanControl-v1.4.0-<label>.exe
  安装包 需 .NET 8 : <InstallerRoot>\FanControl-v1.4.0-<label>.exe

用法：
  powershell -NoProfile -ExecutionPolicy Bypass -File installer\_build-release.ps1

流程：本地构建 C++ 界面 DLL → dotnet publish 宿主（两种 flavor，产物是 FanControl.exe）
→ 把界面 DLL 拷进发布目录 → 打包 zip 与 Inno Setup 安装包。安装包不检测 .NET 运行环境（见 installer.iss）。
#>

param(
    [string]$Version = '1.4.0',
    [string]$Configuration = 'Release',
    [string]$RuntimeIdentifier = 'win-x64',
    [string]$HostProject = "$PSScriptRoot\..\src\FanControl.Host\FanControl.Host.csproj",
    [string]$UiProjectDir = "$PSScriptRoot\..\src\FanControl.UI.Cpp",
    # 产物放在仓库外：仓库只保留源码、构建脚本、文档与测试。
    [string]$ReleaseRoot = "$PSScriptRoot\..\..\artifacts\release",
    [string]$InstallerRoot = "$PSScriptRoot\..\..\artifacts\installer",
    [string]$NamesFile = "$PSScriptRoot\package-names.txt",
    [string]$Iscc = 'D:\APP\Inno Setup 6\ISCC.exe'
)

$ErrorActionPreference = 'Stop'
$HostProject = (Resolve-Path $HostProject).Path
$UiProjectDir = (Resolve-Path $UiProjectDir).Path
$ReleaseRoot = [System.IO.Path]::GetFullPath($ReleaseRoot)
$InstallerRoot = [System.IO.Path]::GetFullPath($InstallerRoot)

# --- 包名（UTF-8 文件，{0} = 版本号） --------------------------------
function Get-PackageNames {
    param([string]$Path, [string]$Version)

    $names = @{}
    foreach ($line in [System.IO.File]::ReadAllLines($Path, [System.Text.Encoding]::UTF8)) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) { continue }
        $parts = $trimmed.Split('=', 2)
        if ($parts.Length -ne 2) { continue }
        $names[$parts[0].Trim()] = $parts[1].Trim().Replace('{0}', $Version)
    }

    foreach ($required in @('zip-selfcontained', 'zip-framework', 'setup-selfcontained', 'setup-framework')) {
        if (-not $names.ContainsKey($required)) { throw "package-names.txt is missing key: $required" }
    }

    return $names
}

$packageNames = Get-PackageNames -Path $NamesFile -Version $Version

Write-Host "== FanControl $Version release packaging ==" -ForegroundColor Cyan
Write-Host "Host: $HostProject"
Write-Host "UI  : $UiProjectDir"

# --- 1) 构建 C++ 界面 ------------------------------------------------
function Invoke-UiBuild {
    $bat = Join-Path $script:UiProjectDir 'build.bat'
    if (-not (Test-Path $bat)) { throw "build.bat not found: $bat" }

    Write-Host "`n-- build C++ UI -> $script:UiProjectDir\build" -ForegroundColor Yellow
    # 正在运行的宿主会锁住 DLL，先结束自己拉起的实例
    Get-Process FanControl -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

    $cmd = 'cd /d "' + $script:UiProjectDir + '" && build.bat > build.log 2>&1'
    & cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) {
        Get-Content (Join-Path $script:UiProjectDir 'build.log') -Tail 20 | Write-Host
        throw "C++ UI build failed (exit $LASTEXITCODE)"
    }

    $dll = Join-Path $script:UiProjectDir 'build\FanControl.UI.dll'
    if (-not (Test-Path $dll)) { throw "FanControl.UI.dll missing after build: $dll" }
    Write-Host ("   ok: FanControl.UI.dll {0:N0} bytes" -f (Get-Item $dll).Length)
    return $dll
}

# --- 2) 发布后台 ------------------------------------------------------
function Invoke-Publish {
    param([string]$Name, [string]$OutDir, [bool]$SelfContained, [string]$UiDll)

    Write-Host "`n-- publish [$Name] -> $OutDir" -ForegroundColor Yellow
    if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }

    $publishArgs = @(
        'publish', $script:HostProject,
        '-c', $script:Configuration,
        '-r', $script:RuntimeIdentifier,
        "--self-contained=$($SelfContained.ToString().ToLower())",
        '-p:PublishSingleFile=false',
        '-p:PublishReadyToRun=false',
        '-p:DebugType=none',
        "-p:Version=$script:Version",
        '-o', $OutDir,
        '-m:1'
    )

    & dotnet @publishArgs
    if ($LASTEXITCODE -ne 0) { throw "publish failed: $Name (exit $LASTEXITCODE)" }

    $hostExe = Join-Path $OutDir 'FanControl.exe'
    if (-not (Test-Path $hostExe)) { throw "FanControl.exe missing in publish output: $OutDir" }

    # 界面 DLL 由 csproj 的 None/ CopyToPublishDirectory 带出来，这里再兜一次底
    Copy-Item $UiDll (Join-Path $OutDir 'FanControl.UI.dll') -Force

    $files = Get-ChildItem $OutDir -Recurse -File
    $size = [math]::Round(($files | Measure-Object Length -Sum).Sum / 1MB, 1)
    Write-Host ("   ok: FanControl.exe + FanControl.UI.dll, {0} files / {1} MB" -f $files.Count, $size)
}

function New-ZipPackage {
    param([string]$SourceDir, [string]$ZipPath)

    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    Write-Host "`n-- zip -> $ZipPath" -ForegroundColor Yellow
    Compress-Archive -Path (Join-Path $SourceDir '*') -DestinationPath $ZipPath -CompressionLevel Optimal
    $size = [math]::Round((Get-Item $ZipPath).Length / 1MB, 1)
    Write-Host ("   ok: {0} MB" -f $size)
}

function Invoke-Iscc {
    param(
        [string]$Compiler,
        [string]$Script,
        [string]$SourceDir,
        [string]$Name,
        [string]$OutDir
    )

    Write-Host "`n-- installer [$Name]" -ForegroundColor Yellow
    & $Compiler $Script "/DAppVersion=$script:Version" "/DAppSource=$SourceDir" "/DOutputName=$Name" "/DOutputDir=$OutDir" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed: $Name (exit $LASTEXITCODE)" }
}

# --- 主流程 -----------------------------------------------------------
$uiDll = Invoke-UiBuild

$selfContainedDir = Join-Path $ReleaseRoot 'selfcontained'
$frameworkDir = Join-Path $ReleaseRoot 'framework'

Invoke-Publish -Name 'self-contained' -OutDir $selfContainedDir -SelfContained $true -UiDll $uiDll
Invoke-Publish -Name 'framework-dependent (.NET 8 required)' -OutDir $frameworkDir -SelfContained $false -UiDll $uiDll

New-ZipPackage -SourceDir $selfContainedDir -ZipPath (Join-Path $ReleaseRoot $packageNames['zip-selfcontained'])
New-ZipPackage -SourceDir $frameworkDir -ZipPath (Join-Path $ReleaseRoot $packageNames['zip-framework'])

if (-not (Test-Path $Iscc)) { throw "Inno Setup compiler not found: $Iscc" }
if (-not (Test-Path $InstallerRoot)) { New-Item -ItemType Directory -Path $InstallerRoot | Out-Null }

$iss = Join-Path $PSScriptRoot 'installer.iss'
Invoke-Iscc -Compiler $Iscc -Script $iss -SourceDir $selfContainedDir -Name $packageNames['setup-selfcontained'] -OutDir $InstallerRoot
Invoke-Iscc -Compiler $Iscc -Script $iss -SourceDir $frameworkDir -Name $packageNames['setup-framework'] -OutDir $InstallerRoot

Write-Host "`n== artifacts ==" -ForegroundColor Cyan
foreach ($zip in @($packageNames['zip-selfcontained'], $packageNames['zip-framework'])) {
    $path = Join-Path $ReleaseRoot $zip
    if (Test-Path $path) { Write-Host ("{0,-62} {1,8:N1} MB" -f $zip, ((Get-Item $path).Length / 1MB)) }
}

foreach ($setup in @($packageNames['setup-selfcontained'], $packageNames['setup-framework'])) {
    $path = Join-Path $InstallerRoot ($setup + '.exe')
    if (Test-Path $path) { Write-Host ("{0,-62} {1,8:N1} MB" -f ($setup + '.exe'), ((Get-Item $path).Length / 1MB)) }
}
