# VBS Android Optics

Use an Android phone as a hand-held optic in VBS3. The phone's ARCore tracking aims the view, and the view is
streamed back to the phone's screen. It is one plugin DLL and one app, with no SQF, mission changes or `.pbo`.

## How it works

| | |
|---|---|
| `plugin/` | `VBSAndroidOptics.dll`, a VBS3 plugin (`plugins64`) |
| `android/` | The VBS Android Optics app (Kotlin, ARCore) |

While the app is connected, the plugin does the following:
- **Camera:** looks through its own camera at the player's eye, aimed by the phone.
  - **Heading** is relative to where the player faced when the phone connected. **Calibrate** re-aligns it to wherever the phone points.
  - **Pitch and roll** come from gravity.
- **Zoom:** sets the field of view picked on the phone (1×, NVG, 4×, 7× binos, 10×, 15×).
- **Video:** streams the VBS window to the phone from a background thread, using hardware H.264 on the GPU that shows VBS. The phone decodes it in low-latency mode.
- **Lag fix:** the phone turns each frame by how far it has rotated since VBS drew it, using its gyro. The **Lag fix** button compares with and without.
- **Hand-back:** when the phone stops sending for 3 s, the normal VBS view comes back.

The crosshair (and later any reticles) is drawn by the app.

## Connecting

| Link | Tracking | Video | How |
|---|---|---|---|
| Wi-Fi | UDP 47830 | UDP 47831 | Type the VBS PC's IP address into the app |
| USB | TCP 47830 via `adb reverse` | not yet | Leave the address as `usb` and run `android\install.ps1` |

VBS (`VBS3_64.exe`) needs inbound UDP allowed through Windows Firewall. The VBS installer normally adds this rule.

To debug from the VBS debug console, run `(pluginFunction ["VBSAndroidOptics", "status"]) select 0`. It shows the view, the phone link, tracking, angles, field of view and video stats.

## Build and install

Close VBS first, because it locks plugin DLLs.

```powershell
.\build.ps1                 # plugin (VS 2022) and app (Android SDK, JDK 17)
.\deploy.ps1                # VBSAndroidOptics.dll -> D:\VBS3\plugins64
android\install.ps1         # app -> the USB-connected phone
```

Hold the phone like a camera: landscape, with the screen towards you.
- ARCore needs to see some movement against a detailed, well-lit scene before it starts tracking, so move the phone side to side at first.
- Until it tracks, the view holds level and ahead.

## Tests

`plugin/orientation_test.cpp` checks the pose maths:

```powershell
cl /EHsc plugin\orientation_test.cpp
```
