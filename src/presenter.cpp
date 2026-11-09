// SPDX-License-Identifier: MIT
#include "presenter.h"

#include <chrono>

using namespace hidpp;

void Presenter::Start(HWND notify) {
  hwnd_ = notify;
  th_ = std::thread([this] { Run(); });
}

void Presenter::Stop() {
  {
    std::lock_guard<std::mutex> lk(m_);
    quit_ = true;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

void Presenter::Configure(const Config& c) {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (cfg_ == c && !cfgDirty_) return;  // look-only changes don't concern the remote
    cfg_ = c;
    cfgDirty_ = true;
  }
  cv_.notify_all();
}

void Presenter::PlayHaptic(uint8_t waveform) {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (hapticQueue_.size() < 4) hapticQueue_.push_back(waveform);
  }
  cv_.notify_all();
}

void Presenter::Rescan() {
  {
    std::lock_guard<std::mutex> lk(m_);
    rescan_ = true;
  }
  cv_.notify_all();
}

void Presenter::Reconfigure() {
  {
    std::lock_guard<std::mutex> lk(m_);
    reconfigure_ = true;
  }
  cv_.notify_all();
}

void Presenter::Pause(bool pause) {
  std::unique_lock<std::mutex> lk(m_);
  if (pause) {
    pauseReq_ = true;
    resumeReq_ = false;
    cv_.notify_all();
    pausedCv_.wait_for(lk, std::chrono::seconds(4), [this] { return paused_.load(); });
  } else {
    resumeReq_ = true;
    pauseReq_ = false;
    cv_.notify_all();
  }
}

std::wstring Presenter::DeviceName() const {
  std::lock_guard<std::mutex> lk(m_);
  return name_;
}

void Presenter::OnGone() {
  {
    std::lock_guard<std::mutex> lk(m_);
    gone_ = true;
  }
  cv_.notify_all();
}

// Runs on HID reader threads: keep it short.
void Presenter::OnNotify(const uint8_t* r, size_t n) {
  if (n < 4) return;
  const bool rx = viaReceiver_.load();
  if (r[0] == kLong) {
    if (rx && r[1] != devIdx_.load()) return;
    const uint8_t feat = r[2], fnsw = r[3];
    if ((fnsw & 0x0F) != 0) return;  // a reply to some request, not an event
    const uint8_t fn = fnsw >> 4;
    const uint8_t ir = idxReprog_.load();
    if (ir && feat == ir && fn == 0 && n >= 12) {
      LPARAM mask = 0;
      for (int i = 0; i < 4; ++i) {
        const uint16_t cid = uint16_t((r[4 + 2 * i] << 8) | r[5 + 2 * i]);
        if (cid == CID_HIGHLIGHT_SOFT) mask |= BTN_SOFT;
        else if (cid == CID_HIGHLIGHT_HARD) mask |= BTN_HARD;
        else if (cid == CID_ACTION) mask |= BTN_ACTION;
      }
      if (buttons_.exchange(mask) != mask) Post(PE_BUTTONS, mask);
      return;
    }
    const uint8_t iw = idxWireless_.load();
    if (iw && feat == iw) {  // the device announces it (re)connected: it forgot our setup
      Log("presenter: wireless status event -> reconfigure");
      Reconfigure();
      return;
    }
    const uint8_t ib = idxBattery_.load();
    if (ib && feat == ib && fn == 0) {
      battery_ = r[4];
      charging_ = unifiedBattery_.load() ? (r[6] == 1 || r[6] == 2) : (r[6] == 1 || r[6] == 4);
      Post(PE_BATTERY, r[4] | (charging_ ? 0x100 : 0));
    }
  } else if (r[0] == kShort && rx && n >= 7) {
    if (r[2] == 0x41) {  // receiver: device connection notification
      const bool linkUp = !(r[4] & 0x40);
      const bool ours = r[1] == devIdx_.load();
      Log("presenter: receiver says device %d link %s", r[1], linkUp ? "up" : "down");
      if (linkUp && (ours || !connected_.load())) {
        std::lock_guard<std::mutex> lk(m_);
        if (connected_.load())
          reconfigure_ = true;
        else
          rescan_ = true;
        cv_.notify_all();
      }
    }
  }
}

