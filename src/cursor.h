// SPDX-License-Identifier: MIT
// Hides the mouse pointer system-wide while an effect is on screen by swapping
// the standard system cursors for a blank one, and restores the user's scheme
// afterwards. A marker file lets the next start repair things after a crash.
#pragma once
#include "common.h"

namespace syscursor {
void Hide();
void Restore();
bool Hidden();
void RepairAfterCrash();  // call once at startup
}  // namespace syscursor
