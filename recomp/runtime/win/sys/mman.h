// Windows stand-in for <sys/mman.h>: the few calls the runtime makes, on VirtualAlloc
// (win_compat.c). An anonymous PROT_NONE map reserves address space; mprotect commits it.
#pragma once
#include <stddef.h>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANON 0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FIXED 0x10
#define MAP_FAILED ((void *)-1)
void *mmap(void *address, size_t size, int protection, int flags, int file, long long offset);
int mprotect(void *address, size_t size, int protection);
int munmap(void *address, size_t size);
