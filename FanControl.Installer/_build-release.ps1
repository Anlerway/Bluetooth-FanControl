# FanControl release packaging script (four distribution flavors)
# ------------------------------------------------------------------
# Outputs:
#   Zip  - self-contained   artifacts\release\FanControl-1.2.0-selfcontained-win-x64.zip
#   Zip  - needs .NET 8     artifacts\release\FanControl-1.2.0-framework-win-x64.zip
#   Setup- self-contained   artifacts\installer\FanControl-Setup-Full-1.2.0.exe
#   Setup- needs .NET 8     artifacts\installer\FanControl-Setup-Slim-1.2.0.exe
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File FanControl.Installer\_build-release.ps1
#
# The app is published as a normal folder (PublishSingleFile disabled), then the
# folder is zipped / fed to Inno Setup. The installer never checks for the .NET
# runtime (see installer.iss).

param(
    [string]$Version = '1.2.0',
    [string]$Configuration = 'Release',
    [string]$RuntimeIdentifier = 'win-x64',
    [string]$Project = "$PSScriptRoot\..\FanControl.UI\FanControl.UI.csproj",
    [string]$ReleaseRoot = "$PSScriptRoot\..\artifacts\release",
    [string]$InstallerRoot = "$PSScriptRoot\..\artifacts\installer",
    [string]$Iscc = 'D:\APP\Inno Setup 6\ISCC.exe'
)

$ErrorActionPreference = 'Stop'
$Project = (Resolve-Path $Project).Path
$ReleaseRoot = [System.IO.Path]::GetFullPath($ReleaseRoot)
$InstallerRoot = [System.IO.Path]::GetFullPath($InstallerRoot)

Write-Host "== FanControl $Version release packaging ==" -ForegroundColor Cyan
Write-Host "Project: $Project"

function Invoke-Publish {
    param([string]$Name, [string]$OutDir, [bool]$SelfContained)

    Write-Host "`n-- build [$Name] -> $OutDir" -ForegroundColor Yellow
    if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }

    $publishArgs = @(
        'publish', $Project,
        '-c', $Configuration,
        '-r', $RuntimeIdentifier,
        "--self-contained=$($SelfContained.ToString().ToLower())",
        '-p:PublishSingleFile=false',
        '-p:PublishReadyToRun=false',
        '-p:DebugType=none',
        "-p:Version=$Version",
        '-o', $OutDir
    )

    & dotnet @publishArgs
    if ($LASTEXITCODE -ne 0) { throw "publish failed: $Name (exit $LASTEXITCODE)" }

    $exe = Join-Path $OutDir 'FanControl.exe'
    if (-not (Test-Path $exe)) { throw "FanControl.exe missing in publish output: $OutDir" }
    $files = Get-ChildItem $OutDir -Recurse -File
    $size = [math]::Round(($files | Measure-Object Length -Sum).Sum / 1MB, 1)
    Write-Host ("   ok: FanControl.exe, {0} files / {1} MB" -f $files.Count, $size)
}

function New-ZipPackage {
    param([string]$SourceDir, [string]$ZipPath)

    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    Write-Host "`n-- zip -> $ZipPath" -ForegroundColor Yellow
    Compress-Archive -Path (Join-Path $SourceDir '*') -DestinationPath $ZipPath -CompressionLevel Optimal
    $size = [math]::Round((Get-Item $ZipPath).Length / 1MB, 1)
    Write-Host ("   ok: {0} MB" -f $size)
}

# 1) two application builds -----------------------------------------
$selfContainedDir = Join-Path $ReleaseRoot 'selfcontained'
$frameworkDir = Join-Path $ReleaseRoot 'framework'

Invoke-Publish -Name 'self-contained' -OutDir $selfContainedDir -SelfContained $true
Invoke-Publish -Name 'framework-dependent (.NET 8 required)' -OutDir $frameworkDir -SelfContained $false

# 2) two zip packages ------------------------------------------------
New-ZipPackage -SourceDir $selfContainedDir -ZipPath (Join-Path $ReleaseRoot "FanControl-$Version-selfcontained-$RuntimeIdentifier.zip")
New-ZipPackage -SourceDir $frameworkDir -ZipPath (Join-Path $ReleaseRoot "FanControl-$Version-framework-$RuntimeIdentifier.zip")

# 3) two installers (no .NET runtime detection) ----------------------
if (-not (Test-Path $Iscc)) { throw "Inno Setup compiler not found: $Iscc" }
if (-not (Test-Path $InstallerRoot)) { New-Item -ItemType Directory -Path $InstallerRoot | Out-Null }

$iss = Join-Path $PSScriptRoot 'installer.iss'
foreach ($pair in @(
        @{ Source = $selfContainedDir; Name = "FanControl-Setup-Full-$Version" },
        @{ Source = $frameworkDir; Name = "FanControl-Setup-Slim-$Version" })) {

    Write-Host "`n-- installer [$($pair.Name)]" -ForegroundColor Yellow
    & $Iscc $iss "/DAppSource=$($pair.Source)" "/DOutputName=$($pair.Name)" "/DOutputDir=$InstallerRoot"
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed: $($pair.Name) (exit $LASTEXITCODE)" }
}

# 4) summary ---------------------------------------------------------
Write-Host "`n== artifacts ==" -ForegroundColor Cyan
Get-ChildItem $ReleaseRoot -Filter "FanControl-$Version-*.zip" |
    ForEach-Object { "{0,-58} {1,8:N1} MB" -f $_.Name, ($_.Length / 1MB) }
Get-ChildItem $InstallerRoot -Filter "FanControl-Setup-*-$Version.exe" |
    ForEach-Object { "{0,-58} {1,8:N1} MB" -f $_.Name, ($_.Length / 1MB) }
