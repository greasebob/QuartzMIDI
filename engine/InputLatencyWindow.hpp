#pragma once
#include <windows.h>

// Modeless latency diagnostics window. Closing it removes the keyboard hook.
void ShowInputLatencyWindow(HWND owner);

