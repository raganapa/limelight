// SPDX-License-Identifier: MIT
// Runs the real Presenter / HID++ code against the simulated remote in mock_hid.cpp.
//   ./build.sh test   (cross-compiles; run the exe on Windows or under Wine)
#include <cstdio>

#include "../src/presenter.h"
#include "mock_hid.h"

namespace {
std::vector<std::pair<WPARAM, LPARAM>> g_events;
int g_failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (cond) {                                                         \
      printf("  ok    %s\n", #cond);                                    \
    } else {                                                            \
      printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);               \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

LRESULT CALLBACK Wnd(HWND h, UINT msg, WPARAM w, LPARAM l) {
  if (msg == WM_APP_PRESENTER) {
    g_events.push_back({w, l});
    return 0;
  }
  return DefWindowProcW(h, msg, w, l);
}

void Pump(int ms) {
  const double end = NowMs() + ms;
  while (NowMs() < end) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    Sleep(5);
  }
}

bool WaitFor(WPARAM ev, int ms) {
  const double end = NowMs() + ms;
  while (NowMs() < end) {
    Pump(20);
    for (auto& e : g_events)
      if (e.first == ev) return true;
  }
  return false;
}

// Effective state in the simulated remote: 1 = diverted, 0 = normal, 0xEE = never touched.
// Raw XY must never end up on (it would freeze the pointer).
uint8_t Rep(uint16_t cid) {
  std::lock_guard<std::mutex> lk(mock::m);
  auto it = mock::reporting.find(cid);
  if (it == mock::reporting.end()) return 0xEE;
  if (it->second & 0x10) return 0x99;
  return it->second & 0x01;
}

uint16_t Remap(uint16_t cid) {
  std::lock_guard<std::mutex> lk(mock::m);
  auto it = mock::remaps.find(cid);
  return it == mock::remaps.end() ? 0 : it->second;
}

std::vector<LPARAM> Buttons() {
  std::vector<LPARAM> v;
  for (auto& e : g_events)
    if (e.first == PE_BUTTONS) v.push_back(e.second);
  return v;
}
}  // namespace

