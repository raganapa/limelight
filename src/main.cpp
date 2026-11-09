// SPDX-License-Identifier: MIT
// Limelight: tray app that turns the Logitech Spotlight 2 highlight button into
// smooth on-screen effects, without Logi Options+.
#include "common.h"

#include <dbt.h>
#include <tlhelp32.h>

#include <map>

#include "cursor.h"
#include "diag.h"
#include "overlay.h"
#include "presenter.h"
#include "settings.h"
#include "ui.h"

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "../res/resource.h"

namespace {

enum : UINT_PTR {
  TIMER_HOLD = 1,
  TIMER_MOTION,
  TIMER_OPTIONS_PLUS,
  TIMER_RESCAN,
  TIMER_SAVE,
  TIMER_DIAG,
  TIMER_TAP,
  TIMER_DEMO,
};

enum : UINT {
  HK_SPOTLIGHT = 1,
  HK_SQUARE,
  HK_MAGNIFIER,
  HK_LASER,
  HK_DRAW,
  HK_CLEAR,
};

enum : UINT {
  ID_STATUS = 100,
  ID_SOFT_BASE = 200,  // + Effect
  ID_HARD_BASE = 300,  // + Effect
  ID_REAL_LASER = 400,
  ID_CLEAR_INK,
  ID_PAUSE,
  ID_SETTINGS,
  ID_DIAGNOSTICS,
  ID_AUTOSTART,
  ID_QUIT,
};

const GUID kHidGuid = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

// Haptic waveforms of feature 0x19B0.
constexpr uint8_t kWaveSharp = 0x00;
constexpr uint8_t kWaveSubtle = 0x04;

bool IsLogiOptionsRunning() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof pe;
  bool found = false;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, L"logioptionsplus_agent.exe") == 0 ||
          _wcsicmp(pe.szExeFile, L"LogiOptionsPlus.exe") == 0 ||
          _wcsicmp(pe.szExeFile, L"LogiOptions.exe") == 0) {
        found = true;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return found;
}

void SendKeyTap(WORD vk) {
  INPUT in[2] = {};
  in[0].type = in[1].type = INPUT_KEYBOARD;
  in[0].ki.wVk = in[1].ki.wVk = vk;
  in[1].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(2, in, sizeof(INPUT));
}

void SendLeftClick() {
  INPUT in[2] = {};
  in[0].type = in[1].type = INPUT_MOUSE;
  in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
  in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
  SendInput(2, in, sizeof(INPUT));
}

class App : public UiHost {
 public:
  int Run(HINSTANCE inst, bool background, Effect demo = Effect::None);

  // UiHost
  Settings& Prefs() override { return s_; }
  void PrefsChanged() override;
  std::wstring StatusLine() override;
  int StatusLevel() override;
  void TryEffect(Effect e) override;
  void Preview(Effect e) override {
    previewEffect_ = e;
    UpdateEffect();
  }
  void OpenDiagnostics() override;
  bool HasHaptics() override { return presenter_.HasHaptics(); }
  bool HasActionButton() override { return presenter_.HasActionButton(); }
  bool Autostart() override { return AutostartEnabled(); }
  void SetAutostartPref(bool on) override { SetAutostart(on); }

