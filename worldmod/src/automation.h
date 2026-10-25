#pragma once
// Test-automation channel (see automation.cpp).
#include "d3dtrace.h"
struct IDirect3DDevice9;
void automationInit(const wchar_t* dir, TraceLogFn log);
void automationOnEndScene(IDirect3DDevice9* device);
void commandInit(const wchar_t* dir, TraceLogFn log);   // command channel only (no window/input hooks)
void autoSkipInit(const wchar_t* dir, TraceLogFn log);  // auto_skip=1 in normal play
void autoSkipOnStageLoad();                  // a map stage was loaded: click through the opening dialogs
void automationCamera(float* x, float* z);   // implemented in worldmod.cpp (last camera look-at point)
void automationGoto(float x, float z);        // implemented in worldmod.cpp (move the camera at its next update)
