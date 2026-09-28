#pragma once
// Background thread that streams VBS's own window to the phone (see streamer.cpp).
#include <cstdint>
#include <string>

void streamer_start();           // safe to call every step; starts the thread once
void streamer_stop();
// Every simulation step: where to send (0 = no phone on Wi-Fi, stop capturing)
void streamer_update(uint32_t phone_ip);
std::string streamer_status();
// Width / height of VBS's window (0 if not found)
double vbs_window_aspect();

