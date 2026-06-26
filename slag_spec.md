# Slag Language Specification
**Version 0.8 — Draft**

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
- String handling with ptr/len tracking
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
var   global   local
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

All arrays are one-dimensional. Array size is fixed at declaration time. Dynamic sizing is permitted only when initialized from string data, in which case length is capped to the actual data length — never larger.

```
int[8] counts;
float[16] vertices;
str[n] tokens;        // n resolved at parse time, capped to match count
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


### 5.1 Variable Scope Modifiers

Variables can be declared with scope modifiers to control their visibility and storage:

#### Global Variables

```
global int counter = 0;
global str appName = "MyApp";
```

Global variables are declared at file scope (outside any function) and are:
- Stored in the `.data` section of the executable
- Accessible from any function in the program
- Initialized at compile-time with literal values

#### Global Arrays

```
global int[] xpos = {50, 150, 250, 350};
global float[] scales = {1.0, 0.5, 0.25};
```

Global arrays are declared at file scope and are:
- Stored in the `.data` section with their element values
- Accessible from any function via indexing (`xpos[i]`)
- Support `.len` to get the array length
- Initialized with brace-enclosed literal values

#### Local Variables

```
local int x = 10;
local float temp = 0.0;
```

Local variables are declared inside functions or blocks and are:
- Stack-allocated (same as `var`)
- Scoped to the enclosing block

The `var` keyword remains available for standard function-local variables.
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

## 9. String Handling

### 9.1 String Literals

```
var str path = "/var/log/app.log";
```



### 9.3 File Reading

```
var str content = readfile("data.txt");
```

Returns the full file contents as a `str`. File I/O uses `kernel32.dll` directly — no CRT file functions.

### 9.4 Config File Parsing

Text configuration files can be parsed by combining `readfile()` with `mem.peek8()` for byte-by-byte access. A typical pattern:

```
function parse_int(int ptr, int start, int end) {
    var int val = 0;
    var int i = start;
    while (i < end) {
        var int byte = mem.peek8(ptr, i);
        if (byte >= 48) {
            if (byte <= 57) {
                val = $(($val * 10 + $byte - 48));
            }
        }
        i = $(($i + 1));
    }
    return int val;
}

