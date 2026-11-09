// SPDX-License-Identifier: MIT
#pragma once
#include "common.h"

enum class Effect : int { None = 0, Spotlight, Squarelight, Magnifier, Laser, Draw, Count };
enum class HapticMode : int { Off = 0, HardPress = 1, EveryPress = 2 };
enum class ActionMode : int { Default = 0, CycleEffects = 1, BlackScreen = 2, ClearDrawing = 3 };

const wchar_t* EffectName(Effect e);       // "Spotlight", ...
const char* EffectKey(Effect e);           // "spotlight", ... (ini)
Effect EffectFromKey(const std::string& s, Effect fallback);

struct Settings {
  // What the highlight button does.
  Effect softPress = Effect::Spotlight;
  Effect hardPress = Effect::Laser;
  ActionMode actionButton = ActionMode::Default;
  int pressure = 2;         // hard press force: 0 lightest, 1 soft, 2 medium, 3 firm
  int hardCid = 0;          // control the hard press arrives as, if not 0x01A8 (finder)

  // Look (sizes are in pixels at 100% display scaling).
  int spotRadius = 190;     // 80..500
  int dimPercent = 72;      // 20..95
  int softness = 30;        // 0..100
  int squareWidth = 560;    // 200..1200 (height follows at 9:16 of width)
  int zoomTenths = 25;      // 15..50  => 1.5x .. 5.0x
  int lensRadius = 170;     // 80..400
  uint32_t laserColor = 0xFF2E2E;  // 0xRRGGBB
  int laserSize = 14;       // 6..30 (dot radius)
  uint32_t penColor = 0xFF3B30;
  int penWidth = 6;         // 2..24

  // Behaviour.
  bool hideCursor = true;
  bool recenter = true;
  bool clearInkOnSlide = true;
  HapticMode haptics = HapticMode::HardPress;
  int hapticStrength = 60;  // 0..100

  // Real laser (found by the laser finder in diagnostics).
  bool physicalLaser = false;
  int laserSrcCid = 0;      // control that should fire the laser
  int laserDstCid = 0;      // laser function it is remapped to (0 = native, no remap)
  bool laserKnown = false;  // laser finder succeeded

  bool paused = false;
  bool welcomed = false;    // first-run welcome shown

  void Load();
  void Save() const;
  void Sanitize();
  static std::wstring Path();
};

// Start-with-Windows lives in HKCU\...\Run, not in the ini.
bool AutostartEnabled();
void SetAutostart(bool on);
