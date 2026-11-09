// SPDX-License-Identifier: MIT
#include "settings.h"

static const char* kEffectKeys[] = {"none", "spotlight", "squarelight", "magnifier", "laser", "draw"};
static const wchar_t* kEffectNames[] = {L"Nothing", L"Spotlight", L"Squarelight", L"Magnifier",
                                        L"Laser dot", L"Draw"};

const wchar_t* EffectName(Effect e) {
  int i = int(e);
  return (i >= 0 && i < int(Effect::Count)) ? kEffectNames[i] : L"?";
}
const char* EffectKey(Effect e) {
  int i = int(e);
  return (i >= 0 && i < int(Effect::Count)) ? kEffectKeys[i] : "none";
}
Effect EffectFromKey(const std::string& s, Effect fallback) {
  for (int i = 0; i < int(Effect::Count); ++i)
    if (_stricmp(s.c_str(), kEffectKeys[i]) == 0) return Effect(i);
  return fallback;
}

std::wstring Settings::Path() { return AppDataDir(true) + L"\\settings.ini"; }

static int GetInt(const wchar_t* sec, const wchar_t* key, int def, const std::wstring& path) {
  return int(GetPrivateProfileIntW(sec, key, def, path.c_str()));
}
static std::string GetStr(const wchar_t* sec, const wchar_t* key, const char* def,
                          const std::wstring& path) {
  wchar_t buf[256];
  GetPrivateProfileStringW(sec, key, Widen(def).c_str(), buf, 256, path.c_str());
  return Narrow(buf);
}
static uint32_t GetColor(const wchar_t* sec, const wchar_t* key, uint32_t def,
                         const std::wstring& path) {
  char d[16];
  snprintf(d, sizeof d, "%06X", unsigned(def));
  std::string s = GetStr(sec, key, d, path);
  if (!s.empty() && s[0] == '#') s.erase(0, 1);
  char* end = nullptr;
  unsigned long v = strtoul(s.c_str(), &end, 16);
  return (end && *end == 0 && s.size() == 6) ? uint32_t(v) : def;
}
static void PutInt(const wchar_t* sec, const wchar_t* key, int v, const std::wstring& path) {
  WritePrivateProfileStringW(sec, key, std::to_wstring(v).c_str(), path.c_str());
}
static void PutStr(const wchar_t* sec, const wchar_t* key, const std::string& v,
                   const std::wstring& path) {
  WritePrivateProfileStringW(sec, key, Widen(v).c_str(), path.c_str());
}
static void PutColor(const wchar_t* sec, const wchar_t* key, uint32_t v, const std::wstring& path) {
  wchar_t b[16];
  swprintf(b, 16, L"#%06X", unsigned(v & 0xFFFFFF));
  WritePrivateProfileStringW(sec, key, b, path.c_str());
}

void Settings::Load() {
  const std::wstring p = Path();
  Settings d;  // defaults
  softPress = EffectFromKey(GetStr(L"Buttons", L"LightPress", EffectKey(d.softPress), p), d.softPress);
  hardPress = EffectFromKey(GetStr(L"Buttons", L"HardPress", EffectKey(d.hardPress), p), d.hardPress);
  actionButton = ActionMode(GetInt(L"Buttons", L"ActionButton", int(d.actionButton), p));
  pressure = GetInt(L"Buttons", L"HardPressForce", d.pressure, p);

  spotRadius = GetInt(L"Look", L"SpotlightSize", d.spotRadius, p);
  dimPercent = GetInt(L"Look", L"Dim", d.dimPercent, p);
  softness = GetInt(L"Look", L"SoftEdge", d.softness, p);
  squareWidth = GetInt(L"Look", L"SquareWidth", d.squareWidth, p);
  zoomTenths = GetInt(L"Look", L"ZoomTenths", d.zoomTenths, p);
  lensRadius = GetInt(L"Look", L"LensSize", d.lensRadius, p);
  laserColor = GetColor(L"Look", L"LaserColor", d.laserColor, p);
  laserSize = GetInt(L"Look", L"LaserSize", d.laserSize, p);
  penColor = GetColor(L"Look", L"PenColor", d.penColor, p);
  penWidth = GetInt(L"Look", L"PenWidth", d.penWidth, p);

  hideCursor = GetInt(L"Behaviour", L"HideCursor", d.hideCursor, p) != 0;
  recenter = GetInt(L"Behaviour", L"StartFromCentre", d.recenter, p) != 0;
  clearInkOnSlide = GetInt(L"Behaviour", L"ClearDrawingOnSlideChange", d.clearInkOnSlide, p) != 0;
  haptics = HapticMode(GetInt(L"Behaviour", L"Vibration", int(d.haptics), p));
  hapticStrength = GetInt(L"Behaviour", L"VibrationStrength", d.hapticStrength, p);
  paused = GetInt(L"Behaviour", L"Paused", 0, p) != 0;
  welcomed = GetInt(L"Behaviour", L"Welcomed", 0, p) != 0;

  physicalLaser = GetInt(L"Laser", L"Enabled", 0, p) != 0;
  laserKnown = GetInt(L"Laser", L"Found", 0, p) != 0;
  laserSrcCid = GetInt(L"Laser", L"SourceCid", 0, p);
  laserDstCid = GetInt(L"Laser", L"TargetCid", 0, p);
  Sanitize();
}