  static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l);

 private:
  LRESULT Handle(UINT msg, WPARAM w, LPARAM l);
  void AddTrayIcon();
  void UpdateTray();
  void Balloon(const wchar_t* title, const wchar_t* text);
  void ShowMenu();
  void OnCommand(UINT id);
  void ApplySettings();
  Presenter::Config PresenterConfig() const;
  Effect Desired() const;
  void UpdateEffect();
  void Recenter();
  void OnButtons(LPARAM mask);
  void OnAction();
  void OnRawInput(HRAWINPUT h);
  int Classify(HANDLE dev);
  void OnRemoteMotion();
  void Latch(Effect e);
  void Haptic(uint8_t wf) {
    if (s_.haptics != HapticMode::Off) presenter_.PlayHaptic(wf);
  }

  HINSTANCE inst_ = nullptr;
  HWND hwnd_ = nullptr;
  NOTIFYICONDATAW nid_{};
  UINT wmTaskbarCreated_ = 0;
  HICON trayIcon_ = nullptr, appIcon_ = nullptr;
  HDEVNOTIFY devNotify_ = nullptr;
  Settings s_;
  Overlay overlay_;
  Presenter presenter_;

  LPARAM buttons_ = 0;
  bool hidppSeen_ = false;
  double lastButtonChange_ = -1e9;
  Effect latched_ = Effect::None;
  Effect previewEffect_ = Effect::None;
  double motionUntil_ = 0;
  int motionBurst_ = 0;
  double motionBurstStart_ = 0;
  double lastNudge_ = -1e9;
  Effect shown_ = Effect::None;
  bool shownFromRemote_ = false;
  double lastEffectEnd_ = -1e9;
  double holdHardUntil_ = 0;
  double softDownAt_ = 0;
  double lastTapAt_ = -1e9;
  bool optionsPlus_ = false;
  bool optionsPlusWarned_ = false;
  HANDLE diagProcess_ = nullptr;
  Effect demo_ = Effect::None;
  std::map<HANDLE, int> rawKinds_;
};

App* g_app = nullptr;

LRESULT CALLBACK App::WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  if (msg == WM_NCCREATE) {
    g_app->hwnd_ = h;
  }
  if (g_app && g_app->hwnd_ == h) return g_app->Handle(msg, w, l);
  return DefWindowProcW(h, msg, w, l);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS*) {
  syscursor::Restore();
  return EXCEPTION_CONTINUE_SEARCH;
}