void Presenter::Run() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  while (true) {
    bool doRescan = false, doReconfigure = false, gone = false, pauseReq = false,
         resumeReq = false, cfgDirty = false;
    std::vector<uint8_t> haptics;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait_for(lk, std::chrono::milliseconds(1000), [this] {
        return quit_ || rescan_ || reconfigure_ || gone_ || pauseReq_ || resumeReq_ ||
               !hapticQueue_.empty() || cfgDirty_;
      });
      if (quit_) break;
      doRescan = rescan_;
      doReconfigure = reconfigure_;
      gone = gone_;
      pauseReq = pauseReq_;
      resumeReq = resumeReq_;
      cfgDirty = cfgDirty_;
      rescan_ = reconfigure_ = gone_ = pauseReq_ = resumeReq_ = false;
      haptics.swap(hapticQueue_);
    }

    if (pauseReq) {
      if (dev_) {
        RestoreDevice();
        Disconnect();
      }
      {
        std::lock_guard<std::mutex> lk(m_);
        paused_ = true;
      }
      pausedCv_.notify_all();
      Post(PE_STATUS, 0);
      Log("presenter: paused");
      continue;
    }
    if (resumeReq) {
      paused_ = false;
      doRescan = true;
      Log("presenter: resumed");
    }
    if (paused_) continue;

    if (gone && dev_) {
      Log("presenter: device went away");
      Disconnect();
      doRescan = true;
    }

    const double now = NowMs();
    if (!dev_) {
      // Retry quickly at first, then back off; device arrival triggers a rescan anyway.
      const double interval = failedAttempts_ < 5 ? 3000 : 12000;
      if (doRescan || now - lastAttempt_ > interval) TryConnect();
      continue;
    }

    if (doReconfigure) {
      if (now - lastReconfigure_ > 400) {
        Sleep(300);  // give a waking device a moment
        ApplyConfig(true);
        lastReconfigure_ = NowMs();
        ReadBattery();
      }
    } else if (cfgDirty) {
      ApplyConfig(false);
    }
    for (uint8_t wf : haptics) {
      const uint8_t ih = idxHaptic_.load();
      if (ih) dev_->Send(ih, 4, {wf});
    }
    if (now - lastBattery_ > 5 * 60 * 1000) ReadBattery();
  }
  if (dev_) {
    RestoreDevice();
    Disconnect();
  }
  CoUninitialize();
}

bool Presenter::TryConnect() {
  lastAttempt_ = NowMs();
  std::vector<HidInfo> infos = EnumerateHid(0x046D, false);
  std::vector<HidInfo> longs;
  for (auto& i : infos)
    if (i.usagePage >= 0xFF00 && i.outLen >= 20 && (IsSpotlightPid(i.pid) || IsReceiverPid(i.pid)))
      longs.push_back(i);
  // Bluetooth Spotlight 2 first, then Bolt receivers, then the rest.
  // Bluetooth Spotlight 2 first, then Bolt receivers; collections whose output report
  // is exactly 20 bytes (HID++ long) before anything else (e.g. 32-byte DJ reports).
  auto rank = [](const HidInfo& i) {
    int r = i.pid == 0xB506 ? 0 : i.pid == 0xC548 ? 1 : IsSpotlightPid(i.pid) ? 2 : 3;
    return r + (i.outLen == 20 ? 0 : 10);
  };
  std::stable_sort(longs.begin(), longs.end(),
                   [&](const HidInfo& a, const HidInfo& b) { return rank(a) < rank(b); });

  for (auto& L : longs) {
    HidInfo shortInfo;
    const HidInfo* S = nullptr;
    if (IsReceiverPid(L.pid)) {
      const std::wstring key = HidInterfaceKey(L.path);
      for (auto& i : infos)
        if (i.pid == L.pid && i.usagePage >= 0xFF00 && i.outLen > 0 && i.outLen < 20 &&
            HidInterfaceKey(i.path) == key) {
          shortInfo = i;
          S = &shortInfo;
          break;
        }
    }
    auto d = std::make_unique<Device>();
    if (!d->Open(L, S, [this](const uint8_t* r, size_t n) { OnNotify(r, n); },
                 [this] { OnGone(); }))
      continue;
    // Let the reader threads see the receiver flag before any event arrives.
    viaReceiver_ = d->viaReceiver;
    std::string name;
    if (d->Locate(&name)) {
      Adopt(std::move(d), name);
      failedAttempts_ = 0;
      return true;
    }
    d->Close();
  }
  if (failedAttempts_++ == 0) Log("presenter: no Spotlight found (%zu candidate channels)", longs.size());
  if (connected_.exchange(false)) Post(PE_DISCONNECTED, 0);
  return false;
}

