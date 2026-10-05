# Builds the Pico W button firmware (vbs_buttons, Arduino) into pico\build, and with -Flash puts it on the Pico.
#   .\build.ps1            build
#   .\build.ps1 -Flash     build and flash: to a Pico in BOOTSEL mode (the RPI-RP2 drive), else to one already
#                          running this firmware on USB (it is restarted into BOOTSEL by itself)
# Needs arduino-cli with Earle Philhower's core:
#   arduino-cli config set board_manager.additional_urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
#   arduino-cli core install rp2040:rp2040
param(
    [switch]$Flash,
    [string]$Cli = "$env:LOCALAPPDATA\Programs\arduino-cli\arduino-cli.exe"
)
$ErrorActionPreference = "Stop"
$fqbn = "rp2040:rp2040:rpipicow:ipbtstack=ipv4btcble" # Pico W, Bluetooth on
$out = Join-Path $PSScriptRoot "build"

& $Cli compile --fqbn $fqbn --output-dir $out (Join-Path $PSScriptRoot "vbs_buttons")
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
if (-not $Flash) { return }

$uf2 = Join-Path $out "vbs_buttons.ino.uf2"
$boot = Get-Volume | Where-Object { $_.FileSystemLabel -eq "RPI-RP2" } | Select-Object -First 1
if ($boot) {
    Copy-Item $uf2 "$($boot.DriveLetter):\"
    Write-Host "Flashed to the Pico on $($boot.DriveLetter): - it restarts by itself"
    return
}
$port = Get-PnpDevice -PresentOnly -Class Ports | Where-Object { $_.InstanceId -match "VID_2E8A" } | Select-Object -First 1
if (-not $port) { throw "No Pico found: hold BOOTSEL while plugging it in, then run this again" }
$com = $port.FriendlyName -replace ".*\((COM\d+)\).*", '$1'
& $Cli upload --fqbn $fqbn -p $com --input-dir $out
if ($LASTEXITCODE -ne 0) { throw "Flash failed: hold BOOTSEL while plugging it in, then run this again" }
