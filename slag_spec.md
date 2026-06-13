# Slag Language Specification
**Version 0.1 — Draft**

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

Slag is statically typed. All variables must be declared with an explicit type. No implicit type coercion is permitted.

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

Mixed `int` and `float` operands require an explicit cast:

```
var float f = $((float($my_int) * 2.5));
```

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

Regex match returns a `str` array sized exactly to the number of matches. No extra allocation is performed — the array length is capped to match count.

```
var str[] tokens = match(input, /[A-Za-z]+/);
```

The resulting array behaves identically to any other `str` array:

```
var int count = tokens.len;
var str first = tokens[0];
```

### 9.3 Regex Syntax

Slag uses a built-in regex engine with POSIX ERE-compatible syntax. No external regex library is linked.

Supported constructs:

| Pattern  | Meaning                        |
|----------|--------------------------------|
| `.`      | Any character                  |
| `*`      | Zero or more                   |
| `+`      | One or more                    |
| `?`      | Zero or one                    |
| `[abc]`  | Character class                |
| `[^abc]` | Negated character class        |
| `^`      | Start of string                |
| `$`      | End of string                  |
| `()`     | Capture group                  |
| `\d`     | Digit                          |
| `\w`     | Word character                 |
| `\s`     | Whitespace                     |

### 9.4 File Reading

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

At program startup, before `main` executes, Slag queries hardware topology via `GetLogicalProcessorInformation` from `kernel32.dll` and populates a read-only global struct accessible as `cpu`:

```
cpu.physical_cores      // int — physical core count
cpu.logical_cores       // int — logical processor count (includes SMT)
cpu.threads_per_core    // int — 1 = no SMT, 2 = hyperthreaded
cpu.safe_thread_limit   // int — recommended max threads to spawn
```

`cpu.safe_thread_limit` is set to `cpu.logical_cores` on SMT systems and `cpu.physical_cores` on non-SMT systems. Thread spawning is capped to this value unless explicitly overridden.

### 11.2 Thread Blocks

```
thread {
    // executes in a new OS thread
    // spawned via CreateThread (kernel32.dll)
}
```

### 11.3 Sync and Lock

```
sync {
    // all threads spawned in this scope must complete before execution continues
}

lock mylock;

lock(mylock) {
    // mutually exclusive critical section
}
```

### 11.4 Shared Data

Variables declared at file scope are shared across threads. Access to shared mutable data must be wrapped in a `lock` block. The compiler emits a warning for shared mutable access outside a lock.

---

## 12. Graphical Output

### 12.1 Execution Model

Terminal mode is the default. A graphical window is an explicit construct. Programs may open a window, interact with it, and close it, then continue terminal execution. Both contexts may be active simultaneously.

The PE is compiled as a console subsystem executable. Win32 window creation works alongside the console without subsystem conflict.

### 12.2 Window Lifecycle

```
window.open(1280, 1024, "My Window");

// ... rendering and event loop ...

window.close();
```

Internally, `window.open` calls `RegisterClassEx`, `CreateWindowEx`, creates a DIB section for the pixel buffer, and enters the Win32 message pump on a dedicated thread.

### 12.3 Pixel Buffer

Direct pixel write to the DIB buffer:

```
pixel(x, y, r, g, b);
```

- `x`, `y` — `int` coordinates, origin top-left
- `r`, `g`, `b` — `int` values 0–255
- Writes to the flat 32-bit BGRA DIB buffer at offset `(y * width + x) * 4`
- Out-of-bounds coordinates are silently clipped

### 12.4 Flush

Push the DIB buffer to the window surface:

```
window.flush();
```

Internally calls `BitBlt` to blit the DIB to the window DC. Call once per frame.

### 12.5 Window Dimensions

```
window.width     // int
window.height    // int
```

### 12.6 Keyboard Events

```
on key_down(int keycode) {
    // fires on WM_KEYDOWN
}

on key_up(int keycode) {
    // fires on WM_KEYUP
}
```

Keycodes are Win32 virtual key codes exposed as named constants:

```
KEY_A ... KEY_Z
KEY_0 ... KEY_9
KEY_UP  KEY_DOWN  KEY_LEFT  KEY_RIGHT
KEY_ESC  KEY_ENTER  KEY_SPACE
```

### 12.7 Mouse Events

```
on mouse_move(int x, int y) {
    // fires on WM_MOUSEMOVE
}

on mouse_down(int button, int x, int y) {
    // fires on WM_LBUTTONDOWN / WM_RBUTTONDOWN
    // button: 0 = left, 1 = right, 2 = middle
}

on mouse_up(int button, int x, int y) {
    // fires on WM_LBUTTONUP / WM_RBUTTONUP
}
```

---

## 13. 3D Rendering

### 13.1 Overview

Slag includes a software 3D rendering pipeline targeting PS2-era graphical capability. All rendering is CPU-based, writing directly to the pixel buffer. Multithreaded tile rendering is automatic — the framebuffer is divided into horizontal bands, one per `cpu.safe_thread_limit`, each rasterized on its own thread.

### 13.2 Coordinate System

Right-handed, Y-up. Screen space origin is top-left.

### 13.3 Vertex Data

Vertices are expressed as `float` arrays. Convention is interleaved `x, y, z` per vertex:

