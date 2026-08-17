# FYI

Slag is pre-1.0 and under active development — I'm one person building it, and it's taken 6 years to reach this point. I have only recently made a public repository for this because I finally feel like it is ready for others to try, even if incomplete as far as my personal standards are concerned. What's here now is fully capable and I use it daily for real projects (including the engine for a co-op survival game). Some syntax and APIs may still shift as I push toward the core goals: raw CPU performance and simplicity.
My goal is for Slag to be seen as a serious tool, not just an experiment. If you try it and hit issues, please report them — include your hardware specs and OS/version so I can dig in.


# Slag

A statically typed, compiled systems programming language targeting native Win64 PE executables.
Slag compiles directly to NASM x86-64 assembly, links with no C runtime dependency, and is designed
for bare-metal control on Windows — file processing, multithreaded workloads, and software-rendered
graphics.

No CRT is linked into Slag-compiled programs; only the Win32 API (`kernel32`, `user32`, `gdi32`,
`ws2_32`, `winmm`, `bcrypt`, `dxgi`, `d3d11`, `hid`) is used as needed. The compiler emits only the
runtime modules a program actually references, so unused subsystems add nothing to the output.

## What it can do

- Ints, floats, strings, bools, fixed-size and global arrays
- Functions, control flow, typed returns
- Threads, sync, and locks for real Win32 concurrency
- Raw memory access (`mem.*`) and bit ops (`bit.*`)
- File I/O and per-handle directory listing (`file.*`)
- 32-slot software-mixed audio (`audio.*`) with automatic WAV loop-point support
- TCP networking (`net.*`) with a persistent multi-client server and LAN discovery
- Encrypted P2P: ECDH key exchange + AES via CNG (`crypto.*`)
- Windowing and software-rendered graphics: pixels, textured/shaded/z-buffered triangle
  rasterization, keyboard/mouse input, meshes (`mesh.*`), procedural textures (`tex.*`)
- GPU-accelerated 3D via Direct3D 11 (`gpu.*`): per-pixel lit ubershader with unlimited dynamic
  point lights, multi-light PCF shadow maps, hardware tessellation + displacement, and a
  GPU-resident rigid-body physics solver; plus matrix stack (`mat.*`) and SIMD ops (`simd.*`)
- HID gamepad input for any controller (`gpad.*`, `on gpad_button`)
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
x86_64-w64-mingw32-gcc program.obj -o program.exe -nostdlib -lkernel32 -luser32 -lgdi32 -lws2_32 -lwinmm -lbcrypt -ldxgi -ld3d11 -lhid -e _start
```

Or use the helper script: `slagrun program.slag`

## Documentation

- [`slag_spec.md`](documentation/slag_spec.md) — full language specification
- `documentation/man1/slag.1` — man page (`man slag`)
- `syntax_examples.txt` — quick syntax reference
