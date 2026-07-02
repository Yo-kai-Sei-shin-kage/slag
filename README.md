# Slag

A statically typed, compiled systems programming language targeting native Win64 PE executables. 
Slag compiles directly to NASM x86-64 assembly, links with no C runtime dependency, and is designed 
for bare-metal control on Windows 11 — file processing, multithreaded workloads, and software-rendered 
graphics (with a PS2-era rendering ceiling as a long-term target).

The full language design is documented in [`slag_spec.md`](slag_spec.md).

## Quick Start

After installation, explore the language with the interactive examples browser:

```bash
cd examples
./run_examples.sh
```

This launches a terminal interface where you can view source code and run any of the 22 included examples covering variables, arrays, functions, graphics, threading, networking, and more.

## Status

Slag is under active development. The pipeline currently supports:

- **Lexer** — full tokenization of all core keywords/operators; arithmetic is plain infix expression syntax, no wrapper blocks or sigils.
- **Parser** — complete recursive descent parser producing a full AST: functions, typed variable and array declarations (declared exclusively with `global`/`local` scope modifiers), if/else/else-if, while, typed returns, thread/sync/lock blocks, and `on` event handlers.
- **Code generator** — emits NASM x86-64 Win64 assembly. Working:
  - Integer and float arithmetic, comparisons, logical short-circuit operators
  - Mixed int/float expressions auto-promote (int operands are converted with `cvtsi2sd`); assigning a float expression to an `int` truncates via `cvttsd2si`. No cast syntax is required
  - Fixed-size arrays (declaration, indexing, `.len`)
  - Global arrays (`global int[] data = {1, 2, 3};`) — accessible from any function, stored in `.data` section
  - Control flow (if/else, while)
  - `print`/`println` for ints, floats, string literals, and string variables
  - String variable ptr+len tracking
  - `readfile()` — reads file contents into a `str` via Win32 `CreateFileA`/`ReadFile`
  - `readline()` — reads a line from stdin via Win32 `ReadConsoleA`
  - User-defined function calls under the Win64 calling convention (int and float args/returns, mixed and nested calls), verified working with high argument counts (19- and 22-argument calls): the first four positional args go in `rcx`/`rdx`/`r8`/`r9` (or `xmm0`-`xmm3` for floats), and the rest are passed on the stack
  - **CPU topology detection** — at startup the runtime calls `GetLogicalProcessorInformation`, walks the `SYSTEM_LOGICAL_PROCESSOR_INFORMATION` buffer, and populates the following read-only globals accessible via builtins:
    - `cpu.physical_cores()` — physical core count
    - `cpu.logical_cores()` — total logical processors (sum of per-core processor mask `popcnt`)
    - `cpu.threads_per_core()` — logical / physical cores
    - `cpu.safe_thread_limit()` — logical cores − 1 (minimum 1), a conservative upper bound for worker thread count
    - `cpu.hyperthreaded()` — 1 if SMT/Hyper-Threading is active on any core (detected via `ProcessorCore.Flags`), 0 otherwise. Falls back to all-1s / hyperthreaded=0 if the API call fails
  - **Concurrency — `thread` / `sync` / `lock`**:
    - `thread { ... }` — spawns a real Win32 thread; the block body is compiled to a standalone thread proc and launched via `CreateThread`, with the handle stored in an internal table (up to 64 outstanding threads between syncs)
    - `sync { ... }` — waits for all threads spawned since the last sync via `WaitForMultipleObjects(..., TRUE, INFINITE)`, closes their handles, and resets the table; the sync body runs after the wait
    - `lock { ... }` — mutual exclusion via a single global `CRITICAL_SECTION` (initialized at startup); only one thread executes inside any `lock` block at a time, reentrant on the owning thread
  - **Memory / buffer primitives — `mem.*`** (raw heap buffers addressed by plain-int pointers; the `peek`/`poke` accessors are inlined to a single `mov` at the call site — no function-call overhead — benchmarked at ~0.3 ns/op, on par with native array access):
    - `mem.alloc(nbytes)` — `HeapAlloc` (zero-initialized); returns the buffer address as an int, or 0 on failure
    - `mem.free(ptr)` — `HeapFree`
    - `mem.poke8(ptr, byteoff, val)` / `mem.peek8(ptr, byteoff)` — single-byte store/load at a byte offset (for network/crypto byte streams)
    - `mem.poke64(ptr, byteoff, val)` / `mem.peek64(ptr, byteoff)` — 8-byte store/load at a byte offset (for bulk moves and bignum limbs)
  - **Bit manipulation — `bit.*`** (inlined to single CPU instructions, no function-call overhead):
    - `bit.shl(val, count)` — left shift; enables 16.16 fixed-point: `bit.shl(n, 16)` converts int to fixed
    - `bit.shr(val, count)` — unsigned right shift; `bit.shr(fixed, 16)` converts fixed to int
  - **Matrix stack — `mat.*`** (3x4 transformation matrices using 16.16 fixed-point; 16-level push/pop stack; pre-computed sin/cos lookup tables):
    - `mat.identity()` — reset current matrix to identity
    - `mat.push()` / `mat.pop()` — push/pop matrix stack for hierarchical transforms
    - `mat.translate(x, y, z)` — multiply translation into current matrix
    - `mat.scale(sx, sy, sz)` — multiply scale into current matrix
    - `mat.rotate_x(angle)` / `mat.rotate_y(angle)` / `mat.rotate_z(angle)` — multiply rotation (angle 0-255 maps to 0-360°)
    - `mat.transform_x(x, y, z)` / `mat.transform_y(...)` / `mat.transform_z(...)` — transform point, return single coordinate
  - **SIMD — `simd.*`** (SSE2 128-bit vector operations for graphics and bulk processing; all pointers are 16-byte buffers):
    - *Arithmetic:* `simd.addf4(dest,a,b)`, `simd.subf4`, `simd.mulf4`, `simd.divf4` — packed 4-float ops
    - *Vector:* `simd.dot4(dest,a,b)` — dot product; `simd.cross3(dest,a,b)` — 3D cross; `simd.normalize4(dest,v)` — unit vector; `simd.lint4(dest,a,b,t)` — linear interpolation
    - *Matrix:* `simd.mat4_mul(dest,a,b)` — 4×4 matrix multiply; `simd.mat4_vec4(dest,m,v)` — matrix-vector multiply
    - *RGB565:* `simd.rgb565_unpack(r,g,b,pixels)` — extract channels from 8 pixels; `simd.rgb565_pack(dest,r,g,b)` — combine channels; `simd.rgb565_blend(dest,a,b,alpha)` — alpha blend 8 pixels
  - **Networking — `net.*`** (TCP via `ws2_32`; supports persistent peer-to-peer sessions between two machines running the same program):
    - `net.start()` / `net.end()` — `WSAStartup` / close sockets + `WSACleanup`
    - `net.bind(port)` — create socket, bind, and listen (no block)
    - `net.accept()` — block until a peer connects to the bound socket; call in a loop for a persistent listener
    - `net.listen(port)` — convenience: bind + listen + accept in one blocking call
    - `net.connect(host, port)` — connect to a peer (host as a string literal, e.g. `"127.0.0.1"`)
    - `net.send(byte)` / `net.recv()` — send/receive a single byte (recv returns int, −1 on failure)
    - `net.send_buf(ptr, len)` — send `len` bytes from a buffer, looping until all are sent
    - `net.recv_buf(ptr, maxlen)` — receive up to `maxlen` bytes into a buffer; returns the count (0 when the peer closes)
    - `net.ack()` — returns 1/0: did the last network op succeed
    - `net.connected()` — returns 1/0: is the active connection still alive (clears on peer disconnect)
  - **Windowing and software-rendered graphics**:
    - `window.open(w, h, title)` — creates a window on its own thread with a BGRA DIB framebuffer; uses TLS so multiple windows can be opened from separate threads
    - `pixel(x, y, r, g, b)` — writes a single pixel into the framebuffer; bounds-checks against the framebuffer dimensions and silently no-ops out-of-range writes (safe to draw off-screen)
    - `window.flush()` — pumps the message queue and blits the framebuffer to the window (sleeps ~16 ms, giving a ~60fps cap)
    - `window.is_open()` — returns 1/0, enabling `while (window.is_open()) { ... }` main loops
    - `window.close()` — requests the window close (posts `WM_CLOSE`)
    - `window.capture_mouse()` — captures mouse, clips cursor to window, hides cursor (for FPS-style controls)
    - `window.release_mouse()` — releases mouse capture, shows cursor
    - `window.native()` — returns native screen resolution as "WxH" string (e.g., "1920x1080")
    - `window.clear(r, g, b)` — fills the DIB framebuffer with a solid color via `rep stosd`
    - `window.text(x, y, value, r, g, b)` — draws a `str` or `int` as text at `(x, y)` via GDI `TextOutA` against the window's memory DC, composited into the same DIB surface `window.flush()` blits
  - **Rasterization and timing builtins** (all `fill_triangle*` variants perform backface culling — a signed-area winding test discards counter-clockwise triangles, so vertices must be supplied in clockwise (CW) order to be drawn):
    - `fill_triangle(x0,y0,x1,y1,x2,y2,r,g,b)` — flat-shaded scanline triangle rasterizer, writes directly to the framebuffer with no per-pixel call overhead; bounds-clamped
    - `fill_triangle_gradient(x0,y0,r0,g0,b0, x1,y1,r1,g1,b1, x2,y2,r2,g2,b2)` — per-vertex color (Gouraud-style) scanline rasterizer with linear interpolation along edges and across spans (smooth color blending across a triangle's interior)
    - `fill_triangle_z(x0,y0,z0, x1,y1,z1, x2,y2,z2, r,g,b)` — depth-tested triangle rasterizer; x/y are int screen coords, z values are float depth; pixels only drawn if closer than existing z-buffer value
    - `fill_triangle_affine(x0,y0,u0,v0, x1,y1,u1,v1, x2,y2,u2,v2, tex_ptr,tex_w,tex_h)` — PS1-style affine texture-mapped triangle; UV coords interpolated linearly (no perspective correction); texture is RGB565 format (2 bytes/pixel)
    - `fill_triangle_persp(x0,y0,z0,u0,v0, x1,y1,z1,u1,v1, x2,y2,z2,u2,v2, tex_ptr,tex_w,tex_h)` — PS2-style perspective-correct texture-mapped triangle; interpolates 1/z, u/z, v/z for correct texture mapping on angled surfaces
    - `fill_triangle_pcolor(verts, tex_ptr,tex_w,tex_h)` — perspective-correct textured triangle with per-vertex colors; verts is pointer to 24 int64s (3 vertices × 8 values: x,y,z,u,v,r,g,b); texture color multiplied by interpolated vertex color
    - `zbuffer.clear()` — resets the z-buffer to max depth (call at start of each frame)
    - `time.now_ms()` — milliseconds since system start (`GetTickCount`), useful for fps counters and frame timing
    - `time.now_us()` — microseconds via `QueryPerformanceCounter`/`QueryPerformanceFrequency`; high-resolution timing for benchmarks and precise frame timing
  - **Keyboard/mouse event handlers** — `on key_down`, `on key_up`, `on mouse_move`, `on mouse_down`, `on mouse_up` (buttons: 0=left, 1=right, 2=middle), and `on mouse_wheel(int delta)` (delta is ±120 per notch) are compiled to standalone procs and dispatched directly from the window's `WndProc`; handlers not defined by the user fall back to no-op stubs
  - **Shared input-state builtins** — since `on` handlers run in their own stack frames, a small fixed set of `.bss` globals plus accessor builtins let handlers communicate with the main loop:
    - `input.drag_x()` / `input.drag_y()` — accumulated drag offset
    - `input.add_drag(dx, dy)` — accumulate a drag delta (called from `on mouse_move`)
    - `input.is_dragging()` / `input.set_dragging(v)` — drag-state flag
    - `input.last_x()` / `input.last_y()` / `input.set_last(x, y)` — last recorded mouse position
    - `input.wheel()` — returns and resets the accumulated scroll-wheel delta
    - `input.add_wheel(delta)` — accumulate wheel delta (called from `on mouse_wheel`)
    - `input.set_bbox(minx, miny, maxx, maxy)` / `input.in_bbox(mx, my)` — axis-aligned bounding box check, used for hit-testing draggable objects
  - **3D pipeline (demonstrated, written entirely in Slag)** — Bresenham line drawing, flat-shaded filled triangles, perspective projection, and per-axis rotation matrices using precomputed sin/cos constants. Per-face Lambertian lighting is also implemented in Slag: each face's normal is computed as the cross product of two rotated edge vectors, dotted with a fixed light direction, remapped to a `[0.3, 1.0]` brightness, and applied to the face color. Demos include a wireframe rotating cube, a mouse-drag + scroll-wheel-rotation cube, a solid 6-face flat-shaded rotating cube (~60fps at 640x800), and a per-face-lit shaded cube whose faces brighten and dim as it rotates
  - **Config file reading** — Text configuration files can be parsed using `readfile()` combined with `mem.peek8()` for byte-by-byte parsing. The `config_tests/` directory contains examples of config-driven animations where parameters (positions, colors, paths) are read from external `.txt` files at runtime
  - **BMP image loading** (`mesh.*`):
    - `mesh.bmp_width(ptr)` / `mesh.bmp_height(ptr)` — dimensions of a 24-bit uncompressed BMP loaded via `readfile()`
    - `mesh.bmp_pixel(ptr, x, y)` — returns 0x00RRGGBB for pixel at (x,y)
    - `mesh.bmp_gray(ptr, x, y)` — returns 0-255 grayscale value
  - **Mesh management** (`mesh.*`):
    - `mesh.create(verts, faces)` — allocate a mesh with capacity for N vertices and M faces; returns handle
    - `mesh.destroy(handle)` — free mesh memory
    - `mesh.set_vertex(h, i, x, y, z)` / `mesh.get_vertex_x/y/z(h, i)` — vertex access (16.16 fixed-point)
    - `mesh.set_face(h, i, v0, v1, v2)` / `mesh.get_face(h, i, c)` — face access (c=0/1/2 for vertex index)
    - `mesh.from_heightmap(bmp_ptr, scale_xz, scale_y)` — generate terrain mesh from grayscale BMP
    - `mesh.vertex_count(h)` / `mesh.face_count(h)` — query mesh size
  - **Procedural textures** (`tex.*`):
    - `tex.checker(x, y, size)` — returns 0 or 255 checkerboard pattern
    - `tex.gradient_h(x, width)` / `tex.gradient_v(y, height)` — linear gradient 0-255
    - `tex.brick(x, y, bw, bh, mortar)` — brick pattern (0=mortar, 255=brick)
    - `tex.noise2d(x, y, seed)` — hash-based noise 0-255
    - `tex.perlin2d(x, y, freq, seed)` — Perlin noise 0-255
    - `tex.wood(x, y, rings, seed)` / `tex.marble(x, y, freq, seed)` — organic patterns

### Not yet implemented

- Dynamic arrays; passing arrays as function parameters (use global arrays or `mem.alloc` + pointers to share buffers across functions instead)
- Per-vertex (Gouraud) lighting on meshes — the gradient rasterizer exists, but the cube demo currently uses flat per-face lighting
- Per-triangle alpha blending and near-plane clipping (planned for 0.13)
- Built-in 3D math/rendering primitives (matrix types, texture mapping) — not strictly needed, since rotation/projection/rasterization pipelines can already be written in Slag itself (see the cube demos)
- Encrypted P2P via Windows CNG (`bcrypt.dll`) Diffie-Hellman key exchange + AES — planned next; the networking and buffer primitives it depends on are now in place
- Self-hosting compiler

## Toolchain requirements

Slag's compiler is written in C and built with MinGW-w64 GCC, targeting `x86_64-w64-mingw32`. Output assembly is assembled with NASM (`-f win64`) and linked with the MinGW-w64 linker. No CRT is linked into Slag-compiled programs; only `kernel32.dll`, `user32.dll`, `gdi32.dll`, and `ws2_32.dll` (networking) are imported as needed.

## Installation

```bash
./install.sh
```

The install script detects the host environment (Cygwin, MSYS2, or Linux), checks for required dependencies (`gcc`, `nasm`, `x86_64-w64-mingw32-gcc`, `git`), builds the compiler, installs the man page, and adds the compiler directory to `PATH`.

## Building the compiler manually

```bash
cd compiler
gcc -Wall -Wextra -o slag main.c lexer.c ast.c parser.c codegen.c \
    window_runtime.c net_runtime.c mem_runtime.c matrix_runtime.c \
    simd_runtime.c mesh_runtime.c texture_runtime.c
```

## Compiling a Slag program

```bash
./slag program.slag
nasm -f win64 program.asm -o program.obj
x86_64-w64-mingw32-gcc program.obj -o program.exe -nostdlib -lkernel32 -luser32 -lgdi32 -lws2_32 -e _start
# Add -mwindows to suppress console window for GUI-only programs
```

## Example: window + pixels + input

```c
function main() {
    window.open(320, 240, "Slag Demo");

    on key_down(int key) {
        if (key == 27) {        // Escape
            window.close();
        }
    }

    on mouse_move(int mx, int my) {
        pixel(mx, my, 255, 255, 0);
    }

    local int x = 0;
    while (x < 320) {
        pixel(x, 100, 0, 255, 0);
        x = x + 1;
    }
    window.flush();

    while (window.is_open()) {
        window.flush();
    }

    return;
}
```

## Example: CPU topology

```c
function main() {
    println(cpu.physical_cores());
    println(cpu.logical_cores());
    println(cpu.threads_per_core());
    println(cpu.hyperthreaded());
    return;
}
```

## Example: threads, sync, and lock

```c
function main() {
    println("main: spawning threads");

    thread {
        lock {
            println("A line 1");
            println("A line 2");
        }
    }
    thread {
        lock {
            println("B line 1");
            println("B line 2");
        }
    }

    sync {
    }

    println("main: all threads done");
    return;
}
```

Each `thread` block runs concurrently. The `lock` block guarantees that A's
lines and B's lines never interleave, and `sync` blocks until both threads
finish before "all threads done" prints.

## Example: peer-to-peer messaging

Listener (binds, accepts one peer, receives a message into a buffer):

```c
function main() {
    net.start();
    net.bind(5555);
    net.accept();
    if (net.ack() == 0) { println("accept failed"); net.end(); return; }

    local int buf = mem.alloc(64);
    local int n = net.recv_buf(buf, 64);
    local int i = 0;
    while (i < n) {
        println(mem.peek8(buf, i));
        i = i + 1;
    }
    mem.free(buf);
    net.end();
    return;
}
```

Sender (connects, builds a message in a buffer, sends it):

```c
function main() {
    net.start();
    net.connect("127.0.0.1", 5555);
    if (net.ack() == 0) { println("connect failed"); net.end(); return; }

    local int buf = mem.alloc(64);
    mem.poke8(buf, 0, 72);   // H
    mem.poke8(buf, 1, 73);   // I
    net.send_buf(buf, 2);
    mem.free(buf);
    net.end();
    return;
}
```

## Examples Directory

The `examples/` directory contains 22 complete, runnable Slag programs demonstrating language features:

| # | Example | Description |
|---|---------|-------------|
| 01 | variables | Variable declarations and types |
| 02 | arrays | Fixed-size array operations |
| 03 | global_arrays | File-scope shared arrays |
| 04 | functions | User-defined functions and calls |
| 05 | arithmetic | Direct infix arithmetic expression syntax |
| 06 | control_flow | if/else and while loops |
| 07 | strings | String handling and manipulation |
| 08 | cpu_info | CPU topology detection |
| 09 | memory | Heap allocation with `mem.*` |
| 10 | window | Basic window creation |
| 11 | pixel | Pixel drawing |
| 12 | fill_triangle | Triangle rasterization |
| 13 | fill_triangle_z | Z-buffered depth testing |
| 14 | keyboard_input | Keyboard event handlers |
| 15 | mouse_input | Mouse event handlers |
| 16 | net_client | TCP networking API demo |
| 17 | threads | Multi-threaded concurrency |
| 18 | multi_window | Multiple independent windows via TLS |
| 19 | 3d_terrain | Heightmap-based 3D terrain rendering |
| 20 | solar_system | Hierarchical matrix-stack transforms |
| 21 | fill_triangle_persp | Perspective-correct texture mapping |
| 22 | fill_triangle_pcolor | Perspective-correct textured triangle, per-vertex color |

Run the interactive browser with `./examples/run_examples.sh`, or compile individual examples with `slagrun examples/01_variables.slag`.

## Config-Driven Demos

The `config_tests/` directory contains animation demos that read parameters from external text files, demonstrating runtime configuration:

- `config_square.slag` + `square.txt` — Position and color from config
- `config_heart.slag` + `heart.txt` — Multi-point path rendering
- `config_bounce.slag` + `bounce.txt` — Bouncing animation parameters
- `config_orbit.slag` + `orbit.txt` — Orbital motion demo
- `config_orbit_3d.slag` + `orbit_3d.txt` — 3D perspective orbit
