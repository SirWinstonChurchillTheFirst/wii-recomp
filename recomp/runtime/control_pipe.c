// The control pipe (WII_RECOMP_CONTROL): a named pipe that takes one command a line, so
// a script or another program can play. The controls' commands are in input.c; the
// console's are here: reset and power to press its buttons, shot <file> to save the next
// picture, record <frames> <file> to record frames for recomp/tools/replay, record-sound
// <frames> <file> to record the audio processor's work for recomp/tools/sound_bench.py,
// timeline <frames> <file> to record what each thread does (timeline.h), dump and skip for
// the graphics inspector, peek <hex address> <words> to print memory,
// state to print the interrupt and IPC state, eject to empty the drive, and insert
// <image> to put a disc in (in a console, the name of an image on its shelf).
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "console.h"
#include "cpu.h"
#include "debug.h"
#include "dsp_record.h"
#include "gx_inspect.h"
#include "gx_shot.h"
#include "gx.h"
#include "hardware.h"
#include "input.h"
#include "timeline.h"
#include "pads.h"

// Prints guest memory as words, for looking into a program that seems stuck.
static void peek(uint32_t address, int words) {
  for (int i = 0; i < words && i < 256; i++) {
    if (i % 4 == 0) fprintf(stderr, "%speek %08x:", i ? "\n" : "", address + i * 4);
    fprintf(stderr, " %08x", rd32((address + i * 4) | 0x80000000u));
  }
  fprintf(stderr, "\n");
}

static void run_command(const char *line) {
  char path[1024];
  int first, last;
  unsigned address;
  if (input_run_command(line) || pads_command(line)) return;
  if (!strncmp(line, "reset", 5)) reset_button_press();
  else if (!strncmp(line, "power", 5)) { if (!power_button_press()) fprintf(stderr, "control: nothing is listening for the power button\n"); }
  else if (!strncmp(line, "dump", 4)) gx_inspect_dump();
  else if (!strncmp(line, "threads", 7)) debug_threads_now = 1;  // Listed on the game's thread, at its next check.
  else if (sscanf(line, "record-sound %d %1023s", &first, path) == 2) dsp_record_request(first, path);
  else if (sscanf(line, "record %d %1023s", &first, path) == 2) gx_record_request(first, path);
  else if (sscanf(line, "timeline %d %1023s", &first, path) == 2 && first > 0) timeline_request((unsigned)first, path);
  else if (sscanf(line, "skip %d %d", &first, &last) == 2) gx_inspect_skip(first, last);
  else if (sscanf(line, "shot %1023s", path) == 1) gx_shot(path);
  else if (sscanf(line, "peek %x %d", &address, &first) == 2) peek(address, first);
  else if (!strncmp(line, "state", 5)) { interrupts_describe(); hardware_describe(); }
  else if (!strncmp(line, "eject", 5)) console_request_disc(0);
  else if (sscanf(line, "insert %1023s", path) == 1) console_request_disc(path);
  else fprintf(stderr, "control: unknown command: %s", line);
}

static void *control_loop(void *path) {
  // Opened for writing too, so it stays open between senders.
  FILE *pipe_file = fdopen(open(path, O_RDWR), "r");
  char line[1200];  // Room for a shot command with a long path.
  while (pipe_file && fgets(line, sizeof line, pipe_file)) run_command(line);
  return 0;
}

#ifdef _WIN32
#include <windows.h>
// Windows has no named pipes in the file system, so WII_RECOMP_CONTROL names a pipe
// there: "wii" is \\.\pipe\wii. One sender at a time; the pipe opens again after each.
static void *control_pipe_loop(void *name) {
  char path[300], line[1200];
  const char *last = strrchr((const char *)name, '/');
  snprintf(path, sizeof path, "\\\\.\\pipe\\%s", last ? last + 1 : (const char *)name);
  while (1) {
    HANDLE pipe = CreateNamedPipeA(path, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 0, 4096, 0, 0);
    if (pipe == INVALID_HANDLE_VALUE) return 0;
    if (ConnectNamedPipe(pipe, 0) || GetLastError() == ERROR_PIPE_CONNECTED) {
      size_t length = 0;
      char byte;
      DWORD got;
      while (ReadFile(pipe, &byte, 1, &got, 0) && got == 1) {
        if (byte == '\r') continue;
        if (length < sizeof line - 2) line[length++] = byte;
        if (byte == '\n') { line[length] = 0; run_command(line); length = 0; }
      }
      if (length) { line[length++] = '\n'; line[length] = 0; run_command(line); }
    }
    CloseHandle(pipe);
  }
}

void input_start_control_pipe(const char *path) {
  static pthread_t thread;
  pthread_create(&thread, 0, control_pipe_loop, (void *)path);
}
#else
void input_start_control_pipe(const char *path) {
  static pthread_t thread;
  unlink(path);
  mkfifo(path, 0600);
  pthread_create(&thread, 0, control_loop, (void *)path);
}
#endif
