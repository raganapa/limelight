// SPDX-License-Identifier: MIT
#include "cursor.h"

namespace syscursor {
namespace {
bool g_hidden = false;
const DWORD kIds[] = {32512 /*NORMAL*/, 32513 /*IBEAM*/,   32514 /*WAIT*/,     32515 /*CROSS*/,
                      32516 /*UP*/,     32642 /*SIZENWSE*/, 32643 /*SIZENESW*/, 32644 /*SIZEWE*/,
                      32645 /*SIZENS*/, 32646 /*SIZEALL*/,  32648 /*NO*/,       32649 /*HAND*/,
                      32650 /*APPSTARTING*/, 32651 /*HELP*/, 32671 /*PIN*/,     32672 /*PERSON*/};

std::wstring MarkerPath() { return AppDataDir(false) + L"\\cursor-hidden.marker"; }

HCURSOR MakeBlank() {
  BYTE andMask[32 * 32 / 8];
  BYTE xorMask[32 * 32 / 8];
  memset(andMask, 0xFF, sizeof andMask);
  memset(xorMask, 0x00, sizeof xorMask);
  return CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32, andMask, xorMask);
}
}  // namespace

void Hide() {
  if (g_hidden) return;
  HANDLE f = CreateFileW(MarkerPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  HCURSOR blank = MakeBlank();
  if (!blank) return;
  for (DWORD id : kIds) {
    HCURSOR c = CopyCursor(blank);  // SetSystemCursor takes ownership
    if (c && !SetSystemCursor(c, id)) DestroyCursor(c);
  }
  DestroyCursor(blank);
  g_hidden = true;
}

void Restore() {
  if (!g_hidden) return;
  SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
  DeleteFileW(MarkerPath().c_str());
  g_hidden = false;
}

bool Hidden() { return g_hidden; }

void RepairAfterCrash() {
  if (GetFileAttributesW(MarkerPath().c_str()) != INVALID_FILE_ATTRIBUTES) {
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
    DeleteFileW(MarkerPath().c_str());
    Log("restored cursors left hidden by a previous run");
  }
}
}  // namespace syscursor
