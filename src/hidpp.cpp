// SPDX-License-Identifier: MIT
#include "hidpp.h"

#include <chrono>

namespace hidpp {

namespace {
struct NameEntry {
  uint16_t id;
  const char* name;
};

// Common HID++ 2.0 feature IDs (descriptive names, not exhaustive).
const NameEntry kFeatures[] = {
    {0x0000, "Root"},
    {0x0001, "Feature set"},
    {0x0002, "Feature info"},
    {0x0003, "Firmware info"},
    {0x0004, "Unit ID"},
    {0x0005, "Device name"},
    {0x0006, "Device groups"},
    {0x0007, "Friendly name"},
    {0x0008, "Keep alive"},
    {0x0011, "Property access"},
    {0x0020, "Config change"},
    {0x0021, "Crypto ID"},
    {0x0030, "Target software"},
    {0x0080, "Wireless signal strength"},
    {0x00C0, "DFU control (legacy)"},
    {0x00C1, "DFU control (unsigned)"},
    {0x00C2, "DFU control (signed)"},
    {0x00C3, "DFU control"},
    {0x00D0, "DFU"},
    {0x1000, "Battery status"},
    {0x1001, "Battery voltage"},
    {0x1004, "Unified battery"},
    {0x1010, "Charging control"},
    {0x1300, "LED control"},
    {0x1500, "Force pairing"},
    {0x1800, "Generic test"},
    {0x1802, "Device reset"},
    {0x1805, "Out-of-box state"},
    {0x1806, "Device properties"},
    {0x1814, "Change host"},
    {0x1815, "Hosts info"},
    {0x1816, "BLE pre-pairing"},
    {0x18A1, "LED test (engineering)"},
    {0x19B0, "Haptic feedback"},
    {0x19C0, "Force sensing button"},
    {0x1A00, "Presenter control"},
    {0x1A01, "Sensor 3D"},
    {0x1B04, "Reprogrammable controls v4"},
    {0x1BC0, "Report HID usage"},
    {0x1C00, "Persistent remappable action"},
    {0x1D4B, "Wireless device status"},
    {0x1DF0, "Remaining pairings"},
    {0x1E00, "Enable hidden features"},
    {0x1F1F, "Firmware properties"},
    {0x1F20, "ADC measurement"},
    {0x2001, "Left/right swap"},
    {0x2005, "Swap cancel button"},
    {0x2201, "Adjustable DPI"},
    {0x2205, "Pointer speed"},
};

const NameEntry kControls[] = {
    {0x0050, "Left click"},
    {0x0051, "Right click"},
    {0x0052, "Middle click"},
    {0x00D8, "Highlight button - light press"},
    {0x01A8, "Highlight button - hard press"},
    {0x00D9, "Next - short press"},
    {0x00DA, "Next - long press"},
    {0x00DB, "Back - short press"},
    {0x00DC, "Back - long press"},
    {0x00DF, "Switch highlight"},
    {0x00FB, "Laser - short press"},
    {0x00FC, "Laser - long press"},
    {0x01B0, "Action button (probably)"},
};
}  // namespace

const char* FeatureName(uint16_t id) {
  for (auto& e : kFeatures)
    if (e.id == id) return e.name;
  return "unknown";
}

const char* ControlName(uint16_t cid) {
  for (auto& e : kControls)
    if (e.id == cid) return e.name;
  return "unknown";
}

const char* ErrorName(uint8_t code) {
  switch (code) {
    case 1: return "unknown";
    case 2: return "invalid argument";
    case 3: return "out of range";
    case 4: return "hardware error";
    case 5: return "internal";
    case 6: return "invalid feature index";
    case 7: return "invalid function";
    case 8: return "busy";
    case 9: return "unsupported";
    default: return "error";
  }
}

std::string Reply::Describe() const {
  char b[96];
  if (ok) return "ok";
  if (writeFailed) return "write failed";
  if (timeout) return "no answer";
  snprintf(b, sizeof b, "error 0x%02X (%s)", error, ErrorName(error));
  return b;
}

double Device::lastWrite_ = 0;

bool IsSpotlightPid(uint16_t pid) { return pid == 0xB506 || pid == 0xB503; }
bool IsReceiverPid(uint16_t pid) {
  return pid == 0xC548 || pid == 0xC53E || pid == 0xC52B || pid == 0xC532 || pid == 0xC534 ||
         pid == 0xC547;
}

bool Device::Open(const HidInfo& longCol, const HidInfo* shortCol, NotifyFn onNotify,
                  GoneFn onGone) {
  Close();
  onNotify_ = std::move(onNotify);
  viaReceiver = IsReceiverPid(longCol.pid);
  devIdx = viaReceiver ? 1 : 0xFF;
  auto rep = [this](const uint8_t* r, size_t n) { OnReport(r, n); };
  if (!long_.Open(longCol, rep, std::move(onGone))) return false;
  if (shortCol) short_.Open(*shortCol, rep, nullptr);
  features_.clear();
  return true;
}

void Device::Close() {
  long_.Close();
  short_.Close();
  features_.clear();
}

void Device::OnReport(const uint8_t* r, size_t n) {
  if (n < 4) return;
  if (r[0] == kShort || r[0] == kLong) {
    std::lock_guard<std::mutex> lk(m_);
    if (pending_ && !done_) {
      const bool devOk = viaReceiver ? (r[1] == pDev_) : true;
      const bool echoOk = pExpectLen_ == 0 || (n >= 4 + pExpectLen_ &&
                                               memcmp(r + 4, pExpect_, pExpectLen_) == 0);
      if (devOk && r[2] == pFeat_ && r[3] == pFnSw_ && echoOk) {
        reply_ = Reply{};
        reply_.ok = true;
        memcpy(reply_.d, r + 4, std::min<size_t>(16, n - 4));
        done_ = true;
        cv_.notify_all();
        return;
      }
      if (n >= 6 && devOk && r[2] == 0xFF && r[3] == pFeat_ && r[4] == pFnSw_) {
        reply_ = Reply{};
        reply_.error = r[5] ? r[5] : 0xFF;
        done_ = true;
        cv_.notify_all();
        return;
      }
      if (n >= 6 && r[1] == pDev_ && r[2] == 0x8F && r[3] == pFeat_ && r[4] == pFnSw_) {
        reply_ = Reply{};
        reply_.error = r[5] ? r[5] : 0xFF;
        done_ = true;
        cv_.notify_all();
        return;
      }
    }
  }
  if (onNotify_) onNotify_(r, n);
}

Reply Device::CallDev(uint8_t dev, uint8_t feat, uint8_t fn, const uint8_t* params, size_t n,
                      int timeoutMs, const uint8_t* expect, size_t expectLen) {
  std::lock_guard<std::mutex> callLock(callMutex_);
  const double since = NowMs() - lastWrite_;
  if (since < 35) Sleep(DWORD(35 - since));
  uint8_t buf[20] = {kLong, dev, feat, uint8_t((fn << 4) | kSwId)};
  if (params && n) memcpy(buf + 4, params, std::min<size_t>(n, 16));
  {
    std::lock_guard<std::mutex> lk(m_);
    pending_ = true;
    done_ = false;
    pDev_ = dev;
    pFeat_ = feat;
    pFnSw_ = buf[3];
    pExpectLen_ = expect ? std::min<size_t>(expectLen, sizeof pExpect_) : 0;
    if (pExpectLen_) memcpy(pExpect_, expect, pExpectLen_);
    reply_ = Reply{};
  }
  const bool wrote = long_.Write(buf, sizeof buf);
  lastWrite_ = NowMs();
  std::unique_lock<std::mutex> lk(m_);
  if (!wrote) {
    pending_ = false;
    Reply r;
    r.writeFailed = true;
    r.timeout = true;
    return r;
  }
  const bool got =
      cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return done_; });
  pending_ = false;
  Reply r = reply_;
  if (!got) {
    r = Reply{};
    r.timeout = true;
  }
  return r;
}

