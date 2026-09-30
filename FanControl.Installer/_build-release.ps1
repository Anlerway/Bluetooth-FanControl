# FanControl release packaging script (four distribution flavors)
# ------------------------------------------------------------------
# Outputs (names come from package-names.txt, which is UTF-8 so the Chinese
# labels survive; this script itself stays ASCII-only for Windows PowerShell):
#   zip   self-contained : <ReleaseRoot>\FanControl-v1.2.0-<label>.zip
#   zip   needs .NET 8   : <ReleaseRoot>\FanControl-v1.2.0-<label>.zip
#   setup self-contained : <InstallerRoot>\FanControl-v1.2.0-<label>.exe
#   setup needs .NET 8   : <InstallerRoot>\FanControl-v1.2.0-<label>.exe
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
    [string]$NamesFile = "$PSScriptRoot\package-names.txt",
    [string]$Iscc = 'D:\APP\Inno Setup 6\ISCC.exe'
)

$ErrorActionPreference = 'Stop'
$Project = (Resolve-Path $Project).Path
$ReleaseRoot = [System.IO.Path]::GetFullPath($ReleaseRoot)
$InstallerRoot = [System.IO.Path]::GetFullPath($InstallerRoot)

# --- package names (UTF-8 file, {0} = version) ----------------------
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

function Invoke-Iscc {
    param(
        [string]$Compiler,
        [string]$Script,
        [string]$SourceDir,
        [string]$Name,
        [string]$OutDir
    )

    Write-Host "`n-- installer [$Name]" -ForegroundColor Yellow
    # Chinese package names survive because PowerShell hands them to the native
    # ISCC process as UTF-16 through CreateProcess
    & $Compiler $Script "/DAppSource=$SourceDir" "/DOutputName=$Name" "/DOutputDir=$OutDir" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed: $Name (exit $LASTEXITCODE)" }
}

# 1) two application builds -----------------------------------------
$selfContainedDir = Join-Path $ReleaseRoot 'selfcontained'
$frameworkDir = Join-Path $ReleaseRoot 'framework'

Invoke-Publish -Name 'self-contained' -OutDir $selfContainedDir -SelfContained $true
Invoke-Publish -Name 'framework-dependent (.NET 8 required)' -OutDir $frameworkDir -SelfContained $false

# 2) two zip packages ------------------------------------------------
New-ZipPackage -SourceDir $selfContainedDir -ZipPath (Join-Path $ReleaseRoot $packageNames['zip-selfcontained'])
New-ZipPackage -SourceDir $frameworkDir -ZipPath (Join-Path $ReleaseRoot $packageNames['zip-framework'])

# 3) two installers (no .NET runtime detection) ----------------------
if (-not (Test-Path $Iscc)) { throw "Inno Setup compiler not found: $Iscc" }
if (-not (Test-Path $InstallerRoot)) { New-Item -ItemType Directory -Path $InstallerRoot | Out-Null }

$iss = Join-Path $PSScriptRoot 'installer.iss'
Invoke-Iscc -Compiler $Iscc -Script $iss -SourceDir $selfContainedDir -Name $packageNames['setup-selfcontained'] -OutDir $InstallerRoot
Invoke-Iscc -Compiler $Iscc -Script $iss -SourceDir $frameworkDir -Name $packageNames['setup-framework'] -OutDir $InstallerRoot

# 4) summary ---------------------------------------------------------
Write-Host "`n== artifacts ==" -ForegroundColor Cyan
foreach ($zip in @($packageNames['zip-selfcontained'], $packageNames['zip-framework'])) {
    $path = Join-Path $ReleaseRoot $zip
    if (Test-Path $path) { Write-Host ("{0,-62} {1,8:N1} MB" -f $zip, ((Get-Item $path).Length / 1MB)) }
}

foreach ($setup in @($packageNames['setup-selfcontained'], $packageNames['setup-framework'])) {
    $path = Join-Path $InstallerRoot ($setup + '.exe')
    if (Test-Path $path) { Write-Host ("{0,-62} {1,8:N1} MB" -f ($setup + '.exe'), ((Get-Item $path).Length / 1MB)) }
}
