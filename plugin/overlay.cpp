// Graticule overlay on the PC: see overlay.h.
//
// The graticule and the rangefinder readouts are the phone's. The graticule: ticks every 10 mils out to 60 left, right, up and down, alternately 10 and 5
// mils long, joined by lines, with 20, 40 and 60 labelled above the horizontal ticks. They are placed by angle
// through VBS's perspective: VBS's view is fov_deg across the client area's width (the plugin sets the frustum
// to match the window's shape), so a direction [a] off-centre lands tan(a) / tan(fov / 2) of the half-width out.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include "overlay.h"
#include "streamer.h"

#pragma comment(lib, "gdiplus.lib")

namespace {

std::atomic<bool> running{ false };
std::atomic<bool> want_show{ false };
std::atomic<float> want_fov{ 0 };
std::thread thread;

// The rangefinder readouts, as the phone shows them (lrf.cpp)
std::mutex lrf_mutex;
std::string lrf_left, lrf_right; // Guarded by lrf_mutex
bool lrf_mark = false;           // Guarded by lrf_mutex
std::atomic<unsigned> lrf_version{ 0 };

const wchar_t* CLASS_NAME = L"VBSAndroidOpticsGraticule";
const double PI = 3.14159265358979;

RECT client_on_screen(HWND hwnd)
{
    RECT r;
    GetClientRect(hwnd, &r);
    POINT tl{ r.left, r.top }, br{ r.right, r.bottom };
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);
    return RECT{ tl.x, tl.y, br.x, br.y };
}

const double RAD_PER_MIL = 2 * PI / 6400;

// Where the graticule falls in the window (the phone's Graticule.kt)
struct Geometry {
    float cx, cy;
    double scale;            // pixels per unit of tangent
    double edge_x, edge_y;   // mils from the centre to the edges
    float outer_x, outer_y;  // pixels from the centre to the outermost ticks drawn

    Geometry(int w, int h, float fov_deg)
    {
        cx = w / 2.0f;
        cy = h / 2.0f;
        scale = (w / 2.0) / tan(fov_deg / 2.0 * PI / 180.0);
        edge_x = atan(w / 2.0 / scale) / RAD_PER_MIL; // out to 60 mils, or the edge if that comes first
        edge_y = atan(h / 2.0 / scale) / RAD_PER_MIL;
        outer_x = offset(floor((std::min)(60.0, edge_x) / 10) * 10);
        outer_y = offset(floor((std::min)(60.0, edge_y) / 10) * 10);
    }
    float offset(double mils) const { return (float)(tan(mils * RAD_PER_MIL) * scale); }
};

void draw_graticule(Gdiplus::Graphics& g, int h, const Geometry& geo)
{
    float cx = geo.cx, cy = geo.cy;
    auto offset = [&](double mils) { return geo.offset(mils); };
    double edge_x = geo.edge_x, edge_y = geo.edge_y;
    float outer_x = geo.outer_x, outer_y = geo.outer_y;

    // As thin as the phone's, for the window's height (1.5 px on the phone's 1080)
    Gdiplus::Pen pen(Gdiplus::Color(255, 0, 0, 0), (float)(std::max)(1.0, h / 720.0));
    g.DrawLine(&pen, cx - outer_x, cy, cx + outer_x, cy);
    g.DrawLine(&pen, cx, cy - outer_y, cx, cy + outer_y);
    for (int n = 0; n <= 6; n++) {
        double mils = n * 10.0;
        float half = offset(n % 2 == 0 ? 5.0 : 2.5); // long ticks 10 mils, short 5, centred on the axis
        float d = offset(mils);
        if (mils <= edge_x) {
            g.DrawLine(&pen, cx - d, cy - half, cx - d, cy + half);
            if (n > 0) g.DrawLine(&pen, cx + d, cy - half, cx + d, cy + half);
        }
        if (n > 0 && mils <= edge_y) {
            g.DrawLine(&pen, cx - half, cy - d, cx + half, cy - d);
            g.DrawLine(&pen, cx - half, cy + d, cx + half, cy + d);
        }
    }

    // "20", "40" and "60" above those ticks either side, a quarter as tall as a long tick, a little clear of it
    float long_half = offset(5.0);
    float em = long_half / 2 / 0.716f; // Arial's digits are 0.716 em tall
    Gdiplus::FontFamily family(L"Arial");
    Gdiplus::Font font(&family, em, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, 0, 0, 0));
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentFar);
    float baseline = cy - long_half - long_half / 3;
    float bottom = baseline + em * 0.212f; // Arial's descent below the baseline
    for (int mils : { 20, 40, 60 }) {
        if (mils > edge_x) break;
        wchar_t text[4];
        swprintf_s(text, L"%d", mils);
        for (float x : { cx - offset(mils), cx + offset(mils) }) {
            Gdiplus::RectF box(x - em * 2, bottom - em * 2, em * 4, em * 2);
            g.DrawString(text, -1, &font, box, &format, &brush);
        }
    }
}

