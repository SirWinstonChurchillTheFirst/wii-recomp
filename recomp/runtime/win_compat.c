// The Windows build's stand-ins for the POSIX calls the runtime makes (win/win_compat.h,
// win/sys/mman.h), and the fixed memory that holds the processor state there (cpu.h).
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#undef interface
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>

#include "guest_memory.h"

// The processor state sits at a fixed address on Windows, as a title's library can't
// reach a variable in the console directly there. Made before main runs.
__attribute__((constructor(101))) static void host_state_make(void) {
  void *at = VirtualAlloc((void *)HOST_STATE_ADDRESS, HOST_STATE_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (at != (void *)HOST_STATE_ADDRESS) {
    fprintf(stderr, "processor state: can't take memory at %#llx (error %lu)\n", HOST_STATE_ADDRESS, GetLastError());
    ExitProcess(1);
  }
}

void *mmap(void *address, size_t size, int protection, int flags, int file, long long offset) {
  (void)offset;
  if (file != -1 || !(flags & MAP_ANON)) { errno = ENOTSUP; return MAP_FAILED; }
  DWORD kind = protection == PROT_NONE ? MEM_RESERVE : MEM_RESERVE | MEM_COMMIT;
  void *at = VirtualAlloc(address, size, kind, protection == PROT_NONE ? PAGE_NOACCESS : PAGE_READWRITE);
  if (!at) { errno = ENOMEM; return MAP_FAILED; }
  return at;
}

int mprotect(void *address, size_t size, int protection) {
  if (protection & PROT_WRITE) {
    if (VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE)) return 0;
    errno = ENOMEM;
    return -1;
  }
  // Read-only or none: committed pages change. Pages never committed have no access and are
  // left so, as the fault handler commits a page at its first touch (guest_memory.c).
  MEMORY_BASIC_INFORMATION info;
  uint8_t *at = address, *end = at + size;
  while (at < end && VirtualQuery(at, &info, sizeof info)) {
    uint8_t *region_end = (uint8_t *)info.BaseAddress + info.RegionSize;
    if (region_end > end) region_end = end;
    DWORD before;
    if (info.State == MEM_COMMIT && !VirtualProtect(at, (size_t)(region_end - at), protection & PROT_READ ? PAGE_READONLY : PAGE_NOACCESS, &before)) {
      errno = EACCES;
      return -1;
    }
    at = region_end;
  }
  return 0;
}

int munmap(void *address, size_t size) {  // The whole reservation at `address`, as mmap made it.
  (void)size;
  if (VirtualFree(address, 0, MEM_RELEASE)) return 0;
  errno = EINVAL;
  return -1;
}

char *win_realpath(const char *path, char *resolved) {
  char full[MAX_PATH];
  if (!_fullpath(full, path, sizeof full)) return 0;
  DWORD kind = GetFileAttributesA(full);
  if (kind == INVALID_FILE_ATTRIBUTES) {
    // A program started by name alone: its own path stands for it.
    char with_suffix[MAX_PATH + 8];
    snprintf(with_suffix, sizeof with_suffix, "%s.exe", full);
    if (GetFileAttributesA(with_suffix) == INVALID_FILE_ATTRIBUTES) { errno = ENOENT; return 0; }
    snprintf(full, sizeof full, "%s", with_suffix);
  }
  for (char *c = full; *c; c++) if (*c == 92) *c = '/';  // The runtime splits paths at '/'.
  if (!resolved) return _strdup(full);
  strcpy(resolved, full);
  return resolved;
}

int win_setenv(const char *name, const char *value, int overwrite) {
  if (!overwrite && getenv(name)) return 0;
  return _putenv_s(name, value);
}

#undef fopen
FILE *win_fopen(const char *path, const char *mode) {
  char binary[16];
  snprintf(binary, sizeof binary, "%s%s", mode, strchr(mode, 'b') ? "" : "b");
  return fopen(path, binary);
}

// A stream over bytes in memory: a temporary file that goes away when it's closed.
FILE *fmemopen(void *data, size_t size, const char *mode) {
  (void)mode;
  char folder[MAX_PATH], path[MAX_PATH];
  if (!GetTempPathA(sizeof folder, folder) || !GetTempFileNameA(folder, "wrc", 0, path)) return 0;
  int descriptor = _open(path, _O_RDWR | _O_BINARY | _O_TEMPORARY | _O_CREAT | _O_TRUNC, 0600);
  if (descriptor < 0) return 0;
  FILE *file = _fdopen(descriptor, "w+b");
  if (!file) { _close(descriptor); return 0; }
  if (fwrite(data, 1, size, file) != size) { fclose(file); return 0; }
  rewind(file);
  return file;
}

void *memmem(const void *hay, size_t hay_bytes, const void *needle, size_t needle_bytes) {
  const uint8_t *at = hay;
  if (!needle_bytes) return (void *)hay;
  for (; hay_bytes >= needle_bytes; at++, hay_bytes--) {
    if (*at == *(const uint8_t *)needle && !memcmp(at, needle, needle_bytes)) return (void *)at;
  }
  return 0;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
int win_nanosleep(const struct timespec *request, struct timespec *remaining) {
  static _Thread_local HANDLE timer;
  static _Thread_local int tried;
  if (remaining) remaining->tv_sec = 0, remaining->tv_nsec = 0;
  long long hundreds = (long long)request->tv_sec * 10000000 + request->tv_nsec / 100;
  if (hundreds <= 0) { SwitchToThread(); return 0; }
  if (!tried) {
    tried = 1;
    timer = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {  // Older Windows: an ordinary timer, with the system's period at 1 ms.
      static LONG once;
      if (!InterlockedExchange(&once, 1)) timeBeginPeriod(1);
      timer = CreateWaitableTimerExW(0, 0, 0, TIMER_ALL_ACCESS);
    }
  }
  LARGE_INTEGER due;
  due.QuadPart = -hundreds;  // Relative, in 100 ns steps.
  if (!timer || !SetWaitableTimer(timer, &due, 0, 0, 0, FALSE)) { Sleep((DWORD)((hundreds + 9999) / 10000)); return 0; }
  WaitForSingleObject(timer, INFINITE);
  return 0;
}

#undef rename
int win_rename(const char *from, const char *to) {
  // Through to the disk before it returns, as files.c's write, flush, then rename expects.
  if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
  errno = GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
  return -1;
}

long sysconf(int name) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return name == _SC_PAGESIZE ? (long)info.dwPageSize : (long)info.dwNumberOfProcessors;
}
#endif
