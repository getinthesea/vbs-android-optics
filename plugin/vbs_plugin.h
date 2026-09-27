#pragma once
#include <windows.h>

// The three exports VBS3 looks for in every plugins64 DLL
#define VBS_PLUGIN_EXPORT __declspec(dllexport)

// VBS hands the plugin this function in RegisterCommandFnc; it executes a string of SQF
typedef int (WINAPI* ExecuteCommandType)(const char* command, char* result, int resultLength);

VBS_PLUGIN_EXPORT void WINAPI RegisterCommandFnc(void* executeCommandFnc);
VBS_PLUGIN_EXPORT void WINAPI OnSimulationStep(float deltaT);
VBS_PLUGIN_EXPORT const char* WINAPI PluginFunction(const char* input);
