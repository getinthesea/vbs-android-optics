# Installs into a VBS3 install (close VBS and the DAGR app first; they lock their files):
#   VBSAndroidOptics.dll          -> plugins64
#   the DAGR app (dagr.exe, .pck) -> plugins64\vbs-android-optics
#   .\deploy.ps1                            -> D:\VBS3
#   .\deploy.ps1 -VBS \\optic-pc\d$\VBS3    another PC's install
# The phone app is installed with android\install.ps1 (phone on USB).
param(
    [string]$VBS = "D:\VBS3",
    [ValidateSet("Release", "Debug")][string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$dll = Join-Path $PSScriptRoot "build\$Config\VBSAndroidOptics.dll"
$dagr = Join-Path $PSScriptRoot "build\dagr"
$plugins = Join-Path $VBS "plugins64"
if (-not (Test-Path $dll)) { throw "Missing $dll - run .\build.ps1 first" }
if (-not (Test-Path (Join-Path $dagr "dagr.pck"))) { throw "Missing the DAGR app in $dagr - run .\build.ps1 first" }
if (-not (Test-Path $plugins)) { throw "No plugins64 folder at $plugins" }
Copy-Item $dll $plugins
Write-Host "VBSAndroidOptics.dll -> $plugins"

$app = Join-Path $plugins "vbs-android-optics"
New-Item -ItemType Directory -Force $app | Out-Null
Copy-Item (Join-Path $dagr "dagr.*") $app
Write-Host "DAGR app -> $app"
