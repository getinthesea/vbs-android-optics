// VBSAndroidOptics.dll - use an Android phone (ARCore) as a hand-held optic in VBS3.
//
// While the VBS Android Optics app is connected, this plugin:
//   - looks through its own camera at the anchor's eye, aimed by the phone (heading relative to where the
//     anchor faced when the phone connected or Calibrate was pressed; pitch and roll from gravity). The anchor
//     is var_ig if the scenario sets it (e.g. JFSim's observation post), else a unit named igl, ig2 or
//     projector, else the player (anchor.h),
//   - hides VBS's HUD (weapon status, crosshair, action menu) and restores it afterwards,
//   - sets the field of view the phone asks for (its zoom presets),
//   - streams the VBS window to the phone,
//   - works the phone's Bearing and Range buttons like the Vector's laser rangefinder (lrf.cpp),
//   - draws the phone's graticule over VBS's window on the PC too (overlay.cpp),
//   - sets the binos' view mode from the DAGR app's menu: day, night vision (JFSim's NVG settings), thermal black
//     or white hot, or thermal fusion,
//   - while the phone is in calibration mode, sets vao_calibrating on every machine, for JFSim's IGs to show a +
//     where the IG faces (its ig.sqf), for the phone's crosshair to be lined up on.
// When the phone stops sending for a few seconds, the normal view is handed back. No SQF or .pbo is needed.
// It also feeds the DAGR app (dagr/, installed in plugins64\vbs-android-optics) whether or not a phone is connected,
// and starts it once per mission on the PC the phone is connected to.
//
//   pluginFunction ["VBSAndroidOptics", "status"]  -> ["one line report", with the plugin's time per frame]
//   pluginFunction ["VBSAndroidOptics", "dagr"]    -> ["what the DAGR app is being sent"]
#include <winsock2.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "vbs_plugin.h"
#include "anchor.h"
#include "dagr.h"
#include "lrf.h"
#include "overlay.h"
#include "phone_link.h"
#include "streamer.h"

namespace {

ExecuteCommandType ExecuteCommand = nullptr;

// Only touched on the VBS thread (OnSimulationStep / PluginFunction)
bool     active = false;          // our camera is in use
uint8_t  last_calibrate = 0;
double   heading_zero = 0;        // phone heading that means "where the player faced"
bool     aligned = false;         // heading_zero came from a tracked pose
float    fov_deg = 0;             // field of view we have set (0 = VBS default)
uint32_t steps = 0;

const float DEFAULT_FOV_DEG = 60.0f;   // if the phone does not ask for one
const double LOST_AFTER_MS = 3000;     // phone silent this long: hand the view back

// Time the plugin spends on VBS's main thread each step (once per frame), by part, reported by "status": the
// average and the worst single step over the last second, and steps a second (VBS's frame rate)
double now_ms()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / freq.QuadPart;
}

struct StepTiming {
    double camera = 0, lrf = 0, dagr = 0;          // this step, ms
    double sum_total = 0, sum_camera = 0, sum_lrf = 0, sum_dagr = 0, worst = 0;
    int steps = 0;
    double window_start = 0;
    std::string text = "not measured yet";

    template <typename F> void time(double& part, F work)
    {
        double t = now_ms();
        work();
        part += now_ms() - t;
    }

    void end_step(double total)
    {
        sum_total += total;
        sum_camera += camera;
        sum_lrf += lrf;
        sum_dagr += dagr;
        worst = (std::max)(worst, total);
        steps++;
        camera = lrf = dagr = 0;
        double now = now_ms();
        if (window_start == 0) window_start = now;
        if (now - window_start < 1000) return;
        double seconds = (now - window_start) / 1000;
        char buf[256];
        sprintf_s(buf, "plugin %.2f ms/frame (worst %.2f): camera %.2f, rangefinder %.2f, DAGR %.2f, rest %.2f | "
            "%.0f frames/s",
            sum_total / steps, worst, sum_camera / steps, sum_lrf / steps, sum_dagr / steps,
            (sum_total - sum_camera - sum_lrf - sum_dagr) / steps, steps / seconds);
        text = buf;
        sum_total = sum_camera = sum_lrf = sum_dagr = worst = 0;
        steps = 0;
        window_start = now;
    }
} timing;

