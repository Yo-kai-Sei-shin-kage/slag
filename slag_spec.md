# Slag Language Specification
**Version 0.4 — Draft**

---

## 1. Overview

Slag is a statically typed, compiled systems programming language targeting native Win64 PE executables via NASM x86-64 assembly. It is designed for bare-metal control, file processing, graphical rendering, and multithreaded execution on Windows 11. The compiler is written in C and compiled with MinGW-w64. Slag is intended to eventually be self-hosting.

**Source file extension:** `.slag`
**Compile command:** `slag program.slag`
**Output:** Native Win64 PE executable, no CRT dependency
**Runtime dependencies:** `kernel32.dll`, `user32.dll`, `gdi32.dll` only

---

## 2. Design Goals

- Bare metal x86-64 machine code output via NASM
- No C runtime dependency — Win32 API called directly
- Static typing with no implicit coercions
- First-class multithreading with hardware topology awareness
- Integrated graphical output with raw pixel access
- PS2-era software 3D rendering capability
- Regex-native string handling
- Terminal-default execution with optional graphical window contexts
- Self-hosting compiler as long-term target

---

## 3. Lexical Structure

### 3.1 Statement Terminator

All statements are terminated with a semicolon:

```
statement ;
```

### 3.2 Comments

```
// Single line comment

/* Multi-line
   comment */
```

### 3.3 Identifiers

Identifiers begin with a letter or underscore, followed by any combination of letters, digits, or underscores.

```
my_variable
_internal
counter1
```

### 3.4 Keywords

```
int   float   str   bool
if    else    while
true  false
function    return
var
thread  sync  lock
window  pixel  flush
```

---

## 4. Type System

Slag is statically typed. All variables must be declared with an explicit type. Mixed `int`/`float` expressions are permitted — int operands are automatically promoted via `cvtsi2sd`; assigning a float expression to an `int` truncates via `cvttsd2si`. No explicit cast syntax is required or available.

### 4.1 Primitive Types

| Type    | Description                         | Size     |
|---------|-------------------------------------|----------|
| `int`   | Signed 64-bit integer               | 8 bytes  |
| `float` | 64-bit IEEE 754 double              | 8 bytes  |
| `bool`  | Boolean — `true` or `false`         | 1 byte   |
| `str`   | Immutable byte string               | ptr+len  |

### 4.2 Arrays

All arrays are one-dimensional. Array size is fixed at declaration time. Dynamic sizing is permitted only when initialized from string/regex data, in which case length is capped to the actual data length — never larger.

```
int[8] counts;
float[16] vertices;
str[n] tokens;        // n resolved at parse/regex time, capped to match count
```

Array indexing is zero-based:

```
counts[0] = 42;
```

Out-of-bounds access is a compile-time error where statically determinable, and a runtime abort otherwise.

### 4.3 String Type

Strings are treated as byte arrays internally. A `str` value exposes:

- `.len` — element count (integer)
- Index access `s[i]` — returns the byte at position i as `int`

Strings are immutable. Manipulation produces new string values.

---

## 5. Variable Declarations

```
var int x = 10;
var float pi = 3.14159;
var bool active = true;
var str name = "slag";
var int[4] data = {1, 2, 3, 4};
```

All variables must be initialized at declaration. Type inference is not supported — the type must always be stated explicitly.

---

## 6. Arithmetic Expressions

### 6.1 The $(( )) Construct

`$(( ))` is the universal arithmetic construct for all numeric expressions — integer and float alike. Variables inside are referenced with a `$` prefix, consistent with bash syntax. The result type is determined by the declared types of the operands.

```
var int sum = $(($a + $b));
var float area = $((3.14159 * $radius * $radius));
var int remainder = $(($x % $y));
var float velocity = $(($distance / $time));
```

Supported operators:

| Operator | Description                          |
|----------|--------------------------------------|
| `+`      | Addition                             |
| `-`      | Subtraction                          |
| `*`      | Multiplication                       |
| `/`      | Division (truncates for int operands)|
| `%`      | Modulo (int only)                    |

Mixed `int` and `float` operands are promoted automatically — no explicit cast is needed.

### 6.2 Comparison Operators

```
==   !=   <   >   <=   >=
```

### 6.3 Logical Operators

```
&&   ||   !
```

---

## 7. Control Flow

### 7.1 If / Else

