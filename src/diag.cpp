// SPDX-License-Identifier: MIT
#include "diag.h"

#include <cstdarg>
#include <set>

#include "hidpp.h"
#include "settings.h"

using namespace hidpp;

namespace {

std::mutex g_out;
FILE* g_report = nullptr;
std::unique_ptr<Device> g_dev;
std::set<uint16_t> g_diverted;          // controls we diverted (to undo)
std::vector<uint16_t> g_remapped;       // controls we remapped (to undo)
std::vector<Control> g_controls;
double g_t0 = 0;
std::wstring g_reportPath;
std::atomic<uint8_t> g_reprogIdx{0};  // read by the HID reader thread

void Out(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  std::lock_guard<std::mutex> lk(g_out);
  fputs(buf, stdout);
  fflush(stdout);
  if (g_report) {
    fputs(buf, g_report);
    fflush(g_report);
  }
}

std::string Hex(const uint8_t* d, size_t n) {
  std::string s;
  char b[4];
  for (size_t i = 0; i < n; ++i) {
    snprintf(b, sizeof b, "%02X", d[i]);
    if (i) s += ' ';
    s += b;
  }
  return s;
}

std::string Ask(const char* prompt) {
  Out("%s", prompt);
  char line[256] = {};
  if (!fgets(line, sizeof line, stdin)) return "";
  std::string s(line);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  if (g_report) {
    std::lock_guard<std::mutex> lk(g_out);
    fprintf(g_report, "%s\n", s.c_str());
  }
  for (auto& c : s) c = char(tolower(uint8_t(c)));
  return s;
}

bool Yes(const char* prompt) {
  for (;;) {
    std::string a = Ask(prompt);
    if (a == "y" || a == "yes" || a == "j" || a == "ja") return true;
    if (a == "n" || a == "no" || a == "nee") return false;
    Out("  Please type y or n and press Enter.\n");
  }
}

uint8_t RawValid(uint16_t cid) {
  for (auto& k : g_controls)
    if (k.cid == cid && (k.flags & KF_RAW_XY)) return 0x20;
  return 0;
}

std::string Flags(uint16_t f) {
  std::string s;
  auto add = [&](uint16_t bit, const char* n) {
    if (f & bit) {
      if (!s.empty()) s += ",";
      s += n;
    }
  };
  add(KF_MOUSE, "mouse");
  add(KF_FN, "fn");
  add(KF_NONSTD, "nonstd");
  add(KF_FN_SENS, "fn-sens");
  add(KF_REPROGRAMMABLE, "reprog");
  add(KF_DIVERTABLE, "divert");
  add(KF_PERSIST_DIVERTABLE, "persist");
  add(KF_VIRTUAL, "virtual");
  add(KF_RAW_XY, "rawXY");
  add(KF_FORCE_RAW_XY, "forceRawXY");
  add(KF_ANALYTICS, "analytics");
  add(KF_RAW_WHEEL, "rawWheel");
  return s.empty() ? "-" : s;
}

const char* kPressureNames[] = {"Soft", "Medium", "Firm"};

// Force sensing (0x19C0): how hard the highlight button must be pressed for each level.
// apply = set it the way Limelight does (the "Hard press force" choice in Settings).
// Returns false if the remote has no adjustable pressure.
bool Pressure(bool apply) {
  const uint8_t fi = g_dev->FeatureIndex(F_FORCE_BUTTON);
  if (!fi) {
    Out("Pressure (force sensing 19C0): not offered by this remote.\n");
    return false;
  }
  Reply n = g_dev->Call(fi, 0, {});
  if (!n.ok) {
    Out("Pressure (force sensing 19C0): count -> %s\n", n.Describe().c_str());
    return false;
  }
  Settings s;
  s.Load();
  s.Sanitize();
  bool any = false;
  Out("Pressure (force sensing 19C0): %u button(s)\n", n.d[0]);
  for (int b = 0; b < std::min<int>(n.d[0], 4); ++b) {
    Reply caps = g_dev->Call(fi, 1, {uint8_t(b)});
    Reply cur = g_dev->Call(fi, 2, {uint8_t(b)});
    if (!caps.ok) {
      Out("  button %d: capabilities -> %s\n", b, caps.Describe().c_str());
      continue;
    }
    const bool changeable = caps.d[1] & 1;
    const uint16_t def = uint16_t((caps.d[2] << 8) | caps.d[3]);
    const uint16_t mx = uint16_t((caps.d[4] << 8) | caps.d[5]);
    const uint16_t mn = uint16_t((caps.d[6] << 8) | caps.d[7]);
    Out("  button %d: now %s, default %u, range %u..%u%s   [%s]\n", b,
        cur.ok ? std::to_string((cur.d[0] << 8) | cur.d[1]).c_str() : cur.Describe().c_str(),
        def, mn, mx, changeable ? "" : ", not changeable", Hex(caps.d, 8).c_str());
    const bool sane = changeable && mx > mn && def >= mn && def <= mx;
    if (!sane) continue;
    any = true;
    if (!apply) continue;
    const uint16_t t = PressureTarget(def, mn, mx, s.pressure);
    const uint8_t p[3] = {uint8_t(b), uint8_t(t >> 8), uint8_t(t & 0xFF)};
    Reply w = g_dev->CallBuf(fi, 3, p, sizeof p, 450);
    Reply g = g_dev->Call(fi, 2, {uint8_t(b)});
    Out("  button %d set to %u (%s, as in Settings): %s, reads back %s\n", b, t,
        kPressureNames[s.pressure], w.Describe().c_str(),
        g.ok ? std::to_string((g.d[0] << 8) | g.d[1]).c_str() : g.Describe().c_str());
    Sleep(40);
  }
  return any;
}

void Undo() {
  if (!g_dev || !g_dev->IsOpen()) return;
  for (uint16_t cid : g_diverted) g_dev->SetCidReporting(cid, uint8_t(0x02 | RawValid(cid)), 0);
  g_diverted.clear();
  for (uint16_t cid : g_remapped) g_dev->SetCidReporting(cid, 0x00, cid);
  g_remapped.clear();
}

std::atomic<int> g_seen{0};  // 1 = light press, 2 = hard press seen during Watch

void OnEvent(const uint8_t* r, size_t n) {
  if (n < 4) return;
  const double t = (NowMs() - g_t0) / 1000.0;
  if (r[0] != kLong) {  // e.g. a receiver notification
    Out("  [%7.3f s] message: %s\n", t, Hex(r, std::min<size_t>(n, 7)).c_str());
    return;
  }
  const uint8_t idx = g_reprogIdx.load();
  if (idx && r[2] == idx && (r[3] >> 4) == 0) {
    for (int i = 0; i < 4; ++i) {
      const uint16_t cid = uint16_t((r[4 + 2 * i] << 8) | r[5 + 2 * i]);
      if (cid == CID_HIGHLIGHT_SOFT) g_seen |= 1;
      if (cid == CID_HIGHLIGHT_HARD) g_seen |= 2;
    }
    std::string pressed;
    for (int i = 0; i < 4; ++i) {
      const uint16_t cid = uint16_t((r[4 + 2 * i] << 8) | r[5 + 2 * i]);
      if (!cid) continue;
      char b[96];
      snprintf(b, sizeof b, "%s%04X %s", pressed.empty() ? "" : " + ", cid, ControlName(cid));
      pressed += b;
    }
    Out("  [%7.3f s] %s   [%s]\n", t, pressed.empty() ? "(all released)" : pressed.c_str(),
        Hex(r, 12).c_str());
    return;
  }
  if (idx && r[2] == idx && (r[3] >> 4) == 1) {
    const int dx = int16_t((r[4] << 8) | r[5]), dy = int16_t((r[6] << 8) | r[7]);
    Out("  [%7.3f s] raw motion dx=%d dy=%d\n", t, dx, dy);
    return;
  }
  Out("  [%7.3f s] message: %s\n", t, Hex(r, std::min<size_t>(n, 12)).c_str());
}

BOOL WINAPI CtrlHandler(DWORD) {
  Undo();
  HWND main = FindWindowW(APP_MAIN_CLASS, nullptr);
  if (main) PostMessageW(main, WM_APP_RESUME_DEVICE, 0, 0);
  return FALSE;
}

// ------------------------------------------------------------------ connect
bool Connect() {
  Out("\nLooking for Logitech HID interfaces...\n");
  std::vector<HidInfo> infos = EnumerateHid(0x046D, true);
  if (infos.empty()) {
    Out("  No Logitech HID devices found. Is the remote paired and awake (press a button)?\n");
    return false;
  }
  for (auto& i : infos)
    Out("  pid %04X  usage %04X:%04X  in %2u out %2u  %-24s\n", i.pid, i.usagePage, i.usage,
        i.inLen, i.outLen, Narrow(i.product).c_str());
  std::vector<HidInfo> longs;
  for (auto& i : infos)
    if (i.usagePage >= 0xFF00 && i.outLen >= 20) longs.push_back(i);
  std::stable_sort(longs.begin(), longs.end(), [](const HidInfo& a, const HidInfo& b) {
    auto rank = [](const HidInfo& i) {
      const int r = i.pid == 0xB506 ? 0 : i.pid == 0xC548 ? 1 : IsSpotlightPid(i.pid) ? 2 : 3;
      return r + (i.outLen == 20 ? 0 : 10);
    };
    return rank(a) < rank(b);
  });
  for (auto& L : longs) {
    HidInfo shortInfo;
    const HidInfo* S = nullptr;
    if (IsReceiverPid(L.pid)) {
      for (auto& i : infos)
        if (i.pid == L.pid && i.usagePage >= 0xFF00 && i.outLen > 0 && i.outLen < 20 &&
            HidInterfaceKey(i.path) == HidInterfaceKey(L.path)) {
          shortInfo = i;
          S = &shortInfo;
          break;
        }
    }
    auto d = std::make_unique<Device>();
    if (!d->Open(L, S, OnEvent, nullptr)) {
      Out("  pid %04X: cannot open (in use?)\n", L.pid);
      continue;
    }
    std::string name;
    Out("  Asking pid %04X (%s)...\n", L.pid, d->viaReceiver ? "receiver" : "direct/Bluetooth");
    if (d->Locate(&name)) {
      Out("  Found '%s' as device index 0x%02X via %s.\n", name.c_str(), d->devIdx,
          d->viaReceiver ? "USB receiver" : "Bluetooth");
      g_dev = std::move(d);
      g_reprogIdx = g_dev->FeatureIndex(F_REPROG_V4);
      return true;
    }
    d->Close();
  }
  Out("  No Spotlight answered. Press a button on the remote to wake it, then try again.\n");
  return false;
}

// ------------------------------------------------------------------ report
void Report() {
  Out("\n=== Device report ===\n");
  uint8_t maj = 0, min = 0;
  if (g_dev->ProtocolVersion(maj, min)) Out("HID++ protocol %u.%u\n", maj, min);
  Out("Name: %s\n", g_dev->ReadName().c_str());
  const HidInfo& li = g_dev->LongInfo();
  Out("Connection: pid %04X, %s, device index 0x%02X\n", li.pid,
      g_dev->viaReceiver ? "USB receiver" : "Bluetooth", g_dev->devIdx);

  if (uint8_t fi = g_dev->FeatureIndex(F_DEVICE_INFO)) {
    Reply r = g_dev->Call(fi, 0, {});
    if (r.ok) {
      Out("Firmware entities: %u, model %s\n", r.d[0], Hex(r.d + 7, 6).c_str());
      for (int e = 0; e < r.d[0] && e < 6; ++e) {
        Reply f = g_dev->Call(fi, 1, {uint8_t(e)});
        if (f.ok)
          Out("  entity %d: type %u '%c%c%c' %02X.%02X build %02X%02X\n", e, f.d[0], f.d[1] ? f.d[1] : ' ',
              f.d[2] ? f.d[2] : ' ', f.d[3] ? f.d[3] : ' ', f.d[4], f.d[5], f.d[6], f.d[7]);
      }
    }
  }

  Out("\nFeatures:\n");
  std::vector<uint16_t> unknown;
  if (uint8_t fs = g_dev->FeatureIndex(F_FEATURE_SET)) {
    Reply c = g_dev->Call(fs, 0, {});
    const int count = c.ok ? c.d[0] : 0;
    for (int i = 0; i <= count; ++i) {
      Reply f = g_dev->Call(fs, 1, {uint8_t(i)});
      if (!f.ok) continue;
      const uint16_t id = uint16_t((f.d[0] << 8) | f.d[1]);
      const uint8_t type = f.d[2];
      Out("  [%2d] %04X v%u %-28s%s%s%s\n", i, id, f.d[3], FeatureName(id),
          (type & 0x80) ? " obsolete" : "", (type & 0x40) ? " hidden" : "",
          (type & 0x20) ? " engineering" : "");
      if (std::string(FeatureName(id)) == "unknown") unknown.push_back(id);
    }
  }

  g_controls = g_dev->ReadControls(true);
  Out("\nControls (buttons):\n");
  for (auto& k : g_controls) {
    Out("  %04X %-32s task %04X flags %-28s group %u gmask %02X", k.cid, ControlName(k.cid), k.tid,
        Flags(k.flags).c_str(), k.group, k.gmask);
    if (k.haveReporting)
      Out("  now: %s%s remap %04X", (k.repFlags & 1) ? "diverted" : "normal",
          (k.repFlags & 4) ? " (persistent)" : "", k.remap);
    Out("\n");
  }
  // Which controls could be remapped to which
  for (auto& src : g_controls) {
    if (!src.gmask) continue;
    std::string to;
    for (auto& dst : g_controls)
      if (dst.group && (src.gmask & (1 << (dst.group - 1))) && dst.cid != src.cid) {
        char b[16];
        snprintf(b, sizeof b, " %04X", dst.cid);
        to += b;
      }
    if (!to.empty()) Out("  %04X can be remapped to:%s\n", src.cid, to.c_str());
  }

  if (uint8_t bi = g_dev->FeatureIndex(F_UNIFIED_BATTERY)) {
    Reply r = g_dev->Call(bi, 1, {});
    if (r.ok) Out("\nBattery: %u%% (level %02X, status %u)\n", r.d[0], r.d[1], r.d[2]);
  }
  if (uint8_t hi = g_dev->FeatureIndex(F_HAPTIC)) {
    Reply c = g_dev->Call(hi, 0, {});
    Reply s = g_dev->Call(hi, 1, {});
    Out("Haptics: capabilities %s / state %s\n", c.ok ? Hex(c.d, 8).c_str() : c.Describe().c_str(),
        s.ok ? Hex(s.d, 4).c_str() : s.Describe().c_str());
  }
  Pressure(false);
  // Read-only peek (function 0) at features we have no name for.
  auto safe = [](uint16_t id) {
    if (id == 0x0020 || (id >= 0x00C0 && id <= 0x00DF)) return false;  // reset / firmware update
    if ((id & 0xFF00) == 0x1800 || (id & 0xFF00) == 0x1E00 || (id & 0xFF00) == 0x1F00) return false;
    return true;
  };
  for (uint16_t id : unknown) {
    if (!safe(id)) continue;
    const uint8_t fi = g_dev->FeatureIndex(id);
    Reply r = g_dev->Call(fi, 0, {});
    Out("  peek %04X fn0: %s\n", id, r.ok ? Hex(r.d, 16).c_str() : r.Describe().c_str());
  }
  Out("=== end of report ===\n");
}

// ------------------------------------------------------------------ watch
void Watch() {
  if (g_controls.empty()) g_controls = g_dev->ReadControls(true);
  Out("\n");
  const bool pressure = Pressure(true);  // like Limelight does
  Out("\nSetting the buttons to report here (temporarily)...\n");
  std::vector<uint16_t> cids = {CID_HIGHLIGHT_SOFT, CID_HIGHLIGHT_HARD};  // always, like the app
  for (auto& k : g_controls)
    if ((k.flags & KF_DIVERTABLE) && !(k.flags & KF_VIRTUAL) &&
        std::find(cids.begin(), cids.end(), k.cid) == cids.end())
      cids.push_back(k.cid);
  for (uint16_t cid : cids) {
    const bool level = cid == CID_HIGHLIGHT_SOFT || cid == CID_HIGHLIGHT_HARD;
    const uint8_t flags = uint8_t(0x03 | (level ? 0x20 : RawValid(cid)));
    Reply r = g_dev->SetCidReporting(cid, flags, 0);
    std::string back;
    if (r.ok) {
      g_diverted.insert(cid);
      Reply g = g_dev->GetCidReporting(cid);
      back = !g.ok ? "" : (g.d[2] & 1) ? ", confirmed" : ", NOT kept by the remote";
    }
    Out("  %04X %-32s flags %02X: %s%s\n", cid, ControlName(cid), flags, r.Describe().c_str(),
        back.c_str());
    Sleep(40);
  }
  Out("\nNow press: lightly, then HARD, then let go. Then Next, Back, the action button,\n"
      "and long presses. (Slides will not change while this runs.)\n"
      "Press Enter here when you are done.\n\n");
  g_seen = 0;
  g_t0 = NowMs();
  char line[64];
  fgets(line, sizeof line, stdin);
  Out("\nSummary: light press %s, hard press %s.\n", (g_seen & 1) ? "SEEN" : "not seen",
      (g_seen & 2) ? "SEEN" : "not seen");
  if ((g_seen & 1) && !(g_seen & 2))
    Out(pressure ? "If you did press hard: in Settings set \"Hard press force\" to Soft and try\n"
                   "again with the Remote check row there.\n"
                 : "This remote does not let Limelight set how hard the hard press is.\n");
  Out("Restoring the buttons...\n");
  for (uint16_t cid : g_diverted) {
    const bool level = cid == CID_HIGHLIGHT_SOFT || cid == CID_HIGHLIGHT_HARD;
    g_dev->SetCidReporting(cid, uint8_t(0x02 | (level ? 0x20 : RawValid(cid))), 0);
    Sleep(40);
  }
  g_diverted.clear();
  Out("Done.\n");
}

// ------------------------------------------------------------------ vibration
const char* WaveName(int w) {
  static const char* n[] = {"sharp state change", "damp state change", "sharp collision",
                            "damp collision", "subtle collision", "happy alert", "angry alert",
                            "completed", "square", "wave", "firework", "mad", "knock", "jingle",
                            "ringing"};
  return w >= 0 && w < 15 ? n[w] : (w == 0x1B ? "whisper collision" : "waveform");
}

void Vibration() {
  const uint8_t hi = g_dev->FeatureIndex(F_HAPTIC);
  if (!hi) {
    const uint8_t pc = g_dev->FeatureIndex(F_PRESENTER);
    if (pc) {
      Out("Using the original Spotlight vibration command...\n");
      Reply r = g_dev->Call(pc, 1, {5, 0xE8, 0x80});
      Out("  -> %s\n", r.Describe().c_str());
    } else {
      Out("This remote reports no vibration feature.\n");
    }
    return;
  }
  Reply caps = g_dev->Call(hi, 0, {});
  Reply state = g_dev->Call(hi, 1, {});
  uint32_t mask = 0;
  if (caps.ok) mask = (uint32_t(caps.d[4]) << 24) | (uint32_t(caps.d[5]) << 16) | (uint32_t(caps.d[6]) << 8) | caps.d[7];
  Out("Capabilities %s, state %s\n", caps.ok ? Hex(caps.d, 8).c_str() : caps.Describe().c_str(),
      state.ok ? Hex(state.d, 4).c_str() : state.Describe().c_str());
  Reply en = g_dev->Call(hi, 2, {1, 80});
  Out("Enable at 80%%: %s\n", en.Describe().c_str());
  std::vector<int> waves;
  for (int w = 0; w < 32; ++w)
    if (mask & (1u << w)) waves.push_back(w);
  if (waves.empty()) waves = {0, 1, 4, 5, 7, 12};
  for (int w : waves) {
    Out("  playing %2d (%s)...", w, WaveName(w));
    Reply r = g_dev->Call(hi, 4, {uint8_t(w)});
    Out(" %s\n", r.Describe().c_str());
    Sleep(1100);
  }
  if (state.ok) g_dev->Call(hi, 2, {uint8_t(state.d[0] & 1), state.d[1] ? state.d[1] : uint8_t(60)});
  Out("Did you feel them? (Note which ones you liked.)\n");
}

// ------------------------------------------------------------------ laser
bool SaveLaser(uint16_t src, uint16_t dst) {
  Settings s;
  s.Load();
  s.laserKnown = true;
  s.laserSrcCid = src;
  s.laserDstCid = dst;
  s.physicalLaser = true;
  s.Save();
  Out("\n*** Found it! Saved: %s%s%04X. ***\n", ControlName(src), dst ? " -> laser function " : "",
      dst);
  Out("Limelight will now switch the real laser on. You can turn it off any time with\n"
      "\"Real laser\" in the tray menu or in Settings.\n");
  return true;
}

void LaserFinder() {
  Out("\n=== Laser finder ===\n"
      "The Spotlight 2 has a real laser diode, but how Logi Options+ switches it on is not\n"
      "documented anywhere. Let's find it together: you press a button, I ask what happened.\n"
      "Nothing is changed permanently - everything is undone when this window closes.\n"
      "Point the remote at a wall (never at eyes).\n\n");
  g_controls = g_dev->ReadControls(true);
  auto has = [](uint16_t cid) {
    for (auto& k : g_controls)
      if (k.cid == cid) return true;
    return false;
  };

  // 1) With nothing diverted, does the laser fire natively?
  for (auto& k : g_controls)
    if ((k.flags & KF_DIVERTABLE) && (k.repFlags & 1)) {
      g_dev->SetCidReporting(k.cid, uint8_t(0x02 | RawValid(k.cid)), 0);
      Sleep(40);
    }
  Out("Step 1: plain mode.\n");
  if (Yes("  Hold the highlight button LIGHTLY for 2 seconds. Did the laser light up? [y/n] "))
    return (void)SaveLaser(CID_HIGHLIGHT_SOFT, 0);
  if (Yes("  Now press the highlight button HARD and hold. Did the laser light up? [y/n] "))
    return (void)SaveLaser(CID_HIGHLIGHT_HARD, 0);
  if (has(CID_ACTION) &&
      Yes("  Press and hold the small ACTION button. Did the laser light up? [y/n] "))
    return (void)SaveLaser(CID_ACTION, 0);

  // 2) Remap a button to one of the laser functions.
  std::vector<std::pair<uint16_t, uint16_t>> tries;
  const uint16_t srcs[] = {CID_HIGHLIGHT_SOFT, CID_HIGHLIGHT_HARD, CID_ACTION};
  const uint16_t dsts[] = {CID_LASER_LONG, CID_LASER_SHORT};
  for (uint16_t s : srcs)
    for (uint16_t d : dsts)
      if (has(s)) tries.push_back({s, d});
  // plus anything the device itself says is remappable to a laser function
  for (auto& src : g_controls)
    for (auto& dst : g_controls)
      if ((dst.cid == CID_LASER_LONG || dst.cid == CID_LASER_SHORT) && dst.group &&
          (src.gmask & (1 << (dst.group - 1)))) {
        bool dup = false;
        for (auto& t : tries)
          if (t.first == src.cid && t.second == dst.cid) dup = true;
        if (!dup) tries.push_back({src.cid, dst.cid});
      }
  Out("\nStep 2: trying %zu button -> laser mappings.\n", tries.size());
  for (auto& t : tries) {
    Reply r = g_dev->SetCidReporting(t.first, uint8_t(0x02 | RawValid(t.first)), t.second);
    if (!r.ok) {
      Out("  %04X -> %04X: the remote refused (%s)\n", t.first, t.second, r.Describe().c_str());
      continue;
    }
    g_remapped.push_back(t.first);
    char q[256];
    snprintf(q, sizeof q, "  Mapped %s -> %s.\n  Press and hold %s for 2 seconds. Laser? [y/n] ",
             ControlName(t.first), ControlName(t.second), ControlName(t.first));
    const bool ok = Yes(q);
    g_dev->SetCidReporting(t.first, 0x00, t.first);
    g_remapped.pop_back();
    if (ok) return (void)SaveLaser(t.first, t.second);
  }
  Out("\nNo luck this time. Your report is saved on the Desktop as \"Limelight report.txt\".\n"
      "Send it to whoever maintains Limelight (or open an issue) so the laser can be added.\n");
  Report();
}

}  // namespace

