// SPDX-License-Identifier: MIT
#include "common.h"

#include <cstdarg>

std::wstring AppDataDir(bool roaming) {
  PWSTR base = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(roaming ? FOLDERID_RoamingAppData : FOLDERID_LocalAppData, 0,
                                     nullptr, &base))) {
    dir = base;
    CoTaskMemFree(base);
  } else {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    dir = tmp;
  }
  dir += L"\\Limelight";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

static std::mutex g_logMutex;
static FILE* g_logFile = nullptr;

void LogInit() {
  std::lock_guard<std::mutex> lock(g_logMutex);
  if (g_logFile) return;
  std::wstring path = AppDataDir(false) + L"\\limelight.log";
  // Keep the log small: start over when it grows past 1 MB.
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  bool big = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
             (fad.nFileSizeHigh > 0 || fad.nFileSizeLow > 1024 * 1024);
  g_logFile = _wfopen(path.c_str(), big ? L"wb" : L"ab");
}

void Log(const char* fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  SYSTEMTIME st;
  GetLocalTime(&st);
  char line[1200];
  snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond,
           st.wMilliseconds, msg);
  OutputDebugStringA(line);
  std::lock_guard<std::mutex> lock(g_logMutex);
  if (g_logFile) {
    fputs(line, g_logFile);
    fflush(g_logFile);
  }
}