// Ends the step's timing however OnSimulationStep returns
struct StepClock {
    double start = now_ms();
    ~StepClock() { timing.end_step(now_ms() - start); }
};

void run(const char* sqf)
{
    if (ExecuteCommand) ExecuteCommand(sqf, nullptr, 0);
}

// Our camera at the anchor's eye (vao_* variables are ours). Created whenever it is missing, not once: the phone
// can connect before a mission has loaded, and a mission restart destroys the camera.
const char* ENSURE_CAMERA_SQF =
    "if (isNil \"vao_cam\") then { vao_cam = objNull; }; "
    "if (isNull vao_cam) then { "
    "vao_base = getDir vao_anchor; "
    "vao_ui_was_hidden = isUIHidden; "
    "vao_cam = \"camera\" camCreate (vao_anchor modelToWorld [0, 0.2, 1.7]); "
    "showCinemaBorder false; "   // before cameraEffect, or VBS letterboxes the view
    "vao_cam cameraEffect [\"internal\", \"BACK\"]; "
    "}; ";

// Tells every machine whether the phone here is calibrating (only when it changes)
void broadcast_calibrating(bool on)
{
    static bool sent = false;
    if (on == sent) return;
    sent = on;
    run(on ? "vao_calibrating = true; publicVariable \"vao_calibrating\";"
           : "vao_calibrating = false; publicVariable \"vao_calibrating\";");
}

// The binos' view mode, by the DAGR menu's number (dagr.h). Night vision uses JFSim's NVG settings (init.sqf's
// func_set_nvg: its noise and contrast, white phosphor), as JFSim's own NVG optic does
const char* VIEW_MODE_SQF[] = {
    "setTIMode -1; setCamSensorOverride [true, \"visible\"];",
    "setSensorNoise [\"nvg\", true, (if (isNil \"global_nvg_noise\") then { 0.01 } else { global_nvg_noise }), 0, 3, 1.1, 1.1, true]; "
    "setNVGContrast (if (isNil \"global_nvg_contrast\") then { 0.01 } else { global_nvg_contrast }); setNVGMode 1; "
    "setTIMode -1; setCamSensorOverride [true, \"nightvision\"];",
    "setTIMode 1; setCamSensorOverride [true, \"thermal\"];",
    "setTIMode 0; setCamSensorOverride [true, \"thermal\"];",
    "setTIMode -1; setCamSensorOverride [true, \"nvgti\"];",
};
int applied_view_mode = -1; // -1: VBS's own (not ours)

void release_view()
{
    applied_view_mode = -1;
    run("setCamSensorOverride [false, \"\"]; setTIMode -1; "
        "setCamFrustumOffsets [false]; setCamFrustum [false]; "
        "if (!isNil \"vao_cam\") then { vao_cam cameraEffect [\"terminate\", \"BACK\"]; camDestroy vao_cam; vao_cam = nil; }; "
        "showHUD true; if (!isNil \"vao_ui_was_hidden\") then { hideUI vao_ui_was_hidden; } else { hideUI false; };");
}

} // namespace

VBS_PLUGIN_EXPORT void WINAPI RegisterCommandFnc(void* executeCommandFnc)
{
    ExecuteCommand = (ExecuteCommandType)executeCommandFnc;
}

