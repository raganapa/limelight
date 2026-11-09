// SPDX-License-Identifier: MIT
// The on-screen effects. Everything is drawn by one render thread into
// per-pixel-alpha layered windows that are click-through and never take focus.
// Frames are paced to the display's vertical blank, and only the pixels that
// changed are uploaded, so the effects follow the pointer smoothly.
#pragma once
#include "common.h"
#include "settings.h"

struct OverlayParams {
  int spotRadius = 190, dimPercent = 72, softness = 30, squareWidth = 560;
  int zoomTenths = 25, lensRadius = 170, laserSize = 14, penWidth = 6;
  uint32_t laserColor = 0xFF2E2E, penColor = 0xFF3B30;
  static OverlayParams From(const Settings& s);
  bool operator==(const OverlayParams& o) const { return memcmp(this, &o, sizeof o) == 0; }
};

class Overlay {
 public:
  bool Start();
  void Stop();
  void SetParams(const OverlayParams& p);
  void SetEffect(Effect e);
  void ClearInk();
  void Toast(const std::wstring& text);
  void DisplayChanged();
  bool HasInk() const { return hasInk_.load(); }

  struct Impl;

 private:
  Impl* impl_ = nullptr;
  std::atomic<bool> hasInk_{false};
};
