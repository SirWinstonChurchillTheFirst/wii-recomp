// Windows stand-in for <sys/resource.h>: getrusage's counts aren't kept, so they read 0.
#pragma once
struct rusage { long ru_nivcsw; };
#define RUSAGE_SELF 0
static inline int getrusage(int who, struct rusage *usage) { (void)who; usage->ru_nivcsw = 0; return 0; }