// Seven-segment readouts, as the phone draws them (LrfView.kt): digits, '-' and ' '
const double DIGIT_MILS = 6.5;     // digit height, in mils, so they zoom with the graticule (as the phone's)
const float DIGIT_WIDTH = 0.55f, SPACING = 0.3f, STROKE = 0.13f, GAP = 0.5f;
const double TICK_MILS = 40.0;     // the readouts centre on these ticks
enum { SEG_A = 1, SEG_B = 2, SEG_C = 4, SEG_D = 8, SEG_E = 16, SEG_F = 32, SEG_G = 64 }; // a top, b top right .. g middle

int segments(char c)
{
    static const int digits[] = {
        SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F, SEG_B | SEG_C, SEG_A | SEG_B | SEG_G | SEG_E | SEG_D,
        SEG_A | SEG_B | SEG_G | SEG_C | SEG_D, SEG_F | SEG_G | SEG_B | SEG_C, SEG_A | SEG_F | SEG_G | SEG_C | SEG_D,
        SEG_A | SEG_F | SEG_G | SEG_E | SEG_D | SEG_C, SEG_A | SEG_B | SEG_C,
        SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G, SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G };
    if (c >= '0' && c <= '9') return digits[c - '0'];
    return c == '-' ? SEG_G : 0;
}

float width_in_digits(const std::string& text)
{
    return text.empty() ? 0 : text.size() * (DIGIT_WIDTH + SPACING) - SPACING;
}

// A segment drawn twice: a dark edge first, then red, so it reads over bright ground and sky
void segment_line(Gdiplus::Graphics& g, Gdiplus::Pen& edge, Gdiplus::Pen& red, float x0, float y0, float x1, float y1)
{
    g.DrawLine(&edge, x0, y0, x1, y1);
    g.DrawLine(&red, x0, y0, x1, y1);
}

void draw_digits(Gdiplus::Graphics& g, Gdiplus::Pen& edge, Gdiplus::Pen& red, const std::string& text,
    float x, float bottom, float d)
{
    float cell = d * DIGIT_WIDTH, inset = d * STROKE;
    for (char c : text) {
        int on = segments(c);
        float r = x + cell, top = bottom - d, mid = bottom - d / 2;
        auto seg = [&](int bit, float x0, float y0, float x1, float y1) {
            if (on & bit) segment_line(g, edge, red, x0, y0, x1, y1);
        };
        seg(SEG_A, x + inset, top, r - inset, top);
        seg(SEG_B, r, top + inset, r, mid - inset);
        seg(SEG_C, r, mid + inset, r, bottom - inset);
        seg(SEG_D, x + inset, bottom, r - inset, bottom);
        seg(SEG_E, x, mid + inset, x, bottom - inset);
        seg(SEG_F, x, top + inset, x, mid - inset);
        seg(SEG_G, x + inset, mid, r - inset, mid);
        x += cell + d * SPACING;
    }
}

