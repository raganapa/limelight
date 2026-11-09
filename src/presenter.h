// SPDX-License-Identifier: MIT
// Finds the Spotlight 2 (Bluetooth or Logi Bolt), keeps it configured across
// sleep/wake, and reports button presses to the main window. All blocking
// HID++ traffic happens on a worker thread; button events are posted to the UI
// straight from the HID reader thread so there is no added latency.
#pragma once
#include "common.h"

#include <condition_variable>
#include <memory>
#include <set>
#include <thread>

#include "hidpp.h"

class Presenter {
 public:
  struct Config {
    bool divertSoft = true;
    bool divertHard = true;
    bool divertAction = false;
    bool laserOn = false;
    uint16_t laserSrc = 0, laserDst = 0;
    bool hapticEnabled = true;
    int hapticLevel = 60;
    int pressure = 2;  // hard press force: 0 lightest, 1 soft, 2 medium, 3 firm
    uint16_t hardCid = 0;  // the hard press arrives as this control too (0 = only 0x01A8)
    bool operator==(const Config& o) const {
      return divertSoft == o.divertSoft && divertHard == o.divertHard &&
             divertAction == o.divertAction && laserOn == o.laserOn && laserSrc == o.laserSrc &&
             laserDst == o.laserDst && hapticEnabled == o.hapticEnabled &&
             hapticLevel == o.hapticLevel && pressure == o.pressure && hardCid == o.hardCid;
    }
  };

  void Start(HWND notify);
  void Stop();
  void Configure(const Config& c);
  void PlayHaptic(uint8_t waveform);
  void Rescan();
  void Reconfigure();
  // Release the device so another program (the diagnostics tool) can use it.
  void Pause(bool pause);

  bool Connected() const { return connected_.load(); }
  int Transport() const { return transport_.load(); }  // 0 none, 1 Bolt receiver, 2 Bluetooth
  int Battery() const { return battery_.load(); }      // -1 unknown
  bool Charging() const { return charging_.load(); }
  bool HasHaptics() const { return hasHaptics_.load(); }
  bool HasActionButton() const { return hasAction_.load(); }
  bool HasPressure() const { return hasPressure_.load(); }  // force needed is adjustable
  bool Paused() const { return paused_.load(); }
  std::wstring DeviceName() const;

 private:
  void Run();
  bool TryConnect();
  void Adopt(std::unique_ptr<hidpp::Device> d, const std::string& name);
  void Disconnect();
  void ApplyConfig(bool deviceForgot);
  void RestoreDevice();
  void QuickCheck();
  uint8_t DivertFlags(uint16_t cid, bool on) const;
  void ReadForceButtons();
  void ApplyPressure(int level, bool always);
  bool CheckPressure(int level);
  void ReadBattery();
  void OnNotify(const uint8_t* r, size_t n);
  void OnGone();
  void Post(WPARAM ev, LPARAM v) { if (hwnd_) PostMessageW(hwnd_, WM_APP_PRESENTER, ev, v); }

  HWND hwnd_ = nullptr;
  std::thread th_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  // requests (guarded by m_)
  bool quit_ = false, rescan_ = true, reconfigure_ = false, gone_ = false;
  bool checkReq_ = false;  // a light press just started: check the setup now
  bool pauseReq_ = false, resumeReq_ = false, cfgDirty_ = true;
  std::vector<uint8_t> hapticQueue_;
  Config cfg_;
  std::wstring name_;
  std::condition_variable pausedCv_;

  // worker thread only
  std::unique_ptr<hidpp::Device> dev_;
  std::vector<hidpp::Control> controls_;
  std::set<uint16_t> diverted_;
  std::set<uint16_t> unsupported_;  // controls the remote said it does not have
  bool laserApplied_ = false;
  uint16_t laserAppliedSrc_ = 0;
  double lastAttempt_ = -1e9, lastBattery_ = 0, lastReconfigure_ = 0;
  double retryAt_ = 0, nextCheck_ = 0;
  int retryRounds_ = 0;
  bool readBackUnreliable_ = false;
  bool needSetup_ = false;  // some button could not be set up yet
  int failedAttempts_ = 0;
  int checkLogs_ = 0;       // limits the log lines QuickCheck writes
  // Force sensing (0x19C0): how hard each pressure-sensing button must be pressed.
  struct ForceButton {
    uint8_t index = 0;
    bool changeable = false, curOk = false, appliedOk = false;
    uint16_t def = 0, max = 0, min = 0, cur = 0;
    uint16_t applied = 0;  // what the remote reported back after we set it
  };
  std::vector<ForceButton> force_;
  int pressureApplied_ = -1;
  // Writes a force value in the layout the remote reads back (fresh = a read just done).
  hidpp::Reply WriteForce(ForceButton& f, uint16_t value, const hidpp::Reply* fresh);

  // read from reader threads
  std::atomic<uint8_t> devIdx_{0xFF}, idxReprog_{0}, idxHaptic_{0}, idxBattery_{0},
      idxWireless_{0}, idxForce_{0};
  std::atomic<bool> viaReceiver_{false}, unifiedBattery_{true};
  std::atomic<LPARAM> buttons_{0};
  std::atomic<uint16_t> hardCid_{0};  // extra control that means "hard press" (finder)
  std::atomic<int> otherMessages_{0};

  // status
  std::atomic<bool> connected_{false}, charging_{false}, hasHaptics_{false}, hasAction_{false},
      hasPressure_{false}, paused_{false};
  std::atomic<int> transport_{0}, battery_{-1};
};
