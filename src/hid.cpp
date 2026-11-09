// SPDX-License-Identifier: MIT
#include "hid.h"

#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

std::vector<HidInfo> EnumerateHid(uint16_t vid, bool readStrings) {
  std::vector<HidInfo> out;
  GUID hidGuid;
  HidD_GetHidGuid(&hidGuid);
  HDEVINFO set =
      SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) return out;
  SP_DEVICE_INTERFACE_DATA ifd{};
  ifd.cbSize = sizeof ifd;
  for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); ++i) {
    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
    if (!need) continue;
    std::vector<BYTE> buf(need + 8);
    auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
    det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) continue;
    HidInfo hi;
    hi.path = det->DevicePath;
    HANDLE h = CreateFileW(hi.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) continue;
    HIDD_ATTRIBUTES at{};
    at.Size = sizeof at;
    if (HidD_GetAttributes(h, &at)) {
      hi.vid = at.VendorID;
      hi.pid = at.ProductID;
    }
    if (vid && hi.vid != vid) {
      CloseHandle(h);
      continue;
    }
    PHIDP_PREPARSED_DATA pp = nullptr;
    if (HidD_GetPreparsedData(h, &pp)) {
      HIDP_CAPS caps{};
      if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
        hi.usagePage = caps.UsagePage;
        hi.usage = caps.Usage;
        hi.inLen = caps.InputReportByteLength;
        hi.outLen = caps.OutputReportByteLength;
      }
      HidD_FreePreparsedData(pp);
    }
    if (readStrings) {
      wchar_t s[128] = {};
      if (HidD_GetProductString(h, s, sizeof s)) hi.product = s;
    }
    CloseHandle(h);
    out.push_back(hi);
  }
  SetupDiDestroyDeviceInfoList(set);
  return out;
}

bool HidChannel::Open(const HidInfo& info, ReportFn onReport, GoneFn onGone) {
  Close();
  h_ = CreateFileW(info.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                   nullptr);
  if (h_ == INVALID_HANDLE_VALUE) {
    Log("hid: cannot open %s (%lu)", Narrow(info.path).c_str(), GetLastError());
    return false;
  }
  HidD_SetNumInputBuffers(h_, 64);
  info_ = info;
  onReport_ = std::move(onReport);
  onGone_ = std::move(onGone);
  closing_ = false;
  stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  reader_ = std::thread([this] { ReaderLoop(); });
  return true;
}

void HidChannel::Close() {
  if (h_ == INVALID_HANDLE_VALUE) return;
  closing_ = true;
  if (stop_) SetEvent(stop_);
  if (reader_.joinable()) reader_.join();
  CloseHandle(h_);
  h_ = INVALID_HANDLE_VALUE;
  if (stop_) CloseHandle(stop_);
  stop_ = nullptr;
}

void HidChannel::ReaderLoop() {
  const DWORD len = std::max<DWORD>(info_.inLen, 8);
  std::vector<uint8_t> buf(len + 8);
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool gone = false;
  while (!closing_) {
    ResetEvent(ov.hEvent);
    DWORD got = 0;
    if (!ReadFile(h_, buf.data(), len, &got, &ov)) {
      DWORD e = GetLastError();
      if (e != ERROR_IO_PENDING) {
        gone = true;
        break;
      }
      HANDLE hs[2] = {ov.hEvent, stop_};
      DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
      if (r != WAIT_OBJECT_0) {
        CancelIoEx(h_, &ov);
        GetOverlappedResult(h_, &ov, &got, TRUE);
        break;
      }
      if (!GetOverlappedResult(h_, &ov, &got, FALSE)) {
        e = GetLastError();
        if (e != ERROR_OPERATION_ABORTED) gone = true;
        break;
      }
    }
    if (got > 0 && onReport_) onReport_(buf.data(), got);
  }
  CloseHandle(ov.hEvent);
  if (gone && !closing_ && onGone_) onGone_();
}

bool HidChannel::Write(const uint8_t* data, size_t len) {
  if (h_ == INVALID_HANDLE_VALUE || info_.outLen == 0) return false;
  std::lock_guard<std::mutex> lk(writeMutex_);
  std::vector<uint8_t> b(info_.outLen, 0);
  memcpy(b.data(), data, std::min<size_t>(len, b.size()));
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD wrote = 0;
  BOOL ok = WriteFile(h_, b.data(), DWORD(b.size()), &wrote, &ov);
  if (!ok && GetLastError() == ERROR_IO_PENDING) {
    if (WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0) {
      ok = GetOverlappedResult(h_, &ov, &wrote, FALSE);
    } else {
      CancelIoEx(h_, &ov);
      GetOverlappedResult(h_, &ov, &wrote, TRUE);
      ok = FALSE;
    }
  }
  CloseHandle(ov.hEvent);
  if (!ok) {
    // Some Bluetooth stacks only accept HidD_SetOutputReport.
    ok = HidD_SetOutputReport(h_, b.data(), ULONG(b.size()));
  }
  return ok != FALSE;
}
