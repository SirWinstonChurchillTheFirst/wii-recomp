// Real Wii Remotes on Windows: none yet, so the simulated remote and board stand in.
// Windows pairs a remote as a HID device, which a backend here could read as
// remote_link_hidraw.c does on Linux.
#include "remote_link.h"

void link_backend_start(int mode) { (void)mode; }
void link_backend_search(void) {}
void link_backend_handover(void) {}
