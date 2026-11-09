// SPDX-License-Identifier: MIT
// Minimal Windows HID access: enumerate collections and talk to one with
// overlapped I/O on a dedicated reader thread.
#pragma once
#include "common.h"

#include <thread>

struct HidInfo {
  std::wstring path;
  uint16_t vid = 0, pid = 0, usagePage = 0, usage = 0;
  uint16_t inLen = 0, outLen = 0;
  std::wstring product;
};

// All present HID collections for a vendor (0 = every vendor).
std::vector<HidInfo> EnumerateHid(uint16_t vid, bool readStrings);

// Path with the collection number removed, so collections of one USB/BLE
// interface compare equal.
std::wstring HidInterfaceKey(const std::wstring& path);

class HidChannel {
 public:
  using ReportFn = std::function<void(const uint8_t*, size_t)>;
  using GoneFn = std::function<void()>;

  HidChannel() = default;
  HidChannel(const HidChannel&) = delete;
  HidChannel& operator=(const HidChannel&) = delete;
  ~HidChannel() { Close(); }

  bool Open(const HidInfo& info, ReportFn onReport, GoneFn onGone);
  void Close();
  bool IsOpen() const { return h_ != INVALID_HANDLE_VALUE; }
  bool Write(const uint8_t* data, size_t len);
  const HidInfo& Info() const { return info_; }

 private:
  void ReaderLoop();
  HANDLE h_ = INVALID_HANDLE_VALUE;
  HANDLE stop_ = nullptr;
  std::thread reader_;
  HidInfo info_;
  ReportFn onReport_;
  GoneFn onGone_;
  std::mutex writeMutex_;
  std::atomic<bool> closing_{false};
};
