#pragma once
#include <stdint.h>

// Time spent per frame, reported for slow frames when WII_RECOMP_ACTIVITY is set.
enum { STAGE_STREAM, STAGE_PRESENT, STAGE_DISC, STAGE_AUDIO, STAGE_IDLE, STAGE_COUNT };
void stage_add(int stage, uint64_t ticks);
void stage_frame(float gap_ms);
void stage_shown(void);  // A picture reached the window, which can be later than the frame that made it.

void debug_threads(void);
extern volatile int debug_threads_now;  // Set by the control pipe's "threads": listed once, soon.
void debug_poll(uint64_t seconds_running);

// Says once, by name and value, that the graphics code met something it doesn't do, and
// counts it under its row ID in docs/hardware_inventory.md (see gaps.h).
void debug_unsupported(const char *id, const char *what, uint32_t value);