void Presenter::Adopt(std::unique_ptr<Device> d, const std::string& name) {
  dev_ = std::move(d);
  viaReceiver_ = dev_->viaReceiver;
  devIdx_ = dev_->devIdx;
  idxReprog_ = dev_->FeatureIndex(F_REPROG_V4);
  idxHaptic_ = dev_->FeatureIndex(F_HAPTIC);
  uint8_t ib = dev_->FeatureIndex(F_UNIFIED_BATTERY);
  unifiedBattery_ = ib != 0;
  if (!ib) ib = dev_->FeatureIndex(F_BATTERY_STATUS);
  idxBattery_ = ib;
  idxWireless_ = dev_->FeatureIndex(F_WIRELESS_STATUS);
  controls_ = dev_->ReadControls(false);
  hasHaptics_ = idxHaptic_.load() != 0;
  bool action = false;
  for (auto& k : controls_)
    if (k.cid == CID_ACTION && (k.flags & KF_DIVERTABLE)) action = true;
  hasAction_ = action;
  {
    std::lock_guard<std::mutex> lk(m_);
    name_ = name.empty() ? L"Spotlight" : Widen(name);
  }
  const HidInfo& li = dev_->LongInfo();
  transport_ = dev_->viaReceiver ? 1 : 2;
  Log("presenter: connected to '%s' (pid %04X, %s, device index 0x%02X), %zu controls, "
      "reprog@%d haptic@%d battery@%d wireless@%d",
      name.c_str(), li.pid, dev_->viaReceiver ? "receiver" : "bluetooth", dev_->devIdx,
      controls_.size(), idxReprog_.load(), idxHaptic_.load(), idxBattery_.load(),
      idxWireless_.load());
  for (auto& k : controls_)
    Log("  control %04X (%s) flags %04X group %d gmask %02X", k.cid, ControlName(k.cid), k.flags,
        k.group, k.gmask);
  diverted_.clear();
  laserApplied_ = false;
  ApplyConfig(true);
  ReadBattery();
  connected_ = true;
  Post(PE_CONNECTED, transport_.load());
}

void Presenter::Disconnect() {
  if (dev_) dev_->Close();
  dev_.reset();
  controls_.clear();
  diverted_.clear();
  laserApplied_ = false;
  idxReprog_ = idxHaptic_ = idxBattery_ = idxWireless_ = 0;
  buttons_ = 0;
  transport_ = 0;
  battery_ = -1;
  hasHaptics_ = false;
  hasAction_ = false;
  if (connected_.exchange(false)) Post(PE_DISCONNECTED, 0);
}

