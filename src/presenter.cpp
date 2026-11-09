// SPDX-License-Identifier: MIT
#include "presenter.h"

#include <chrono>
#include <cstdlib>

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

void Presenter::Activity() {
  {
    std::lock_guard<std::mutex> lk(m_);
    checkReq_ = true;
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
    const uint8_t feat = r[2], fn = uint8_t(r[3] >> 4);
    if (feat == 0xFF) return;  // an error report for some request
    if ((r[3] & 0x0F) == kSwId) return;  // a late answer to one of our own requests
    // Button events normally carry software ID 0, but only our own ID is ruled out (as
    // in foco and Projecteur): a remote that tags its events differently would
    // otherwise look completely dead.
    const uint8_t ir = idxReprog_.load();
    if (ir && feat == ir && fn == 0 && n >= 12) {
      LPARAM mask = 0;
      for (int i = 0; i < 4; ++i) {
        const uint16_t cid = uint16_t((r[4 + 2 * i] << 8) | r[5 + 2 * i]);
        const uint16_t alt = hardCid_.load();
        if (cid == CID_HIGHLIGHT_SOFT) mask |= BTN_SOFT;
        else if (cid == CID_HIGHLIGHT_HARD || (alt && cid == alt)) mask |= BTN_HARD;
        else if (cid == CID_ACTION) mask |= BTN_ACTION;
      }
      const LPARAM prev = buttons_.exchange(mask);
      if ((mask & BTN_SOFT) && !(prev & (BTN_SOFT | BTN_HARD))) {
        // A light press just started, so the remote is awake: let the worker make sure
        // the hard press is still set up before the button is pressed all the way.
        {
          std::lock_guard<std::mutex> lk(m_);
          checkReq_ = true;
        }
        cv_.notify_all();
      }
      if (prev != mask) {
        Log("presenter: buttons %s%s%s%s (%02X %02X %02X %02X %02X %02X)",
            (mask & BTN_SOFT) ? "light " : "", (mask & BTN_HARD) ? "hard " : "",
            (mask & BTN_ACTION) ? "action " : "", mask ? "" : "released", r[3], r[4], r[5],
            r[6], r[7], r[8]);
        Post(PE_BUTTONS, mask);
      }
      return;
    }
    if (ir && feat == ir) return;  // raw motion and the like
    const uint8_t iw = idxWireless_.load();
    if (iw && feat == iw && fn == 0) {  // the device (re)connected: it forgot our setup
      Log("presenter: wireless status event -> reconfigure");
      Reconfigure();
      return;
    }
    const uint8_t ib = idxBattery_.load();
    if (ib && feat == ib && fn == 0 && (r[3] & 0x0F) != kSwId) {
      battery_ = r[4];
      charging_ = unifiedBattery_.load() ? (r[6] == 1 || r[6] == 2) : (r[6] == 1 || r[6] == 4);
      Post(PE_BATTERY, r[4] | (charging_ ? 0x100 : 0));
      return;
    }
    // Anything else (not a late reply of ours) is logged a few times: if a remote reports
    // its buttons some other way, this is where it shows up.
    if ((r[3] & 0x0F) != kSwId && otherMessages_.fetch_add(1) < 40) {
      char hex[64] = {};
      for (size_t i = 0; i < std::min<size_t>(n, 12); ++i)
        snprintf(hex + i * 3, sizeof hex - i * 3, "%02X ", r[i]);
      Log("presenter: other HID++ message: %s", hex);
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
         resumeReq = false, cfgDirty = false, doCheck = false;
    std::vector<uint8_t> haptics;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait_for(lk, std::chrono::milliseconds(1000), [this] {
        return quit_ || rescan_ || reconfigure_ || gone_ || pauseReq_ || resumeReq_ ||
               !hapticQueue_.empty() || cfgDirty_ || checkReq_;
      });
      if (quit_) break;
      doRescan = rescan_;
      doReconfigure = reconfigure_;
      gone = gone_;
      pauseReq = pauseReq_;
      resumeReq = resumeReq_;
      cfgDirty = cfgDirty_;
      doCheck = checkReq_;
      rescan_ = reconfigure_ = gone_ = pauseReq_ = resumeReq_ = checkReq_ = false;
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
        Sleep(120);  // give a waking device a moment
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
    if (retryAt_ && NowMs() >= retryAt_) {
      Log("presenter: retrying the button setup");
      ApplyConfig(false);
    }
    // Checked only right after a press, when the remote is awake anyway: polling a
    // sleeping remote would keep its radio busy and drain the battery.
    if (doCheck) QuickCheck();
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
  idxForce_ = dev_->FeatureIndex(F_FORCE_BUTTON);
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
      "reprog@%d haptic@%d battery@%d wireless@%d force@%d",
      name.c_str(), li.pid, dev_->viaReceiver ? "receiver" : "bluetooth", dev_->devIdx,
      controls_.size(), idxReprog_.load(), idxHaptic_.load(), idxBattery_.load(),
      idxWireless_.load(), idxForce_.load());
  for (auto& k : controls_)
    Log("  control %04X (%s) flags %04X group %d gmask %02X", k.cid, ControlName(k.cid), k.flags,
        k.group, k.gmask);
  ReadForceButtons();
  // Behind a receiver: does another device (a mouse, a keyboard) use it too? Then pointer
  // motion cannot tell us the presenter's light press is held.
  bool shared = false;
  if (dev_->viaReceiver) {
    const uint8_t p[2] = {0x00, 0x01};
    for (uint8_t idx = 1; idx <= 6 && !shared; ++idx) {
      if (idx == dev_->devIdx) continue;
      Reply r = dev_->CallDev(idx, 0x00, 0, p, sizeof p, 250);
      // An answer, or "that is an older device" (1), means someone is there; "unknown
      // device" (8) is an empty slot, and a sleeping device does not answer.
      if (r.ok || r.error == 0x01) {
        shared = true;
        Log("presenter: device %d also uses this receiver", idx);
      }
    }
  }
  receiverShared_ = shared;
  diverted_.clear();
  laserApplied_ = false;
  pressureApplied_ = -1;
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
  unsupported_.clear();
  laserApplied_ = false;
  retryAt_ = 0;
  retryRounds_ = 0;
  readBackUnreliable_ = false;
  needSetup_ = false;
  force_.clear();
  pressureApplied_ = -1;
  checkLogs_ = 0;
  idxReprog_ = idxHaptic_ = idxBattery_ = idxWireless_ = idxForce_ = 0;
  buttons_ = 0;
  transport_ = 0;
  battery_ = -1;
  hasHaptics_ = false;
  hasAction_ = false;
  hasPressure_ = false;
  lightFromMotion_ = false;
  receiverShared_ = false;
  if (connected_.exchange(false)) Post(PE_DISCONNECTED, 0);
}

// setCidReporting flags: divert (bit 0) with its "valid" bit (bit 1). The highlight
// button's two levels also get "raw XY valid, raw XY off" (0x20): exactly the 0x23 that is
// known to work on a real Spotlight 2, and it keeps the gyro moving the pointer.
uint8_t Presenter::DivertFlags(uint16_t cid, bool on) const {
  bool raw = cid == CID_HIGHLIGHT_SOFT || cid == CID_HIGHLIGHT_HARD;
  for (auto& k : controls_)
    if (k.cid == cid && (k.flags & KF_RAW_XY)) raw = true;
  return uint8_t((on ? 0x03 : 0x02) | (raw ? 0x20 : 0x00));
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
  const bool laser = c.laserOn && c.laserSrc;
  // Leave the light press to the remote when it has a hard press and the app can see its
  // pointer motion (not on a receiver another device shares).
  bool hasHard = false;
  for (auto& k : controls_)
    if (k.cid == CID_HIGHLIGHT_HARD) hasHard = true;
  const bool motion = c.lightFromMotion && hasHard && !(dev_->viaReceiver && receiverShared_);
  if (motion != lightFromMotion_.load())
    Log("presenter: light press %s", motion ? "left to the remote (seen as pointer motion)"
                                            : "reported over HID++");
  lightFromMotion_ = motion;
  std::set<uint16_t> want;
  // Both levels of the highlight button are always diverted, even when the remote's
  // control list came back incomplete or did not mark them divertable: the commands are
  // known to work, and a missing hard press is far worse than one extra command.
  if (c.divertSoft && !motion && !(laser && c.laserSrc == CID_HIGHLIGHT_SOFT))
    want.insert(CID_HIGHLIGHT_SOFT);
  if (c.divertHard && !(laser && c.laserSrc == CID_HIGHLIGHT_HARD)) want.insert(CID_HIGHLIGHT_HARD);
  // On some remotes the hard press arrives as another control (found by the hard press
  // finder in diagnostics).
  hardCid_ = c.hardCid;
  if (c.divertHard && c.hardCid && !(laser && c.laserSrc == c.hardCid)) want.insert(c.hardCid);
  if (c.divertAction && !(laser && c.laserSrc == CID_ACTION)) {
    const Control* k = find(CID_ACTION);
    if (k && (k->flags & KF_DIVERTABLE)) want.insert(CID_ACTION);
  }
  for (uint16_t cid : unsupported_) want.erase(cid);

  // 0) The light press must not stay taken over (an earlier version, or a crash, may have
  //    left it that way): that alone stops the hard press from being reported.
  if (motion && deviceForgot && !(laser && c.laserSrc == CID_HIGHLIGHT_SOFT)) {
    Reply r = dev_->SetCidReporting(CID_HIGHLIGHT_SOFT, DivertFlags(CID_HIGHLIGHT_SOFT, false), 0);
    Log("presenter: light press handed back to the remote: %s", r.Describe().c_str());
  }
  // 1) Undo a laser remap that is no longer wanted (before re-diverting that control).
  if (laserApplied_ && (!laser || !c.laserDst || laserAppliedSrc_ != c.laserSrc)) {
    Reply r = dev_->SetCidReporting(laserAppliedSrc_, 0x00, laserAppliedSrc_);
    Log("presenter: laser remap undone: %s", r.Describe().c_str());
    laserApplied_ = false;
  }
  // 2) Release what is no longer wanted.
  for (auto it = diverted_.begin(); it != diverted_.end();) {
    if (want.count(*it)) {
      ++it;
      continue;
    }
    Reply r = dev_->SetCidReporting(*it, DivertFlags(*it, false), 0);
    Log("presenter: release %04X -> %s", *it, r.Describe().c_str());
    it = diverted_.erase(it);
  }
  // 3) Divert. A command the remote drops (no answer) is retried; only confirmed ones
  //    are remembered, so anything that failed is tried again a little later.
  bool missing = false;
  for (uint16_t cid : want) {
    if (diverted_.count(cid)) continue;
    uint8_t flags = DivertFlags(cid, true);
    Reply r;
    for (int attempt = 0; attempt < 4; ++attempt) {
      if (attempt) Sleep(DWORD(90 * attempt));
      r = dev_->SetCidReporting(cid, flags, 0);
      if (r.ok) break;
      Log("presenter: divert %04X (%s) flags %02X -> %s", cid, ControlName(cid), flags,
          r.Describe().c_str());
      if (r.error == 2 && (flags & 0x20)) {
        flags = uint8_t(flags & ~0x20);  // maybe it has no raw XY: try without that bit
        continue;
      }
      if (r.error && r.error != 8) break;  // a definite "no" (8 = busy: try again)
    }
    if (!r.ok) {
      // The two levels of the highlight button exist on every Spotlight 2, so a "no"
      // for them is taken as "not now" (e.g. the remote is still waking up) and they
      // are asked for again later. Anything else that is refused is left alone.
      const bool level =
          cid == CID_HIGHLIGHT_SOFT || cid == CID_HIGHLIGHT_HARD || cid == c.hardCid;
      if (r.error && r.error != 8 && !level) {
        unsupported_.insert(cid);
        Log("presenter: the remote refuses %04X (%s); not asking again", cid, ControlName(cid));
      } else {
        missing = true;
      }
      continue;
    }
    diverted_.insert(cid);
    Reply g = dev_->GetCidReporting(cid);
    bool kept = g.ok && (g.d[2] & 0x01);
    bool secondTry = false;
    if (g.ok && !kept) {
      // It answered but did not keep it (it drops commands that come too close
      // together): say it once more, a little later, and look again.
      secondTry = true;
      Sleep(120);
      dev_->SetCidReporting(cid, flags, 0);
      g = dev_->GetCidReporting(cid);
      kept = g.ok && (g.d[2] & 0x01);
      if (g.ok && !kept) readBackUnreliable_ = true;
    }
    Log("presenter: divert %04X (%s) flags %02X -> ok%s%s", cid, ControlName(cid), flags,
        !g.ok ? "" : kept ? ", confirmed" : ", but reading it back says otherwise",
        secondTry ? " (second try)" : "");
  }
  if (missing) {
    ++retryRounds_;
    retryAt_ = retryRounds_ <= 5 ? NowMs() + 2500 : 0;  // after that: on the next press
    needSetup_ = true;
  } else {
    retryRounds_ = 0;
    retryAt_ = 0;
    needSetup_ = false;
  }
  nextCheck_ = NowMs() + 2000;
  // 4) Laser remap.
  if (laser && c.laserDst && !laserApplied_) {
    Reply r = dev_->SetCidReporting(c.laserSrc, uint8_t(DivertFlags(c.laserSrc, false)), c.laserDst);
    Log("presenter: laser remap %04X -> %04X: %s", c.laserSrc, c.laserDst, r.Describe().c_str());
    laserApplied_ = r.ok;
    laserAppliedSrc_ = c.laserSrc;
  }

  // 5) How hard the hard press is. Sent every time the remote (re)connects, in case it
  //    lost it: without a usable value the hard press may never fire, even though the
  //    button itself is set up.
  ApplyPressure(c.pressure, deviceForgot || pressureApplied_ != c.pressure);
  pressureApplied_ = c.pressure;

  const uint8_t ih = idxHaptic_.load();
  if (ih) {
    Reply r = dev_->Call(ih, 2, {uint8_t(c.hapticEnabled ? 1 : 0),
                                 uint8_t(Clamp(c.hapticLevel, 1, 100))});
    Log("presenter: haptic level %d%s -> %s", c.hapticLevel, c.hapticEnabled ? "" : " (off)",
        r.Describe().c_str());
  }
}

// Right after a light press starts (the remote is awake then, so nothing is polled while
// it sleeps), at most every few seconds: make sure the hard press will still work. The
// remote forgets its setup when it restarts, and over Bluetooth nothing tells us when
// that happens, so this is the moment to notice - before the button is pressed all the way.
void Presenter::QuickCheck() {
  const double now = NowMs();
  if (!dev_ || now < nextCheck_) return;
  nextCheck_ = now + 3000;
  if (needSetup_) {
    Log("presenter: finishing the button setup");
    ApplyConfig(false);
    return;
  }
  Config c;
  {
    std::lock_guard<std::mutex> lk(m_);
    c = cfg_;
  }
  CheckPressure(c.pressure);
  if (buttons_.load() & BTN_HARD) return;
  if (lightFromMotion_.load() && !readBackUnreliable_) {
    // Something took the light press over again (the hard press stops working then).
    Reply g = dev_->GetCidReporting(CID_HIGHLIGHT_SOFT);
    if (g.ok && (g.d[2] & 0x01)) {
      Reply r = dev_->SetCidReporting(CID_HIGHLIGHT_SOFT, DivertFlags(CID_HIGHLIGHT_SOFT, false), 0);
      Log("presenter: the light press had been taken over; handed back: %s", r.Describe().c_str());
    }
  }
  bool forgot = false;
  for (uint16_t cid : {uint16_t(CID_HIGHLIGHT_HARD), c.hardCid}) {
    if (!cid || !diverted_.count(cid)) continue;
    const uint8_t flags = DivertFlags(cid, true);
    if (readBackUnreliable_) {
      // This remote does not tell us the truth when asked, so just say it again.
      Reply r = dev_->SetCidReporting(cid, flags, 0);
      if (checkLogs_ < 3) {
        ++checkLogs_;
        Log("presenter: %04X set up again (as a precaution): %s", cid, r.Describe().c_str());
      }
      continue;
    }
    Reply g = dev_->GetCidReporting(cid);
    if (!g.ok || (g.d[2] & 0x01)) continue;
    Reply r = dev_->SetCidReporting(cid, flags, 0);
    Log("presenter: the remote had forgotten %04X (hard press); set up again: %s", cid,
        r.Describe().c_str());
    forgot = true;
  }
  if (forgot && c.divertAction && diverted_.count(CID_ACTION)) {
    Reply a = dev_->GetCidReporting(CID_ACTION);
    if (a.ok && !(a.d[2] & 0x01))
      dev_->SetCidReporting(CID_ACTION, DivertFlags(CID_ACTION, true), 0);
  }
}

namespace {
uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
}  // namespace

// Force sensing button (0x19C0): fn0 count, fn1 capabilities (changeable, default, max,
// min), fn2 current value, fn3 set value. Larger values need a firmer press.
void Presenter::ReadForceButtons() {
  force_.clear();
  hasPressure_ = false;
  const uint8_t fi = idxForce_.load();
  if (!dev_ || !fi) return;
  Reply n = dev_->Call(fi, 0, {});
  if (!n.ok) {
    Log("presenter: force sensing: count -> %s", n.Describe().c_str());
    return;
  }
  const int count = std::min<int>(n.d[0], 4);
  for (int b = 0; b < count; ++b) {
    Reply caps = dev_->Call(fi, 1, {uint8_t(b)});
    Reply cur = dev_->Call(fi, 2, {uint8_t(b)});
    if (!caps.ok) {
      Log("presenter: force button %d: capabilities -> %s", b, caps.Describe().c_str());
      continue;
    }
    ForceButton f;
    f.index = uint8_t(b);
    f.changeable = (caps.d[1] & 0x01) != 0;
    f.def = Be16(caps.d + 2);
    f.max = Be16(caps.d + 4);
    f.min = Be16(caps.d + 6);
    f.curOk = cur.ok;
    f.cur = cur.ok ? Be16(cur.d) : 0;
    const bool sane = f.max > f.min && f.def >= f.min && f.def <= f.max;
    Log("presenter: force button %d: now %s (default %u, range %u..%u)%s%s", b,
        cur.ok ? std::to_string(f.cur).c_str() : cur.Describe().c_str(), f.def, f.min, f.max,
        f.changeable ? "" : ", not changeable", sane ? "" : ", values look odd: left alone");
    if (!sane) f.changeable = false;
    if (f.changeable) hasPressure_ = true;
    force_.push_back(f);
  }
}

// The force sensing "set" is sent in the same layout as "get" answers (after the button
// number), with the first value replaced: the Spotlight 2 refuses a set that leaves the
// rest of it zero.
Reply Presenter::WriteForce(ForceButton& f, uint16_t value, const Reply* fresh) {
  const uint8_t fi = idxForce_.load();
  Reply cur = fresh ? *fresh : dev_->Call(fi, 2, {f.index});
  uint8_t p[16];
  if (cur.ok) {
    ForceSetParams(f.index, cur.d, value, p);
  } else {
    memset(p, 0, sizeof p);
    p[0] = f.index;
    p[1] = uint8_t(value >> 8);
    p[2] = uint8_t(value & 0xFF);
  }
  Reply r = dev_->CallBuf(fi, 3, p, sizeof p, 450);
  if (!r.ok && cur.ok) {
    // Fall back to the short form (button, value) that other Logitech devices use.
    memset(p + 3, 0, sizeof p - 3);
    Reply r2 = dev_->CallBuf(fi, 3, p, sizeof p, 450);
    if (r2.ok) r = r2;
  }
  return r;
}

void Presenter::ApplyPressure(int level, bool always) {
  const uint8_t fi = idxForce_.load();
  if (!dev_ || !fi) return;
  for (auto& f : force_) {
    if (!f.changeable) continue;
    const uint16_t target = PressureTarget(f.def, f.min, f.max, level);
    if (!always && f.curOk && f.cur == target) continue;
    const uint16_t before = f.cur;
    const bool beforeOk = f.curOk;
    Reply r = WriteForce(f, target, nullptr);
    Reply g = dev_->Call(fi, 2, {f.index});
    f.curOk = g.ok;
    if (g.ok) f.cur = Be16(g.d);
    f.appliedOk = r.ok;
    f.applied = g.ok ? f.cur : target;  // the remote may round what it is given
    char was[16] = "?";
    if (beforeOk) snprintf(was, sizeof was, "%u", before);
    Log("presenter: pressure for force button %u: %s -> %u (%s)%s", f.index, was, target,
        r.Describe().c_str(), g.ok && f.cur != target ? ", but it reads back differently" : "");
  }
}

// Returns true when the remote had lost the pressure setting and it was sent again.
bool Presenter::CheckPressure(int level) {
  const uint8_t fi = idxForce_.load();
  if (!dev_ || !fi) return false;
  bool fixed = false;
  for (auto& f : force_) {
    // Only when setting it ever worked: otherwise every light press would retry in vain.
    if (!f.changeable || !f.appliedOk) continue;
    Reply g = dev_->Call(fi, 2, {f.index});
    if (!g.ok) continue;
    const uint16_t cur = Be16(g.d), target = PressureTarget(f.def, f.min, f.max, level);
    const int slack = std::max(1, (f.max - f.min) / 100);  // ignore rounding in the remote
    f.cur = cur;
    f.curOk = true;
    if (std::abs(int(cur) - int(f.applied)) <= slack) continue;
    Reply r = WriteForce(f, target, &g);
    if (r.ok) {
      f.cur = target;
      f.applied = target;
    }
    fixed = true;
    if (checkLogs_ < 6) {
      ++checkLogs_;
      Log("presenter: the remote had lost the pressure setting (force button %u was %u); "
          "set to %u again: %s", f.index, cur, target, r.Describe().c_str());
    }
  }
  return fixed;
}

void Presenter::RestoreDevice() {
  if (!dev_) return;
  for (uint16_t cid : diverted_) dev_->SetCidReporting(cid, DivertFlags(cid, false), 0);
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
