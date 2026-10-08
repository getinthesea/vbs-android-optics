// DAGR feed: see dagr.h. The app is the Godot project in dagr/.
#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <tlhelp32.h>
#include <vector>
#include "dagr.h"
#include "anchor.h"
#include "protocol.h"
#include "vbs_plugin.h"

namespace {

const ULONGLONG READ_EVERY_MS = 250;

std::atomic<bool> running{ false };
std::thread server;
std::mutex state_mutex;
std::string state_json = "{\"ok\":false}"; // Guarded by state_mutex
std::string last_raw;                      // Guarded by state_mutex: the SQF's last result, for "status"
std::atomic<unsigned> requests{ 0 };
std::atomic<int> view_mode{ 0 };       // from the app's menu (dagr.h)
std::atomic<bool> show_grid{ true };
std::string launch_status = "not yet";     // VBS thread only
std::string target_json;                   // VBS thread only: the last lase with both buttons ("" = none)
unsigned target_id = 0;

const char* APP_EXE = "dagr.exe";

// One expression, evaluated on the VBS thread; returns "" before a mission is running, else
// "<MGRS 10 figure>|<terrain height ASL>|<daytime>|<declination>|<n>=<MGRS>;<n>=<MGRS>;..." with the first 5
// gun batteries of G1-G9 (dagr_* are ours)
const std::string READ_SQF = std::string(
    "if (isNull player) then { \"\" } else { ") + ANCHOR_SQF +
    "dagr_b = \"\"; dagr_n = 0; "
    "for \"_i\" from 1 to 9 do { "
    "if (dagr_n < 5 && !isNil format [\"g%1\", _i]) then { dagr_u = call compile format [\"g%1\", _i]; "
    "if (typeName dagr_u == \"OBJECT\") then { if (!isNull dagr_u) then { "
    "dagr_b = dagr_b + format [\"%1=%2;\", _i, (posToCoord [getPos dagr_u, \"MGRS\"]) select 0]; "
    "dagr_n = dagr_n + 1; }; }; }; }; "
    "format [\"%1|%2|%3|%4|%5\", (posToCoord [getPos vao_anchor, \"MGRS\"]) select 0, "
    "getTerrainHeightASL (position vao_anchor), daytime, getDeclination, dagr_b] "
    "}";

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

// MGRS from VBS is letters and digits only; keep it that way so it can go into JSON as is
std::string clean(const std::string& s)
{
    std::string r;
    for (char c : s)
        if (isalnum((unsigned char)c)) r += c;
    return r;
}

// Turns READ_SQF's result into the JSON the app gets
std::string to_json(std::string raw)
{
    // VBS returns a string value quoted, with inner quotes doubled
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') raw = raw.substr(1, raw.size() - 2);
    std::vector<std::string> f = split(raw, '|');
    if (f.size() != 5 || clean(f[0]).empty()) return "{\"ok\":false}";

    double elevation = atof(f[1].c_str());
    double daytime = atof(f[2].c_str());
    double declination = atof(f[3].c_str());
    int seconds = (int)lround(daytime * 3600) % 86400;
    char head[256];
    sprintf_s(head, "{\"ok\":true,\"mgrs\":\"%s\",\"elevation\":%.1f,\"time\":\"%02d:%02d:%02d\",\"mv_mils\":%ld,\"batteries\":[",
        clean(f[0]).c_str(), elevation, seconds / 3600, seconds / 60 % 60, seconds % 60,
        lround(declination / 360.0 * 6400.0));
    std::string json = head;
    bool first = true;
    for (const std::string& b : split(f[4], ';')) {
        size_t eq = b.find('=');
        if (eq == std::string::npos || clean(b.substr(eq + 1)).empty()) continue;
        json += first ? "" : ",";
        json += "{\"cs\":" + std::to_string(atoi(b.substr(0, eq).c_str())) + ",\"mgrs\":\"" + clean(b.substr(eq + 1)) + "\"}";
        first = false;
    }
    return json + "]}";
}

// Answers the app: any "VDG1" packet gets the latest reading. "VDG1m<mode>g<0|1>" also sets the menu's choices
// (the app sends them only to the VBS it has locked onto, not in its broadcasts)
void serve()
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DAGR_PORT);
    addr.sin_addr.s_addr = INADDR_ANY; // The app may be on another PC
    DWORD timeout = 250;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return;
    }
    while (running) {
        char buffer[16];
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(s, buffer, sizeof(buffer), 0, (sockaddr*)&from, &from_len);
        if (n < 4 || memcmp(buffer, "VDG1", 4) != 0) continue;
        if (n == 8 && buffer[4] == 'm' && buffer[5] >= '0' && buffer[5] <= '4' && buffer[6] == 'g') {
            view_mode = buffer[5] - '0';
            show_grid = buffer[7] != '0';
        }
        requests++;
        std::string reply;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            reply = state_json;
        }
        // What the plugin is using, so the app's menu shows it
        reply.insert(reply.size() - 1, ",\"view_mode\":" + std::to_string(view_mode.load()) +
            ",\"show_grid\":" + (show_grid ? "true" : "false"));
        sendto(s, reply.c_str(), (int)reply.size(), 0, (sockaddr*)&from, from_len);
    }
    closesocket(s);
}

