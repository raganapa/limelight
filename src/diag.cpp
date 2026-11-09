// SPDX-License-Identifier: MIT
#include "diag.h"

#include <cstdarg>
#include <map>
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
// hard press finder: control -> the two flag bytes that put it back to normal
std::map<uint16_t, std::pair<uint8_t, uint8_t>> g_findUndo;
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

const char* kPressureNames[] = {"Lightest", "Soft", "Medium", "Firm"};

// Force sensing (0x19C0): how hard the highlight button must be pressed. Its "set" takes
// the layout its "get" answers, after the button number (see ForceSetParams).
Reply ForceWrite(uint8_t fi, uint8_t b, const Reply& cur, uint16_t v) {
  uint8_t p[16];
  ForceSetParams(b, cur.d, v, p);
  return g_dev->CallBuf(fi, 3, p, sizeof p, 450);
}

// Prints everything about it (raw, so that nothing is lost on a remote we do not know
// yet). apply = set it the way Limelight does (the "Hard press force" choice in Settings).
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
  Out("Pressure (force sensing 19C0): %u button(s)   [%s]\n", n.d[0], Hex(n.d, 8).c_str());
  for (int b = 0; b < std::min<int>(n.d[0], 4); ++b) {
    Reply caps = g_dev->Call(fi, 1, {uint8_t(b)});
    Reply cur = g_dev->Call(fi, 2, {uint8_t(b)});
    Out("  button %d caps: %s\n", b, caps.ok ? Hex(caps.d, 16).c_str() : caps.Describe().c_str());
    Out("  button %d now:  %s\n", b, cur.ok ? Hex(cur.d, 16).c_str() : cur.Describe().c_str());
    if (!caps.ok || !cur.ok) continue;
    const bool changeable = caps.d[1] & 1;
    const uint16_t def = uint16_t((caps.d[2] << 8) | caps.d[3]);
    const uint16_t mx = uint16_t((caps.d[4] << 8) | caps.d[5]);
    const uint16_t mn = uint16_t((caps.d[6] << 8) | caps.d[7]);
    const uint16_t now = uint16_t((cur.d[0] << 8) | cur.d[1]);
    Out("  button %d: now %u, default %u, range %u..%u%s\n", b, now, def, mn, mx,
        changeable ? "" : ", not changeable");
    // Writing back exactly what was read changes nothing, and shows whether we have the
    // layout right.
    Reply echo = ForceWrite(fi, uint8_t(b), cur, now);
    Out("  button %d: writing back the same value: %s\n", b, echo.Describe().c_str());
    Sleep(40);
    const bool sane = changeable && mx > mn && def >= mn && def <= mx;
    if (!sane) continue;
    any = true;
    if (!apply) continue;
    const uint16_t t = PressureTarget(def, mn, mx, s.pressure);
    Reply w = ForceWrite(fi, uint8_t(b), cur, t);
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
  for (auto& u : g_findUndo) {
    const uint16_t cid = u.first;
    uint8_t p[6] = {uint8_t(cid >> 8), uint8_t(cid & 0xFF), u.second.first, 0, 0, u.second.second};
    g_dev->CallDev(g_dev->devIdx, g_reprogIdx.load(), 3, p, u.second.second ? 6 : 5, 450, p, 2);
  }
  g_findUndo.clear();
}

std::atomic<int> g_seen{0};  // 1 = light press, 2 = hard press seen during Watch

// What the remote reported while the hard press finder listened.
struct Rec {
  bool light = false, hard = false;
  bool lightDown = false;       // the highlight button is being held
  std::set<uint16_t> during;    // other controls reported while it was held
  std::set<uint16_t> all;       // every control reported
  int raw = 0, other = 0;
};
std::mutex g_recM;
Rec g_rec;
std::atomic<bool> g_recording{false};

