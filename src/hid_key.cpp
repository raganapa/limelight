// SPDX-License-Identifier: MIT
#include "hid.h"

std::wstring HidInterfaceKey(const std::wstring& path) {
  std::wstring p = path;
  for (auto& c : p) c = wchar_t(towlower(c));
  // drop "&colNN"
  size_t pos;
  while ((pos = p.find(L"&col")) != std::wstring::npos) {
    size_t end = pos + 4;
    while (end < p.size() && iswxdigit(p[end])) ++end;
    p.erase(pos, end - pos);
  }
  // drop the trailing collection instance number before the interface GUID
  size_t g = p.rfind(L"#{");
  if (g != std::wstring::npos) {
    size_t amp = p.rfind(L'&', g);
    size_t hash = p.rfind(L'#', g - 1);
    if (amp != std::wstring::npos && (hash == std::wstring::npos || amp > hash)) p.erase(amp, g - amp);
  }
  return p;
}