int main(int argc, char** argv) {
  const bool rx = argc > 1 && strcmp(argv[1], "receiver") == 0;
  const bool quirky = argc > 1 && strcmp(argv[1], "quirky") == 0;
  mock::SetReceiverMode(rx);
  mock::SetQuirky(quirky);
  printf("=== %s ===\n", rx ? "Logi Bolt receiver"
                      : quirky ? "Bluetooth, quirky remote (hard press not flagged, events "
                                 "tagged, first command dropped)"
                               : "Bluetooth");
  WNDCLASSW wc{};
  wc.lpfnWndProc = Wnd;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"PresenterTest";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowW(L"PresenterTest", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                            wc.hInstance, nullptr);

  printf("connect\n");
  Presenter p;
  Presenter::Config c;
  p.Configure(c);
  p.Start(hwnd);
  CHECK(WaitFor(PE_CONNECTED, 6000));
  CHECK(p.Connected());
  CHECK(p.Transport() == (rx ? 1 : 2));
  CHECK(p.DeviceName() == L"Spotlight 2");
  CHECK(p.Battery() == 83);
  CHECK(p.HasHaptics());
  CHECK(p.HasActionButton());
  CHECK(Rep(0x00D8) == 1);  // diverted
  CHECK(Rep(0x01A8) == 1);
  {
    std::lock_guard<std::mutex> lk(mock::m);
    CHECK(mock::lastFlags[0x00D8] == 0x23);  // ...with raw XY explicitly switched off
  }
  CHECK(Rep(0x01B0) == 0xEE);  // action button left alone by default

  printf("buttons\n");
  g_events.clear();
  mock::PressButtons({0x00D8});
  Pump(150);
  mock::PressButtons({0x00D8, 0x01A8});
  Pump(150);
  mock::PressButtons({0x00D8});
  Pump(150);
  mock::PressButtons({});
  Pump(120);
  auto b = Buttons();
  CHECK(b.size() == 4);
  if (b.size() == 4) {
    CHECK(b[0] == BTN_SOFT);
    CHECK(b[1] == (BTN_SOFT | BTN_HARD));
    CHECK(b[2] == BTN_SOFT);
    CHECK(b[3] == 0);
  }

  printf("pressure (how hard the hard press is)\n");
  if (!quirky) {
    CHECK(p.HasPressure());
    CHECK(mock::ForceValue() == 200);  // Medium = the remote's own default, sent at connect
    c.pressure = 0;
    p.Configure(c);
    Pump(400);
    CHECK(mock::ForceValue() == 120);  // Soft: halfway to the lightest
    c.pressure = 2;
    p.Configure(c);
    Pump(400);
    CHECK(mock::ForceValue() == 300);  // Firm: halfway to the firmest
    c.pressure = 1;
    p.Configure(c);
    Pump(400);
    CHECK(mock::ForceValue() == 200);
  } else {
    CHECK(!p.HasPressure());  // this remote has no such feature: nothing is sent
  }
  g_events.clear();
  mock::Press(300);  // a firm press
  Pump(150);
  mock::Press(0);
  Pump(150);
  b = Buttons();
  CHECK(b.size() == 2 && b[0] == BTN_HARD && b[1] == 0);

  printf("remote lost its pressure setting without telling us\n");
  Pump(2200);  // the setup check runs at most every few seconds
  mock::ForgetPressure();
  g_events.clear();
  mock::Press(60);  // light press: Limelight checks the setup straight away...
  Pump(300);
  mock::Press(300);  // ...so pressing all the way still works
  Pump(150);
  mock::Press(0);
  Pump(150);
  b = Buttons();
  CHECK(b.size() == 3 && b[0] == BTN_SOFT && b[1] == BTN_HARD && b[2] == 0);
  if (!quirky) CHECK(mock::ForceValue() == 200);

  printf("remote stopped reporting the hard press without telling us\n");
  Pump(3200);
  mock::ForgetHardPress();
  g_events.clear();
  mock::Press(60);
  Pump(300);
  mock::Press(300);
  Pump(150);
  mock::Press(0);
  Pump(150);
  b = Buttons();
  CHECK(b.size() == 3 && b[0] == BTN_SOFT && b[1] == BTN_HARD && b[2] == 0);
  CHECK(Rep(0x01A8) == 1);

  printf("haptics\n");
  size_t before;
  {
    std::lock_guard<std::mutex> lk(mock::m);
    before = mock::fromHost.size();
  }
  p.PlayHaptic(0x07);
  Pump(250);
  bool played = false;
  {
    std::lock_guard<std::mutex> lk(mock::m);
    for (size_t i = before; i < mock::fromHost.size(); ++i) {
      const auto& q = mock::fromHost[i];
      if (q[2] == 20 && (q[3] >> 4) == 4 && q[4] == 0x07) played = true;
    }
  }
  CHECK(played);

  printf("action button + laser remap\n");
  c.divertAction = true;
  p.Configure(c);
  Pump(400);
  CHECK(Rep(0x01B0) == 1);
  c.laserOn = true;
  c.laserSrc = 0x00D8;
  c.laserDst = 0x00FC;
  p.Configure(c);
  Pump(400);
  CHECK(Rep(0x00D8) == 0);   // light press handed back to the remote...
  CHECK(Remap(0x00D8) == 0x00FC);  // ...and remapped to the laser
  c.laserOn = false;
  p.Configure(c);
  Pump(400);
  CHECK(Remap(0x00D8) == 0x00D8);  // remap undone
  CHECK(Rep(0x00D8) == 1);      // diverted again

  printf("remote slept and woke up\n");
  mock::ForgetSetup();
  mock::AnnounceReconnect();
  Pump(1500);
  CHECK(Rep(0x00D8) == 1);
  CHECK(Rep(0x01A8) == 1);
  if (!quirky) CHECK(mock::ForceValue() == 200);  // pressure sent again

  printf("pause / resume (diagnostics)\n");
  p.Pause(true);
  CHECK(p.Paused());
  CHECK(Rep(0x00D8) == 0);
  CHECK(Rep(0x01A8) == 0);
  CHECK(Rep(0x01B0) == 0);
  g_events.clear();
  p.Pause(false);
  CHECK(WaitFor(PE_CONNECTED, 6000));
  CHECK(Rep(0x00D8) == 1);

  printf("shutdown\n");
  p.Stop();
  CHECK(Rep(0x00D8) == 0);
  CHECK(Rep(0x01A8) == 0);
  CHECK(mock::dropped == 0);  // never sent commands faster than the remote accepts
  if (quirky) CHECK(mock::DeliberateDrops() == 1);

  printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures,
         g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
