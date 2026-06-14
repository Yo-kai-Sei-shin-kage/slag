# Slag

A statically typed, compiled systems programming language targeting native Win64 PE executables. 
Slag compiles directly to NASM x86-64 assembly, links with no C runtime dependency, and is designed 
for bare-metal control on Windows 11 — file processing, multithreaded workloads, and software-rendered 
graphics (with a PS2-era rendering ceiling as a long-term target).

The full language design is documented in [`slag_spec.md`](slag_spec.md).

## Status

Slag is under active development. The pipeline currently supports:

- **Lexer** — full tokenization including `$((...))` arithmetic blocks, `$variable` references, regex literals, and all core keywords/operators.
- **Parser** — complete recursive descent parser producing a full AST: functions, typed variable and array declarations, if/else/else-if, while, typed returns, thread/sync blocks, and `on` event handlers.
- **Code generator** — emits NASM x86-64 Win64 assembly. Working:
  - Integer and float arithmetic, comparisons, logical short-circuit operators
  - Fixed-size arrays (declaration, indexing, `.len`)
  - Control flow (if/else, while)
  - `print`/`println` for ints, floats, string literals, and string variables
  - String variable ptr+len tracking
  - `readfile()` — reads file contents into a `str` via Win32 `CreateFileA`/`ReadFile`
  - `readline()` — reads a line from stdin via Win32 `ReadConsoleA`
  - User-defined function calls under the Win64 calling convention (int and float args/returns, mixed and nested calls)
  - **Windowing and software-rendered graphics**:
    - `window.open(w, h, title)` — creates a window on its own thread with a BGRA DIB framebuffer
    - `pixel(x, y, r, g, b)` — writes a single pixel into the framebuffer
    - `window.flush()` — pumps the message queue and blits the framebuffer to the window
    - `window.is_open()` — returns 1/0, enabling `while (window.is_open()) { ... }` main loops
    - `window.close()` — requests the window close (posts `WM_CLOSE`)
  - **Keyboard/mouse event handlers** — `on key_down`, `on key_up`, `on mouse_move`, `on mouse_down`, `on mouse_up` (buttons: 0=left, 1=right, 2=middle), and `on mouse_wheel(int delta)` (delta is ±120 per notch) are compiled to standalone procs and dispatched directly from the window's `WndProc`; handlers not defined by the user fall back to no-op stubs
  - `pixel(x, y, r, g, b)` bounds-checks against the framebuffer dimensions and silently no-ops out-of-range writes (safe to draw off-screen)
  - **Shared input-state builtins** — since `on` handlers run in their own stack frames, a small fixed set of `.bss` globals plus accessor builtins let handlers communicate with the main loop:
    - `input.drag_x()` / `input.drag_y()` — accumulated drag offset
    - `input.add_drag(dx, dy)` — accumulate a drag delta (called from `on mouse_move`)
    - `input.is_dragging()` / `input.set_dragging(v)` — drag-state flag
    - `input.last_x()` / `input.last_y()` / `input.set_last(x, y)` — last recorded mouse position
    - `input.wheel()` — returns and resets the accumulated scroll-wheel delta
    - `input.add_wheel(delta)` — accumulate wheel delta (called from `on mouse_wheel`)
    - `input.set_bbox(minx, miny, maxx, maxy)` / `input.in_bbox(mx, my)` — axis-aligned bounding box check, used for hit-testing draggable objects
    - `fill_triangle(x0,y0,x1,y1,x2,y2,r,g,b)` — flat-shaded scanline triangle rasterizer, writes directly to the framebuffer with no per-pixel call overhead; bounds-clamped
    - `time.now_ms()` — milliseconds since system start (`GetTickCount`), useful for fps counters and frame timing
    - `fill_triangle_gradient(x0,y0,r0,g0,b0, x1,y1,r1,g1,b1, x2,y2,r2,g2,b2)` — per-vertex color scanline rasterizer with linear interpolation along edges and across spans (smooth color blending across a triangle's interior)
  - **3D pipeline (demonstrated, written entirely in Slag)** — Bresenham line drawing, flat-shaded filled triangles, perspective projection, and per-axis rotation matrices using precomputed sin/cos constants. Demos include a wireframe rotating cube, a mouse-drag + scroll-wheel-rotation cube, and a solid 6-face flat-shaded rotating cube — all sustaining a steady ~60fps (capped by `window.flush()`'s ~16ms sleep) at 640x800

### Not yet implemented

- `match()` / regex engine (descoped — not planned)
- Dynamic/regex-sized arrays
- `thread` / `sync` / `lock` (currently stubbed)
- CPU topology detection (`cpu.*` fields hardcoded to 1)
- Built-in 3D math/rendering primitives (matrix types, z-buffering, texture mapping) — not needed for basic wireframe work, since a full rotation/projection/line-drawing pipeline can already be written in Slag itself (see rotating-cube demos)
- Self-hosting compiler

## Toolchain requirements

Slag's compiler is written in C and built with MinGW-w64 GCC, targeting `x86_64-w64-mingw32`. Output assembly is assembled with NASM (`-f win64`) and linked with the MinGW-w64 linker. No CRT is linked into Slag-compiled programs; only `kernel32.dll`, `user32.dll`, and `gdi32.dll` are imported as needed.

## Building the compiler

```bash
cd compiler
gcc -Wall -Wextra -o slag main.c lexer.c ast.c parser.c codegen.c window_runtime.c
```

## Compiling a Slag program

```bash
./slag program.slag
nasm -f win64 program.asm -o program.obj
x86_64-w64-mingw32-gcc program.obj -o program.exe -nostdlib -lkernel32 -luser32 -lgdi32 -e _start
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

    var int x = 0;
    while (x < 320) {
        pixel(x, 100, 0, 255, 0);
        x = $(($x + 1));
    }
    window.flush();

    while (window.is_open()) {
        window.flush();
    }

    return;
}
```
