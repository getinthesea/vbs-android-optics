# VBS Android Optics

Use an Android phone as a hand-held optic in VBS3. The phone's orientation aims the view, and the view is
streamed back to the phone's screen. It is one plugin DLL and one app, with no SQF, mission changes or `.pbo`.

## How it works

| | |
|---|---|
| `plugin/` | `VBSAndroidOptics.dll`, a VBS3 plugin (`plugins64`) |
| `android/` | The VBS Android Optics app (Kotlin) |
| `dagr/` | The DAGR app (Godot 4.7), `plugins64\vbs-android-optics\dagr.exe` |

While the app is connected, the plugin does the following:
- **Camera:** looks through its own camera at the player's eye, aimed by the phone.
  - **Heading** is relative to where the player faced when the phone connected. **Calibrate** re-aligns it to wherever the phone points.
  - **Pitch and roll** come from gravity.
- **Zoom:** sets the field of view picked on the phone (1× to 5× in steps of 0.5×; 1× is 60° across).
- **Video:** streams the VBS window to the phone from a background thread, using hardware H.264 on the GPU that shows VBS. The phone decodes it in low-latency mode.
- **Hand-back:** when the phone stops sending for 3 s, the normal VBS view comes back.

The crosshair (and later any reticles) is drawn by the app.

The view looks from the observer: `var_ig` if the scenario sets it (JFSim's OP, where JFSim's binos also look
from), else a unit named `igl`, `ig2` or `projector`, else the player.

## Rangefinder (Vector)

The **Bearing** and **Range** buttons in the phone's bottom corners work like the Vector's two buttons (JFSim's
`vector.sqf`). The readouts show in red below the centre of the view.

| Press | Shows |
|---|---|
| Bearing | Grid bearing in mils, to the 5 below |
| Bearing, again within 3 s | Bearing, and the vertical angle in mils. Locked until the display clears |
| Range | Range in metres. After Bearing, Range is ignored until the display clears |
| Range, again within 3 s | Range, and the difference in altitude (target minus observer, m). Locked until it clears |
| Both together | Bearing and range. JFSim's DAGR and this project's DAGR show the target (LRF TGT MODE) |

The display clears 4 s after the last press. Ranges beyond 8000 m or under 5 m read `- - - -`. The range is to
the ground at the centre of the view; buildings and trees are not seen, as with `vector.sqf`. Each lase is
published as `vector_aim_point` for JFSim's instructor app, and JFSim's LRF-disable switch is honoured.

### Physical buttons

Any USB or Bluetooth keyboard works the rangefinder: **F9** is Bearing and **F10** is Range, held while the key
is down. `pico/` turns a Raspberry Pi Pico W into one, for a mount with real buttons. It is a keyboard over USB
and Bluetooth at once:

- **USB:** plug it into the phone with a USB-C to micro-USB OTG data cable; the phone powers it.
- **Bluetooth:** power it from a battery (VSYS pin 39, GND pin 38, 1.8-5.5 V), and pair **VBS Buttons** once in
  the phone's Bluetooth settings. It reconnects by itself after that.

Wire Bearing between GP3 (pin 5) and GND (pin 3), Range between GP5 (pin 7) and GND (pin 8), and Calibrate
between GP7 (pin 10) and GND (pin 8 or 13). Calibrate types **F8**, which the app treats as its Calibrate button.
The LED lights while a button is held, and blinks every 2 s while nothing is connected. Hold Bearing while powering it up to
forget paired phones, so another can pair.

The firmware is an Arduino sketch (`pico\vbs_buttons`) for Earle Philhower's Pico core. `pico\build.ps1 -Flash`
builds it with arduino-cli and flashes it: to a Pico held in BOOTSEL as it is plugged in, or to one already running
it, on USB. Flashing resets its Bluetooth pairing, so unpair **VBS Buttons** on the phone and pair it again.

## DAGR

`dagr.exe` shows a DAGR for the same observer, whether or not a phone is connected. The plugin reads VBS four
times a second, and the app asks it over UDP 47840: this PC first, else a broadcast on the network.

The plugin starts `dagr.exe` on the PC the phone is connected to (the one showing the binos), once a mission is
running and a phone is connected. It does this once per mission, and not if the DAGR is already open, so closing
it keeps it closed until the next mission. It can also be started by hand on any PC.

