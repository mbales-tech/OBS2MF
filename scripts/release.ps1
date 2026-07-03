# OBS2MF release helper: build the installer for the current version and publish a
# GitHub Release with the installer attached as a downloadable asset.
#
# Prereqs (one-time):
#   - GitHub CLI installed and authenticated:  gh auth login
#   - Version already bumped in src/Vcam.Common/version.h AND installer/OBS2MF.nsi
#
# Usage (from anywhere):
#   pwsh -File scripts/release.ps1                # version read from version.h
#   pwsh -File scripts/release.ps1 -Version 0.9.2 # or force a version
#   pwsh -File scripts/release.ps1 -NoBuild       # skip rebuild, just publish existing installer

param(
    [string]$Version,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
$makensis = "C:\Program Files (x86)\NSIS\makensis.exe"

# derive version from version.h if not supplied
if (-not $Version) {
    $vh = Get-Content "$root\src\Vcam.Common\version.h" -Raw
    $maj = [regex]::Match($vh, 'OBS2MF_VER_MAJOR\s+(\d+)').Groups[1].Value
    $min = [regex]::Match($vh, 'OBS2MF_VER_MINOR\s+(\d+)').Groups[1].Value
    $pat = [regex]::Match($vh, 'OBS2MF_VER_PATCH\s+(\d+)').Groups[1].Value
    $Version = "$maj.$min.$pat"
}
Write-Host "Releasing OBS2MF $Version" -ForegroundColor Cyan

$installer = "$root\installer\OBS2MF-Setup-$Version.exe"

if (-not $NoBuild) {
    Write-Host "Building solution (Release x64)..."
    & $msbuild "$root\OBS2MF.sln" /t:Build /p:Configuration=Release /p:Platform=x64 /m /v:quiet /nologo
    if ($LASTEXITCODE -ne 0) { throw "solution build failed" }
    Write-Host "Building installer..."
    & $makensis "$root\installer\OBS2MF.nsi"
    if ($LASTEXITCODE -ne 0) { throw "installer build failed" }
}

if (-not (Test-Path $installer)) { throw "installer not found: $installer (did you bump the version in OBS2MF.nsi?)" }

# create the release (creates the tag at HEAD if it doesn't exist) and upload the installer
Write-Host "Publishing GitHub Release v$Version..."
gh release create "v$Version" "$installer" --title "OBS2MF $Version" --generate-notes
if ($LASTEXITCODE -ne 0) { throw "gh release create failed (is gh authenticated? run: gh auth login)" }

Write-Host "Done. Asset: OBS2MF-Setup-$Version.exe" -ForegroundColor Green
