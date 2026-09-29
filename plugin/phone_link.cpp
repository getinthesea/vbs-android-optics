#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include "phone_link.h"
#include "protocol.h"
#include "orientation.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

std::atomic<bool> started{ false };
std::atomic<bool> running{ false };
std::thread udp_thread, tcp_thread;
std::mutex state_mutex;

// Guarded by state_mutex
Quat      filt_q{ 1, 0, 0, 0 };
bool      filt_initialized = false;
PhoneState latest;
ULONGLONG last_packet_ms = 0;
ULONGLONG last_tracking_ms = 0;

// ARCore is already smooth; this only takes the edge off network jitter
const double SMOOTHING = 0.5;
const double DEAD_ZONE_DEG = 0.02;

// Returns the ack to send back, or false if the packet is not ours
bool handle_packet(const PosePacket& p, const char* link, uint32_t ip, PoseAck& ack)
{
    if (memcmp(p.magic, "VAO1", 4) != 0)
        return false;

    std::lock_guard<std::mutex> lock(state_mutex);
    ULONGLONG now = GetTickCount64();
    last_packet_ms = now;
    latest.packets++;
    latest.link = link;
    latest.ip = ip;
    latest.tracking = p.tracking != 0;
    latest.calibrate = p.calibrate;
    latest.fov_deg = p.fov_cdeg / 100.0f;

    if (p.tracking) {
        Quat q{ p.qw, p.qx, p.qy, p.qz };
        if (!filt_initialized) {
            filt_q = q;
            filt_initialized = true;
        }
        else if (angle_between_deg(filt_q, q) >= DEAD_ZONE_DEG) {
            filt_q = smooth_towards(filt_q, q, SMOOTHING);
        }
        // ARCore's display-oriented pose: X right, Y up, looking along -Z, i.e. a "-z y" mount
        device_angles(filt_q, Mount{}, latest.heading, latest.pitch, latest.roll);
        latest.seq = p.seq;
        last_tracking_ms = now;
    }

    memcpy(ack.magic, "VAA1", 4);
    ack.seq = p.seq;
    ack.received = latest.packets;
    return true;
}

void udp_receiver()
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(POSE_PORT);
    addr.sin_addr.s_addr = INADDR_ANY; // The phone is on Wi-Fi; packets are only parsed as numbers
    DWORD timeout = 250;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return;
    }
    // Discovery replies carry this PC's name, so the phone can tell PCs apart
    DiscoverReply reply{};
    memcpy(reply.magic, "VAR1", 4);
    DWORD name_len = sizeof(reply.name);
    if (!GetComputerNameA(reply.name, &name_len))
        strcpy_s(reply.name, "VBS PC");
    while (running) {
        char buffer[64];
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(s, buffer, sizeof(buffer), 0, (sockaddr*)&from, &from_len);
        if (n == sizeof(DiscoverRequest) && memcmp(buffer, "VAD1", 4) == 0) {
            sendto(s, (const char*)&reply, sizeof(reply), 0, (sockaddr*)&from, from_len);
            continue;
        }
        if (n != sizeof(PosePacket)) continue;
        PosePacket p;
        memcpy(&p, buffer, sizeof(p));
        PoseAck ack;
        if (handle_packet(p, "wifi", from.sin_addr.s_addr, ack) && p.seq % POSE_ACK_EVERY == 0)
            sendto(s, (const char*)&ack, sizeof(ack), 0, (sockaddr*)&from, from_len);
    }
    closesocket(s);
}

// USB: the phone connects to its own 127.0.0.1:47830, which adb reverse tunnels to ours
void tcp_receiver()
{
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(POSE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR || listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        return;
    }
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listener, &fds);
        timeval tv{ 0, 250000 };
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        DWORD timeout = 250;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        BOOL nodelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

        char buffer[sizeof(PosePacket)];
        int filled = 0;
        while (running) {
            int n = recv(client, buffer + filled, sizeof(buffer) - filled, 0);
            if (n == 0) break;
            if (n < 0) {
                if (WSAGetLastError() == WSAETIMEDOUT) continue;
                break;
            }
            filled += n;
            if (filled < (int)sizeof(buffer)) continue;
            filled = 0;
            PosePacket p;
            memcpy(&p, buffer, sizeof(p));
            PoseAck ack;
            if (!handle_packet(p, "usb", 0, ack)) break; // Out of step with the stream: drop the connection
            if (p.seq % POSE_ACK_EVERY == 0)
                send(client, (const char*)&ack, sizeof(ack), 0);
        }
        closesocket(client);
    }
    closesocket(listener);
}

} // namespace

void phone_link_start()
{
    if (started.exchange(true)) return;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    running = true;
    udp_thread = std::thread(udp_receiver);
    tcp_thread = std::thread(tcp_receiver);
}

void phone_link_stop()
{
    // Called from DllMain: joining threads there can deadlock, so just ask them to finish (they time out in 250 ms)
    running = false;
    if (udp_thread.joinable()) udp_thread.detach();
    if (tcp_thread.joinable()) tcp_thread.detach();
}

PhoneState phone_link_read()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    PhoneState r = latest;
    ULONGLONG now = GetTickCount64();
    r.age_ms = last_packet_ms ? double(now - last_packet_ms) : -1;
    r.connected = last_packet_ms && now - last_packet_ms < 2000;
    r.tracking = last_tracking_ms && now - last_tracking_ms < 1000;
    return r;
}
