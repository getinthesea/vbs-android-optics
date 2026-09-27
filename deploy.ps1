# Installs VBSAndroidOptics.dll into a VBS3 install (close VBS first; it locks plugins).
#   .\deploy.ps1                            -> D:\VBS3\plugins64
#   .\deploy.ps1 -VBS \\optic-pc\d$\VBS3    another PC's install
# The phone app is installed with android\install.ps1 (phone on USB).
param(
    [string]$VBS = "D:\VBS3",
    [ValidateSet("Release", "Debug")][string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$dll = Join-Path $PSScriptRoot "build\$Config\VBSAndroidOptics.dll"
$plugins = Join-Path $VBS "plugins64"
if (-not (Test-Path $dll)) { throw "Missing $dll - run .\build.ps1 first" }
if (-not (Test-Path $plugins)) { throw "No plugins64 folder at $plugins" }
Copy-Item $dll $plugins
Write-Host "VBSAndroidOptics.dll -> $plugins"
