// SPDX-License-Identifier: MIT
// A simulated Logitech Spotlight 2 (Bluetooth) behind the HidChannel interface,
// used to exercise the HID++ and Presenter logic without hardware.
#include <condition_variable>
#include <deque>
#include <map>
#include <set>

#include "../src/hid.h"
#include "mock_hid.h"

namespace mock {

std::mutex m;
std::condition_variable cv;
std::deque<std::vector<uint8_t>> toHost;
std::vector<std::vector<uint8_t>> fromHost;  // every request the host wrote
std::map<uint16_t, uint8_t> reporting;       // cid -> flags as last set
std::map<uint16_t, uint16_t> remaps;
std::map<uint16_t, uint8_t> lastFlags;
int dropped = 0;
double lastWrite = -1e9;
bool replyAsIndex01 = true;  // the real device answers 0xFF requests as device 0x01
bool receiver = false;       // simulate a Logi Bolt receiver instead of Bluetooth
// "Quirky" remote: lists the hard press without the divertable flag, tags its button
// events with a non-zero software ID, and drops the first command that diverts the
// hard press (no answer). Each of these used to leave the hard press dead.
bool quirky = false;
bool droppedHardOnce = false;
// Force sensing (0x19C0), one button. After a restart the remote has no usable pressure
// setting until a host sends one (the quirky remote has no such feature: fixed threshold).
// Like the real Spotlight 2, "get" answers with a second value after the first, and a
// "set" that does not carry it along is refused.
const uint16_t kForceDef = 200, kForceMin = 40, kForceMax = 400, kForceUnset = 0xFFFF;
const uint16_t kForceSecond = 0x0190;
uint16_t forceCur = kForceUnset;
uint16_t hardAs = 0x01A8;  // the control the remote reports for a hard press
bool needsDefaultForce = false;  // hard press only fires once the force is at most default
int deliberateDrops = 0;
const uint8_t kSlot = 2;     // the Spotlight's pairing slot on the receiver
const uint8_t kOtherSlot = 1;  // another Logitech device (a mouse) on the same receiver
std::atomic<int> opens{0};

struct Feat {
  uint8_t idx;
  uint16_t id;
};
const Feat kFeats[] = {{0, 0x0000}, {1, 0x0001}, {2, 0x0003}, {3, 0x0005}, {9, 0x1004},
                       {11, 0x1B04}, {12, 0x1814}, {13, 0x1815}, {17, 0x1A01}, {20, 0x19B0},
                       {21, 0x1D4B}, {22, 0x19C0}, {27, 0x18A1}};
struct Ctl {
  uint16_t cid, flags;
  uint8_t group, gmask;
};
const Ctl kCtls[] = {{0x00D8, 0x0130, 1, 0x02}, {0x01A8, 0x0130, 1, 0x02}, {0x00D9, 0x0030, 0, 0},
                     {0x00DA, 0x0030, 0, 0},    {0x00DB, 0x0030, 0, 0},    {0x00DC, 0x0030, 0, 0},
                     {0x01B0, 0x0030, 0, 0},    {0x00FB, 0x00A0, 2, 0},    {0x00FC, 0x00A0, 2, 0}};
const char* kName = "Spotlight 2";

void Push(std::vector<uint8_t> r) {
  {
    std::lock_guard<std::mutex> lk(m);
    toHost.push_back(std::move(r));
  }
  cv.notify_all();
}

void Reply(const uint8_t* req, std::initializer_list<uint8_t> data) {
  std::vector<uint8_t> r(20, 0);
  r[0] = 0x11;
  r[1] = (replyAsIndex01 && req[1] == 0xFF) ? 0x01 : req[1];
  r[2] = req[2];
  r[3] = req[3];
  size_t i = 4;
  for (uint8_t v : data)
    if (i < 20) r[i++] = v;
  Push(r);
}

void Error(const uint8_t* req, uint8_t code) {
  std::vector<uint8_t> r(20, 0);
  r[0] = 0x11;
  r[1] = req[1];
  r[2] = 0xFF;
  r[3] = req[2];
  r[4] = req[3];
  r[5] = code;
  Push(r);
}

void Rx10Error(const uint8_t* q, uint8_t code) {  // HID++ 1.0 error from the receiver
  std::vector<uint8_t> r(7, 0);
  r[0] = 0x10;
  r[1] = q[1];
  r[2] = 0x8F;
  r[3] = q[2];
  r[4] = q[3];
  r[5] = code;
  Push(r);
}

void HandleMouse(const uint8_t* q) {  // a mouse paired on the same receiver
  const uint8_t feat = q[2], fn = q[3] >> 4;
  if (feat == 0 && fn == 0) {
    const uint16_t want = uint16_t((q[4] << 8) | q[5]);
    const uint8_t idx = want == 0x1B04 ? 8 : want == 0x0005 ? 3 : 0;
    return Reply(q, {idx, 0, 0});
  }
  if (feat == 3 && fn == 0) return Reply(q, {12});
  if (feat == 3 && fn == 1) {
    std::vector<uint8_t> r(20, 0);
    r[0] = 0x11; r[1] = q[1]; r[2] = q[2]; r[3] = q[3];
    const char* n = "MX Master 3S";
    for (size_t i = 0; q[4] + i < strlen(n) && i < 16; ++i) r[4 + i] = uint8_t(n[q[4] + i]);
    return Push(r);
  }
  if (feat == 8 && fn == 0) return Reply(q, {1});
  if (feat == 8 && fn == 1) return Reply(q, {0x00, 0x52, 0, 0, 0x30, 0, 0, 0, 0});
  Error(q, 6);
}

void Handle(const uint8_t* q) {
  if (q[0] != 0x11) return;  // the Spotlight 2 ignores short reports
  if (receiver) {
    if (q[1] == kOtherSlot) return HandleMouse(q);
    if (q[1] != kSlot) return Rx10Error(q, 0x08);  // empty slot: unknown device
  } else if (q[1] != 0xFF && q[1] != 0x01) {
    return;  // nobody else on this link
  }
  const uint8_t feat = q[2], fn = q[3] >> 4;
  const uint8_t* p = q + 4;
  uint16_t id = 0xFFFF;
  for (auto& f : kFeats)
    if (f.idx == feat) id = f.id;
  switch (id) {
    case 0x0000:
      if (fn == 0) {
        const uint16_t want = uint16_t((p[0] << 8) | p[1]);
        uint8_t idx = 0;
        for (auto& f : kFeats)
          if (f.id == want) idx = f.idx;
        if (quirky && want == 0x19C0) idx = 0;
        Reply(q, {idx, 0, 0});
      } else if (fn == 1) {
        Reply(q, {4, 5, p[2]});
      }
      return;
    case 0x0001:
      if (fn == 0) Reply(q, {uint8_t(sizeof kFeats / sizeof kFeats[0] - 1)});
      if (fn == 1) {
        for (auto& f : kFeats)
          if (f.idx == p[0]) return Reply(q, {uint8_t(f.id >> 8), uint8_t(f.id), 0, 0});
        Reply(q, {0, 0, 0, 0});
      }
      return;
    case 0x0005:
      if (fn == 0) Reply(q, {uint8_t(strlen(kName))});
      if (fn == 1) {
        std::vector<uint8_t> r(20, 0);
        r[0] = 0x11;
        r[1] = (replyAsIndex01 && q[1] == 0xFF) ? 0x01 : q[1];
        r[2] = q[2];
        r[3] = q[3];
        const size_t off = p[0];
        for (size_t i = 0; off + i < strlen(kName) && i < 16; ++i) r[4 + i] = uint8_t(kName[off + i]);
        Push(r);
      }
      return;
    case 0x1004:
      if (fn == 1) Reply(q, {83, 4, 0});
      return;
    case 0x19B0:
      if (fn == 0) Reply(q, {0, 0, 0, 0, 0x00, 0x00, 0x40, 0xBF});
      else Reply(q, {});
      return;
    case 0x19C0:
      if (quirky) return Error(q, 6);
      if (fn == 0) return Reply(q, {1});
      if (p[0] != 0) return Error(q, 2);
      if (fn == 1)
        return Reply(q, {0x00, 0x01, uint8_t(kForceDef >> 8), uint8_t(kForceDef),
                         uint8_t(kForceMax >> 8), uint8_t(kForceMax), uint8_t(kForceMin >> 8),
                         uint8_t(kForceMin)});
      if (fn == 2)
        return Reply(q, {uint8_t(forceCur >> 8), uint8_t(forceCur), uint8_t(kForceSecond >> 8),
                         uint8_t(kForceSecond)});
      if (fn == 3) {
        const uint16_t v = uint16_t((p[1] << 8) | p[2]);
        const uint16_t second = uint16_t((p[3] << 8) | p[4]);
        if (v < kForceMin || v > kForceMax || second != kForceSecond) return Error(q, 2);
        forceCur = v;
        return Reply(q, {});
      }
      return Error(q, 7);
    case 0x1B04: {
      if (fn == 0) return Reply(q, {uint8_t(sizeof kCtls / sizeof kCtls[0])});
      if (fn == 1) {
        if (p[0] >= sizeof kCtls / sizeof kCtls[0]) return Error(q, 2);
        const Ctl& c = kCtls[p[0]];
        const uint16_t flags = (quirky && c.cid == 0x01A8) ? 0x0200 : c.flags;
        return Reply(q, {uint8_t(c.cid >> 8), uint8_t(c.cid), 0, 0, uint8_t(flags & 0xFF), 0,
                         c.group, c.gmask, uint8_t(flags >> 8)});
      }
      const uint16_t cid = uint16_t((p[0] << 8) | p[1]);
      if (fn == 2) {
        const uint16_t rm = remaps.count(cid) ? remaps[cid] : 0;
        return Reply(q, {p[0], p[1], reporting[cid], uint8_t(rm >> 8), uint8_t(rm)});
      }
      if (fn == 3) {
        if (quirky && cid == 0x01A8 && (p[2] & 0x01) && !droppedHardOnce) {
          droppedHardOnce = true;  // swallowed: no answer at all
          ++deliberateDrops;
          return;
        }
        uint8_t st = reporting.count(cid) ? reporting[cid] : 0;
        const uint8_t f = p[2];
        if (f & 0x02) st = uint8_t((st & ~0x01) | (f & 0x01));  // divert (+ dvalid)
        if (f & 0x08) st = uint8_t((st & ~0x04) | (f & 0x04));  // persist (+ pvalid)
        if (f & 0x20) st = uint8_t((st & ~0x10) | (f & 0x10));  // raw XY (+ rvalid)
        reporting[cid] = st;
        lastFlags[cid] = f;
        const uint16_t rm = uint16_t((p[3] << 8) | p[4]);
        if (rm) remaps[cid] = rm;
        return Reply(q, {p[0], p[1], p[2], p[3], p[4]});
      }
      return;
    }
    default:
      if (id == 0xFFFF) return Error(q, 6);
      Reply(q, {});
  }
}

void PressButtons(std::initializer_list<uint16_t> cids) {
  std::vector<uint8_t> r(20, 0);
  r[0] = 0x11;
  r[1] = receiver ? kSlot : 0x01;
  r[2] = 11;
  r[3] = quirky ? 0x05 : 0x00;
  size_t i = 4;
  for (uint16_t c : cids) {
    r[i++] = uint8_t(c >> 8);
    r[i++] = uint8_t(c);
  }
  Push(r);
}

void Press(int force) {
  std::vector<uint16_t> cids;
  {
    std::lock_guard<std::mutex> lk(m);
    const int threshold = quirky ? 150 : forceCur;
    const bool hard = force > 0 && force >= threshold && (reporting[hardAs] & 0x01) &&
                      (!needsDefaultForce || forceCur <= kForceDef);
    if (hard) cids.push_back(hardAs);  // like the real remote: the hard level alone
    else if (force > 0 && (reporting[0x00D8] & 0x01)) cids.push_back(0x00D8);
  }
  std::vector<uint8_t> r(20, 0);
  r[0] = 0x11;
  r[1] = receiver ? kSlot : 0x01;
  r[2] = 11;
  r[3] = quirky ? 0x05 : 0x00;
  for (size_t i = 0; i < cids.size(); ++i) {
    r[4 + 2 * i] = uint8_t(cids[i] >> 8);
    r[5 + 2 * i] = uint8_t(cids[i]);
  }
  Push(r);
}

void SetForceScenario(uint16_t start, bool needsDefault) {
  std::lock_guard<std::mutex> lk(m);
  forceCur = start;
  needsDefaultForce = needsDefault;
}

void SetHardAs(uint16_t cid) {
  std::lock_guard<std::mutex> lk(m);
  hardAs = cid;
}

int ForceValue() {
  std::lock_guard<std::mutex> lk(m);
  return forceCur;
}

void ForgetPressure() {
  std::lock_guard<std::mutex> lk(m);
  forceCur = kForceUnset;
}

void ForgetHardPress() {
  std::lock_guard<std::mutex> lk(m);
  reporting[0x01A8] = 0;
}

void ForgetSetup() {
  std::lock_guard<std::mutex> lk(m);
  reporting.clear();
  remaps.clear();
  lastFlags.clear();
  forceCur = kForceUnset;
}

void AnnounceReconnect() {
  if (receiver) {  // the receiver reports the link coming up (short 0x41 notification)
    std::vector<uint8_t> r(7, 0);
    r[0] = 0x10;
    r[1] = kSlot;
    r[2] = 0x41;
    r[3] = 0x10;
    r[4] = 0x06;  // presenter, link established (bit 6 clear)
    return Push(r);
  }
  std::vector<uint8_t> r(20, 0);
  r[0] = 0x11;
  r[1] = 0x01;
  r[2] = 21;  // 0x1D4B
  r[3] = 0x00;
  r[4] = 1;
  Push(r);
}

void SetReceiverMode(bool on) { receiver = on; }
void SetQuirky(bool on) { quirky = on; }
int DeliberateDrops() { return deliberateDrops; }

}  // namespace mock