Reply Device::CallBuf(uint8_t featIdx, uint8_t fn, const uint8_t* params, size_t n,
                      int timeoutMs) {
  return CallDev(devIdx, featIdx, fn, params, n, timeoutMs);
}

Reply Device::Call(uint8_t featIdx, uint8_t fn, std::initializer_list<uint8_t> params,
                   int timeoutMs) {
  uint8_t p[16] = {};
  size_t n = 0;
  for (uint8_t v : params)
    if (n < 16) p[n++] = v;
  return CallDev(devIdx, featIdx, fn, p, n, timeoutMs);
}

bool Device::Send(uint8_t featIdx, uint8_t fn, std::initializer_list<uint8_t> params) {
  std::lock_guard<std::mutex> callLock(callMutex_);
  const double since = NowMs() - lastWrite_;
  if (since < 35) Sleep(DWORD(35 - since));
  uint8_t buf[20] = {kLong, devIdx, featIdx, uint8_t((fn << 4) | kSwId)};
  size_t i = 4;
  for (uint8_t v : params)
    if (i < 20) buf[i++] = v;
  const bool ok = long_.Write(buf, sizeof buf);
  lastWrite_ = NowMs();
  return ok;
}

uint8_t Device::FeatureIndex(uint16_t id) {
  if (id == F_ROOT) return 0;
  auto it = features_.find(id);
  if (it != features_.end()) return it->second;
  const uint8_t p[2] = {uint8_t(id >> 8), uint8_t(id & 0xFF)};
  Reply r = CallDev(devIdx, 0x00, 0, p, 2, 450);
  const uint8_t idx = r.ok ? r.d[0] : 0;
  if (r.ok || r.error) features_[id] = idx;  // cache definitive answers only
  return idx;
}

