// Saves the next picture the game shows, for scripts that need to see the screen.
// Taken from the game's own picture, so it doesn't matter where the window is or
// what covers it. Written as a binary PPM, which needs no library. Vulkan hands the
// picture over through vk_picture_sink; OpenGL asks gx_shot_wanted() as it presents.
//
// The picture is copied as it's shown and written out on a thread of its own: writing
// 3.5 MB where it's handed over held the game's thread for a fifth of a second, which
// changed when its interrupts came and so what the game did (Super Paper Mario's map
// drawing stopped on an assertion). A tool must never change what it measures.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_shot.h"
#ifdef GX_WITH_VULKAN
#include "gx_vulkan.h"
#endif

static char wanted[1024];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
  char path[sizeof wanted];
  int width, height;
  uint8_t pixels[];  // RGB, a row after another.
} Shot;

int gx_shot_wanted(void) { return wanted[0] != 0; }

static void *write_shot(void *argument) {
  Shot *shot = argument;
  char temporary[sizeof shot->path + 8];
  snprintf(temporary, sizeof temporary, "%s.part", shot->path);
  FILE *file = fopen(temporary, "wb");
  if (!file) {
    fprintf(stderr, "shot: can't write %s\n", shot->path);
  } else {
    fprintf(file, "P6\n%d %d\n255\n", shot->width, shot->height);
    fwrite(shot->pixels, 3, (size_t)shot->width * (size_t)shot->height, file);
    fclose(file);
    rename(temporary, shot->path);  // Whole or not at all, for whoever is waiting on it.
  }
  free(shot);
  return 0;
}

void gx_shot_write(const uint8_t *rgba, int width, int height) {
  char path[sizeof wanted];
  pthread_mutex_lock(&lock);
  memcpy(path, wanted, sizeof path);
  wanted[0] = 0;
#ifdef GX_WITH_VULKAN
  vk_picture_sink = 0;
#endif
  pthread_mutex_unlock(&lock);
  if (!path[0]) return;
  size_t count = (size_t)width * (size_t)height;
  Shot *shot = malloc(sizeof *shot + count * 3);
  if (!shot) {
    fprintf(stderr, "shot: no memory for %s\n", path);
    return;
  }
  memcpy(shot->path, path, sizeof shot->path);
  shot->width = width, shot->height = height;
  for (size_t i = 0; i < count; i++) memcpy(shot->pixels + i * 3, rgba + i * 4, 3);
  pthread_t thread;
  if (pthread_create(&thread, 0, write_shot, shot)) write_shot(shot);
  else pthread_detach(thread);
}

void gx_shot(const char *path) {
  pthread_mutex_lock(&lock);
  snprintf(wanted, sizeof wanted, "%s", path);
#ifdef GX_WITH_VULKAN
  vk_picture_sink = gx_shot_write;
#endif
  pthread_mutex_unlock(&lock);
}
