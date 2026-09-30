# Builds the plugin, the DAGR app and the phone app into build\ and android\app\build\.
#   .\build.ps1                    all three
#   .\build.ps1 -Parts plugin      just VBSAndroidOptics.dll (Visual Studio 2022)
#   .\build.ps1 -Parts dagr        just the DAGR app -> build\dagr\dagr.exe + .pck (Godot 4.7.2 + export templates)
#   .\build.ps1 -Parts android     just the phone app (Android SDK + JDK 17)
param(
    [ValidateSet("plugin", "dagr", "android")][string[]]$Parts = @("plugin", "dagr", "android"),
    [ValidateSet("Release", "Debug")][string]$Config = "Release",
    [string]$Godot = "D:\Godot\Godot_v4.7.2-stable_win64_console.exe"
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

if ($Parts -contains "dagr") {
    Write-Host "`n== dagr (Godot export)" -ForegroundColor Cyan
    $out = Join-Path $root "build\dagr"
    New-Item -ItemType Directory -Force $out | Out-Null
    Remove-Item (Join-Path $out "dagr.*") -ErrorAction SilentlyContinue
    & $Godot --headless --path (Join-Path $root "dagr") --export-release "Windows Desktop" (Join-Path $out "dagr.exe") 2>&1 |
        Where-Object { $_ -match "ERROR|export template" } | ForEach-Object { Write-Host $_ }
    if (-not (Test-Path (Join-Path $out "dagr.pck"))) { $failed += "dagr" }
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
