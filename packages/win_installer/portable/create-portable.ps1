#!/usr/bin/env powershell

param (
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$BuildRoot,
    [Parameter(Position = 1, Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$SourceRoot,
    [Parameter(Position = 2)]
    [ValidateSet('x64', 'arm64')]
    [string]$Architecture = 'x64'
)

$ErrorActionPreference = 'Stop'

function Copy-ToDirectory {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Destination,
        [switch]$Recurse
    )
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Copy-Item -Path $Path -Destination $Destination -Recurse:$Recurse -Force
}

# Keep in sync with the number of Write-Step calls below.
$script:stepNum = 0
$script:stepTotal = 14

# Report progress both via an interactive bar and a textual trail for CI logs.
function Write-Step {
    param([Parameter(Mandatory)][string]$Status)
    $script:stepNum++
    Write-Progress -Activity 'Creating portable Aegisub' -Status $Status -PercentComplete (100 * $script:stepNum / $script:stepTotal)
    Write-Host "[$script:stepNum/$script:stepTotal] $Status"
}

Write-Host "BUILD_ROOT=$BuildRoot"
Write-Host "SOURCE_ROOT=$SourceRoot"
$InstallerDir = Join-Path $BuildRoot "install"
$InstallerDepsDir = Join-Path $BuildRoot "installer-deps"
$PortableOutputDir = Join-Path $BuildRoot "aegisub-portable"
New-Item -ItemType Directory -Path $InstallerDepsDir -Force | Out-Null
$GitVersionHeader = Join-Path $BuildRoot 'git_version.h'
$GitVersionMatch = Select-String -Path $GitVersionHeader -Pattern '^#define BUILD_GIT_VERSION_STRING "(.+)"$' | Select-Object -First 1
if (-not $GitVersionMatch) {
    throw "Could not read BUILD_GIT_VERSION_STRING from $GitVersionHeader"
}
$GitVersion = $GitVersionMatch.Matches[0].Groups[1].Value
$PortableZipPath = Join-Path $BuildRoot "Aegisub-$GitVersion-$Architecture-portable.zip"

Write-Step 'Removing previous output'
Remove-Item -LiteralPath $PortableOutputDir -Force -Recurse -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $InstallerDir -Force -Recurse -ErrorAction SilentlyContinue

Write-Step 'Installing build output'
meson install -C $BuildRoot --no-rebuild --destdir $InstallerDir
if ($LASTEXITCODE -ne 0) { throw "meson install failed (exit $LASTEXITCODE)" }

Write-Step 'Copying executable'
Copy-ToDirectory $InstallerDir\bin\aegisub.exe  $PortableOutputDir

Write-Step 'Copying translations'
$LocaleDir = Join-Path $InstallerDir "share\locale"
if (Test-Path -LiteralPath $LocaleDir) {
    Copy-ToDirectory "$LocaleDir\*" "$PortableOutputDir\locale" -Recurse
} else {
    Write-Host "No compiled translations were installed; portable build will use English."
}


Write-Step 'Preparing portable dependencies'

# Dictionaries
$DictionariesDir = Join-Path $InstallerDepsDir "dictionaries"
New-Item -ItemType Directory -Path $DictionariesDir -Force | Out-Null
$AffPath = Join-Path $DictionariesDir "en_US.aff"
$DicPath = Join-Path $DictionariesDir "en_US.dic"
if (!(Test-Path -LiteralPath $AffPath)) {
    try { Invoke-WebRequest "https://raw.githubusercontent.com/TypesettingTools/Aegisub-dictionaries/master/dicts/en_US.aff" -OutFile $AffPath -UseBasicParsing }
    catch { Write-Warning "Could not download en_US.aff; continuing without bundled dictionary." }
}
if (!(Test-Path -LiteralPath $DicPath)) {
    try { Invoke-WebRequest "https://raw.githubusercontent.com/TypesettingTools/Aegisub-dictionaries/master/dicts/en_US.dic" -OutFile $DicPath -UseBasicParsing }
    catch { Write-Warning "Could not download en_US.dic; continuing without bundled dictionary." }
}

# VC++ redistributable
$RedistDir = Join-Path $InstallerDepsDir "VC_redist"
$RedistPath = Join-Path $RedistDir ("VC_redist." + $Architecture + ".exe")
New-Item -ItemType Directory -Path $RedistDir -Force | Out-Null
if (!(Test-Path -LiteralPath $RedistPath)) {
    try { Invoke-WebRequest ("https://aka.ms/vs/17/release/VC_redist." + $Architecture + ".exe") -OutFile $RedistPath -UseBasicParsing }
    catch { Write-Warning "Could not download the VC++ runtime; continuing without bundling it." }
}

# YTSubConverter (MIT licensed)
$YTSubDir = Join-Path $InstallerDepsDir "YTSubConverter"
$YTSubExe = Join-Path $YTSubDir "YTSubConverter.exe"
$YTSubLicense = Join-Path $YTSubDir "LICENSE.txt"
New-Item -ItemType Directory -Path $YTSubDir -Force | Out-Null
if (!(Test-Path -LiteralPath $YTSubExe)) {
    try {
        Invoke-WebRequest "https://github.com/arcusmaximus/YTSubConverter/releases/download/1.6.6/YTSubConverter.exe" -OutFile $YTSubExe -UseBasicParsing
    }
    catch { Write-Warning "Could not download YTSubConverter; direct YTT export will be unavailable in this portable package." }
}
if (!(Test-Path -LiteralPath $YTSubLicense)) {
    try {
        Invoke-WebRequest "https://raw.githubusercontent.com/arcusmaximus/YTSubConverter/master/LICENSE" -OutFile $YTSubLicense -UseBasicParsing
    }
    catch { Write-Warning "Could not download the YTSubConverter license file." }
}

# DependencyControl
$DepCtrlDir = Join-Path $InstallerDepsDir "DependencyControl"
$DepCtrlMarker = Join-Path $DepCtrlDir "automation\include\l0\DependencyControl.moon"
if (!(Test-Path -LiteralPath $DepCtrlMarker)) {
    $DepCtrlZip = Join-Path $InstallerDepsDir "DependencyControl.zip"
    $DepCtrlStage = Join-Path $InstallerDepsDir "DependencyControl-stage"
    try {
        Remove-Item -LiteralPath $DepCtrlStage -Force -Recurse -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $DepCtrlZip -Force -ErrorAction SilentlyContinue
        Invoke-WebRequest "https://github.com/TypesettingTools/DependencyControl/releases/download/v0.8.1/DependencyControl-v0.8.1.zip" -OutFile $DepCtrlZip -UseBasicParsing
        Expand-Archive -LiteralPath $DepCtrlZip -DestinationPath $DepCtrlStage -Force
        Remove-Item -LiteralPath $DepCtrlDir -Force -Recurse -ErrorAction SilentlyContinue
        Rename-Item -LiteralPath $DepCtrlStage -NewName (Split-Path $DepCtrlDir -Leaf)
        Remove-Item -LiteralPath $DepCtrlZip -Force -ErrorAction SilentlyContinue
    }
    catch {
        Write-Warning "Could not prepare DependencyControl; continuing without it."
        Remove-Item -LiteralPath $DepCtrlStage -Force -Recurse -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $DepCtrlZip -Force -ErrorAction SilentlyContinue
    }
}

Write-Step 'Copying dictionaries'
if ((Test-Path -LiteralPath $AffPath) -and (Test-Path -LiteralPath $DicPath)) {
    Copy-ToDirectory $AffPath $PortableOutputDir\dictionaries
    Copy-ToDirectory $DicPath $PortableOutputDir\dictionaries
} else {
    Write-Host "Dictionary files unavailable; skipping bundled dictionary."
}

# Write-Step 'AviSynth'
# Copy-ToDirectory $InstallerDepsDir\AvisynthPlus64\x64\Output\system\DevIL.dll  $PortableOutputDir
# Copy-ToDirectory $InstallerDepsDir\AvisynthPlus64\x64\Output\AviSynth.dll  $PortableOutputDir
# Copy-ToDirectory $InstallerDepsDir\AvisynthPlus64\x64\Output\plugins\DirectShowSource.dll  $PortableOutputDir

Write-Step 'Copying VSFilter'
$VSFilterPath = Join-Path $InstallerDepsDir "VSFilter\x64\VSFilter.dll"
if ($Architecture -eq 'x64' -and (Test-Path -LiteralPath $VSFilterPath)) {
    Copy-ToDirectory $VSFilterPath $PortableOutputDir\csri
} else {
    Write-Host "VSFilter is not cached; skipping optional CSRI/VSFilter component."
}

Write-Step 'Copying VC++ runtime'
if (Test-Path -LiteralPath $RedistPath) {
    Copy-ToDirectory $RedistPath $PortableOutputDir\Microsoft.CRT
} else {
    Write-Host "VC++ redistributable unavailable; skipping bundled runtime installer."
}

Write-Step 'Copying automation'
Copy-ToDirectory "$InstallerDir\share\aegisub\automation\*"  "$PortableOutputDir\automation\"  -Recurse

Write-Step 'Copying DependencyControl'
$DepCtrlAutomation = Join-Path $DepCtrlDir "automation"
if (Test-Path -LiteralPath $DepCtrlAutomation) {
    Copy-ToDirectory "$DepCtrlAutomation\*" "$PortableOutputDir\automation\" -Recurse
} else {
    Write-Host "DependencyControl unavailable; skipping it."
}

Write-Step 'Copying portable config'
Copy-ToDirectory $SourceRoot\packages\win_installer\portable\config.json  $PortableOutputDir

Write-Step 'Copying YTSubConverter'
if (Test-Path -LiteralPath $YTSubExe) {
    Copy-ToDirectory $YTSubExe $PortableOutputDir
    if (Test-Path -LiteralPath $YTSubLicense) {
        Copy-ToDirectory $YTSubLicense (Join-Path $PortableOutputDir "licenses")
    }
} else {
    Write-Host "YTSubConverter unavailable; skipping bundled converter."
}

Write-Step 'Creating portable zip'
Remove-Item -LiteralPath $PortableZipPath -Force -ErrorAction SilentlyContinue

# Build the zip in a way that avoids some PowerShell versions emitting backslashes in the entry names.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipRoot = Split-Path $PortableOutputDir -Leaf
$baseLen = $PortableOutputDir.Length + 1
$zip = [System.IO.Compression.ZipFile]::Open($PortableZipPath, 'Create')
try {
    foreach ($file in Get-ChildItem -LiteralPath $PortableOutputDir -Recurse -File) {
        $entryName = "$zipRoot/" + $file.FullName.Substring($baseLen).Replace('\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $entryName, [System.IO.Compression.CompressionLevel]::Optimal)
    }
}
finally {
    $zip.Dispose()
}

Write-Progress -Activity 'Creating portable Aegisub' -Completed
Write-Host "Done: $PortableZipPath"