void OnEvent(const uint8_t* r, size_t n) {
  if (n < 4) return;
  const double t = (NowMs() - g_t0) / 1000.0;
  if (r[0] != kLong) {  // e.g. a receiver notification
    Out("  [%7.3f s] message: %s\n", t, Hex(r, std::min<size_t>(n, 7)).c_str());
    return;
  }
  const uint8_t idx = g_reprogIdx.load();
  if (idx && r[2] == idx && (r[3] >> 4) == 0) {
    bool any = false;
    for (int i = 0; i < 4; ++i) {
      const uint16_t cid = uint16_t((r[4 + 2 * i] << 8) | r[5 + 2 * i]);
      if (cid == CID_HIGHLIGHT_SOFT) g_seen |= 1;
      if (cid == CID_HIGHLIGHT_HARD) g_seen |= 2;
      if (cid) any = true;
    }
    if (g_recording) {
      std::lock_guard<std::mutex> lk(g_recM);
      bool soft = false;
      for (int i = 0; i < 4; ++i) {
        const uint16_t cid = uint16_t((r[4 + 2 * i] << 8) | r[5 + 2 * i]);
        if (!cid) continue;
        g_rec.all.insert(cid);
        if (cid == CID_HIGHLIGHT_SOFT) soft = g_rec.light = true;
        if (cid == CID_HIGHLIGHT_HARD) g_rec.hard = true;
      }
      // Pressing harder may replace the light press in the list, so anything that shows
      // up between a light press and letting go counts.
      if (soft) g_rec.lightDown = true;
      if (g_rec.lightDown)
        for (int i = 0; i < 4; ++i) {
          const uint16_t cid = uint16_t((r[4 + 2 * i] << 8) | r[5 + 2 * i]);
          if (cid && cid != CID_HIGHLIGHT_SOFT) g_rec.during.insert(cid);
        }
      if (!any) g_rec.lightDown = false;
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
    if (g_recording) {  // the finder only counts these (there are lots)
      std::lock_guard<std::mutex> lk(g_recM);
      if (g_rec.raw++ < 3) Out("  [%7.3f s] raw motion dx=%d dy=%d\n", t, dx, dy);
      return;
    }
    Out("  [%7.3f s] raw motion dx=%d dy=%d\n", t, dx, dy);
    return;
  }
  if (g_recording) {
    std::lock_guard<std::mutex> lk(g_recM);
    ++g_rec.other;
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
  Out("\nSetting the buttons to report here (temporarily)...\n"
      "  (The light press is left to the remote, like Limelight does: taking it over stops\n"
      "  the remote from reporting the hard press.)\n");
  g_dev->SetCidReporting(CID_HIGHLIGHT_SOFT, 0x22, 0);
  Sleep(40);
  std::vector<uint16_t> cids = {CID_HIGHLIGHT_HARD};  // always, like the app
  for (auto& k : g_controls)
    if ((k.flags & KF_DIVERTABLE) && !(k.flags & KF_VIRTUAL) && k.cid != CID_HIGHLIGHT_SOFT &&
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
  Out("\nSummary: hard press %s.\n", (g_seen & 2) ? "SEEN" : "not seen");
  if (!(g_seen & 2))
    Out("If you did press hard: try option 3, the Hard press finder.%s\n",
        pressure ? "" : " (This remote does not let Limelight set how hard the hard press is.)");
  Out("Restoring the buttons...\n");
  for (uint16_t cid : g_diverted) {
    const bool level = cid == CID_HIGHLIGHT_SOFT || cid == CID_HIGHLIGHT_HARD;
    g_dev->SetCidReporting(cid, uint8_t(0x02 | (level ? 0x20 : RawValid(cid))), 0);
    Sleep(40);
  }
  g_diverted.clear();
  Out("Done.\n");
}

// ------------------------------------------------------------------ hard press finder
// Tries, one at a time, the ways the remote could be set up so that it reports the hard
// press, and watches for it by itself: the user only presses. Everything is undone
// afterwards; a way that Limelight can use is saved to settings.ini.
namespace finder {

struct ForceBtn {
  uint8_t i = 0;
  uint16_t def = 0, mn = 0, mx = 0;
  Reply orig;  // as read at the start, put back at the end
};
uint8_t g_fi = 0, g_hi = 0;     // force sensing / haptic feature indexes (0 = none)
std::vector<ForceBtn> g_force;  // adjustable force buttons
bool g_forceChanged = false;
Reply g_hapticOrig;

uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

// Report a control to us for this step (flags2: the second flags byte of newer remotes).
// Returns false (and says why in note) if the remote refuses.
bool Divert(uint16_t cid, uint8_t flags, std::string& note, uint8_t flags2 = 0) {
  const uint8_t idx = g_reprogIdx.load();
  uint8_t p[6] = {uint8_t(cid >> 8), uint8_t(cid & 0xFF), flags, 0, 0, flags2};
  Reply r = g_dev->CallDev(g_dev->devIdx, idx, 3, p, flags2 ? 6 : 5, 450, p, 2);
  Sleep(40);
  if (r.ok) {
    auto& u = g_findUndo[cid];
    u.first |= uint8_t(0x02 | (flags & 0x20) | (flags & 0x80));
    if (flags2) u.second |= 0x02;
    return true;
  }
  char b[96];
  snprintf(b, sizeof b, "%s%04X flags %02X%s refused: %s", note.empty() ? "" : "; ", cid, flags,
           flags2 ? "+" : "", r.Describe().c_str());
  note += b;
  return false;
}

// Set every force button to its default (which = 0) or lowest value (which = 1).
bool Force(int which, std::string& note) {
  bool ok = !g_force.empty();
  for (auto& f : g_force) {
    Reply cur = g_dev->Call(g_fi, 2, {f.i});
    if (!cur.ok) {
      ok = false;
      continue;
    }
    const uint16_t v = which == 1 ? f.mn : f.def;
    Reply w = ForceWrite(g_fi, f.i, cur, v);
    g_forceChanged = true;
    Sleep(40);
    char b[96];
    snprintf(b, sizeof b, "%sforce %u -> %u: %s", note.empty() ? "" : "; ", Be16(cur.d), v,
             w.Describe().c_str());
    note += b;
    if (!w.ok) ok = false;
  }
  return ok;
}

void Haptic(bool on) {
  if (!g_hi) return;
  g_dev->Call(g_hi, 2, {uint8_t(on ? 1 : 0), 60});
  Sleep(40);
}

void RestoreForce() {
  for (auto& f : g_force)
    if (f.orig.ok) {
      ForceWrite(g_fi, f.i, f.orig, Be16(f.orig.d));
      Sleep(40);
    }
  g_forceChanged = false;
}

void Reset() {  // put every control (and the force setting) back the way it was
  for (auto& u : g_findUndo) {
    const uint16_t cid = u.first;
    uint8_t p[6] = {uint8_t(cid >> 8), uint8_t(cid & 0xFF), u.second.first, 0, 0, u.second.second};
    g_dev->CallDev(g_dev->devIdx, g_reprogIdx.load(), 3, p, u.second.second ? 6 : 5, 450, p, 2);
    Sleep(40);
  }
  g_findUndo.clear();
  if (g_forceChanged) RestoreForce();
}

bool Base(std::string& note) {  // both levels taken over (what Limelight 1.0.3 did)
  const bool a = Divert(CID_HIGHLIGHT_SOFT, 0x23, note);
  const bool b = Divert(CID_HIGHLIGHT_HARD, 0x23, note);
  Haptic(true);
  return a && b;
}

bool Motion(std::string& note) {  // what Limelight does now: only the hard press taken over
  g_dev->SetCidReporting(CID_HIGHLIGHT_SOFT, 0x22, 0);
  Sleep(40);
  const bool ok = Divert(CID_HIGHLIGHT_HARD, 0x23, note);
  Haptic(true);
  return ok;
}

// Listen for up to `seconds`; stop early once a hard press was seen and let go.
Rec Listen(int seconds) {
  {
    std::lock_guard<std::mutex> lk(g_recM);
    g_rec = Rec{};
  }
  g_recording = true;
  const double end = NowMs() + seconds * 1000.0;
  while (NowMs() < end) {
    Sleep(40);
    std::lock_guard<std::mutex> lk(g_recM);
    if ((g_rec.hard || !g_rec.during.empty()) && !g_rec.lightDown) break;
  }
  Sleep(200);
  g_recording = false;
  std::lock_guard<std::mutex> lk(g_recM);
  return g_rec;
}

std::string Cids(const std::set<uint16_t>& s) {
  std::string out;
  for (uint16_t c : s) {
    char b[8];
    snprintf(b, sizeof b, "%s%04X", out.empty() ? "" : " ", c);
    out += b;
  }
  return out;
}

// One screen with everything that matters for the hard press (easy to photograph).
void Fingerprint() {
  uint8_t maj = 0, min = 0;
  g_dev->ProtocolVersion(maj, min);
  Out("Remote: '%s' via %s, HID++ %u.%u, Limelight %s\n", g_dev->ReadName().c_str(),
      g_dev->viaReceiver ? "USB receiver" : "Bluetooth", maj, min, Narrow(APP_VERSION).c_str());
  for (uint16_t cid : {CID_HIGHLIGHT_SOFT, CID_HIGHLIGHT_HARD}) {
    uint16_t flags = 0;
    bool listed = false;
    for (auto& k : g_controls)
      if (k.cid == cid) {
        flags = k.flags;
        listed = true;
      }
    Reply g = g_dev->GetCidReporting(cid);
    Out("Control %04X (%s): %s%04X, reporting now [%s]\n", cid,
        cid == CID_HIGHLIGHT_SOFT ? "light" : "hard", listed ? "flags " : "NOT LISTED ", flags,
        g.ok ? Hex(g.d, 6).c_str() : g.Describe().c_str());
  }
  if (g_hi) {
    Reply st = g_dev->Call(g_hi, 1, {});
    Out("Haptics 19B0: state [%s]\n", st.ok ? Hex(st.d, 4).c_str() : st.Describe().c_str());
  } else {
    Out("Haptics 19B0: not offered\n");
  }
  Pressure(false);  // raw force sensing data, and whether writing it back works
  Settings s;
  s.Load();
  s.Sanitize();
  Out("Settings: hard press force %s, hard press control %04X, real laser %s (%04X)\n",
      kPressureNames[s.pressure], s.hardCid ? s.hardCid : 0x01A8, s.physicalLaser ? "ON" : "off",
      s.laserSrcCid);
}

}  // namespace finder

void HardPressFinder() {
  using namespace finder;
  Out("\n=== Hard press finder ===\n");
  Undo();
  g_controls = g_dev->ReadControls(true);
  g_fi = g_dev->FeatureIndex(F_FORCE_BUTTON);
  g_hi = g_dev->FeatureIndex(F_HAPTIC);
  g_force.clear();
  if (g_fi) {
    Reply n = g_dev->Call(g_fi, 0, {});
    for (int b = 0; n.ok && b < std::min<int>(n.d[0], 4); ++b) {
      Reply caps = g_dev->Call(g_fi, 1, {uint8_t(b)});
      if (!caps.ok) continue;
      ForceBtn f;
      f.i = uint8_t(b);
      f.def = Be16(caps.d + 2);
      f.mx = Be16(caps.d + 4);
      f.mn = Be16(caps.d + 6);
      f.orig = g_dev->Call(g_fi, 2, {uint8_t(b)});
      if (f.mx > f.mn && f.def >= f.mn && f.def <= f.mx) g_force.push_back(f);
    }
  }
  g_hapticOrig = g_hi ? g_dev->Call(g_hi, 1, {}) : Reply{};
  Fingerprint();

  struct Step {
    const char* name;
    const char* note;  // shown to the user, or nullptr
    std::function<bool(std::string&)> apply;  // false = not possible here: skipped
    int save;  // 0 nothing needed, 1 force Medium, 2 force Lightest, 3 other control,
               // 4 vibration off, 5 needs a change in Limelight, 6 both levels taken over
  };
  const std::vector<Step> steps = {
      {"Limelight's setup (light press left alone)", "the light press shows nothing here",
       [](std::string& n) { return Motion(n); }, 0},
      {"Pressure set to the remote's default", nullptr,
       [](std::string& n) { return Motion(n) && Force(0, n); }, 1},
      {"Pressure set to the lightest", nullptr,
       [](std::string& n) { return Motion(n) && Force(1, n); }, 2},
      {"Both levels taken over (Limelight 1.0.3)", nullptr,
       [](std::string& n) { return Base(n); }, 6},
      {"Motion sent to the app, like Options+",
       "the pointer will not move during this step",
       [](std::string& n) {
         const bool a = Divert(CID_HIGHLIGHT_SOFT, 0x33, n);
         const bool b = Divert(CID_HIGHLIGHT_HARD, 0x33, n);
         Haptic(true);
         return a && b;
       },
       5},
      {"Every button reported", "press only the highlight button",
       [](std::string& n) {
         const bool ok = Motion(n);
         for (auto& k : g_controls)  // virtual ones too: the hard press might be one
           if ((k.flags & KF_DIVERTABLE) && k.cid != CID_HIGHLIGHT_SOFT &&
               k.cid != CID_HIGHLIGHT_HARD)
             Divert(k.cid, uint8_t(0x03 | RawValid(k.cid)), n);
         return ok;
       },
       3},
      {"Key analytics reports on", nullptr,
       [](std::string& n) {
         const bool a = Divert(CID_HIGHLIGHT_SOFT, 0x23, n, 0x03);
         const bool b = Divert(CID_HIGHLIGHT_HARD, 0x23, n, 0x03);
         Haptic(true);
         return a && b;
       },
       5},
      {"Forced motion reports on the hard press", nullptr,
       [](std::string& n) {
         const bool a = Divert(CID_HIGHLIGHT_SOFT, 0x23, n);
         const bool b = Divert(CID_HIGHLIGHT_HARD, 0xE3, n);
         Haptic(true);
         return a && b;
       },
       5},
      {"Vibration switched off", nullptr,
       [](std::string& n) {
         if (!g_hi) return false;
         const bool ok = Motion(n);
         Haptic(false);
         return ok;
       },
       4},
  };

  Out("\nI'll try %zu ways of setting up the remote. In EACH step, when it says NOW:\n"
      "press the highlight button HARD - all the way - hold it for a second, then let go.\n"
      "Each step listens for 9 seconds and moves on by itself. Nothing is changed\n"
      "permanently.\n",
      steps.size());
  Ask("Press Enter to start... ");

  std::vector<std::string> results(steps.size());
  int found = -1;
  uint16_t foundCid = 0;
  for (size_t i = 0; i < steps.size() && found < 0; ++i) {
    const Step& st = steps[i];
    Out("\nStep %zu of %zu: %s\n", i + 1, steps.size(), st.name);
    g_t0 = NowMs();
    std::string note;
    if (!st.apply(note)) {
      results[i] = note.empty() ? "skipped (not offered by this remote)" : "skipped: " + note;
      Out("  %s\n", results[i].c_str());
      Reset();
      continue;
    }
    if (!note.empty()) Out("  (%s)\n", note.c_str());
    Out("  NOW: press the highlight button HARD, hold it, let go%s%s%s\n", st.note ? " (" : "",
        st.note ? st.note : "", st.note ? ")" : "");
    const Rec rec = Listen(9);
    // Controls that might be the hard press arriving as something else: not the light
    // press, not a quick tap (left click) and not one of the other physical buttons.
    std::set<uint16_t> others = rec.during;
    for (uint16_t c : rec.all) others.insert(c);
    for (uint16_t c : {0x00D8, 0x01A8, 0x0050, 0x00D9, 0x00DA, 0x00DB, 0x00DC}) others.erase(c);
    std::set<uint16_t> extra = rec.all;
    extra.erase(CID_HIGHLIGHT_SOFT);
    extra.erase(CID_HIGHLIGHT_HARD);
    char b[200];
    if (rec.hard) {
      snprintf(b, sizeof b, "HARD PRESS SEEN");
      found = int(i);
    } else if (st.save == 3 && !others.empty()) {
      snprintf(b, sizeof b, "HARD PRESS SEEN as %s", Cids(others).c_str());
      found = int(i);
      foundCid = *others.begin();
    } else if (rec.light) {
      snprintf(b, sizeof b, "light only%s%s", extra.empty() ? "" : ", also ", Cids(extra).c_str());
    } else if (!extra.empty()) {
      snprintf(b, sizeof b, "only %s", Cids(extra).c_str());
    } else if (rec.raw) {
      snprintf(b, sizeof b, "motion only");
    } else {
      snprintf(b, sizeof b, "no hard press");
    }
    results[i] = b;
    if (rec.raw) results[i] += ", motion x" + std::to_string(rec.raw);
    if (rec.other) results[i] += ", other msgs x" + std::to_string(rec.other);
    if (!note.empty() && (st.save == 1 || st.save == 2)) results[i] += " (" + note + ")";
    Out("  -> %s\n", results[i].c_str());
    Reset();
  }
  RestoreForce();
  if (g_hi && g_hapticOrig.ok)
    g_dev->Call(g_hi, 2, {uint8_t(g_hapticOrig.d[0] & 1),
                          g_hapticOrig.d[1] ? g_hapticOrig.d[1] : uint8_t(60)});

  Out("\n=== Hard press finder: results ===\n");
  for (size_t i = 0; i < steps.size(); ++i)
    Out(" %zu %-46s %s\n", i + 1, steps[i].name, results[i].empty() ? "-" : results[i].c_str());
  if (found < 0) {
    Out("\nNothing made the remote report the hard press. Please send a photo of this\n"
        "screen (from \"=== Hard press finder ===\" down) to the Limelight developer.\n");
    return;
  }
  Settings s;
  s.Load();
  s.Sanitize();
  switch (steps[found].save) {
    case 0:
      Out("\nThe hard press works with Limelight's own setup right now.\n");
      return;
    case 1:
      s.pressure = 2;  // Medium = the remote's default
      break;
    case 2:
      s.pressure = 0;  // Lightest
      break;
    case 3:
      s.hardCid = foundCid;
      break;
    case 4:
      s.haptics = HapticMode::Off;
      break;
    case 6:
      s.lightFromMotion = false;
      break;
    default:
      Out("\n*** Found a way (step %d), but Limelight needs a small change to use it.\n"
          "Please send a photo of this screen to the Limelight developer. ***\n",
          found + 1);
      return;
  }
  s.Save();
  Out("\n*** Found it (step %d) and saved it: Limelight uses it from now on. ***\n", found + 1);
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
#ifndef LIMELIGHT_DIAG_TEST  // the test drives it through ordinary stdin/stdout
  AllocConsole();
  FILE* f;
  freopen_s(&f, "CONOUT$", "w", stdout);
  freopen_s(&f, "CONIN$", "r", stdin);
#endif
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
        "  3  Hard press finder (when the hard press does nothing)\n"
        "  4  Test vibration\n"
        "  5  Laser finder (switch the real laser on)\n"
        "  6  Quit\n");
    const std::string a = Ask("> ");
    if (a == "1") Report();
    else if (a == "2") Watch();
    else if (a == "3") HardPressFinder();
    else if (a == "4") Vibration();
    else if (a == "5") LaserFinder();
    else if (a == "6" || a == "q" || a == "quit") break;
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