// Each readout centred on a 40 mil tick (left and right, level with the one below centre), moved in or shrunk
// only as far as needed to stay inside the graticule; the aiming mark a small square of segments at the centre
void draw_lrf(Gdiplus::Graphics& g, int h, const Geometry& geo, const std::string& left, const std::string& right,
    bool mark)
{
    if (left.empty() && right.empty() && !mark) return;
    float top = geo.offset(5.0); // below centre, clear of the horizontal scale's long ticks
    float d = (std::min)(geo.offset(DIGIT_MILS), (geo.outer_y - top) / (1 + 2 * STROKE));
    for (const std::string* text : { &left, &right }) {
        if (!text->empty()) d = (std::min)(d, geo.outer_x / (GAP + width_in_digits(*text) + STROKE));
    }
    Gdiplus::Pen red(Gdiplus::Color(255, 255, 40, 30), d * STROKE);
    Gdiplus::Pen edge(Gdiplus::Color(160, 0, 0, 0), d * STROKE + (std::max)(2.0f, h / 540.0f));
    for (Gdiplus::Pen* pen : { &red, &edge }) {
        pen->SetStartCap(Gdiplus::LineCapRound);
        pen->SetEndCap(Gdiplus::LineCapRound);
    }
    auto clamp = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? (std::max)(lo, hi) : v); };
    float half = d * (1 + STROKE) / 2;
    float centre_y = clamp(geo.offset(TICK_MILS), top + half, geo.outer_y - half);
    int side = -1;
    for (const std::string* text : { &left, &right }) {
        if (!text->empty()) {
            float width = width_in_digits(*text) * d;
            float extent = (width + STROKE * d) / 2;
            float centre_x = clamp(geo.offset(TICK_MILS), GAP * d + extent, geo.outer_x - extent);
            draw_digits(g, edge, red, *text, geo.cx + side * centre_x - width / 2, geo.cy + centre_y + d / 2, d);
        }
        side = 1;
    }
    if (mark) {
        float m = h / 120.0f, inset = d * STROKE;
        float l = geo.cx - m, r = geo.cx + m, t = geo.cy - m, b = geo.cy + m;
        segment_line(g, edge, red, l + inset, t, r - inset, t);
        segment_line(g, edge, red, r, t + inset, r, b - inset);
        segment_line(g, edge, red, l + inset, b, r - inset, b);
        segment_line(g, edge, red, l, t + inset, l, b - inset);
    }
}

// Draws the graticule and readouts into a transparent bitmap the size of VBS's client area and puts it there
void render(HWND overlay, const RECT& r, float fov_deg)
{
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        memset(bits, 0, (size_t)w * h * 4);
        {
            // Premultiplied alpha, as UpdateLayeredWindow wants
            Gdiplus::Bitmap bitmap(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
            Gdiplus::Graphics g(&bitmap);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            Geometry geo(w, h, fov_deg);
            draw_graticule(g, h, geo);
            std::string left, right;
            bool mark;
            {
                std::lock_guard<std::mutex> lock(lrf_mutex);
                left = lrf_left;
                right = lrf_right;
                mark = lrf_mark;
            }
            draw_lrf(g, h, geo, left, right, mark);
        }
        HGDIOBJ old = SelectObject(mem, dib);
        POINT pos{ r.left, r.top }, origin{ 0, 0 };
        SIZE size{ w, h };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(overlay, screen, &pos, &size, mem, &origin, 0, &blend, ULW_ALPHA);
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void run()
{
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    HWND overlay = nullptr, owner = nullptr;
    RECT drawn{};
    float drawn_fov = 0;
    unsigned drawn_lrf = 0;
    bool shown = false;
    while (running) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);

        HWND vbs = vbs_window();
        bool show = want_show && vbs && !IsIconic(vbs);
        // Owned by VBS's window, so it stays just above it; made again if VBS's window changes
        if (vbs != owner || !overlay) {
            if (overlay) DestroyWindow(overlay);
            overlay = nullptr;
            owner = vbs;
            shown = false;
            drawn = RECT{};
            if (vbs) overlay = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CLASS_NAME, L"",
                WS_POPUP, 0, 0, 1, 1, vbs, nullptr, wc.hInstance, nullptr);
        }
        if (!overlay) continue;
        if (show) {
            RECT r = client_on_screen(vbs);
            float fov = want_fov;
            unsigned lrf = lrf_version;
            if (memcmp(&r, &drawn, sizeof(r)) != 0 || fov != drawn_fov || lrf != drawn_lrf) {
                render(overlay, r, fov);
                drawn = r;
                drawn_fov = fov;
                drawn_lrf = lrf;
            }
        }
        if (show != shown) {
            ShowWindow(overlay, show ? SW_SHOWNOACTIVATE : SW_HIDE);
            shown = show;
        }
    }
    if (overlay) DestroyWindow(overlay);
    UnregisterClassW(CLASS_NAME, wc.hInstance);
    Gdiplus::GdiplusShutdown(token);
}

} // namespace

void overlay_update(bool show, float fov_deg)
{
    want_show = show && fov_deg > 0;
    want_fov = fov_deg;
    if (show && !running.exchange(true)) thread = std::thread(run);
}

void overlay_set_lrf(const std::string& left, const std::string& right, bool mark)
{
    std::lock_guard<std::mutex> lock(lrf_mutex);
    if (left == lrf_left && right == lrf_right && mark == lrf_mark) return;
    lrf_left = left;
    lrf_right = right;
    lrf_mark = mark;
    lrf_version++;
}

void overlay_stop()
{
    // Called from DllMain: joining threads there can deadlock, so just ask it to finish (it wakes every 30 ms)
    running = false;
    if (thread.joinable()) thread.detach();
}
