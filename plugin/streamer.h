#pragma once
// Background thread that streams VBS's own window to the phone (see streamer.cpp).
#include <cstdint>
#include <string>

void streamer_start();           // safe to call every step; starts the thread once
void streamer_stop();
// Every simulation step: where to send (0 = no phone on Wi-Fi, stop capturing), the pose the view is aimed
// with, and the tangent of half the horizontal field of view (for lag correction on the phone)
void streamer_update(uint32_t phone_ip, uint32_t pose_seq, float tan_half_fov_x);
std::string streamer_status();

// In plugin.cpp: the pose seq VBS drew a frame with, from the frame's present time (QPC, 100 ns units);
// age_ms receives how long before the present that pose was applied
uint32_t pose_seq_drawn_at(long long present_100ns, double* age_ms);
