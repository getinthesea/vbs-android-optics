#pragma once
// Feeds the VBS DAGR app (dagr/) with the observer's grid, elevation, scenario time, declination and gun batteries.
// The app asks with a "VDG1" packet to UDP DAGR_PORT (protocol.h) and gets the latest reading back as JSON.
// The PC a phone is connected to (the one showing the binos) starts the app, dagr.exe in plugins64/vbs-android-optics,
// once per mission: when a mission is running and a phone is connected, unless it is already open.
#include <string>

void dagr_start();                       // safe to call every step; starts the server once
// VBS thread, every step: re-reads VBS a few times a second (execute_command is VBS's ExecuteCommand function),
// and starts the app if it is time to
void dagr_update(void* execute_command, bool phone_connected);
void dagr_stop();
// A lase with both rangefinder buttons (lrf.cpp): the app shows it in JFSim's DAGR target mode. dn in mils,
// dif_alt_m observer - target, as JFSim's DAGR shows them
void dagr_set_target(const std::string& mgrs, long dn_mils, long dist_m, long dif_alt_m);
std::string dagr_status();