bool app_running()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    for (BOOL ok = Process32First(snapshot, &entry); ok && !found; ok = Process32Next(snapshot, &entry))
        found = _stricmp(entry.szExeFile, APP_EXE) == 0;
    CloseHandle(snapshot);
    return found;
}

// Starts plugins64\vbs-android-optics\dagr.exe (next to this DLL) unless it is already open
void launch_app()
{
    if (app_running()) {
        launch_status = "already open";
        return;
    }
    HMODULE self = nullptr;
    char dll_path[MAX_PATH] = {};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&launch_app, &self);
    GetModuleFileNameA(self, dll_path, MAX_PATH);
    std::string dir = dll_path;
    dir = dir.substr(0, dir.find_last_of('\\') + 1) + "vbs-android-optics";
    std::string exe = dir + "\\" + APP_EXE;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        AllowSetForegroundWindow(pi.dwProcessId); // VBS has the foreground: let the DAGR come up in front of it
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        launch_status = "started " + exe;
    }
    else {
        launch_status = "could not start " + exe + " (error " + std::to_string(GetLastError()) + ")";
    }
}

} // namespace

void dagr_start()
{
    if (running.exchange(true)) return;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    server = std::thread(serve);
}

void dagr_update(void* execute_command, bool phone_connected)
{
    static ULONGLONG last_read = 0;
    static bool mission = false;
    static bool launched = false; // this mission
    ULONGLONG now = GetTickCount64();
    if (execute_command && now - last_read >= READ_EVERY_MS) {
        last_read = now;
        static char result[4096];
        result[0] = 0;
        ((ExecuteCommandType)execute_command)(READ_SQF.c_str(), result, sizeof(result));
        std::string json = to_json(result);
        mission = json != "{\"ok\":false}";
        if (!mission) target_json.clear(); // a lase belongs to its mission
        if (!target_json.empty()) json.insert(json.size() - 1, ",\"target\":" + target_json);
        std::lock_guard<std::mutex> lock(state_mutex);
        state_json = json;
        last_raw = result;
    }
    // Once per mission, so closing the app keeps it closed until the next mission
    if (!mission) {
        launched = false;
    }
    else if (phone_connected && !launched) {
        launched = true;
        launch_app();
    }
}

void dagr_set_target(const std::string& mgrs, long dn_mils, long dist_m, long dif_alt_m)
{
    char buf[256];
    sprintf_s(buf, "{\"id\":%u,\"mgrs\":\"%s\",\"dn_mils\":%ld,\"dist\":%ld,\"dif_alt\":%ld}",
        ++target_id, mgrs.c_str(), dn_mils, dist_m, dif_alt_m);
    target_json = buf;
}

void dagr_clear_target()
{
    target_json.clear();
}

void dagr_stop()
{
    // Called from DllMain: joining threads there can deadlock, so just ask the server to finish (it times out in 250 ms)
    running = false;
    if (server.joinable()) server.detach();
}

int dagr_view_mode()
{
    return view_mode;
}

bool dagr_show_grid()
{
    return show_grid;
}

std::string dagr_status()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return "app: " + launch_status + " | requests=" + std::to_string(requests.load()) + " sqf=" + last_raw +
        " json=" + state_json;
}