| Field | From |
|---|---|
| MGRS | The observer's 10-figure grid |
| Date | Today, from the PC clock (DD-MM-YY) |
| Time | Scenario time (`daytime`) |
| Elevation | Terrain height at the grid (`getTerrainHeightASL`) |
| Accuracy | Always +/- 7m |
| Battery Locations | The first 5 of the units `G1`-`G9` that exist, as 6-figure grids. With none, the heading is hidden too |
| Compass MV | `getDeclination`, in mils |

The figures match JFSim's own DAGR (`dagr.sqf`). With no VBS or no mission running, the app shows dashes.
After a lase with both rangefinder buttons it shows the target, as JFSim's DAGR does (grid, Dn, Dist and DifAlt,
observer minus target), until the observer moves.
To see what the app is being sent, run `(pluginFunction ["VBSAndroidOptics", "dagr"]) select 0` in the debug console.

## Connecting

| Link | Tracking | Video | How |
|---|---|---|---|
| Wi-Fi, found automatically | UDP 47830 | UDP 47831 | Leave the address as `auto` (the default). With more than one VBS PC on the network, the app asks which one |
| Wi-Fi, fixed address | UDP 47830 | UDP 47831 | Type the VBS PC's IP address into the app |
| USB | TCP 47830 via `adb reverse` | not yet | Leave the address as `usb` and run `android\install.ps1` |

VBS (`VBS3_64.exe`) needs inbound UDP allowed through Windows Firewall. The VBS installer normally adds this rule.

To debug from the VBS debug console, run `(pluginFunction ["VBSAndroidOptics", "status"]) select 0`. It shows the view, the phone link, tracking, angles, field of view and video stats.

## Build and install

Close VBS and the DAGR app first, because they lock their files.

```powershell
.\build.ps1                 # plugin (VS 2022), DAGR app (Godot 4.7.2 + export templates), phone app (Android SDK, JDK 17)
.\deploy.ps1                # VBSAndroidOptics.dll -> D:\VBS3\plugins64, dagr.exe + .pck -> plugins64\vbs-android-optics
android\install.ps1         # phone app -> the USB-connected phone
```

Hold the phone like a camera: landscape, with the screen towards you.

### Calibrating

1. Press **Calibrate** (on screen, the Pico's Calibrate button, or a tap on the picture in Cardboard). The phone
   shows its camera with a white + in the middle, and the Calibrate icon turns red.
2. With JFSim, its IG screens show a red + on the horizon where the IG faces (JFSim's `ig.sqf`, from
   `vao_calibrating`, which the plugin sets on every machine). Line the phone's + up on it. Without an IG, face the
   way you want as forward.
3. Press **Bearing** or **Range**: that calibrates, and the phone goes back to the VBS picture. The press is not
   used as a rangefinder press.

Press **Calibrate** again before Bearing or Range to go back without calibrating.

For Google Cardboard, set **View: Cardboard** in Settings (remembered). The video, crosshair and rounded mask are shown once per eye, side by side. The same picture goes to both eyes, and there is no lens-distortion correction. With the phone in the viewer, pressing the viewer's button (any tap on the picture) calibrates.

## Tracking

The app has two tracking sources, switched with the **Gyro** / **ARCore** button (remembered):

| | Gyro | ARCore (default) |
|---|---|---|
| Uses | Android's game rotation vector (gyro + accelerometer) | The camera plus motion sensors |
| Rate | ~100 poses/s | Camera rate (30/s on a Galaxy A54) |
| Start-up | Instant | Needs sideways movement against a detailed, well-lit scene |
| Heading | Drifts slowly; re-zero with **Calibrate** | Holds once tracking |
| Dark (dome) | Works | Needs light |

Pitch and roll come from gravity in both. With ARCore, until it tracks the view holds level and ahead.

ARCore reads the gyro and accelerometer at 200 Hz, which Android only allows apps that declare
`HIGH_SAMPLING_RATE_SENSORS`. Without it, ARCore's sensor requests fail quietly, it gets about 6 samples a
second, and it cannot track (its log fills with "IMU buffer is empty"). The app declares it.

## Tests

`plugin/orientation_test.cpp` checks the pose maths:

```powershell
cl /EHsc plugin\orientation_test.cpp
```
