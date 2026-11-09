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
// Press the highlight button with some force (0 = let go). The remote reports the hard
// press only if it is set up to and the force reaches its force-sensing threshold.
void Press(int force);
int ForceValue();         // the remote's current force-sensing threshold
void ForgetPressure();    // the remote lost its pressure setting (nobody told the host)
void ForgetHardPress();   // the remote stopped reporting the hard press (nobody told the host)
void ForgetSetup();
void AnnounceReconnect();
void SetReceiverMode(bool on);
void SetQuirky(bool on);
int DeliberateDrops();
}  // namespace mock