```
var float[9] tri = {
    0.0,  1.0, 1.0,    // vertex 0: x, y, z
   -1.0, -1.0, 1.0,    // vertex 1
    1.0, -1.0, 1.0     // vertex 2
};
```

### 13.4 Projection

Perspective projection built-in function:

```
project(float x, float y, float z, float focal) // returns screen x, y as float[2]
```

Internally computes:

```
screen_x = (x / z) * focal + window.width  / 2.0
screen_y = (y / z) * focal + window.height / 2.0
```

### 13.5 Triangle Rasterization

```
draw_triangle(
    float x0, float y0, float z0,
    float x1, float y1, float z1,
    float x2, float y2, float z2,
    int r, int g, int b
);
```

Rasterizes a flat-shaded triangle into the pixel buffer using a scanline algorithm. Perspective-correct texture coordinates are computed internally when a texture is bound.

### 13.6 Z-Buffer

A float array equal in size to the pixel buffer is maintained automatically when 3D rendering is active. Cleared each frame via `zbuffer.clear()`. Depth test is `less-than` — closer fragments overwrite.

```
zbuffer.clear();
```

### 13.7 Texture Mapping

```
var int[w * h] tex = loadtex("texture.raw");   // raw 32-bit BGRA pixel data
bind_texture(tex, w, h);
```

Once a texture is bound, subsequent `draw_triangle` calls perform affine (PS1-style) or perspective-correct (PS2-style) texture mapping depending on the active mode:

```
texmode_affine();        // PS1-style, faster
texmode_perspective();   // PS2-style, correct
```

### 13.8 Lighting

```
light_ambient(float intensity);
light_directional(float dx, float dy, float dz, float intensity);
light_point(float px, float py, float pz, float intensity, float falloff);
```

Vertex normals are supplied as additional float components in the vertex buffer. Gouraud shading interpolates lighting across triangle faces.

### 13.9 Matrix Stack

```
matrix_push();
matrix_pop();
matrix_identity();
matrix_translate(float tx, float ty, float tz);
matrix_rotate_x(float angle);
matrix_rotate_y(float angle);
matrix_rotate_z(float angle);
matrix_scale(float sx, float sy, float sz);
```

Fixed-point math is used internally for the matrix pipeline for performance. The matrix stack depth is 32.

### 13.10 Frame Sequence

Standard frame loop pattern:

```
while (window.open) {
    zbuffer.clear();
    // geometry submission and draw calls
    window.flush();
}
```

---

## 14. Memory Model

- No garbage collector
- No heap allocator exposed to the programmer in v0.1
- All variables are stack-allocated or statically allocated
- Array sizes must be compile-time constants or regex/string-derived lengths resolved at parse time
- Dynamic heap allocation is used internally by the runtime for the pixel buffer and z-buffer only
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

| Built-in            | Description                                  |
|---------------------|----------------------------------------------|
| `print(str)`        | Write to stdout                              |
| `println(str)`      | Write to stdout with newline                 |
| `readline()`        | Read line from stdin                         |
| `readfile(str)`     | Read file contents as str                    |
| `match(str, regex)` | Regex match, returns str array               |
| `pixel(x,y,r,g,b)`  | Write pixel to framebuffer                   |
| `window.open(w,h,t)`| Open graphical window                        |
| `window.close()`    | Close graphical window                       |
| `window.flush()`    | Blit framebuffer to window                   |
| `zbuffer.clear()`   | Clear depth buffer                           |
| `project(...)`      | Perspective project 3D point to screen       |
| `draw_triangle(...)`| Rasterize shaded/textured triangle           |
| `bind_texture(...)` | Bind texture for subsequent draw calls       |
| `matrix_push/pop()` | Matrix stack management                      |
| `cpu.safe_thread_limit` | Max safe thread count from topology     |

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

### 17.3 File Parsing with Regex

```
function main() {
    var str log = readfile("app.log");
    var str[] errors = match(log, /ERROR:[^\n]+/);
    var int i = 0;
    while ($i < errors.len) {
        println(errors[$i]);
        i = $(($i + 1));
    }
    return;
}
```

### 17.4 Graphical Window with Input

```
function main() {
    window.open(1280, 1024, "Slag Window");

    on key_down(int k) {
        if ($k == KEY_ESC) {
            window.close();
        }
    }

    on mouse_move(int x, int y) {
        pixel(x, y, 255, 128, 0);
    }

    while (window.open) {
        window.flush();
    }
    return;
}
```

### 17.5 Multithreaded Workload

```
function main() {
    println("Cores available:");
    println(cpu.safe_thread_limit);

    var int i = 0;
    while ($i < cpu.safe_thread_limit) {
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

| Version | Target                                                      |
|---------|-------------------------------------------------------------|
| 0.1     | Lexer, parser, basic types, arithmetic, if/while, print     |
| 0.2     | Functions, arrays, string literals                          |
| 0.3     | Regex engine, file I/O                                      |
| 0.4     | Win32 window, pixel write, keyboard/mouse events            |
| 0.5     | Multithreading, CPU topology detection                      |
| 0.6     | Software rasterizer — flat shading, z-buffer                |
| 0.7     | Texture mapping, Gouraud shading, matrix stack              |
| 0.8     | Lighting model, perspective correction                      |
| 0.9     | Threaded tile renderer                                      |
| 1.0     | Self-hosting compiler bootstrap                             |

---

*Slag Language Specification v0.1 — Subject to revision*
