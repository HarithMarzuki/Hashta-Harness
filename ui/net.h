// The panel's link to engine_server.py: it owns the child process and reads
// the event stream. Nothing here draws or decides anything.
#pragma once

#include <windows.h>

#include <string>

constexpr UINT WM_APP_EVENT = WM_APP + 2;  // LPARAM: new std::string (JSON)
constexpr UINT WM_APP_NET = WM_APP + 3;    // wParam: 1 connected, 0 dropped

bool EngineStart();
void EngineStop();

void EventsStart(HWND notify);
void EventsStop();

bool ApiSend(const std::wstring& text);
bool ApiStopTurn();
bool ApiReset();
bool ApiState(std::string& out);
