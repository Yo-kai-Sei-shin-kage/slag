# FYI

Slag is in development and is not a full V1.0 release yet. Please keep this in mind as I am actively working to optimize, bug fix, and add new features as I go. I am one person developing this language and it's taken 6 years to get to this point where I feel it can be useful even as an I complete language. It's current state is very powerful for what it is. I am hoping I can make it into something more developers see as a very useful tool rather than just a toy to play with. My ultimate goal is absolute raw CPU performance and syntax simplicity. Some features and API calls may change in syntax over the future work to more closely align with this goal. Please keep this in mind if you choose to try out slag for yourself. I hope you enjoy slag as much as I do. Please let me know if you have any issues at all by reporting in the issues section. Include your hardware specs and your OS + OS version so I can dive in and figure out what is causing the issue you are having.


# Slag

A statically typed, compiled systems programming language targeting native Win64 PE executables.
Slag compiles directly to NASM x86-64 assembly, links with no C runtime dependency, and is designed
for bare-metal control on Windows — file processing, multithreaded workloads, and software-rendered
graphics.

No CRT is linked into Slag-compiled programs; only the Win32 API (`kernel32`, `user32`, `gdi32`,
`ws2_32`) is used as needed.

## What it can do

- Ints, floats, strings, bools, fixed-size and global arrays
- Functions, control flow (if/else, while), typed returns
- Threads, sync, and locks for real Win32 concurrency
- Raw memory access (`mem.*`) and bit ops (`bit.*`)
- File I/O and per-handle directory listing (`file.*`)
- 32-slot software-mixed audio (`audio.*`) with automatic WAV loop-point support
- TCP networking (`net.*`)
- Windowing and software-rendered graphics: pixels, textured/shaded/z-buffered triangle
  rasterization, keyboard/mouse input, meshes, procedural textures
- CPU topology and SIMD feature detection

## Getting started

```bash
./install.sh
```

Then explore the language with the interactive examples browser:

```bash
cd examples
./run_examples.sh
```

This walks through 22 runnable programs covering variables, arrays, functions, graphics,
threading, networking, and more.

## Compiling a program

```bash
slag program.slag                          # compile to .asm
nasm -f win64 program.asm -o program.obj   # assemble
x86_64-w64-mingw32-gcc program.obj -o program.exe -nostdlib -lkernel32 -luser32 -lgdi32 -lws2_32 -lwinmm -e _start
```

Or use the helper script: `slagrun program.slag`

## Documentation

- [`slag_spec.md`](documentation/slag_spec.md) — full language specification
- `documentation/man1/slag.1` — man page (`man slag`)
- `syntax_examples.txt` — quick syntax reference
