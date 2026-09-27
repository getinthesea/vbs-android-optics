// Checks device_angles() recovers a known device orientation for several T265 mounts.
// Build: cl /nologo /EHsc orientation_test.cpp && orientation_test.exe
#include "orientation.h"
#include <cstdio>
#include <cstring>

typedef double Mat[3][3];
const double PI = 3.14159265358979323846;
const double D2R = PI / 180.0;

void mul(const Mat a, const Mat b, Mat out)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}

void rot_x(double a, Mat m) { Mat r = { {1, 0, 0}, {0, cos(a), -sin(a)}, {0, sin(a), cos(a)} }; memcpy(m, r, sizeof(r)); }
void rot_y(double a, Mat m) { Mat r = { {cos(a), 0, sin(a)}, {0, 1, 0}, {-sin(a), 0, cos(a)} }; memcpy(m, r, sizeof(r)); }
void rot_z(double a, Mat m) { Mat r = { {cos(a), -sin(a), 0}, {sin(a), cos(a), 0}, {0, 0, 1} }; memcpy(m, r, sizeof(r)); }

Quat to_quat(const Mat m)
{
    double w = sqrt(1.0 + m[0][0] + m[1][1] + m[2][2]) / 2.0;
    if (w > 1e-6)
        return { w, (m[2][1] - m[1][2]) / (4 * w), (m[0][2] - m[2][0]) / (4 * w), (m[1][0] - m[0][1]) / (4 * w) };
    // 180 degree rotations
    double x = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) / 2.0;
    if (x > 1e-6)
        return { (m[2][1] - m[1][2]) / (4 * x), x, (m[0][1] + m[1][0]) / (4 * x), (m[0][2] + m[2][0]) / (4 * x) };
    double y = sqrt(1.0 - m[0][0] + m[1][1] - m[2][2]) / 2.0;
    if (y > 1e-6)
        return { (m[0][2] - m[2][0]) / (4 * y), (m[0][1] + m[1][0]) / (4 * y), y, (m[1][2] + m[2][1]) / (4 * y) };
    double z = sqrt(1.0 - m[0][0] - m[1][1] + m[2][2]) / 2.0;
    return { (m[1][0] - m[0][1]) / (4 * z), (m[0][2] + m[2][0]) / (4 * z), (m[1][2] + m[2][1]) / (4 * z), z };
}

// Device orientation in the world: turn right by heading, raise the nose by pitch, drop the right side by roll
void device_orientation(double heading, double pitch, double roll, Mat out)
{
    Mat y, x, z, t;
    rot_y(-heading * D2R, y);
    rot_x(pitch * D2R, x);
    rot_z(-roll * D2R, z);
    mul(y, x, t);
    mul(t, z, out);
}

double wrap(double a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }

int main()
{
    struct { const char* fwd; const char* up; } mounts[] = {
        { "-z", "y" },  // T265 level, lenses along the line of sight
        { "y", "-z" },  // T265 lying on top, lenses to the sky, its top towards the target
        { "x", "y" },   // T265 turned sideways, lenses facing left
    };
    double cases[][3] = {
        { 0, 0, 0 }, { 30, 0, 0 }, { -30, 0, 0 }, { 170, 0, 0 }, { -170, 0, 0 },
        { 0, 20, 0 }, { 0, -20, 0 }, { 0, 0, 10 }, { 45, 15, 5 }, { 170, 30, -15 }, { -120, -40, 20 },
    };
    int failures = 0;
    for (auto& mt : mounts) {
        Mount m;
        if (!parse_mount(mt.fwd, mt.up, m)) { printf("parse failed %s %s\n", mt.fwd, mt.up); return 1; }
        Vec3 f = m.forward, u = m.up, r = cross(f, u);
        // Device axes in T265 coords are r (right), u (up), -f (back); invert (transpose) to get T265 axes in device coords
        Mat device_from_t265 = { {r.x, r.y, r.z}, {u.x, u.y, u.z}, {-f.x, -f.y, -f.z} };
        for (auto& c : cases) {
            Mat world_from_device, world_from_t265;
            device_orientation(c[0], c[1], c[2], world_from_device);
            mul(world_from_device, device_from_t265, world_from_t265);
            double h, p, rl;
            device_angles(to_quat(world_from_t265), m, h, p, rl);
            bool ok = fabs(wrap(h - c[0])) < 0.01 && fabs(p - c[1]) < 0.01 && fabs(wrap(rl - c[2])) < 0.01;
            if (!ok) failures++;
            printf("%s mount %-2s %-2s  in h=%7.1f p=%6.1f r=%6.1f  out h=%7.2f p=%6.2f r=%6.2f\n",
                ok ? "ok  " : "FAIL", mt.fwd, mt.up, c[0], c[1], c[2], h, p, rl);
        }
    }

    Mount bad;
    bool rejects = !parse_mount("z", "-z", bad) && !parse_mount("q", "y", bad) && !parse_mount("y", "y", bad);
    printf("%s rejects invalid mounts\n", rejects ? "ok  " : "FAIL");
    if (!rejects) failures++;

    Quat a{ 1, 0, 0, 0 }, b{ -0.7071, 0, -0.7071, 0 }; // b is +90 about Y written with a negative w
    Quat s = smooth_towards(a, b, 0.5);
    double h, p, rl;
    device_angles(s, Mount{}, h, p, rl);
    bool short_way = fabs(h + 45) < 0.5; // halfway between 0 and -90, not the long way round
    printf("%s smoothing takes the short way round (h=%.1f)\n", short_way ? "ok  " : "FAIL", h);
    if (!short_way) failures++;

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