int RunDiagnostics() {
  AllocConsole();
  FILE* f;
  freopen_s(&f, "CONOUT$", "w", stdout);
  freopen_s(&f, "CONIN$", "r", stdin);
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleTitleW(L"Limelight diagnostics");
  SetConsoleCtrlHandler(CtrlHandler, TRUE);

  PWSTR desk = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) {
    g_reportPath = std::wstring(desk) + L"\\Limelight report.txt";
    CoTaskMemFree(desk);
    g_report = _wfopen(g_reportPath.c_str(), L"ab");
  }
  SYSTEMTIME st;
  GetLocalTime(&st);
  Out("Limelight %s diagnostics - %04d-%02d-%02d %02d:%02d\n", Narrow(APP_VERSION).c_str(),
      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
  Out("Report file: %s\n", Narrow(g_reportPath).c_str());

  // Ask the running Limelight to let go of the remote while we work.
  HWND main = FindWindowW(APP_MAIN_CLASS, nullptr);
  if (main) {
    DWORD_PTR res = 0;
    SendMessageTimeoutW(main, WM_APP_PAUSE_DEVICE, GetCurrentProcessId(), 0, SMTO_ABORTIFHUNG,
                        6000, &res);
    Out("Limelight paused its connection to the remote.\n");
  }

  for (;;) {
    if (!g_dev && !Connect()) {
      if (!Yes("\nTry again? [y/n] ")) break;
      continue;
    }
    Out("\nWhat would you like to do?\n"
        "  1  Device report (safe, read-only)\n"
        "  2  Watch the buttons (see what each press sends)\n"
        "  3  Test vibration\n"
        "  4  Laser finder (switch the real laser on)\n"
        "  5  Quit\n");
    const std::string a = Ask("> ");
    if (a == "1") Report();
    else if (a == "2") Watch();
    else if (a == "3") Vibration();
    else if (a == "4") LaserFinder();
    else if (a == "5" || a == "q" || a == "quit") break;
  }

  Undo();
  if (g_dev) g_dev->Close();
  g_dev.reset();
  if (main) {
    PostMessageW(main, WM_APP_RELOAD_SETTINGS, 0, 0);
    PostMessageW(main, WM_APP_RESUME_DEVICE, 0, 0);
  }
  Out("\nAll changes undone. You can close this window.\n");
  if (g_report) fclose(g_report);
  Sleep(1200);
  return 0;
}