```
if (condition) {
    // body
}

if (condition) {
    // body
} else {
    // body
}

if (condition) {
    // body
} else if (condition) {
    // body
} else {
    // body
}
```

### 7.2 While Loop

```
while (condition) {
    // body
}
```

No `for` loop syntax. Counted iteration is expressed as `while` with an explicit counter:

```
var int i = 0;
while (i < 10) {
    // body
    i = i + 1;
}
```

---

## 8. Functions

```
function add(int a, int b) {
    return int $(($a + $b));
}

function area(float r) {
    return float $((3.14159 * $r * $r));
}

function greet(str name) {
    println(name);
    return;
}
```

- Function keyword is `function`
- Parameter list declares typed arguments
- Return type is declared at the `return` statement, not in the signature
- Void functions use bare `return;`
- Recursion is supported
- Functions must be declared before use

---

## 9. String Handling and Regex

### 9.1 String Literals

```
var str path = "/var/log/app.log";
```

### 9.2 Regex Matching

> **Note:** The regex engine (`match()`) is descoped and not planned for implementation. String processing is performed via `readfile()`, `readline()`, indexing, and `.len`.

### 9.3 File Reading

```
var str content = readfile("data.txt");
```

Returns the full file contents as a `str`. File I/O uses `kernel32.dll` directly — no CRT file functions.

---

## 10. Terminal I/O

```
print("Hello, world\n");
println("Line with newline");
var str input = readline();
```

All terminal I/O uses Win32 console handles directly via `kernel32.dll`.

---

## 11. Multithreading

### 11.1 CPU Topology Detection

At program startup, before `main` executes, Slag queries hardware topology via `GetLogicalProcessorInformation` from `kernel32.dll`, walks the `SYSTEM_LOGICAL_PROCESSOR_INFORMATION` buffer, and populates a set of read-only globals accessible via the `cpu.*` builtins:

```
cpu.physical_cores()      // int — physical core count
cpu.logical_cores()       // int — total logical processors (includes SMT threads)
cpu.threads_per_core()    // int — logical / physical; 2 = SMT active, 1 = no SMT
cpu.safe_thread_limit()   // int — logical_cores - 1 (minimum 1)
cpu.hyperthreaded()       // int — 1 if SMT detected on any core, 0 otherwise
```

`cpu.hyperthreaded()` is detected via the `ProcessorCore.Flags` field of `SYSTEM_LOGICAL_PROCESSOR_INFORMATION` — a non-zero flags value on any `RelationProcessorCore` entry indicates SMT is active on that core.

`cpu.safe_thread_limit()` is set to `logical_cores - 1` (minimum 1), leaving one logical core free for the OS and window thread.

If `GetLogicalProcessorInformation` fails, all `cpu.*` values fall back to 1 and `cpu.hyperthreaded()` returns 0.

### 11.2 Thread Blocks

```
thread {
    // parallel work unit
}
```

### 11.3 Sync Blocks

```
sync {
    // wait for all threads spawned above
}
```

### 11.4 Lock

```
lock {
    // mutually exclusive section
}
```

> **Note:** `thread`, `sync`, and `lock` are parsed and appear in the AST but are not yet emitted as functional assembly. Stub implementation only.

---

## 12. Windowing and Graphics

### 12.1 Window Lifecycle

```
window.open(width, height, title);   // create window on its own thread
window.is_open()                     // returns 1 if open, 0 if closed
window.close();                      // post WM_CLOSE to the window
```

### 12.2 Pixel Write

```
pixel(x, y, r, g, b);
```

Writes a single BGRA pixel to the framebuffer at `(x, y)`. Out-of-bounds coordinates are silently ignored (bounds-checked against framebuffer dimensions).

### 12.3 Triangle Rasterization

```
fill_triangle(x0, y0, x1, y1, x2, y2, r, g, b);
```

Flat-shaded scanline triangle rasterizer. Writes directly to the framebuffer with no per-pixel call overhead. Bounds-clamped.

```
fill_triangle_gradient(x0, y0, r0, g0, b0,
                       x1, y1, r1, g1, b1,
                       x2, y2, r2, g2, b2);
```

Per-vertex color (Gouraud-style) scanline rasterizer with linear interpolation along edges and across spans.

```
fill_triangle_z(x0, y0, z0, x1, y1, z1, x2, y2, z2, r, g, b);
```

Flat-shaded triangle with per-pixel depth testing against the active z-buffer. A pixel is drawn only when its depth is less than the stored value.

### 12.4 Flush