void Presenter::ApplyConfig(bool deviceForgot) {
  if (!dev_) return;
  Config c;
  {
    std::lock_guard<std::mutex> lk(m_);
    c = cfg_;
    cfgDirty_ = false;
  }
  if (deviceForgot) {
    diverted_.clear();
    laserApplied_ = false;
  }
  auto find = [&](uint16_t cid) -> const Control* {
    for (auto& k : controls_)
      if (k.cid == cid) return &k;
    return nullptr;
  };
  auto rawValid = [&](uint16_t cid) -> uint8_t {
    const Control* k = find(cid);
    // Explicitly keep raw XY off (with its "valid" bit) where the control supports it,
    // otherwise the gyro would stop moving the pointer while the button is held.
    return (k && (k->flags & KF_RAW_XY)) ? 0x20 : 0x00;
  };
  const bool laser = c.laserOn && c.laserSrc;
  std::set<uint16_t> want;
  auto consider = [&](bool on, uint16_t cid) {
    const Control* k = find(cid);
    if (on && k && (k->flags & KF_DIVERTABLE) && !(laser && c.laserSrc == cid)) want.insert(cid);
  };
  consider(c.divertSoft, CID_HIGHLIGHT_SOFT);
  consider(c.divertHard, CID_HIGHLIGHT_HARD);
  consider(c.divertAction, CID_ACTION);

  // 1) Undo a laser remap that is no longer wanted (before re-diverting that control).
  if (laserApplied_ && (!laser || !c.laserDst || laserAppliedSrc_ != c.laserSrc)) {
    Reply r = dev_->SetCidReporting(laserAppliedSrc_, 0x00, laserAppliedSrc_);
    Log("presenter: laser remap undone: %s", r.Describe().c_str());
    laserApplied_ = false;
  }
  // 2) Diversions.
  for (uint16_t cid : diverted_)
    if (!want.count(cid)) {
      Reply r = dev_->SetCidReporting(cid, uint8_t(0x02 | rawValid(cid)), 0);
      Log("presenter: release %04X -> %s", cid, r.Describe().c_str());
    }
  for (uint16_t cid : want)
    if (!diverted_.count(cid)) {
      Reply r = dev_->SetCidReporting(cid, uint8_t(0x03 | rawValid(cid)), 0);
      Log("presenter: divert %04X -> %s", cid, r.Describe().c_str());
      if (!r.ok) {  // one retry: the device sometimes drops commands
        Sleep(60);
        r = dev_->SetCidReporting(cid, uint8_t(0x03 | rawValid(cid)), 0);
        Log("presenter: divert %04X retry -> %s", cid, r.Describe().c_str());
      }
    }
  diverted_ = want;
  // 3) Laser remap.
  if (laser && c.laserDst && !laserApplied_) {
    Reply r = dev_->SetCidReporting(c.laserSrc, uint8_t(0x02 | rawValid(c.laserSrc)), c.laserDst);
    Log("presenter: laser remap %04X -> %04X: %s", c.laserSrc, c.laserDst, r.Describe().c_str());
    laserApplied_ = r.ok;
    laserAppliedSrc_ = c.laserSrc;
  }

  const uint8_t ih = idxHaptic_.load();
  if (ih) {
    Reply r = dev_->Call(ih, 2, {uint8_t(c.hapticEnabled ? 1 : 0),
                                 uint8_t(Clamp(c.hapticLevel, 1, 100))});
    Log("presenter: haptic level %d%s -> %s", c.hapticLevel, c.hapticEnabled ? "" : " (off)",
        r.Describe().c_str());
  }
}

void Presenter::RestoreDevice() {
  if (!dev_) return;
  for (uint16_t cid : diverted_) {
    uint8_t rv = 0;
    for (auto& k : controls_)
      if (k.cid == cid && (k.flags & KF_RAW_XY)) rv = 0x20;
    dev_->SetCidReporting(cid, uint8_t(0x02 | rv), 0);
  }
  diverted_.clear();
  if (laserApplied_) dev_->SetCidReporting(laserAppliedSrc_, 0x00, laserAppliedSrc_);
  laserApplied_ = false;
}

void Presenter::ReadBattery() {
  lastBattery_ = NowMs();
  const uint8_t ib = idxBattery_.load();
  if (!dev_ || !ib) return;
  Reply r = unifiedBattery_.load() ? dev_->Call(ib, 1, {}) : dev_->Call(ib, 0, {});
  if (!r.ok) return;
  battery_ = r.d[0];
  charging_ = unifiedBattery_.load() ? (r.d[2] == 1 || r.d[2] == 2) : (r.d[2] == 1 || r.d[2] == 4);
  Post(PE_BATTERY, r.d[0] | (charging_ ? 0x100 : 0));
}
