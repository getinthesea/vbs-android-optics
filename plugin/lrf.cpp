// Laser rangefinder: the Vector's two buttons (see lrf.h), ported from JFSim's vector.sqf.
//
//   Bearing        bearing (grid, mils, to the 5 below) on the left
//   Bearing twice  within 3 s: bearing, and the vertical angle (mils) on the right; then locked until it clears
//   Range          range (m) on the right. After Bearing, Range is ignored until the display clears
//   Range twice    within 3 s: range on the left, difference in altitude (target - observer, m) on the right
//   Both           bearing and range, and the lase goes to JFSim's DAGR (and to ours)
// The display clears 4 s after the last press. Ranges beyond 8000 m or under 5 m read "- - - -". Each lase is
// published as vector_aim_point, which JFSim's instructor app picks up; JFSim's LRF-disable switch
// (global_vector_lrf_disable) is honoured.
//
// The range is to the ground where the centre of the view points, traced from the observer's eye along the
// direction the camera is aimed (vector.sqf used screenToWorld, which does not follow our frustum-offset aiming).
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "lrf.h"
#include "dagr.h"
#include "phone_link.h"

namespace {

const double DOUBLE_CLICK_S = 3;
const double CLEAR_AFTER_S = 4;
const double MAX_RANGE_M = 8000, MIN_RANGE_M = 5;
const char* NO_RANGE = "- - - -";

// vector.sqf's variables, same names without var_
bool left_down = false, right_down = false, double_down = false;
bool disable_input = false, disable_input_on_release = false, disable_right = false;
bool update_aos = false, left_release = false;
double timeout = -1;                    // time of the last press (-1: nothing to clear)
double double_left = -1e9, double_right = -1e9;
std::string left_text, right_text;
bool mark = false;

struct Lase {
    bool   ok = false;       // the SQF ran
    double az_deg = 0;       // direction of the view, degrees clockwise from grid north
    double range_m = -1;     // eye to the ground, -1 if no ground within MAX_RANGE_M
    double target_alt = 0, observer_alt = 0; // terrain heights ASL
    double x = 0, y = 0;     // where it hit
    std::string mgrs;        // 10 figure, of the hit
};

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t end = s.find(sep, start);
        parts.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) return parts;
        start = end + 1;
    }
}

std::string unquote(std::string s)
{
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

// Traces the centre of the view to the ground: terrainIntersectASL out to MAX_RANGE_M, then halved down to a few mm
Lase lase(ExecuteCommandType execute, double heading_delta, double pitch)
{
    char sqf[2048];
    sprintf_s(sqf,
        "if (isNil \"vao_base\" || isNil \"vao_anchor\") then { \"\" } else { if (isNull vao_anchor) then { \"\" } else { "
        "vao_lrf_az = vao_base + %.4f; vao_lrf_p = %.4f; "
        "vao_lrf_eye = getPosASL vao_anchor; vao_lrf_eye set [2, (vao_lrf_eye select 2) + 1.7]; "
        "vao_lrf_dir = [(sin vao_lrf_az) * (cos vao_lrf_p), (cos vao_lrf_az) * (cos vao_lrf_p), sin vao_lrf_p]; "
        "vao_lrf_at = { [(vao_lrf_eye select 0) + (vao_lrf_dir select 0) * _this, "
        "(vao_lrf_eye select 1) + (vao_lrf_dir select 1) * _this, (vao_lrf_eye select 2) + (vao_lrf_dir select 2) * _this] }; "
        "vao_lrf_d = -1; "
        "if (terrainIntersectASL [vao_lrf_eye, %.0f call vao_lrf_at]) then { "
        "vao_lrf_lo = 0; vao_lrf_hi = %.0f; "
        "for \"_k\" from 1 to 24 do { vao_lrf_mid = (vao_lrf_lo + vao_lrf_hi) / 2; "
        "if (terrainIntersectASL [vao_lrf_eye, vao_lrf_mid call vao_lrf_at]) then { vao_lrf_hi = vao_lrf_mid; } else { vao_lrf_lo = vao_lrf_mid; }; }; "
        "vao_lrf_d = vao_lrf_hi; }; "
        "vao_lrf_hit = (if (vao_lrf_d < 0) then { %.0f } else { vao_lrf_d }) call vao_lrf_at; "
        "vao_lrf_hit = [vao_lrf_hit select 0, vao_lrf_hit select 1, 0]; "
        "format [\"%%1|%%2|%%3|%%4|%%5|%%6|%%7\", vao_lrf_az, vao_lrf_d, getTerrainHeightASL vao_lrf_hit, "
        "getTerrainHeightASL (position vao_anchor), vao_lrf_hit select 0, vao_lrf_hit select 1, "
        "(posToCoord [vao_lrf_hit, \"MGRS\"]) select 0] }; }",
        heading_delta, pitch, MAX_RANGE_M, MAX_RANGE_M, MAX_RANGE_M);
    static char result[1024];
    result[0] = 0;
    execute(sqf, result, sizeof(result));
    Lase l;
    std::vector<std::string> f = split(unquote(result), '|');
    if (f.size() != 7) return l;
    l.ok = true;
    l.az_deg = atof(f[0].c_str());
    l.range_m = atof(f[1].c_str());
    l.target_alt = atof(f[2].c_str());
    l.observer_alt = atof(f[3].c_str());
    l.x = atof(f[4].c_str());
    l.y = atof(f[5].c_str());
    for (char c : f[6])
        if (isalnum((unsigned char)c)) l.mgrs += c;
    return l;
}

bool has_range(const Lase& l)
{
    return l.ok && l.range_m >= MIN_RANGE_M && l.range_m <= MAX_RANGE_M;
}

// Bearing in mils, 0-6399: rounded, then down to the 5 below (JFSim's func_round_to_five)
long bearing_mils(const Lase& l)
{
    long m = lround(fmod(fmod(l.az_deg, 360.0) + 360.0, 360.0) / 360.0 * 6400.0) % 6400;
    return m - m % 5;
}

std::string range_text(const Lase& l)
{
    return has_range(l) ? std::to_string(lround(l.range_m)) : NO_RANGE;
}

// Hands a lase to JFSim as vector.sqf does: vector_aim_point for the instructor app; for Both, also the
// global_vector_* values and JFSim's DAGR target mode. Nothing to hand over without a ground return.
void publish(ExecuteCommandType execute, const Lase& l, bool both, double pitch)
{
    if (!has_range(l)) return;
    char sqf[1024];
    sprintf_s(sqf, "vector_aim_point = [%.2f, %.2f, 0]; publicVariable \"vector_aim_point\"; ", l.x, l.y);
    std::string cmd = sqf;
    if (both) {
        sprintf_s(sqf,
            "global_vector_dir = %ld; global_vector_dist = %ld; global_vector_gr = vector_aim_point; "
            "global_vector_aos = %.2f; "
            "publicExec [\"player == dagr\", \"if (!isNil \"\"func_set_dagr_targeting\"\") then "
            "{ [[%.2f, %.2f, 0]] call func_set_dagr_targeting; };\"]; ",
            bearing_mils(l), lround(l.range_m), pitch / 360.0 * 6400.0, l.x, l.y);
        cmd += sqf;
        // Our DAGR app shows JFSim's DAGR target mode: Dn rounded, DifAlt observer - target
        long dn = lround(fmod(fmod(l.az_deg, 360.0) + 360.0, 360.0) / 360.0 * 6400.0) % 6400;
        dagr_set_target(l.mgrs, dn, lround(l.range_m), lround(l.observer_alt - l.target_alt));
    }
    execute(cmd.c_str(), nullptr, 0);
}

bool lrf_disabled(ExecuteCommandType execute)
{
    char result[64] = {};
    execute("if (isNil \"global_vector_lrf_disable\") then { false } else { global_vector_lrf_disable }", result, sizeof(result));
    return strstr(result, "true") != nullptr;
}

void reset()
{
    left_down = right_down = double_down = false;
    disable_input = disable_input_on_release = disable_right = false;
    update_aos = left_release = false;
    timeout = -1;
    double_left = double_right = -1e9;
    left_text.clear();
    right_text.clear();
    mark = false;
}

} // namespace

