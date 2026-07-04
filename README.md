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
x86_64-w64-mingw32-gcc program.obj -o program.exe -nostdlib -lkernel32 -luser32 -lgdi32 -lws2_32 -e _start
```

Or use the helper script: `slagrun program.slag`

## Documentation

- [`slag_spec.md`](slag_spec.md) — full language specification
- `documentation/man1/slag.1` — man page (`man slag`)
- `syntax_examples.txt` — quick syntax reference