int App::Run(HINSTANCE inst, bool background, Effect demo) {
  inst_ = inst;
  s_.Load();
  const bool firstRun = !s_.welcomed;

  appIcon_ = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
  trayIcon_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_TRAY), IMAGE_ICON,
                                            GetSystemMetrics(SM_CXSMICON),
                                            GetSystemMetrics(SM_CYSMICON), 0));
  if (!trayIcon_) trayIcon_ = appIcon_;

  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = APP_MAIN_CLASS;
  wc.hIcon = appIcon_;
  RegisterClassExW(&wc);
  // A hidden top-level window (not message-only) so display/device broadcasts reach us.
  CreateWindowExW(WS_EX_TOOLWINDOW, APP_MAIN_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, nullptr,
                  nullptr, inst, nullptr);
  if (!hwnd_) return 1;

  wmTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
  ChangeWindowMessageFilterEx(hwnd_, wmTaskbarCreated_, MSGFLT_ALLOW, nullptr);

  // Raw input: watch (never block) the remote's motion and slide keys.
  RAWINPUTDEVICE rid[2] = {};
  rid[0].usUsagePage = 0x01;
  rid[0].usUsage = 0x02;  // mouse
  rid[0].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
  rid[0].hwndTarget = hwnd_;
  rid[1].usUsagePage = 0x01;
  rid[1].usUsage = 0x06;  // keyboard
  rid[1].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
  rid[1].hwndTarget = hwnd_;
  if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)))
    Log("RegisterRawInputDevices failed: %lu", GetLastError());

  DEV_BROADCAST_DEVICEINTERFACE_W filter{};
  filter.dbcc_size = sizeof filter;
  filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
  filter.dbcc_classguid = kHidGuid;
  devNotify_ = RegisterDeviceNotificationW(hwnd_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

  struct HotKey {
    UINT id, vk;
  } keys[] = {{HK_SPOTLIGHT, VK_F1}, {HK_SQUARE, VK_F2}, {HK_MAGNIFIER, VK_F3},
              {HK_LASER, VK_F4},     {HK_DRAW, VK_F5},   {HK_CLEAR, VK_F6}};
  for (auto& k : keys)
    if (!RegisterHotKey(hwnd_, k.id, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, k.vk))
      Log("hotkey Ctrl+Alt+F%u is taken by another program", k.vk - VK_F1 + 1);

  overlay_.Start();
  overlay_.SetParams(OverlayParams::From(s_));
  presenter_.Configure(PresenterConfig());
  presenter_.Start(hwnd_);
  AddTrayIcon();

  optionsPlus_ = IsLogiOptionsRunning();
  SetTimer(hwnd_, TIMER_OPTIONS_PLUS, 30000, nullptr);

  if (firstRun) {
    s_.welcomed = true;
    s_.Save();
    ShowSettingsWindow(inst_, this);
    Balloon(L"Limelight is running",
            L"Hold the highlight button for the spotlight, press harder for the laser dot. "
            L"Settings live in the tray icon.");
  } else if (!background) {
    ShowSettingsWindow(inst_, this);
  }
  if (optionsPlus_) {
    optionsPlusWarned_ = true;
    Balloon(L"Logi Options+ is running",
            L"It also controls the Spotlight 2 and will fight with Limelight. Quit it from its "
            L"tray icon, or uninstall it.");
  }

  if (demo != Effect::None) {  // developer aid: --demo=<effect> shows it for 5 seconds
    Latch(demo);
    demo_ = demo;
    SetTimer(hwnd_, TIMER_DEMO, 5000, nullptr);
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (IsSettingsDialogMessage(&msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  CloseSettingsWindow();
  syscursor::Restore();
  presenter_.Stop();
  overlay_.Stop();
  Shell_NotifyIconW(NIM_DELETE, &nid_);
  if (devNotify_) UnregisterDeviceNotification(devNotify_);
  s_.Save();
  return 0;
}

void App::AddTrayIcon() {
  nid_ = NOTIFYICONDATAW{};
  nid_.cbSize = sizeof nid_;
  nid_.hWnd = hwnd_;
  nid_.uID = 1;
  nid_.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
  nid_.uCallbackMessage = WM_APP_TRAY;
  nid_.hIcon = trayIcon_;
  wcscpy_s(nid_.szTip, L"Limelight");
  Shell_NotifyIconW(NIM_ADD, &nid_);
  nid_.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &nid_);
  UpdateTray();
}

void App::UpdateTray() {
  std::wstring tip = L"Limelight — " + StatusLine();
  if (s_.paused) tip += L" (paused)";
  wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
  nid_.uFlags = NIF_TIP | NIF_SHOWTIP;
  Shell_NotifyIconW(NIM_MODIFY, &nid_);
  RefreshSettingsWindow();
}

void App::Balloon(const wchar_t* title, const wchar_t* text) {
  NOTIFYICONDATAW n = nid_;
  n.uFlags = NIF_INFO;
  wcsncpy_s(n.szInfoTitle, title, _TRUNCATE);
  wcsncpy_s(n.szInfo, text, _TRUNCATE);
  n.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
  n.hBalloonIcon = appIcon_;
  Shell_NotifyIconW(NIM_MODIFY, &n);
}

std::wstring App::StatusLine() {
  if (presenter_.Paused()) return L"Running diagnostics…";
  if (!presenter_.Connected()) return L"Looking for your Spotlight 2…";
  std::wstring t = presenter_.DeviceName();
  t += presenter_.Transport() == 2 ? L" · Bluetooth" : L" · USB receiver";
  const int b = presenter_.Battery();
  if (b >= 0) {
    t += L" · " + std::to_wstring(b) + L"%";
    if (presenter_.Charging()) t += L" charging";
  }
  if (optionsPlus_) t += L" · Logi Options+ is running";
  return t;
}

int App::StatusLevel() {
  if (!presenter_.Connected()) return 0;
  return optionsPlus_ ? 2 : 1;
}

Presenter::Config App::PresenterConfig() const {
  Presenter::Config c;
  c.divertSoft = !s_.paused;
  c.divertHard = !s_.paused;
  c.divertAction = !s_.paused && s_.actionButton != ActionMode::Default;
  c.laserOn = s_.physicalLaser && s_.laserKnown;
  c.laserSrc = uint16_t(s_.laserSrcCid);
  c.laserDst = uint16_t(s_.laserDstCid);
  c.hapticEnabled = s_.haptics != HapticMode::Off;
  c.hapticLevel = s_.hapticStrength;
  return c;
}

void App::ApplySettings() {
  s_.Sanitize();
  overlay_.SetParams(OverlayParams::From(s_));
  presenter_.Configure(PresenterConfig());
  UpdateEffect();
  UpdateTray();
}

void App::PrefsChanged() {
  ApplySettings();
  SetTimer(hwnd_, TIMER_SAVE, 400, nullptr);
}

void App::TryEffect(Effect e) {
  if (e == Effect::None) return;
  Latch(e);
}

void App::OpenDiagnostics() {
  if (diagProcess_) return;
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --diagnostics";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                     nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    diagProcess_ = pi.hProcess;
    SetTimer(hwnd_, TIMER_DIAG, 1000, nullptr);
  }
}

// -------------------------------------------------------------------------- effects
Effect App::Desired() const {
  if (s_.paused) return Effect::None;
  const double now = NowMs();
  const bool laserOnSoft = s_.physicalLaser && s_.laserKnown && s_.laserSrcCid == 0x00D8;
  const bool laserOnHard = s_.physicalLaser && s_.laserKnown && s_.laserSrcCid == 0x01A8;
  if (buttons_ & BTN_HARD) return laserOnHard ? Effect::None : s_.hardPress;
  if (buttons_ & BTN_SOFT) {
    if (now < holdHardUntil_) return laserOnHard ? Effect::None : s_.hardPress;
    return laserOnSoft ? Effect::None : s_.softPress;
  }
  if (now < motionUntil_) return s_.softPress;
  if (previewEffect_ != Effect::None) return previewEffect_;
  return latched_;
}

void App::Recenter() {
  // Prefer the PowerPoint slide show window, else the foreground window's monitor.
  HWND target = FindWindowW(L"screenClass", nullptr);
  if (!target) target = GetForegroundWindow();
  HMONITOR mon = target ? MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST)
                        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{sizeof mi};
  if (GetMonitorInfoW(mon, &mi))
    SetCursorPos((mi.rcMonitor.left + mi.rcMonitor.right) / 2,
                 (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2);
}

void App::UpdateEffect() {
  const Effect e = Desired();
  if (e == shown_) return;
  const bool previewing = previewEffect_ != Effect::None && e == previewEffect_ &&
                          !(buttons_ & (BTN_SOFT | BTN_HARD));
  const bool fromRemote =
      !previewing && (latched_ == Effect::None || (buttons_ & (BTN_SOFT | BTN_HARD)));
  if (shown_ == Effect::None && e != Effect::None && fromRemote && s_.recenter &&
      NowMs() - lastEffectEnd_ > 3000 && e != Effect::Draw)
    Recenter();
  shown_ = e;
  shownFromRemote_ = fromRemote;
  overlay_.SetEffect(e);
  if (e == Effect::None) {
    syscursor::Restore();
    lastEffectEnd_ = NowMs();
  } else if (s_.hideCursor && !previewing) {
    syscursor::Hide();
  } else {
    syscursor::Restore();
  }
}

void App::Latch(Effect e) {
  latched_ = (latched_ == e) ? Effect::None : e;
  if (latched_ != Effect::None)
    overlay_.Toast(std::wstring(EffectName(latched_)) + L"  —  Esc to stop");
  UpdateEffect();
}

void App::OnButtons(LPARAM mask) {
  const LPARAM old = buttons_;
  buttons_ = mask;
  hidppSeen_ = true;
  lastButtonChange_ = NowMs();
  motionUntil_ = 0;
  const bool hardDown = (mask & BTN_HARD) && !(old & BTN_HARD);
  const bool hardUp = !(mask & BTN_HARD) && (old & BTN_HARD);
  const bool anyDown = (mask & (BTN_SOFT | BTN_HARD)) && !(old & (BTN_SOFT | BTN_HARD));
  const bool allUp = !(mask & (BTN_SOFT | BTN_HARD)) && (old & (BTN_SOFT | BTN_HARD));
  // Letting go of a hard press passes through the light press for a moment;
  // keep the hard-press effect briefly so that does not flash the other effect.
  if (hardUp && (mask & BTN_SOFT)) {
    holdHardUntil_ = NowMs() + 170;
    SetTimer(hwnd_, TIMER_HOLD, 180, nullptr);
  }
  if (hardDown) {
    holdHardUntil_ = 0;
    Haptic(kWaveSharp);
  } else if (anyDown && s_.haptics == HapticMode::EveryPress) {
    Haptic(kWaveSubtle);
  }
  if (anyDown) {
    softDownAt_ = NowMs();
    latched_ = Effect::None;  // the remote takes over from a keyboard toggle
  }
  if (allUp && !(old & BTN_HARD)) {
    // Double-tap the highlight button = left click (for links and videos).
    const double now = NowMs();
    if (now - softDownAt_ < 260) {
      if (now - lastTapAt_ < 450) {
        lastTapAt_ = -1e9;
        SendLeftClick();
      } else {
        lastTapAt_ = now;
      }
    }
  }
  if ((mask & BTN_ACTION) && !(old & BTN_ACTION)) OnAction();
  UpdateEffect();
}

void App::OnAction() {
  switch (s_.actionButton) {
    case ActionMode::CycleEffects: {
      static const Effect order[] = {Effect::Spotlight, Effect::Squarelight, Effect::Magnifier,
                                     Effect::Laser, Effect::Draw};
      int i = 0;
      for (int k = 0; k < 5; ++k)
        if (order[k] == s_.softPress) i = k;
      s_.softPress = order[(i + 1) % 5];
      overlay_.Toast(std::wstring(L"Light press: ") + EffectName(s_.softPress));
      Haptic(kWaveSubtle);
      PrefsChanged();
      break;
    }
    case ActionMode::BlackScreen:
      SendKeyTap('B');
      break;
    case ActionMode::ClearDrawing:
      overlay_.ClearInk();
      break;
    default:
      break;
  }
}

int App::Classify(HANDLE dev) {
  auto it = rawKinds_.find(dev);
  if (it != rawKinds_.end()) return it->second;
  int kind = 0;
  UINT len = 0;
  GetRawInputDeviceInfoW(dev, RIDI_DEVICENAME, nullptr, &len);
  if (len) {
    std::wstring name(len + 1, L'\0');
    if (GetRawInputDeviceInfoW(dev, RIDI_DEVICENAME, name.data(), &len) != UINT(-1)) {
      for (auto& c : name) c = wchar_t(towlower(c));
      if (name.find(L"pid&b506") != std::wstring::npos || name.find(L"pid_b506") != std::wstring::npos ||
          name.find(L"pid&b503") != std::wstring::npos || name.find(L"pid_b503") != std::wstring::npos)
        kind = 1;  // the Spotlight itself over Bluetooth: unambiguous
      else if (name.find(L"vid_046d&pid_c548") != std::wstring::npos ||
               name.find(L"vid_046d&pid_c53e") != std::wstring::npos)
        kind = 2;  // a Logitech receiver: may also carry other devices
    }
  }
  rawKinds_[dev] = kind;
  return kind;
}

void App::OnRemoteMotion() {
  const double now = NowMs();
  if (now - motionBurstStart_ > 200) {
    motionBurstStart_ = now;
    motionBurst_ = 0;
  }
  ++motionBurst_;
  if (buttons_ & (BTN_SOFT | BTN_HARD)) return;   // HID++ is doing its job
  if (now - lastButtonChange_ < 400) return;      // trailing motion after a release
  if (motionBurst_ < 4) return;                   // ignore stray packets
  // The remote moves the pointer but we got no button event: it is not
  // configured (asleep, just reconnected, or HID++ unavailable). Fall back to
  // "motion means pressed" and ask the presenter to set the device up again.
  motionUntil_ = now + 250;
  SetTimer(hwnd_, TIMER_MOTION, 60, nullptr);
  if (presenter_.Connected() && now - lastNudge_ > 2000) {
    lastNudge_ = now;
    presenter_.Reconfigure();
  } else if (!presenter_.Connected() && now - lastNudge_ > 4000) {
    lastNudge_ = now;
    presenter_.Rescan();
  }
  UpdateEffect();
}

void App::OnRawInput(HRAWINPUT h) {
  alignas(8) BYTE buf[512];
  UINT size = sizeof buf;
  if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
  const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
  if (ri->header.dwType == RIM_TYPEMOUSE) {
    if ((ri->data.mouse.lLastX || ri->data.mouse.lLastY) && Classify(ri->header.hDevice) == 1)
      OnRemoteMotion();
  } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
    const RAWKEYBOARD& kb = ri->data.keyboard;
    if (kb.Message != WM_KEYDOWN && kb.Message != WM_SYSKEYDOWN) return;
    switch (kb.VKey) {
      case VK_ESCAPE:
        if (latched_ != Effect::None) {
          latched_ = Effect::None;
          UpdateEffect();
        }
        if (overlay_.HasInk()) overlay_.ClearInk();
        break;
      case VK_LEFT:
      case VK_RIGHT:
      case VK_PRIOR:
      case VK_NEXT:
      case VK_SPACE:
        if (s_.clearInkOnSlide && overlay_.HasInk()) overlay_.ClearInk();
        break;
    }
  }
}