std::string Device::ReadName() {
  const uint8_t idx = FeatureIndex(F_DEVICE_NAME);
  if (!idx) return {};
  Reply r = Call(idx, 0, {});
  if (!r.ok) return {};
  const int len = std::min<int>(r.d[0], 64);
  std::string name;
  int off = 0;
  while (off < len) {
    Reply c = Call(idx, 1, {uint8_t(off)});
    if (!c.ok) break;
    const int take = std::min(16, len - off);
    bool end = false;
    for (int i = 0; i < take; ++i) {
      if (!c.d[i]) {
        end = true;
        break;
      }
      name += char(c.d[i]);
    }
    if (end) break;
    off += take;
  }
  return name;
}

bool Device::ProtocolVersion(uint8_t& major, uint8_t& minor) {
  Reply r = Call(0x00, 1, {0, 0, 0x5A});
  if (!r.ok) return false;
  major = r.d[0];
  minor = r.d[1];
  return true;
}

bool Device::Locate(std::string* nameOut) {
  std::vector<uint8_t> idxs;
  if (viaReceiver)
    idxs = {1, 2, 3, 4, 5, 6};
  else
    idxs = {0xFF, 1, 2, 3, 4, 5, 6};
  struct Cand {
    uint8_t idx;
    std::string name;
  };
  std::vector<Cand> others;
  for (uint8_t idx : idxs) {
    const uint8_t p[2] = {uint8_t(F_REPROG_V4 >> 8), uint8_t(F_REPROG_V4 & 0xFF)};
    Reply r = CallDev(idx, 0x00, 0, p, 2, viaReceiver ? 300 : (idx == 0xFF ? 450 : 250));
    // Over Bluetooth a failed write means the remote is not there (asleep): asking
    // other device numbers would only make each attempt slower.
    if (r.writeFailed && !viaReceiver) return false;
    if (!r.ok || r.d[0] == 0) continue;
    devIdx = idx;
    features_.clear();
    features_[F_REPROG_V4] = r.d[0];
    std::string name = ReadName();
    std::string low = name;
    for (auto& ch : low) ch = char(tolower(uint8_t(ch)));
    if (!viaReceiver || low.find("spotlight") != std::string::npos ||
        low.find("presenter") != std::string::npos) {
      if (nameOut) *nameOut = name;
      return true;
    }
    others.push_back({idx, name});
  }
  // Behind a receiver with an unexpected name: a device that has the highlight
  // button controls is the presenter.
  for (auto& c : others) {
    devIdx = c.idx;
    features_.clear();
    for (auto& k : ReadControls(false))
      if (k.cid == CID_HIGHLIGHT_SOFT || k.cid == CID_HIGHLIGHT_HARD) {
        if (nameOut) *nameOut = c.name;
        return true;
      }
  }
  return false;
}

std::vector<Control> Device::ReadControls(bool withReporting) {
  std::vector<Control> out;
  const uint8_t idx = FeatureIndex(F_REPROG_V4);
  if (!idx) return out;
  Reply r = Call(idx, 0, {});
  if (!r.ok) return out;
  const int count = std::min<int>(r.d[0], 64);
  for (int i = 0; i < count; ++i) {
    Reply c = Call(idx, 1, {uint8_t(i)});
    if (!c.ok) continue;
    Control k;
    k.cid = uint16_t((c.d[0] << 8) | c.d[1]);
    k.tid = uint16_t((c.d[2] << 8) | c.d[3]);
    k.flags = uint16_t(c.d[4] | (c.d[8] << 8));
    k.pos = c.d[5];
    k.group = c.d[6];
    k.gmask = c.d[7];
    if (withReporting) {
      Reply g = GetCidReporting(k.cid);
      if (g.ok) {
        k.haveReporting = true;
        k.repFlags = uint16_t(g.d[2] | (g.d[5] << 8));
        k.remap = uint16_t((g.d[3] << 8) | g.d[4]);
      }
    }
    out.push_back(k);
  }
  return out;
}

Reply Device::SetCidReporting(uint16_t cid, uint8_t flags, uint16_t remap) {
  const uint8_t idx = FeatureIndex(F_REPROG_V4);
  if (!idx) {
    Reply r;
    r.error = 9;
    return r;
  }
  const uint8_t p[5] = {uint8_t(cid >> 8), uint8_t(cid & 0xFF), flags, uint8_t(remap >> 8),
                        uint8_t(remap & 0xFF)};
  return CallDev(devIdx, idx, 3, p, sizeof p, 450, p, 2);  // the reply echoes the CID
}

Reply Device::GetCidReporting(uint16_t cid) {
  const uint8_t idx = FeatureIndex(F_REPROG_V4);
  if (!idx) {
    Reply r;
    r.error = 9;
    return r;
  }
  const uint8_t p[2] = {uint8_t(cid >> 8), uint8_t(cid & 0xFF)};
  return CallDev(devIdx, idx, 2, p, sizeof p, 450, p, 2);
}

}  // namespace hidpp