VBS_PLUGIN_EXPORT void WINAPI OnSimulationStep(float)
{
    if (!ExecuteCommand) return;
    StepClock clock;
    phone_link_start();
    streamer_start();
    dagr_start();
    steps++;

    PhoneState phone = phone_link_read();
    bool connected = phone.age_ms >= 0 && phone.age_ms < LOST_AFTER_MS;
    timing.time(timing.dagr, [&] { dagr_update((void*)ExecuteCommand, connected); }); // the binos' PC shows the DAGR

    if (!connected) {
        if (active) {
            release_view();
            active = false;
        }
        streamer_update(0);
        timing.time(timing.lrf, [&] { lrf_step(ExecuteCommand, false, 0, 0, 0); });
        overlay_update(false, 0);
        broadcast_calibrating(false);
        return;
    }

    fov_deg = phone.fov_deg > 0 ? phone.fov_deg : DEFAULT_FOV_DEG;
    if (!active) {
        active = true;
        aligned = false;
        heading_zero = phone.heading;
        last_calibrate = phone.calibrate;
    }

    // Line the phone up with where the anchor faces: on the first tracked pose, and on Calibrate
    const char* realign = "";
    if ((phone.tracking && !aligned) || phone.calibrate != last_calibrate) {
        heading_zero = phone.heading;
        aligned = phone.tracking;
        last_calibrate = phone.calibrate;
        realign = "vao_base = getDir vao_anchor; ";
    }

    // The frustum's height must match the window's shape, or VBS keeps some other aspect and draws black bars.
    // Checked every second or so in case the window is resized.
    static double aspect = 0;
    if (aspect <= 0 || steps % 60 == 0) {
        double a = vbs_window_aspect();
        if (a > 0) aspect = a;
    }
    double tan_half_h = tan(fov_deg * 3.14159265 / 360.0);
    double tan_half_v = tan_half_h / (aspect > 0 ? aspect : 16.0 / 9.0);

    // Every step, once a mission is running: pick the anchor (the scenario may set var_ig later), make sure our
    // camera exists, keep the HUD hidden and the zoom set, follow the anchor and aim with the phone
    // (held at the last tracked direction while ARCore is not tracking)
    char cmd[2048];
    sprintf_s(cmd,
        "if (!isNull player) then { "
        "%s%s%s"
        "hideUI true; showHUD false; showCinemaBorder false; "
        "setCamFrustum [true, %f, %f]; "
        "vao_cam camSetPos (vao_anchor modelToWorld [0, 0.2, 1.7]); "
        "vao_cam camSetFocus [-1, -1]; "   // no depth-of-field blur: everything in focus, like the eye
        "vao_cam camCommit 0; "
        "setCamFrustumOffsets [true, vao_base + %.3f, %.3f, %.3f]; "
        "};",
        ANCHOR_SQF, ENSURE_CAMERA_SQF, realign,
        tan_half_h, tan_half_v, phone.heading - heading_zero, phone.pitch, phone.roll);
    timing.time(timing.camera, [&] { run(cmd); });
    // The DAGR menu's view mode: when it changes, and every few seconds in case a mission restart reset it
    int mode = dagr_view_mode();
    if (mode >= 0 && mode < 5 && (mode != applied_view_mode || steps % 300 == 0)) {
        run(VIEW_MODE_SQF[mode]);
        applied_view_mode = mode;
    }
    timing.time(timing.lrf, [&] { lrf_step(ExecuteCommand, true, phone.buttons, phone.heading - heading_zero, phone.pitch); });
    overlay_update(true, fov_deg);
    broadcast_calibrating(phone.calibrating); // JFSim's IGs show a + where they face

    streamer_update(phone.ip);
}

VBS_PLUGIN_EXPORT const char* WINAPI PluginFunction(const char* input)
{
    static std::string reply;
    std::string text;
    if (input && std::string(input) == "status") {
        PhoneState p = phone_link_read();
        char buf[1024];
        sprintf_s(buf, "view=%s phone: link=%s packets=%u last=%.0fms tracking=%s h=%.1f p=%.1f r=%.1f fov=%.1f | %s | video: %s",
            active ? "phone" : "normal", p.link.empty() ? "none" : p.link.c_str(), p.packets, p.age_ms,
            p.tracking ? "yes" : "no", p.heading - heading_zero, p.pitch, p.roll, fov_deg, timing.text.c_str(),
            streamer_status().c_str());
        text = buf;
    }
    else if (input && std::string(input) == "dagr") {
        text = dagr_status();
    }
    else {
        text = "error: the commands are \"status\" and \"dagr\"";
    }
    // VBS evaluates the reply as SQF and pluginFunction returns an array, so reply ["..."]
    reply = "[\"";
    for (char ch : text) reply += ch == '"' ? std::string("\"\"") : std::string(1, ch);
    reply += "\"]";
    return reply.c_str();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH) {
        streamer_stop();
        phone_link_stop();
        dagr_stop();
        overlay_stop();
    }
    return TRUE;
}