// -------------------------------------------------------------------------- tray menu
void App::ShowMenu() {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING | MF_GRAYED, ID_STATUS, StatusLine().c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  auto effectMenu = [&](UINT base, Effect current) {
    HMENU sub = CreatePopupMenu();
    const Effect list[] = {Effect::Spotlight, Effect::Squarelight, Effect::Magnifier,
                           Effect::Laser, Effect::Draw, Effect::None};
    for (Effect e : list)
      AppendMenuW(sub, MF_STRING | (e == current ? MF_CHECKED : 0), base + UINT(e), EffectName(e));
    return sub;
  };
  AppendMenuW(m, MF_POPUP, UINT_PTR(effectMenu(ID_SOFT_BASE, s_.softPress)), L"Light press");
  AppendMenuW(m, MF_POPUP, UINT_PTR(effectMenu(ID_HARD_BASE, s_.hardPress)), L"Hard press");
  if (s_.laserKnown)
    AppendMenuW(m, MF_STRING | (s_.physicalLaser ? MF_CHECKED : 0), ID_REAL_LASER,
                L"Real laser");
  else
    AppendMenuW(m, MF_STRING | MF_GRAYED, ID_REAL_LASER, L"Real laser (run the Laser finder)");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (overlay_.HasInk() ? 0 : MF_GRAYED), ID_CLEAR_INK,
              L"Clear drawing\tCtrl+Alt+F6");
  AppendMenuW(m, MF_STRING | (s_.paused ? MF_CHECKED : 0), ID_PAUSE, L"Pause effects");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | MF_DEFAULT, ID_SETTINGS, L"Settings…");
  AppendMenuW(m, MF_STRING, ID_DIAGNOSTICS, L"Laser finder && diagnostics…");
  AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART,
              L"Start with Windows");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit Limelight");
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd_);
  const UINT cmd = UINT(TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                         pt.x, pt.y, hwnd_, nullptr));
  DestroyMenu(m);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  if (cmd) OnCommand(cmd);
}

