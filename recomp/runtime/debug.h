#pragma once
#include <stdint.h>

// Time spent per frame, reported for slow frames when WII_RECOMP_ACTIVITY is set.
enum { STAGE_STREAM, STAGE_PRESENT, STAGE_DISC, STAGE_AUDIO, STAGE_IDLE, STAGE_HANDED, STAGE_COUNT };
// STAGE_HANDED is the game's thread waiting for the graphics thread to do something for it at
// once: read a pixel, the bounding box or a held picture copy. Each such wait is noted by kind.
enum { NOTE_PEEK, NOTE_POKE, NOTE_BOX, NOTE_TOUCH, NOTE_COUNT };
void stage_note(int kind);
void stage_add(int stage, uint64_t ticks);
void stage_frame(float gap_ms);
void stage_shown(void);  // A picture reached the window, which can be later than the frame that made it.
// Called once on the game's thread. With WII_RECOMP_SLOW_SAMPLES set (Linux), that thread is
// sampled ten thousand times a second, and each slow frame's line is followed by where the
// samples fell, as "<file>+<offset>" with a count, most first: what the frame's time went on.
// WII_RECOMP_SLOW_SAMPLES=graphics samples the graphics thread instead, every frame counted,
// for a game whose frames wait on that thread: the caller says which thread it is.
void stage_sample_thread(int graphics);

void debug_threads(void);
extern volatile int debug_threads_now;  // Set by the control pipe's "threads": listed once, soon.
void debug_poll(uint64_t seconds_running);

// Says once, by name and value, that the graphics code met something it doesn't do, and
// counts it under its row ID in docs/hardware_inventory.md (see gaps.h).
void debug_unsupported(const char *id, const char *what, uint32_t value);