Push the DIB buffer to the window surface:

```
window.flush();
```

Internally calls `BitBlt` to blit the DIB to the window DC. Sleeps ~16ms to cap frame rate to ~60fps. Call once per frame.

### 12.5 Z-Buffer

```
zbuffer.clear();
```

Resets the depth buffer to a far value prior to rendering a frame.

### 12.6 Timing

```
time.now_ms()    // int — milliseconds since system start (GetTickCount)
```

### 12.7 Keyboard Events

```
on key_down(int keycode) {
    // fires on WM_KEYDOWN
}

on key_up(int keycode) {
    // fires on WM_KEYUP
}
```

Keycodes are Win32 virtual key codes. Handlers not declared by the program default to no-op stubs.

### 12.8 Mouse Events

```
on mouse_move(int x, int y) {
    // fires on WM_MOUSEMOVE
}

on mouse_down(int button) {
    // fires on WM_LBUTTONDOWN / WM_RBUTTONDOWN
    // button: 0 = left, 1 = right, 2 = middle
}

on mouse_up(int button) {
    // fires on WM_LBUTTONUP / WM_RBUTTONUP
}

on mouse_wheel(int delta) {
    // fires on WM_MOUSEWHEEL
    // delta: +-120 per notch
}
```

### 12.9 Shared Input State

Because event handlers execute in their own stack frames, separate from the main loop, a set of accessor builtins is provided:

```
input.drag_x() / input.drag_y()
input.add_drag(dx, dy)
input.is_dragging() / input.set_dragging(v)
input.last_x() / input.last_y() / input.set_last(x, y)
input.wheel()           // returns and resets accumulated wheel delta
input.add_wheel(delta)
input.set_bbox(minx, miny, maxx, maxy)
input.in_bbox(mx, my)   // axis-aligned hit test; returns 1 if inside
```

---

## 13. 3D Rendering

### 13.1 Overview

Slag includes a software 3D rendering pipeline targeting PS2-era graphical capability. All rendering is CPU-based, writing directly to the pixel buffer. The 3D pipeline (perspective projection, rotation matrices, Lambertian lighting) can be implemented entirely in Slag itself — see the cube demos in the `tests/` directory.

### 13.2 Coordinate System

Right-handed, Y-up. Screen space origin is top-left.

### 13.3 Projection

Perspective projection built-in function:

```
project(float x, float y, float z, float focal)
```

Internally computes:

```
screen_x = (x / z) * focal + window.width  / 2.0
screen_y = (y / z) * focal + window.height / 2.0
```

### 13.4 Z-Buffer

A float depth buffer equal in size to the pixel buffer. Cleared each frame via `zbuffer.clear()`. Depth test is `less-than` — closer fragments overwrite.

### 13.5 Texture Mapping

> **Not yet implemented.** Planned for a future version.

### 13.6 Lighting

Per-face Lambertian lighting can be implemented in Slag: compute each face normal as the cross product of two rotated edge vectors, dot with a light direction, remap to a brightness range, and apply to the face color. See the shaded cube demo.

---

## 14. Memory Model

- No garbage collector
- No heap allocator exposed to the programmer
- All variables are stack-allocated or statically allocated
- Array sizes must be compile-time constants
- Dynamic heap allocation is used internally by the runtime for the pixel buffer, z-buffer, and file I/O only
- Explicit manual memory management may be added in a future version

---

## 15. Compiler Architecture

### 15.1 Bootstrap Pipeline

```
.slag source
    → Lexer (C)
    → Parser (C) — produces AST
    → Code Generator (C) — emits NASM x86-64 assembly
    → NASM — assembles to Win64 COFF object
    → MinGW-w64 linker — links to PE executable
```

### 15.2 Output Format

- NASM format: `win64`
- PE subsystem: `console`
- Imports: `kernel32.dll`, `user32.dll`, `gdi32.dll`
- No CRT linkage

### 15.3 Self-Hosting Target

Once the language is expressive enough to implement its own lexer, parser, and code generator, the compiler will be rewritten in Slag. The bootstrap C compiler remains available as a fallback.

---

## 16. Standard Built-ins Summary