void App::OnCommand(UINT id) {
  if (id >= ID_SOFT_BASE && id < ID_SOFT_BASE + UINT(Effect::Count)) {
    s_.softPress = Effect(id - ID_SOFT_BASE);
    PrefsChanged();
    return;
  }
  if (id >= ID_HARD_BASE && id < ID_HARD_BASE + UINT(Effect::Count)) {
    s_.hardPress = Effect(id - ID_HARD_BASE);
    PrefsChanged();
    return;
  }
  switch (id) {
    case ID_REAL_LASER:
      if (s_.laserKnown) {
        s_.physicalLaser = !s_.physicalLaser;
        PrefsChanged();
      }
      break;
    case ID_CLEAR_INK:
      overlay_.ClearInk();
      break;
    case ID_PAUSE:
      s_.paused = !s_.paused;
      latched_ = Effect::None;
      PrefsChanged();
      break;
    case ID_SETTINGS:
      ShowSettingsWindow(inst_, this);
      break;
    case ID_DIAGNOSTICS:
      OpenDiagnostics();
      break;
    case ID_AUTOSTART:
      SetAutostart(!AutostartEnabled());
      RefreshSettingsWindow();
      break;
    case ID_QUIT:
      DestroyWindow(hwnd_);
      break;
  }
}

