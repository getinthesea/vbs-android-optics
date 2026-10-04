// Graticule overlay on the PC: see overlay.h.
//
// The graticule is the phone's: ticks every 10 mils out to 60 left, right, up and down, alternately 10 and 5
// mils long, joined by lines, with 20, 40 and 60 labelled above the horizontal ticks. They are placed by angle
// through VBS's perspective: VBS's view is fov_deg across the client area's width (the plugin sets the frustum
// to match the window's shape), so a direction [a] off-centre lands tan(a) / tan(fov / 2) of the half-width out.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <atomic>
#include <cmath>
#include <cstring>
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

void draw_graticule(Gdiplus::Graphics& g, int w, int h, float fov_deg)
{
    float cx = w / 2.0f, cy = h / 2.0f;
    double scale = (w / 2.0) / tan(fov_deg / 2.0 * PI / 180.0); // pixels per unit of tangent
    double rad_per_mil = 2 * PI / 6400;
    auto offset = [&](double mils) { return (float)(tan(mils * rad_per_mil) * scale); };
    double edge_x = atan(w / 2.0 / scale) / rad_per_mil; // out to 60 mils, or the edge if that comes first
    double edge_y = atan(h / 2.0 / scale) / rad_per_mil;

    // As thin as the phone's, for the window's height (1.5 px on the phone's 1080)
    Gdiplus::Pen pen(Gdiplus::Color(255, 0, 0, 0), (float)(std::max)(1.0, h / 720.0));
    float outer_x = offset(floor((std::min)(60.0, edge_x) / 10) * 10);
    float outer_y = offset(floor((std::min)(60.0, edge_y) / 10) * 10);
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

// Draws the graticule into a transparent bitmap the size of VBS's client area and puts it there
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
            draw_graticule(g, w, h, fov_deg);
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
            if (memcmp(&r, &drawn, sizeof(r)) != 0 || fov != drawn_fov) {
                render(overlay, r, fov);
                drawn = r;
                drawn_fov = fov;
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

void overlay_stop()
{
    // Called from DllMain: joining threads there can deadlock, so just ask it to finish (it wakes every 30 ms)
    running = false;
    if (thread.joinable()) thread.detach();
}