void lrf_step(ExecuteCommandType execute, bool active, uint8_t buttons, double heading_delta, double pitch)
{
    if (!execute || !active) {
        reset();
        phone_link_set_lrf("", "", false);
        return;
    }
    bool left = (buttons & 1) != 0, right = (buttons & 2) != 0;
    // Only ask VBS about the switch while something is happening
    if ((left || right || left_down || right_down || double_down) && lrf_disabled(execute)) return;
    double now = GetTickCount64() / 1000.0;

    if (left && right && !disable_input) { // BOTH PRESS
        disable_input = true;
        double_down = true;
        left_down = right_down = false; // vector.sqf left these set, so a stale release fired after the clear
        timeout = now;
        mark = true;
    }
    if (!left && !right && double_down) { // BOTH RELEASE
        Lase l = lase(execute, heading_delta, pitch);
        left_text = std::to_string(bearing_mils(l));
        right_text = range_text(l);
        double_down = false;
        timeout = now;
        publish(execute, l, true, pitch);
    }
    if (left && !disable_input) { // LEFT PRESS (every step while held)
        left_down = true;
        mark = true;
        Lase l = lase(execute, heading_delta, pitch);
        left_text = std::to_string(bearing_mils(l));
        right_text.clear();
        if (now - double_left <= DOUBLE_CLICK_S && update_aos && left_release) { // DOUBLE LEFT: vertical angle
            right_text = std::to_string(lround(pitch / 360.0 * 6400.0));
            disable_input_on_release = true;
        }
        disable_right = true;
        timeout = now;
        double_left = now;
    }
    if (!left && left_down && !disable_input) { // LEFT RELEASE
        left_down = false;
        left_release = true;
        disable_right = true;
        if (now - double_left <= DOUBLE_CLICK_S) update_aos = true;
        if (disable_input_on_release) {
            disable_input = true;
            update_aos = false;
            disable_input_on_release = false;
        }
        double_left = now;
        timeout = now;
        publish(execute, lase(execute, heading_delta, pitch), false, pitch);
    }
    if (right && !disable_input && !disable_right) { // RIGHT PRESS
        right_down = true;
        mark = true;
        timeout = now;
    }
    if (!right && right_down && !disable_input && !disable_right) { // RIGHT RELEASE
        right_down = false;
        Lase l = lase(execute, heading_delta, pitch);
        if (now - double_right >= DOUBLE_CLICK_S) { // single: range
            left_text.clear();
            right_text = range_text(l);
        }
        else { // double: range and difference in altitude
            left_text = range_text(l);
            right_text = has_range(l) ? std::to_string(lround(l.target_alt - l.observer_alt)) : "- - -";
            disable_input = true;
        }
        double_right = now;
        publish(execute, l, false, pitch);
    }
    if (timeout >= 0 && now - timeout >= CLEAR_AFTER_S) { // CLEAR DISPLAY
        left_text.clear();
        right_text.clear();
        mark = false;
        disable_input = false;
        disable_right = false;
        timeout = -1;
    }
    phone_link_set_lrf(left_text, right_text, mark);
}
