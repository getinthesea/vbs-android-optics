#pragma once
// Device orientation from a T265 pose, for a T265 mounted at any 90-degree orientation on the device.
// No RealSense SDK dependency, so it can be unit tested on its own.
//
// T265 axes (body and world): X right, Y up, Z back (the lenses look along -Z).
// The world frame is gravity aligned (Y up); heading zero is wherever the T265 faced at startup.
#include <cmath>
#include <string>

struct Vec3 { double x, y, z; };
struct Quat { double w, x, y, z; };

inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

inline double dot(const Quat& a, const Quat& b)
{
    return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
}

// Rotate v by unit quaternion q
inline Vec3 rotate(const Quat& q, const Vec3& v)
{
    Vec3 u{ q.x, q.y, q.z };
    Vec3 t = cross(u, v);
    t = { 2 * t.x, 2 * t.y, 2 * t.z };
    Vec3 c = cross(u, t);
    return { v.x + q.w * t.x + c.x, v.y + q.w * t.y + c.y, v.z + q.w * t.z + c.z };
}

// Step a smoothed orientation towards a target (normalised lerp, taking the short way round)
inline Quat smooth_towards(const Quat& from, Quat to, double alpha)
{
    if (dot(from, to) < 0) {
        to = { -to.w, -to.x, -to.y, -to.z };
    }
    Quat q{ from.w + (to.w - from.w) * alpha, from.x + (to.x - from.x) * alpha,
            from.y + (to.y - from.y) * alpha, from.z + (to.z - from.z) * alpha };
    double n = std::sqrt(dot(q, q));
    return { q.w / n, q.x / n, q.y / n, q.z / n };
}

// Angle between two orientations in degrees
inline double angle_between_deg(const Quat& a, const Quat& b)
{
    double d = std::fabs(dot(a, b));
    if (d > 1.0) d = 1.0;
    return 2.0 * std::acos(d) * 180.0 / 3.14159265358979323846;
}

// Parse a T265 axis name: "x", "-x", "y", "-y", "z", "-z" (a leading "+" is allowed)
inline bool parse_axis(const std::string& s, Vec3& out)
{
    std::string a = s;
    double sign = 1.0;
    if (!a.empty() && (a[0] == '-' || a[0] == '+')) {
        sign = a[0] == '-' ? -1.0 : 1.0;
        a = a.substr(1);
    }
    if (a == "x" || a == "X") { out = { sign, 0, 0 }; return true; }
    if (a == "y" || a == "Y") { out = { 0, sign, 0 }; return true; }
    if (a == "z" || a == "Z") { out = { 0, 0, sign }; return true; }
    return false;
}

// How the T265 sits on the device: which T265 axis points along the device's line of sight,
// and which points out of the top of the device. A T265 mounted normally (lenses forward, level) is "-z y".
struct Mount {
    Vec3 forward{ 0, 0, -1 };
    Vec3 up{ 0, 1, 0 };
};

inline bool parse_mount(const std::string& fwd, const std::string& up, Mount& out)
{
    Vec3 f, u;
    if (!parse_axis(fwd, f) || !parse_axis(up, u)) return false;
    if (std::fabs(f.x * u.x + f.y * u.y + f.z * u.z) > 0.5) return false; // same or opposite axis
    out.forward = f;
    out.up = u;
    return true;
}

// Device heading / pitch / roll in degrees:
//   heading clockwise from above (full +-180), pitch nose up, roll right side down
inline void device_angles(const Quat& q, const Mount& m, double& heading, double& pitch, double& roll)
{
    const double rad2deg = 180.0 / 3.14159265358979323846;
    Vec3 f = rotate(q, m.forward);
    Vec3 u = rotate(q, m.up);
    Vec3 r = cross(f, u);
    heading = std::atan2(f.x, -f.z) * rad2deg;
    pitch = std::asin(f.y > 1.0 ? 1.0 : (f.y < -1.0 ? -1.0 : f.y)) * rad2deg;
    roll = std::atan2(-r.y, u.y) * rad2deg;
}
