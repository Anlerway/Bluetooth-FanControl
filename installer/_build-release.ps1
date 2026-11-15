<#
FanControl release packaging (single-process FanControl.exe + in-process FanControl.UI.dll)
-------------------------------------------------------------------------------
Artifacts (names from package-names.txt, UTF-8, {0} = version):
  zip   self-contained : <ReleaseRoot>\FanControl-<version>-selfcontained-win-x64.zip
  zip   needs .NET 8   : <ReleaseRoot>\FanControl-<version>-framework-win-x64.zip
  installer (one package, two payloads) : <InstallerRoot>\FanControl-Setup-<version>.exe

Usage:
  powershell -NoProfile -ExecutionPolicy Bypass -File installer\_build-release.ps1

Flow: build the C++ UI DLL -> dotnet publish the host (two flavors, output is FanControl.exe)
-> copy the UI DLL into each publish folder -> zip both -> build the native installer
(installer\native\build.bat) -> append both payloads with installer\pack.py (raw LZMA2).
#>

param(
    [string]$Version = '2.0.0',
    [string]$Configuration = 'Release',
    [string]$RuntimeIdentifier = 'win-x64',
    [string]$HostProject = "$PSScriptRoot\..\src\FanControl.Host\FanControl.Host.csproj",
    [string]$UiProjectDir = "$PSScriptRoot\..\src\FanControl.UI.Cpp",
    # Release artifacts stay outside the repo: the repo holds sources, build scripts, docs, tests only.
    [string]$ReleaseRoot = "$PSScriptRoot\..\..\artifacts\release",
    [string]$InstallerRoot = "$PSScriptRoot\..\..\artifacts\installer",
    [string]$NamesFile = "$PSScriptRoot\package-names.txt",
    # pack.py only needs the standard library; empty = take python from PATH
    [string]$Python = ''
)

$ErrorActionPreference = 'Stop'
$HostProject = (Resolve-Path $HostProject).Path
$UiProjectDir = (Resolve-Path $UiProjectDir).Path
$ReleaseRoot = [System.IO.Path]::GetFullPath($ReleaseRoot)
$InstallerRoot = [System.IO.Path]::GetFullPath($InstallerRoot)

# --- package names (UTF-8 file, {0} = version) ----------------------------------
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

    foreach ($required in @('zip-selfcontained', 'zip-framework', 'setup')) {
        if (-not $names.ContainsKey($required)) { throw "package-names.txt is missing key: $required" }
    }

    return $names
}

$packageNames = Get-PackageNames -Path $NamesFile -Version $Version

Write-Host "== FanControl $Version release packaging ==" -ForegroundColor Cyan
Write-Host "Host: $HostProject"
Write-Host "UI  : $UiProjectDir"

# --- 1) build the C++ UI ---------------------------------------------------------
function Invoke-UiBuild {
    $bat = Join-Path $script:UiProjectDir 'build.bat'
    if (-not (Test-Path $bat)) { throw "build.bat not found: $bat" }

    Write-Host "`n-- build C++ UI -> $script:UiProjectDir\build" -ForegroundColor Yellow
    # A running host locks the DLL, stop the instances we launched
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

# --- 2) publish the host ---------------------------------------------------------
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

    # The UI DLL is copied by csproj None/CopyToPublishDirectory; copy once more as a safety net
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

function Invoke-NativeSetup {
    param([string]$FullDir, [string]$SlimDir, [string]$Name, [string]$OutDir)

    $nativeDir = Join-Path $script:PSScriptRoot 'native'
    $bat = Join-Path $nativeDir 'build.bat'
    if (-not (Test-Path $bat)) { throw "build.bat not found: $bat" }
    $packer = Join-Path $script:PSScriptRoot 'pack.py'
    if (-not (Test-Path $packer)) { throw "pack.py not found: $packer" }

    Write-Host "`n-- native installer (setup.exe + uninstall.exe)" -ForegroundColor Yellow
    $cmd = 'cd /d "' + $nativeDir + '" && build.bat ' + $script:Version
    & cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "native installer build failed (exit $LASTEXITCODE)" }

    $setupExe = Join-Path $nativeDir 'build\setup.exe'
    if (-not (Test-Path $setupExe)) { throw "setup.exe missing: $setupExe" }

    $python = $script:Python
    if (-not $python) { $python = (Get-Command python -ErrorAction SilentlyContinue).Source }
    if (-not $python) { throw "python not found: pass -Python <python.exe> (standard library only)" }

    if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
    $outExe = Join-Path $OutDir ($Name + '.exe')
    Write-Host "`n-- payloads (raw LZMA2) -> $outExe" -ForegroundColor Yellow
    & $python $packer --exe $setupExe --out $outExe --payload "full=$FullDir" --payload "slim=$SlimDir"
    if ($LASTEXITCODE -ne 0) { throw "payload packing failed (exit $LASTEXITCODE)" }
}

# --- main -----------------------------------------------------------------------
$uiDll = Invoke-UiBuild

$selfContainedDir = Join-Path $ReleaseRoot 'selfcontained'
$frameworkDir = Join-Path $ReleaseRoot 'framework'

Invoke-Publish -Name 'self-contained' -OutDir $selfContainedDir -SelfContained $true -UiDll $uiDll
Invoke-Publish -Name 'framework-dependent (.NET 8 required)' -OutDir $frameworkDir -SelfContained $false -UiDll $uiDll

New-ZipPackage -SourceDir $selfContainedDir -ZipPath (Join-Path $ReleaseRoot $packageNames['zip-selfcontained'])
New-ZipPackage -SourceDir $frameworkDir -ZipPath (Join-Path $ReleaseRoot $packageNames['zip-framework'])

Invoke-NativeSetup -FullDir $selfContainedDir -SlimDir $frameworkDir -Name $packageNames['setup'] -OutDir $InstallerRoot

Write-Host "`n== artifacts ==" -ForegroundColor Cyan
foreach ($zip in @($packageNames['zip-selfcontained'], $packageNames['zip-framework'])) {
    $path = Join-Path $ReleaseRoot $zip
    if (Test-Path $path) { Write-Host ("{0,-62} {1,8:N1} MB" -f $zip, ((Get-Item $path).Length / 1MB)) }
}

$setup = $packageNames['setup'] + '.exe'
$setupPath = Join-Path $InstallerRoot $setup
if (Test-Path $setupPath) { Write-Host ("{0,-62} {1,8:N1} MB" -f $setup, ((Get-Item $setupPath).Length / 1MB)) }
