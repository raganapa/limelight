// SPDX-License-Identifier: MIT
// Logitech HID++ 2.0 over a Windows HID collection.
//
// What we know about the Spotlight 2 (measured on real hardware by the foco
// and Projecteur projects; Logitech does not document any of this):
//  * It only accepts LONG reports (0x11, 20 bytes). Short reports get silence.
//  * Over Bluetooth it is device 0xFF; behind a Logi Bolt receiver it is 1..6.
//  * Buttons report nothing over HID++ until diverted with 0x1B04 setCidReporting.
//    CID 0x00D8 = highlight button light press, 0x01A8 = hard press.
//  * Do not divert raw XY, or the gyro stops moving the pointer.
//  * Commands sent back-to-back are silently dropped, so they are spaced out.
//  * Vibration is feature 0x19B0 (setState fn 2, playWaveform fn 4).
#pragma once
#include "common.h"

#include <condition_variable>
#include <map>

#include "hid.h"

namespace hidpp {

constexpr uint8_t kShort = 0x10;
constexpr uint8_t kLong = 0x11;
constexpr uint8_t kSwId = 0x0B;

enum : uint16_t {
  F_ROOT = 0x0000,
  F_FEATURE_SET = 0x0001,
  F_DEVICE_INFO = 0x0003,
  F_DEVICE_NAME = 0x0005,
  F_BATTERY_STATUS = 0x1000,
  F_UNIFIED_BATTERY = 0x1004,
  F_HAPTIC = 0x19B0,
  F_PRESENTER = 0x1A00,
  F_REPROG_V4 = 0x1B04,
  F_WIRELESS_STATUS = 0x1D4B,
};

enum : uint16_t {
  CID_HIGHLIGHT_SOFT = 0x00D8,
  CID_HIGHLIGHT_HARD = 0x01A8,
  CID_ACTION = 0x01B0,
  CID_LASER_SHORT = 0x00FB,
  CID_LASER_LONG = 0x00FC,
};

// Reprog-controls capability flags (getCidInfo bytes 4 | 8 << 8)
enum : uint16_t {
  KF_MOUSE = 0x01, KF_FN = 0x02, KF_NONSTD = 0x04, KF_FN_SENS = 0x08,
  KF_REPROGRAMMABLE = 0x10, KF_DIVERTABLE = 0x20, KF_PERSIST_DIVERTABLE = 0x40,
  KF_VIRTUAL = 0x80, KF_RAW_XY = 0x100, KF_FORCE_RAW_XY = 0x200,
  KF_ANALYTICS = 0x400, KF_RAW_WHEEL = 0x800,
};

const char* FeatureName(uint16_t id);
const char* ControlName(uint16_t cid);
const char* ErrorName(uint8_t code);

struct Reply {
  bool ok = false;
  bool timeout = false;
  bool writeFailed = false;
  uint8_t error = 0;
  uint8_t d[16]{};
  std::string Describe() const;
};

struct Control {
  uint16_t cid = 0, tid = 0, flags = 0;
  uint8_t pos = 0, group = 0, gmask = 0;
  // current reporting (getCidReporting)
  bool haveReporting = false;
  uint16_t repFlags = 0, remap = 0;
};

class Device {
 public:
  using NotifyFn = std::function<void(const uint8_t*, size_t)>;
  using GoneFn = std::function<void()>;

  ~Device() { Close(); }
  // longCol: the collection that carries 20-byte reports. shortCol (optional):
  // a receiver's 7-byte collection, used for connect/disconnect notifications.
  bool Open(const HidInfo& longCol, const HidInfo* shortCol, NotifyFn onNotify, GoneFn onGone);
  void Close();
  bool IsOpen() const { return long_.IsOpen(); }

  // Find a Spotlight on this channel. Sets devIdx. Returns false if none answers.
  bool Locate(std::string* nameOut);

  Reply Call(uint8_t featIdx, uint8_t fn, std::initializer_list<uint8_t> params,
             int timeoutMs = 450);
  Reply CallBuf(uint8_t featIdx, uint8_t fn, const uint8_t* params, size_t n, int timeoutMs);
  Reply CallDev(uint8_t dev, uint8_t featIdx, uint8_t fn, const uint8_t* params, size_t n,
                int timeoutMs);
  // Fire and forget (no wait for the reply).
  bool Send(uint8_t featIdx, uint8_t fn, std::initializer_list<uint8_t> params);

  uint8_t FeatureIndex(uint16_t featureId);  // 0 = not supported
  std::string ReadName();
  bool ProtocolVersion(uint8_t& major, uint8_t& minor);
  std::vector<Control> ReadControls(bool withReporting);
  Reply SetCidReporting(uint16_t cid, uint8_t flags, uint16_t remap);
  Reply GetCidReporting(uint16_t cid);

  uint8_t devIdx = 0xFF;
  bool viaReceiver = false;
  const HidInfo& LongInfo() const { return long_.Info(); }

 private:
  void OnReport(const uint8_t* r, size_t n);
  HidChannel long_, short_;
  NotifyFn onNotify_;
  std::mutex callMutex_;   // one request at a time
  std::mutex m_;
  std::condition_variable cv_;
  bool pending_ = false, done_ = false;
  uint8_t pDev_ = 0, pFeat_ = 0, pFnSw_ = 0;
  Reply reply_;
  static double lastWrite_;  // shared: the remote cares about spacing, not about our objects
  std::map<uint16_t, uint8_t> features_;
};

bool IsSpotlightPid(uint16_t pid);
bool IsReceiverPid(uint16_t pid);

}  // namespace hidpp