// -------------------------------------------------------------------------- messages
LRESULT App::Handle(UINT msg, WPARAM w, LPARAM l) {
  if (msg == wmTaskbarCreated_ && wmTaskbarCreated_) {
    AddTrayIcon();
    return 0;
  }
  switch (msg) {
    case WM_APP_TRAY:
      switch (LOWORD(l)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
          ShowMenu();
          break;
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONUP:
          ShowSettingsWindow(inst_, this);
          break;
      }
      return 0;

    case WM_APP_PRESENTER:
      switch (w) {
        case PE_BUTTONS:
          OnButtons(l);
          break;
        case PE_CONNECTED:
          buttons_ = 0;
          UpdateEffect();
          UpdateTray();
          break;
        case PE_DISCONNECTED:
          buttons_ = 0;
          UpdateEffect();
          UpdateTray();
          break;
        default:
          UpdateTray();
          break;
      }
      return 0;

    case WM_INPUT:
      OnRawInput(reinterpret_cast<HRAWINPUT>(l));
      return DefWindowProcW(hwnd_, msg, w, l);

    case WM_INPUT_DEVICE_CHANGE:
      rawKinds_.erase(reinterpret_cast<HANDLE>(l));
      return 0;

    case WM_HOTKEY:
      switch (w) {
        case HK_SPOTLIGHT: Latch(Effect::Spotlight); break;
        case HK_SQUARE: Latch(Effect::Squarelight); break;
        case HK_MAGNIFIER: Latch(Effect::Magnifier); break;
        case HK_LASER: Latch(Effect::Laser); break;
        case HK_DRAW: Latch(Effect::Draw); break;
        case HK_CLEAR: overlay_.ClearInk(); break;
      }
      return 0;

    case WM_TIMER:
      switch (w) {
        case TIMER_HOLD:
          KillTimer(hwnd_, TIMER_HOLD);
          UpdateEffect();
          break;
        case TIMER_DEMO:
          KillTimer(hwnd_, TIMER_DEMO);
          if (latched_ == demo_) Latch(demo_);
          break;
        case TIMER_MOTION:
          if (NowMs() >= motionUntil_) KillTimer(hwnd_, TIMER_MOTION);
          UpdateEffect();
          break;
        case TIMER_OPTIONS_PLUS: {
          const bool running = IsLogiOptionsRunning();
          if (running != optionsPlus_) {
            optionsPlus_ = running;
            UpdateTray();
            if (running && !optionsPlusWarned_) {
              optionsPlusWarned_ = true;
              Balloon(L"Logi Options+ is running",
                      L"It also controls the Spotlight 2 and will fight with Limelight.");
            }
            if (!running) presenter_.Reconfigure();
          }
          break;
        }
        case TIMER_RESCAN:
          KillTimer(hwnd_, TIMER_RESCAN);
          presenter_.Rescan();
          break;
        case TIMER_SAVE:
          KillTimer(hwnd_, TIMER_SAVE);
          s_.Save();
          break;
        case TIMER_DIAG:
          if (!diagProcess_ || WaitForSingleObject(diagProcess_, 0) == WAIT_OBJECT_0) {
            KillTimer(hwnd_, TIMER_DIAG);
            if (diagProcess_) CloseHandle(diagProcess_);
            diagProcess_ = nullptr;
            if (presenter_.Paused()) presenter_.Pause(false);
            s_.Load();
            ApplySettings();
          }
          break;
      }
      return 0;

    case WM_APP_PAUSE_DEVICE: {
      // The diagnostics tool wants the remote. wParam = its process id.
      presenter_.Pause(true);
      if (!diagProcess_ && w) {
        diagProcess_ = OpenProcess(SYNCHRONIZE, FALSE, DWORD(w));
        SetTimer(hwnd_, TIMER_DIAG, 1000, nullptr);
      }
      UpdateTray();
      return 1;
    }
    case WM_APP_RESUME_DEVICE:
      presenter_.Pause(false);
      UpdateTray();
      return 1;
    case WM_APP_RELOAD_SETTINGS:
      s_.Load();
      ApplySettings();
      return 1;
    case WM_APP_SHOW_SETTINGS:
      ShowSettingsWindow(inst_, this);
      return 0;
    case WM_APP_QUIT:
      DestroyWindow(hwnd_);
      return 0;

    case WM_DEVICECHANGE:
      if (w == DBT_DEVICEARRIVAL || w == DBT_DEVICEREMOVECOMPLETE)
        SetTimer(hwnd_, TIMER_RESCAN, 700, nullptr);
      return TRUE;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
      overlay_.DisplayChanged();
      return 0;

    case WM_POWERBROADCAST:
      if (w == PBT_APMRESUMEAUTOMATIC) SetTimer(hwnd_, TIMER_RESCAN, 1500, nullptr);
      return TRUE;

    case WM_SETTINGCHANGE:
      RefreshSettingsWindow();
      return 0;

    case WM_ENDSESSION:
      if (w) {
        syscursor::Restore();
        s_.Save();
      }
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd_, msg, w, l);
}

