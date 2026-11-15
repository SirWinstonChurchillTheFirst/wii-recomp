// What the runtime uses from POSIX that mingw-w64 doesn't have, for the Windows build.
// build_file.py includes this ahead of every runtime file there. See win_compat.c.
#pragma once
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define mkdir(path, mode) _mkdir(path)
#define realpath(path, resolved) win_realpath(path, resolved)
#define setenv(name, value, overwrite) win_setenv(name, value, overwrite)
#define fopen(path, mode) win_fopen(path, mode)
char *win_realpath(const char *path, char *resolved);
int win_setenv(const char *name, const char *value, int overwrite);
FILE *win_fopen(const char *path, const char *mode);  // Always binary, as on POSIX.
FILE *fmemopen(void *data, size_t size, const char *mode);

#define _SC_PAGESIZE 1
#define _SC_NPROCESSORS_ONLN 2
long sysconf(int name);
void *memmem(const void *hay, size_t hay_bytes, const void *needle, size_t needle_bytes);
#define fsync(descriptor) _commit(descriptor)
#define rename(from, to) win_rename(from, to)
int win_rename(const char *from, const char *to);  // Replaces `to`, as on POSIX.

// Short sleeps: mingw's nanosleep rounds up to Windows' timer, a millisecond at best and
// 15.6 by default, which turned the commands thread's 50 µs naps and the game's idle waits
// into lost frames. A high-resolution waitable timer sleeps for what's asked (Windows 10
// 1803 on), with the system timer at 1 ms as the fallback.
#include <time.h>
#include <pthread.h>
#define nanosleep(request, remaining) win_nanosleep(request, remaining)
int win_nanosleep(const struct timespec *request, struct timespec *remaining);
#endif
