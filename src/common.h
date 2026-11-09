// Limelight - a lightweight, local companion app for the Logitech Spotlight 2.
// SPDX-License-Identifier: MIT
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#define APP_NAME L"Limelight"
#define APP_VERSION L"1.0.4"
#define APP_MAIN_CLASS L"LimelightMainWindow"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Messages understood by the main (tray) window.
enum : UINT {
  WM_APP_TRAY = WM_APP + 1,      // tray icon callback
  WM_APP_PRESENTER,              // wParam = PresenterEvent, lParam = value
  WM_APP_PAUSE_DEVICE,           // sent by the diagnostics tool: release the remote
  WM_APP_RESUME_DEVICE,          // sent by the diagnostics tool: take the remote back
  WM_APP_RELOAD_SETTINGS,        // settings.ini changed on disk (laser finder)
  WM_APP_SHOW_SETTINGS,          // second instance asks us to show the window
  WM_APP_SETTINGS_EDITED,        // the settings window changed something
  WM_APP_QUIT,                   // installer / --quit
};

enum PresenterEvent : WPARAM {
  PE_BUTTONS = 1,      // lParam = ButtonMask
  PE_CONNECTED,        // lParam = transport (1 = Bolt receiver, 2 = Bluetooth)
  PE_DISCONNECTED,
  PE_BATTERY,          // lParam = percent (0..100) | (charging << 8)
  PE_STATUS,           // status text changed (read with Presenter::StatusText)
};

// Logical buttons reported by the presenter (bit mask).
enum ButtonMask : LPARAM {
  BTN_SOFT = 1 << 0,    // highlight button, light press   (CID 0x00D8)
  BTN_HARD = 1 << 1,    // highlight button, hard press    (CID 0x01A8)
  BTN_ACTION = 1 << 2,  // action button                   (CID 0x01B0, when diverted)
};

template <class T>
inline T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline double NowMs() {
  static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return double(c.QuadPart) * 1000.0 / double(freq.QuadPart);
}

inline std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

inline std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

// %LOCALAPPDATA%\Limelight (logs) and %APPDATA%\Limelight (settings)
std::wstring AppDataDir(bool roaming);

// Simple thread-safe logger -> %LOCALAPPDATA%\Limelight\limelight.log
void LogInit();
void Log(const char* fmt, ...);