int QuitRunningInstance() {
  HWND h = FindWindowW(APP_MAIN_CLASS, nullptr);
  if (!h) return 0;
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
  PostMessageW(h, WM_APP_QUIT, 0, 0);
  if (p) {
    WaitForSingleObject(p, 5000);
    CloseHandle(p);
  }
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
  const std::wstring cmd = cmdLine ? cmdLine : L"";
  auto has = [&](const wchar_t* flag) { return cmd.find(flag) != std::wstring::npos; };

  if (has(L"--diagnostics")) return RunDiagnostics();
  if (has(L"--quit")) return QuitRunningInstance();

  LogInit();
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\LimelightSingleInstance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND h = FindWindowW(APP_MAIN_CLASS, nullptr))
      if (!has(L"--background")) {
        AllowSetForegroundWindow(ASFW_ANY);
        PostMessageW(h, WM_APP_SHOW_SETTINGS, 0, 0);
      }
    return 0;
  }

  Log("Limelight %s starting", Narrow(APP_VERSION).c_str());
  SetUnhandledExceptionFilter(CrashFilter);
  syscursor::RepairAfterCrash();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Gdiplus::GdiplusStartupInput gsi;
  ULONG_PTR gdipToken = 0;
  Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr);

  App app;
  g_app = &app;
  Effect demo = Effect::None;
  const size_t dpos = cmd.find(L"--demo=");
  if (dpos != std::wstring::npos) {
    std::wstring v = cmd.substr(dpos + 7);
    v = v.substr(0, v.find(L' '));
    demo = EffectFromKey(Narrow(v), Effect::None);
  }
  const int rc = app.Run(inst, has(L"--background"), demo);
  g_app = nullptr;

  Gdiplus::GdiplusShutdown(gdipToken);
  CoUninitialize();
  if (mutex) CloseHandle(mutex);
  Log("Limelight exiting");
  return rc;
}
