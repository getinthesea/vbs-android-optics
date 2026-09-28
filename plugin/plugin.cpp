// VBSAndroidOptics.dll - use an Android phone (ARCore) as a hand-held optic in VBS3.
//
// While the VBS Android Optics app is connected, this plugin:
//   - looks through its own camera at the anchor's eye, aimed by the phone (heading relative to where the
//     anchor faced when the phone connected or Calibrate was pressed; pitch and roll from gravity). The anchor
//     is var_ig if the scenario sets it (e.g. JFSim's observation post), else a unit named ig2 or projector,
//     else the player,
//   - hides VBS's HUD (weapon status, crosshair, action menu) and restores it afterwards,
//   - sets the field of view the phone asks for (its zoom presets),
//   - streams the VBS window to the phone.
// When the phone stops sending for a few seconds, the normal view is handed back. No SQF or .pbo is needed.
//
//   pluginFunction ["VBSAndroidOptics", "status"]  -> ["one line report"]
#include <winsock2.h>
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <string>
#include "vbs_plugin.h"
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

void run(const char* sqf)
{
    if (ExecuteCommand) ExecuteCommand(sqf, nullptr, 0);
}

// Picks the anchor for the view: var_ig (the scenario's observation post), ig2, projector, else the player
const char* ANCHOR_SQF =
    "vao_anchor = player; "
    "if (!isNil \"projector\") then { if (!isNull projector) then { vao_anchor = projector; }; }; "
    "if (!isNil \"ig2\") then { if (!isNull ig2) then { vao_anchor = ig2; }; }; "
    "if (!isNil \"var_ig\") then { if (!isNull var_ig) then { vao_anchor = var_ig; }; }; ";

// Our camera at the anchor's eye (vao_* variables are ours). Created whenever it is missing, not once: the phone
// can connect before a mission has loaded, and a mission restart destroys the camera.
const char* ENSURE_CAMERA_SQF =
    "if (isNil \"vao_cam\") then { vao_cam = objNull; }; "
    "if (isNull vao_cam) then { "
    "vao_base = getDir vao_anchor; "
    "vao_ui_was_hidden = isUIHidden; "
    "vao_cam = \"camera\" camCreate (vao_anchor modelToWorld [0, 0.2, 1.7]); "
    "vao_cam cameraEffect [\"internal\", \"BACK\"]; "
    "showCinemaBorder false; "
    "}; ";

void release_view()
{
    run("setCamFrustumOffsets [false]; setCamFrustum [false]; "
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
    phone_link_start();
    streamer_start();
    steps++;

    PhoneState phone = phone_link_read();
    bool connected = phone.age_ms >= 0 && phone.age_ms < LOST_AFTER_MS;

    if (!connected) {
        if (active) {
            release_view();
            active = false;
        }
        streamer_update(0);
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

    // Every step, once a mission is running: pick the anchor (the scenario may set var_ig later), make sure our
    // camera exists, keep the HUD hidden and the zoom set, follow the anchor and aim with the phone
    // (held at the last tracked direction while ARCore is not tracking)
    char cmd[2048];
    sprintf_s(cmd,
        "if (!isNull player) then { "
        "%s%s%s"
        "hideUI true; showHUD false; "
        "setCamFrustum [true, %f]; "
        "vao_cam camSetPos (vao_anchor modelToWorld [0, 0.2, 1.7]); vao_cam camCommit 0; "
        "setCamFrustumOffsets [true, vao_base + %.3f, %.3f, %.3f]; "
        "};",
        ANCHOR_SQF, ENSURE_CAMERA_SQF, realign,
        tan(fov_deg * 3.14159265 / 360.0), phone.heading - heading_zero, phone.pitch, phone.roll);
    run(cmd);

    streamer_update(phone.ip);
}

VBS_PLUGIN_EXPORT const char* WINAPI PluginFunction(const char* input)
{
    static std::string reply;
    std::string text;
    if (input && std::string(input) == "status") {
        PhoneState p = phone_link_read();
        char buf[512];
        sprintf_s(buf, "view=%s phone: link=%s packets=%u last=%.0fms tracking=%s h=%.1f p=%.1f r=%.1f fov=%.1f | video: %s",
            active ? "phone" : "normal", p.link.empty() ? "none" : p.link.c_str(), p.packets, p.age_ms,
            p.tracking ? "yes" : "no", p.heading - heading_zero, p.pitch, p.roll, fov_deg, streamer_status().c_str());
        text = buf;
    }
    else {
        text = "error: the only command is \"status\"";
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
    }
    return TRUE;
}