function main() {
    var str config = readfile("settings.txt");
    var int ptr = config;
    // Parse line-by-line, looking for newline (10) or null (0)
    // ...
}
```

This enables runtime configuration without recompilation. See the `config_tests/` directory for complete examples.

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

Each `thread { ... }` block spawns a real Win32 thread. The block body is
compiled to a standalone thread procedure (`_slag_thread_proc_N`) and launched
with `CreateThread`; the returned handle is stored in an internal handle table.
Multiple `thread` blocks may be spawned before a `sync`. A maximum of 64
outstanding threads is supported between syncs.

### 11.3 Sync Blocks

```
sync {
    // runs after all threads spawned above have finished
}
```

`sync` waits for every thread spawned since the last sync via
`WaitForMultipleObjects(count, handles, TRUE, INFINITE)`, then closes each
thread handle and resets the handle table so it can be reused. Any statements
inside the `sync` body run after the wait completes.

### 11.4 Lock

```
lock {
    // mutually exclusive section
}
```

`lock` provides mutual exclusion via a single global `CRITICAL_SECTION`,
initialized at startup. Entering a `lock` block calls `EnterCriticalSection`
and leaving it calls `LeaveCriticalSection`, so only one thread executes inside
any `lock` block at a time. The critical section is reentrant on the owning
thread. All `lock` blocks currently share one global lock.

---

## 11A. Networking

Slag provides TCP networking via `ws2_32.dll`, sufficient for persistent
peer-to-peer sessions between two machines running the same program. Programs
that use networking must link `ws2_32` (`-lws2_32`).

All connection state is held in runtime globals; one active connection is
tracked at a time. The builtins:

```
net.start()              // WSAStartup — begin a networking session
net.bind(port)           // create socket, bind, listen (does not block)
net.accept()             // block until a peer connects to the bound socket
net.listen(port)         // convenience: bind + listen + accept in one call
net.connect(host, port)  // connect to a peer; host is a string literal
net.send(byte)           // send one byte
net.recv()               // receive one byte -> int (-1 on failure)
net.send_buf(ptr, len)   // send len bytes from a buffer, looping until done
net.recv_buf(ptr, max)   // receive up to max bytes into a buffer -> count
net.ack()                // -> 1/0: did the last network op succeed
net.connected()          // -> 1/0: is the active connection still alive
net.end()                // close sockets + WSACleanup
```

A persistent listener binds once and calls `net.accept()` (then loops on
`net.recv_buf` while `net.connected()` is true). A symmetric peer may attempt
`net.connect` and fall back to `net.bind`/`net.accept` if no peer is present.
Multi-byte transfer pairs with the `mem.*` primitives: build a message with
`mem.poke8`, send with `net.send_buf`, receive with `net.recv_buf`, read with
`mem.peek8`.

> **Note:** `net.recv_buf` performs a single `recv`, which over TCP may return
> fewer bytes than requested. For length-delimited protocols, loop until the
> expected byte count has been collected.

---

## 12. Windowing and Graphics

### 12.1 Window Lifecycle

```
window.open(width, height, title);   // create window on its own thread
window.is_open()                     // returns 1 if open, 0 if closed
window.close();                      // post WM_CLOSE to the window
window.capture_mouse();              // capture mouse, clip to window, hide cursor
window.release_mouse();              // release capture, show cursor
```

**Thread-Local Storage (TLS):** Window state is stored per-thread via `TlsAlloc`/`TlsSetValue`, enabling multiple independent windows from a single program. Each thread that calls `window.open()` gets its own window with separate framebuffer, z-buffer, and event handling.

Mouse capture is useful for FPS-style controls where the cursor should be hidden and constrained to the window. Press ESC or call `release_mouse()` to restore normal cursor behavior.

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

`time.now_ms()` returns milliseconds since system start via `GetTickCount` (~15 ms resolution). `time.now_us()` returns microseconds via `QueryPerformanceCounter`/`QueryPerformanceFrequency` for high-resolution benchmarking and precise frame timing.

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

- No garbage collector; memory management is manual and explicit
- Local variables are stack-allocated; array sizes must be compile-time constants
- Arrays cannot be passed into functions. To share a buffer across functions
  (or with the networking/threading runtimes), use the `mem.*` heap primitives
- Raw heap buffers are available via `mem.*`, addressed by plain-int pointers:
  - `mem.alloc(nbytes)` — `HeapAlloc` with zero-initialized memory; returns the
    buffer address as an `int`, or `0` on failure
  - `mem.free(ptr)` — `HeapFree`
  - `mem.poke8(ptr, byteoff, val)` / `mem.peek8(ptr, byteoff)` — single-byte
    store/load at a byte offset
  - `mem.poke64(ptr, wordoff, val)` / `mem.peek64(ptr, wordoff)` — 8-byte
    store/load at a word offset (byte offset = `wordoff * 8`)
- Accessors are **unchecked** and **inlined** — each `peek`/`poke` compiles to a single `mov` emitted directly at the call site (no function-call overhead), benchmarked at ~0.3 ns/op, comparable to native array access.
  Bounds are the programmer's responsibility (as in C). `alloc` returning `0`
  is the only built-in safety signal
- The runtime also uses heap allocation internally for the pixel buffer,
  z-buffer, and file I/O

### 14.2 Bit Manipulation — `bit.*`

Bit shift operations for fixed-point arithmetic and low-level manipulation:

```
bit.shl(value, count)   // left shift — inlined to shl instruction
bit.shr(value, count)   // unsigned right shift — inlined to shr instruction
```

**16.16 Fixed-Point Example:**
```
var int fixed = bit.shl(3, 16);           // 3 << 16 = 196608 (3.0 in 16.16)
var int product = bit.shr($(($a * $b)), 16);  // fixed multiply
var int back = bit.shr(fixed, 16);        // convert back to int = 3
```

These are inlined to single CPU instructions with no function-call overhead.

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
| `window.open(w,h,title)`        | Open graphical window on its own thread            |
| `window.close()`                | Post WM_CLOSE to window                            |
| `window.is_open()`              | Returns 1 if window is open                        |
| `window.flush()`                | Blit framebuffer to window (~60fps cap)            |
| `window.capture_mouse()`        | Capture mouse, clip to window, hide cursor         |
| `window.release_mouse()`        | Release capture, show cursor                       |
| `zbuffer.clear()`               | Reset depth buffer                                 |
| `time.now_ms()`                 | Milliseconds since system start (GetTickCount)     |
| `time.now_us()`                 | Microseconds (QueryPerformanceCounter, high-res)   |
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
| `mem.alloc(n)`                  | Heap-allocate n zeroed bytes -> ptr (0 on fail)    |
| `mem.free(ptr)`                 | Free a heap buffer                                 |
| `mem.poke8(ptr,off,v)`          | Store byte v at ptr[off]                           |
| `mem.peek8(ptr,off)`            | Load byte at ptr[off] -> int                       |
| `mem.poke64(ptr,woff,v)`        | Store 8 bytes at ptr + woff*8                      |
| `mem.peek64(ptr,woff)`          | Load 8 bytes at ptr + woff*8 -> int                |
| `bit.shl(val,count)`            | Left shift (inlined to shl instruction)            |
| `bit.shr(val,count)`            | Unsigned right shift (inlined to shr instruction)  |
| `net.start()` / `net.end()`     | Begin / end a networking session (ws2_32)          |
| `net.bind(port)`                | Socket + bind + listen (no block)                  |
| `net.accept()`                  | Block for a peer on the bound socket               |
| `net.listen(port)`              | Convenience: bind + listen + accept                |
| `net.connect(host,port)`        | Connect to a peer (host is a string literal)       |
| `net.send(byte)` / `net.recv()` | Send / receive a single byte                       |
| `net.send_buf(ptr,len)`         | Send len bytes from a buffer (loops until done)    |
| `net.recv_buf(ptr,max)`         | Receive up to max bytes -> count                   |
| `net.ack()`                     | 1/0: did the last network op succeed               |
| `net.connected()`               | 1/0: is the active connection alive                |

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
| 0.5     | Multithreading (thread/sync/lock), CPU topology             | ✅ Complete |
| 0.6     | Memory primitives (mem.*), TCP networking (net.*), multi-byte P2P messaging, global/local scope | ✅ Complete |
| 0.7     | Global arrays, z-buffer depth testing (fill_triangle_z, zbuffer.clear) | ✅ Complete |
| 0.8     | Config file parsing, examples directory with interactive browser | ✅ Complete |
| 0.9     | Encrypted P2P: bcrypt (CNG) Diffie-Hellman key exchange + AES | 🔲 Planned  |
| 0.10    | Texture mapping, matrix stack                               | 🔲 Planned  |
| 0.11    | Lighting model, perspective correction                      | 🔲 Planned  |
| 1.0     | Self-hosting compiler bootstrap                             | 🔲 Planned  |

---

*Slag Language Specification v0.8 — Subject to revision*