std::vector<HidInfo> EnumerateHid(uint16_t, bool) {
  if (mock::receiver) {
    HidInfo kb;
    kb.path = L"\\\\?\\hid#vid_046d&pid_c548&mi_00#8&aa&0&0000#{4d1e55b2}";
    kb.vid = 0x046D; kb.pid = 0xC548; kb.usagePage = 0x0001; kb.usage = 0x0006; kb.inLen = 8;
    HidInfo dj = kb;  // DJ collection: 32-byte reports, must not be used for HID++
    dj.path = L"\\\\?\\hid#vid_046d&pid_c548&mi_02&col01#8&bb&0&0000#{4d1e55b2}";
    dj.usagePage = 0xFF00; dj.usage = 0x0001; dj.inLen = 32; dj.outLen = 32;
    HidInfo sh = kb;
    sh.path = L"\\\\?\\hid#vid_046d&pid_c548&mi_02&col02#8&bb&0&0001#{4d1e55b2}";
    sh.usagePage = 0xFF00; sh.usage = 0x0002; sh.inLen = 7; sh.outLen = 7;
    HidInfo lg = kb;
    lg.path = L"\\\\?\\hid#vid_046d&pid_c548&mi_02&col03#8&bb&0&0002#{4d1e55b2}";
    lg.usagePage = 0xFF00; lg.usage = 0x0002; lg.inLen = 20; lg.outLen = 20;
    return {kb, dj, sh, lg};
  }
  HidInfo kb;
  kb.path = L"\\\\?\\hid#{00001812-0000-1000-8000-00805f9b34fb}_dev_vid&02046d_pid&b506&col01#mock";
  kb.vid = 0x046D;
  kb.pid = 0xB506;
  kb.usagePage = 0x0001;
  kb.usage = 0x0006;
  kb.inLen = 8;
  HidInfo hp = kb;
  hp.path = L"\\\\?\\hid#{00001812-0000-1000-8000-00805f9b34fb}_dev_vid&02046d_pid&b506&col03#mock";
  hp.usagePage = 0xFF43;
  hp.usage = 0x0202;
  hp.inLen = 20;
  hp.outLen = 20;
  return {kb, hp};
}

