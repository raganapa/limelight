// SPDX-License-Identifier: MIT
// Runs the diagnostics' hard press finder against the simulated remote, with a simulated
// user pressing the highlight button over and over. Feed it the menu choices on stdin:
//   printf '3\n\n6\n' | wine build/test_diag.exe [alt]
#include <cstdio>
#include <thread>

#include "../src/diag.h"
#include "../src/settings.h"
#include "mock_hid.h"

int main(int argc, char** argv) {
  const bool alt = argc > 1 && strcmp(argv[1], "alt") == 0;
  {  // start from default settings
    Settings s;
    s.Save();
  }
  if (alt) {
    mock::SetHardAs(0x00FB);  // the hard press arrives as another control
    mock::SetForceScenario(200, false);
  } else
    mock::SetForceScenario(233, true);  // stuck on a force value that is too firm
  std::atomic<bool> stop{false};
  std::thread user([&] {
    while (!stop) {
      Sleep(900);
      mock::Press(60);   // light...
      Sleep(250);
      mock::Press(300);  // ...then hard...
      Sleep(250);
      mock::Press(0);    // ...then let go
    }
  });
  RunDiagnostics();
  stop = true;
  user.join();
  Settings s;
  s.Load();
  s.Sanitize();
  printf("\nRESULT pressure=%d hardCid=%04X force=%d diverted00D8=%d diverted01A8=%d\n",
         s.pressure, s.hardCid, mock::ForceValue(), int(mock::reporting[0x00D8] & 1),
         int(mock::reporting[0x01A8] & 1));
  const bool ok = alt ? (s.hardCid == 0x00FB && mock::ForceValue() == 200)
                      : (s.pressure == 2 && mock::ForceValue() == 233);
  printf("%s\n", ok ? "PASSED" : "FAILED");
  return ok ? 0 : 1;
}
