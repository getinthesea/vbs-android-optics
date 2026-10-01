#pragma once
// The phone's Bearing and Range buttons, worked like the Vector's (JFSim's vector.sqf): bearing, vertical angle,
// range and difference in altitude on the phone's display, and each lase handed to JFSim as the Vector does.
#include <cstdint>
#include "vbs_plugin.h"

// VBS thread, every step. active: our camera is in use (a phone is connected); buttons: bit 0 Bearing, bit 1 Range;
// heading_delta and pitch: what the view is turned by (heading added to vao_base, degrees), as the camera is aimed
void lrf_step(ExecuteCommandType execute, bool active, uint8_t buttons, double heading_delta, double pitch);