void Settings::Sanitize() {
  auto validEffect = [](Effect e) { return int(e) >= 0 && int(e) < int(Effect::Count); };
  if (!validEffect(softPress)) softPress = Effect::Spotlight;
  if (!validEffect(hardPress)) hardPress = Effect::Laser;
  if (int(actionButton) < 0 || int(actionButton) > 3) actionButton = ActionMode::Default;
  pressure = Clamp(pressure, 0, 2);
  spotRadius = Clamp(spotRadius, 80, 500);
  dimPercent = Clamp(dimPercent, 20, 95);
  softness = Clamp(softness, 0, 100);
  squareWidth = Clamp(squareWidth, 200, 1200);
  zoomTenths = Clamp(zoomTenths, 15, 50);
  lensRadius = Clamp(lensRadius, 80, 400);
  laserSize = Clamp(laserSize, 6, 30);
  penWidth = Clamp(penWidth, 2, 24);
  if (int(haptics) < 0 || int(haptics) > 2) haptics = HapticMode::HardPress;
  hapticStrength = Clamp(hapticStrength, 0, 100);
  if (!laserKnown) physicalLaser = false;
}

void Settings::Save() const {
  const std::wstring p = Path();
  PutStr(L"Buttons", L"LightPress", EffectKey(softPress), p);
  PutStr(L"Buttons", L"HardPress", EffectKey(hardPress), p);
  PutInt(L"Buttons", L"ActionButton", int(actionButton), p);
  PutInt(L"Buttons", L"HardPressForce", pressure, p);

  PutInt(L"Look", L"SpotlightSize", spotRadius, p);
  PutInt(L"Look", L"Dim", dimPercent, p);
  PutInt(L"Look", L"SoftEdge", softness, p);
  PutInt(L"Look", L"SquareWidth", squareWidth, p);
  PutInt(L"Look", L"ZoomTenths", zoomTenths, p);
  PutInt(L"Look", L"LensSize", lensRadius, p);
  PutColor(L"Look", L"LaserColor", laserColor, p);
  PutInt(L"Look", L"LaserSize", laserSize, p);
  PutColor(L"Look", L"PenColor", penColor, p);
  PutInt(L"Look", L"PenWidth", penWidth, p);

  PutInt(L"Behaviour", L"HideCursor", hideCursor, p);
  PutInt(L"Behaviour", L"StartFromCentre", recenter, p);
  PutInt(L"Behaviour", L"ClearDrawingOnSlideChange", clearInkOnSlide, p);
  PutInt(L"Behaviour", L"Vibration", int(haptics), p);
  PutInt(L"Behaviour", L"VibrationStrength", hapticStrength, p);
  PutInt(L"Behaviour", L"Paused", paused, p);
  PutInt(L"Behaviour", L"Welcomed", welcomed, p);

  PutInt(L"Laser", L"Enabled", physicalLaser, p);
  PutInt(L"Laser", L"Found", laserKnown, p);
  PutInt(L"Laser", L"SourceCid", laserSrcCid, p);
  PutInt(L"Laser", L"TargetCid", laserDstCid, p);
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool AutostartEnabled() {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
  DWORD type = 0, size = 0;
  bool on = RegQueryValueExW(k, APP_NAME, nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
  RegCloseKey(k);
  return on;
}

void SetAutostart(bool on) {
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) !=
      ERROR_SUCCESS)
    return;
  if (on) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --background";
    RegSetValueExW(k, APP_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                   DWORD((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(k, APP_NAME);
  }
  RegCloseKey(k);
}
