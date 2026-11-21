// Real Wii Remotes on Windows: none yet, so the simulated remote and board stand in.
// Windows pairs a remote as a HID device, which a backend here could read as
// remote_link_hidraw.c does on Linux.
#include <stdio.h>

#include "remote_link.h"

void link_backend_start(int mode) {
  // Asked for real ones only (WII_RECOMP_REMOTES=real), none will come: said, not waited for.
  if (mode == LINK_REAL) fprintf(stderr, "remotes: real Wii Remotes aren't read on Windows yet, so none will connect\n");
}
void link_backend_search(void) {}
void link_backend_handover(void) {}
