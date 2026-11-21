# Windows

The console and the games build and run on Windows 10 and 11, 64-bit, with a Vulkan
graphics card. This is a port of the Mac and Linux code, kept in the same files.

## Tools

Install [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`), then its CLANG64
packages, from an MSYS2 window:

```sh
pacman -S --needed mingw-w64-clang-x86_64-{clang,lld,ninja,glfw,sdl3,vulkan-headers,vulkan-loader,shaderc}
```

Python 3 for Windows runs the tools. `MSYS2_ROOT` names MSYS2's folder if it isn't
`C:\msys64`.

## Add and play a game

```bat
python recomp\tools\add_game.py "path\to\game.rvz"
recomp\build\wii-recomp.exe games\<game ID>.wrd
```

The console needs MSYS2's `clang64\bin` on `PATH` for the libraries it loads (GLFW, SDL3,
shaderc, the C++ runtime), until they're copied beside it.

## What differs from the Mac and Linux

- **Memory.** The guest window is reserved with `VirtualAlloc` at the same fixed address,
  and a vectored exception handler takes the signal handler's place (`guest_memory.c`).
- **Processor state.** A title's library can't name a variable in the console directly on
  Windows, so `cpu` and `interrupt_budget` sit at a fixed address just past the window
  (`cpu.h`, `win_compat.c`). The title links against the console's import library and
  exports only its description (`translate/title.def`).
- **Threads.** There's no ucontext, and the calling convention keeps more registers, so
  `threads.c` has its own save and resume for Win64. Each guest thread's host stack is
  reserved whole and grows through a guard page, by the bounds kept in the TEB.
- **Graphics commands** run on a thread of their own, as on the Mac and Linux, through a
  ring mapped twice in a row with placeholders (`VirtualAlloc2`, `MapViewOfFile3`, from
  Windows 10 1803 on). Short sleeps use a high-resolution waitable timer.
- **Sound** goes out through SDL3 (`audio_output_win.c`).
- **The control pipe** (`WII_RECOMP_CONTROL`) is a Windows named pipe: `wii` is
  `\\.\pipe\wii`.
- **POSIX calls** mingw-w64 lacks are in `win_compat.c`, declared in `win/`.

## Not there yet

- The network (`net_win.c` answers that there's no link).
- Real Wii Remotes (`remote_link_win.c` reads none; the simulated remote is used, and
  `WII_RECOMP_REMOTES=real` says none will connect).
- The Wii Menu's hand-over between titles, which uses `exec`.