| Built-in                        | Description                                        |
|---------------------------------|----------------------------------------------------|
| `print(expr)`                   | Write int/float/str to stdout                      |
| `println(expr)`                 | Write int/float/str to stdout with newline         |
| `readline()`                    | Read line from stdin; returns str                  |
| `readfile(path)`                | Read file contents; returns str                    |
| `pixel(x,y,r,g,b)`             | Write pixel to framebuffer                         |
| `fill_triangle(...)`            | Flat-shaded scanline triangle                      |
| `fill_triangle_gradient(...)`   | Gouraud-shaded scanline triangle                   |
| `fill_triangle_z(...)`          | Depth-tested flat-shaded triangle                  |
| `window.open(w,h,title)`        | Open graphical window on its own thread            |
| `window.close()`                | Post WM_CLOSE to window                            |
| `window.is_open()`              | Returns 1 if window is open                        |
| `window.flush()`                | Blit framebuffer to window (~60fps cap)            |
| `zbuffer.clear()`               | Reset depth buffer                                 |
| `time.now_ms()`                 | Milliseconds since system start                    |
| `project(x,y,z,focal)`         | Perspective project 3D point to screen             |
| `cpu.physical_cores()`          | Physical core count                                |
| `cpu.logical_cores()`           | Logical processor count                            |
| `cpu.threads_per_core()`        | Logical / physical cores                           |
| `cpu.safe_thread_limit()`       | Logical cores − 1 (min 1)                         |
| `cpu.hyperthreaded()`           | 1 if SMT active, 0 otherwise                       |
| `input.drag_x/y()`             | Accumulated drag offset                            |
| `input.add_drag(dx,dy)`         | Accumulate drag delta                              |
| `input.is_dragging()`           | Drag state flag                                    |
| `input.set_dragging(v)`         | Set drag state                                     |
| `input.last_x/y()`             | Last recorded mouse position                       |
| `input.set_last(x,y)`           | Set last mouse position                            |
| `input.wheel()`                 | Returns and resets accumulated wheel delta         |
| `input.add_wheel(delta)`        | Accumulate wheel delta                             |
| `input.set_bbox(x0,y0,x1,y1)`  | Set hit-test bounding box                          |
| `input.in_bbox(mx,my)`          | Returns 1 if point is inside bounding box          |

---

## 17. Example Programs

### 17.1 Hello World

```
function main() {
    println("Hello from Slag.");
    return;
}
```

### 17.2 Arithmetic

```
function main() {
    var float radius = 5.0;
    var float area = $((3.14159 * $radius * $radius));
    println(area);
    return;
}
```

### 17.3 File Reading

```
function main() {
    var str content = readfile("data.txt");
    println(content);
    return;
}
```

### 17.4 CPU Topology

```
function main() {
    println(cpu.physical_cores());
    println(cpu.logical_cores());
    println(cpu.threads_per_core());
    println(cpu.hyperthreaded());
    return;
}
```

### 17.5 Graphical Window with Input

```
function main() {
    window.open(1280, 1024, "Slag Window");

    on key_down(int k) {
        if (k == 27) {
            window.close();
        }
    }

    on mouse_move(int x, int y) {
        pixel(x, y, 255, 128, 0);
    }

    while (window.is_open()) {
        window.flush();
    }
    return;
}
```

### 17.6 Multithreaded Workload

```
function main() {
    println(cpu.safe_thread_limit());

    var int i = 0;
    while (i < cpu.safe_thread_limit()) {
        thread {
            // parallel work unit
        }
        i = $(($i + 1));
    }

    sync {
        // wait for all threads to complete
    }
    return;
}
```

---

## 18. Version Roadmap

| Version | Target                                                      | Status      |
|---------|-------------------------------------------------------------|-------------|
| 0.1     | Lexer, parser, basic types, arithmetic, if/while, print     | ✅ Complete |
| 0.2     | Functions, arrays, string literals, string ptr+len tracking | ✅ Complete |
| 0.3     | File I/O (readfile, readline)                               | ✅ Complete |
| 0.4     | Win32 window, pixel write, triangle rasterizer, keyboard/mouse events, input state, CPU topology detection | ✅ Complete |
| 0.5     | Multithreading (thread/sync/lock), threaded tile renderer   | 🔲 Planned  |
| 0.6     | Depth buffer, back-face culling, z-tested triangles         | 🔲 Planned  |
| 0.7     | Texture mapping, matrix stack                               | 🔲 Planned  |
| 0.8     | Lighting model, perspective correction                      | 🔲 Planned  |
| 1.0     | Self-hosting compiler bootstrap                             | 🔲 Planned  |

---

*Slag Language Specification v0.4 — Subject to revision*
