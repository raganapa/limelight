// SPDX-License-Identifier: MIT
// The settings window: a small custom-drawn, DPI-aware window that follows the
// Windows light/dark theme and accent colour.
#pragma once
#include "common.h"
#include "settings.h"

struct UiHost {
  virtual ~UiHost() = default;
  virtual Settings& Prefs() = 0;
  virtual void PrefsChanged() = 0;          // apply + schedule save
  virtual std::wstring StatusLine() = 0;    // "Spotlight 2 · Bluetooth · 85%"
  virtual int StatusLevel() = 0;            // 0 = searching, 1 = ok, 2 = warning
  virtual void TryEffect(Effect e) = 0;     // toggle an effect on/off (like the hotkeys)
  virtual void Preview(Effect e) = 0;       // live preview while a slider is dragged (None = stop)
  virtual void OpenDiagnostics() = 0;
  virtual bool HasHaptics() = 0;
  virtual bool HasActionButton() = 0;
  virtual bool Autostart() = 0;
  virtual void SetAutostartPref(bool on) = 0;
};

void ShowSettingsWindow(HINSTANCE inst, UiHost* host);
void RefreshSettingsWindow();  // status or settings changed elsewhere
void CloseSettingsWindow();
bool IsSettingsDialogMessage(MSG* msg);
