// VBSAndroidOptics.dll - use an Android phone (ARCore) as a hand-held optic in VBS3.
//
// While the VBS Android Optics app is connected, this plugin:
//   - looks through its own camera at the player's eye, aimed by the phone (heading relative to where the
//     player faced when the phone connected or Calibrate was pressed; pitch and roll from gravity),
//   - sets the field of view the phone asks for (its zoom presets),
//   - streams the VBS window to the phone, which corrects for lag with its gyro.
// When the phone stops sending for a few seconds, the normal view is handed back. No SQF or .pbo is needed.
//
//   pluginFunction ["VBSAndroidOptics", "status"]  -> ["one line report"]
#include <winsock2.h>
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
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

// Our camera at the player's eye. vao_* variables are ours; nothing else in VBS needs to know about them.
void take_view(float fov)
{
    char cmd[512];
    sprintf_s(cmd,
        "vao_base = getDir player; "
        "vao_cam = \"camera\" camCreate (player modelToWorld [0, 0.2, 1.7]); "
        "vao_cam cameraEffect [\"internal\", \"BACK\"]; "
        "showCinemaBorder false; "
        "setCamFrustum [true, %f]; "
        "vao_cam camCommit 0;",
        tan(fov * 3.14159265 / 360.0));
    run(cmd);
}

void release_view()
{
    run("setCamFrustumOffsets [false]; setCamFrustum [false]; "
        "if (!isNil \"vao_cam\") then { vao_cam cameraEffect [\"terminate\", \"BACK\"]; camDestroy vao_cam; vao_cam = nil; };");
}

// When each pose was applied, so a captured frame can be matched to the pose VBS actually drew it with
// (read by the streamer thread). Times are QPC in 100 ns units, the same clock as Windows Graphics Capture.
std::mutex applied_mutex;
struct Applied { long long t; uint32_t seq; };
std::deque<Applied> applied;
double step_interval = 400000; // smoothed time between simulation steps (40 ms to start)
long long last_step = 0;

long long now_100ns()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (long long)(c.QuadPart * (10000000.0 / freq.QuadPart));
}

void record_applied(uint32_t seq)
{
    long long t = now_100ns();
    std::lock_guard<std::mutex> lock(applied_mutex);
    if (last_step) step_interval = step_interval * 0.9 + (t - last_step) * 0.1;
    last_step = t;
    applied.push_back({ t, seq });
    while (applied.size() > 256) applied.pop_front();
}

} // namespace

// A frame presented at present_100ns was rendered in the frame before, from the pose applied at the
// simulation step before that render
uint32_t pose_seq_drawn_at(long long present_100ns, double* age_ms)
{
    std::lock_guard<std::mutex> lock(applied_mutex);
    if (applied.empty()) return 0;
    long long target = present_100ns - (long long)step_interval;
    const Applied* best = &applied.front();
    for (const auto& a : applied) {
        if (a.t > target) break;
        best = &a;
    }
    if (age_ms) *age_ms = (present_100ns - best->t) / 10000.0;
    return best->seq;
}

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
        streamer_update(0, 0, 0);
        return;
    }

    float want_fov = phone.fov_deg > 0 ? phone.fov_deg : DEFAULT_FOV_DEG;
    char cmd[512];
    if (!active) {
        take_view(want_fov);
        active = true;
        fov_deg = want_fov;
        aligned = false;
        heading_zero = phone.heading;
        last_calibrate = phone.calibrate;
    }

    // Line the phone up with where the player faces: on the first tracked pose, and on Calibrate
    if ((phone.tracking && !aligned) || phone.calibrate != last_calibrate) {
        heading_zero = phone.heading;
        aligned = phone.tracking;
        last_calibrate = phone.calibrate;
        run("vao_base = getDir player;");
    }

    if (fabs(want_fov - fov_deg) > 0.01f) {
        fov_deg = want_fov;
        sprintf_s(cmd, "setCamFrustum [true, %f];", tan(fov_deg * 3.14159265 / 360.0));
        run(cmd);
    }

    // Aim (held at the last tracked direction while ARCore is not tracking) and follow the player's position
    sprintf_s(cmd,
        "vao_cam camSetPos (player modelToWorld [0, 0.2, 1.7]); vao_cam camCommit 0; "
        "setCamFrustumOffsets [true, vao_base + %.3f, %.3f, %.3f];",
        phone.heading - heading_zero, phone.pitch, phone.roll);
    run(cmd);
    record_applied(phone.seq);

    streamer_update(phone.ip, phone.seq, (float)tan(fov_deg * 3.14159265 / 360.0));
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
