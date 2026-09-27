# Builds the plugin and the Android app into build\ and android\app\build\.
#   .\build.ps1                    both
#   .\build.ps1 -Parts plugin      just VBSAndroidOptics.dll (Visual Studio 2022)
#   .\build.ps1 -Parts android     just the app (Android SDK + JDK 17)
param(
    [ValidateSet("plugin", "android")][string[]]$Parts = @("plugin", "android"),
    [ValidateSet("Release", "Debug")][string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$failed = @()

if ($Parts -contains "plugin") {
    Write-Host "`n== plugin ($Config)" -ForegroundColor Cyan
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
    & $msbuild (Join-Path $root "plugin\VBSAndroidOptics.sln") /p:Configuration=$Config /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { $failed += "plugin" }
}

if ($Parts -contains "android") {
    Write-Host "`n== android" -ForegroundColor Cyan
    Push-Location (Join-Path $root "android")
    .\gradlew.bat assembleDebug --console=plain -q
    if ($LASTEXITCODE -ne 0) { $failed += "android" }
    Pop-Location
}

if ($failed) { Write-Host "`nFAILED: $($failed -join ', ')" -ForegroundColor Red; exit 1 }
Write-Host "`nBuilt: $($Parts -join ', ')" -ForegroundColor Green