bool HidChannel::Open(const HidInfo& info, ReportFn onReport, GoneFn onGone) {
  Close();
  info_ = info;
  onReport_ = std::move(onReport);
  onGone_ = std::move(onGone);
  closing_ = false;
  h_ = reinterpret_cast<HANDLE>(1);
  ++mock::opens;
  reader_ = std::thread([this] { ReaderLoop(); });
  return true;
}

void HidChannel::Close() {
  if (h_ == INVALID_HANDLE_VALUE) return;
  closing_ = true;
  mock::cv.notify_all();
  if (reader_.joinable()) reader_.join();
  h_ = INVALID_HANDLE_VALUE;
}

void HidChannel::ReaderLoop() {
  while (!closing_) {
    std::vector<uint8_t> r;
    {
      std::unique_lock<std::mutex> lk(mock::m);
      mock::cv.wait_for(lk, std::chrono::milliseconds(50),
                        [this] { return closing_ || !mock::toHost.empty(); });
      if (closing_) break;
      if (mock::toHost.empty()) continue;
      r = mock::toHost.front();
      mock::toHost.pop_front();
    }
    if (onReport_) onReport_(r.data(), r.size());
  }
}

bool HidChannel::Write(const uint8_t* data, size_t len) {
  if (info_.outLen != 20) return false;
  std::vector<uint8_t> q(data, data + std::min<size_t>(len, 20));
  q.resize(20, 0);
  const double now = NowMs();
  {
    std::lock_guard<std::mutex> lk(mock::m);
    mock::fromHost.push_back(q);
    // The real remote silently drops commands that arrive too close together.
    if (now - mock::lastWrite < 20) {
      ++mock::dropped;
      mock::lastWrite = now;
      return true;
    }
    mock::lastWrite = now;
  }
  mock::Handle(q.data());
  return true;
}
