# Builds VBS Android Optics, installs it on the USB-connected phone, and starts it.
#   .\install.ps1            build, install, start
#   .\install.ps1 -NoBuild   just reinstall the last build and start
# Also sets up the USB pose link (adb reverse). Re-run after re-plugging the phone. Video needs Wi-Fi.
param([switch]$NoBuild)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if (-not $NoBuild) {
    .\gradlew.bat assembleDebug --console=plain -q
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
}
adb install -r app\build\outputs\apk\debug\app-debug.apk
adb shell pm grant nz.vbs.androidoptics android.permission.CAMERA
adb reverse tcp:47830 tcp:47830 | Out-Null     # phone 127.0.0.1:47830 -> this PC's VBSAndroidOptics.dll
adb shell am start -n nz.vbs.androidoptics/.MainActivity | Out-Null
Write-Host "VBS Android Optics started. Type the VBS PC's IP address in the app for Wi-Fi (needed for video), or 'usb' for tracking only."
