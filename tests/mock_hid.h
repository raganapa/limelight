// SPDX-License-Identifier: MIT
#pragma once
#include <map>
#include <mutex>
#include <vector>

#include "../src/common.h"

namespace mock {
extern std::mutex m;
extern std::vector<std::vector<uint8_t>> fromHost;
extern std::map<uint16_t, uint8_t> reporting;
extern std::map<uint16_t, uint16_t> remaps;
extern std::map<uint16_t, uint8_t> lastFlags;
extern int dropped;
void PressButtons(std::initializer_list<uint16_t> cids);
void ForgetSetup();
void AnnounceReconnect();
void SetReceiverMode(bool on);
}  // namespace mock
