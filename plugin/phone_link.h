#pragma once
// Receives the phone's ARCore pose (UDP over Wi-Fi, or TCP over USB via adb reverse) and turns it into
// heading / pitch / roll for a phone held like a camera.
#include <cstdint>
#include <string>

struct PhoneState {
    bool     connected = false;  // a packet arrived in the last 2 s
    bool     tracking = false;   // a tracking pose arrived in the last second
    double   heading = 0, pitch = 0, roll = 0; // last tracked; degrees, heading clockwise, pitch up, roll right down
    uint32_t seq = 0;            // of the last tracked pose
    uint8_t  calibrate = 0;      // the phone's Calibrate counter
    float    fov_deg = 0;        // wanted horizontal field of view (0 = leave VBS's)
    uint8_t  buttons = 0;        // held now: bit 0 Bearing, bit 1 Range
    bool     calibrating = false; // the phone is in calibration mode
    uint32_t ip = 0;             // phone's IPv4 (network order) on Wi-Fi, 0 over USB
    uint32_t packets = 0;
    double   age_ms = -1;        // since the last packet, -1 if none yet
    std::string link;            // "wifi", "usb" or ""
};

void phone_link_start();         // safe to call every step; starts the receivers once
void phone_link_stop();
PhoneState phone_link_read();
// What the phone's rangefinder display shows (sent back in the acks, straight away when it changes)
void phone_link_set_lrf(const std::string& left, const std::string& right, bool mark, const std::string& grid);
