# Slag Language Specification
**Version 0.13.5**

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
global   local
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

Every variable must be declared with an explicit scope modifier — `global` or `local`. There is no unscoped declaration form.

All variables must be initialized at declaration. Type inference is not supported — the type must always be stated explicitly.

### 5.1 Local Variables

```
local int x = 10;
local float pi = 3.14159;
local bool active = true;
local str name = "slag";
local int[4] data = {1, 2, 3, 4};
```

Local variables are declared inside functions or blocks and are:
- Stack-allocated
- Scoped to the enclosing block

### 5.2 Global Variables

```
global int counter = 0;
global str appName = "MyApp";
```

Global variables are declared at file scope (outside any function) and are:
- Stored in the `.data` section of the executable
- Accessible from any function in the program
- Initialized at compile-time with literal values

### 5.3 Global Arrays

```
global int[] xpos = {50, 150, 250, 350};
global float[] scales = {1.0, 0.5, 0.25};
```

Global arrays are declared at file scope and are:
- Stored in the `.data` section with their element values
- Accessible from any function via indexing (`xpos[i]`)
- Support `.len` to get the array length
- Initialized with brace-enclosed literal values

### 5.4 Local Arrays

```
local int[] digits = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
```

Local arrays follow the same stack-allocated, block-scoped rules as other local variables.

---

## 6. Arithmetic Expressions

### 6.1 Direct Infix Arithmetic

Arithmetic operators work directly on expressions — no wrapper syntax or `$` variable prefix is required. The result type is determined by the declared types of the operands.

```
local int sum = a + b;
local float area = 3.14159 * radius * radius;
local int remainder = x % y;
local float velocity = distance / time;
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
local int i = 0;
while (i < 10) {
    // body
    i = i + 1;
}
```

---

## 8. Functions

```
function add(int a, int b) {
    return int a + b;
}

function area(float r) {
    return float 3.14159 * r * r;
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
local str path = "/var/log/app.log";
```



### 9.3 File Reading

```
local str content = readfile("data.txt");
```

Returns the full file contents as a `str`. File I/O uses `kernel32.dll` directly — no CRT file functions.

### 9.4 Config File Parsing

Text configuration files can be parsed by combining `readfile()` with `mem.peek8()` for byte-by-byte access. A typical pattern:

```
function parse_int(int ptr, int start, int end) {
    local int val = 0;
    local int i = start;
    while (i < end) {
        local int byte = mem.peek8(ptr, i);
        if (byte >= 48) {
            if (byte <= 57) {
                val = val * 10 + byte - 48;
            }
        }
        i = i + 1;
    }
    return int val;
}

function main() {
    local str config = readfile("settings.txt");
    local int ptr = config;
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
local str input = readline();
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

#### SIMD feature detection — `cpu.simd_detect()` / `cpu.has_*()`

At startup Slag also runs `CPUID` (leaf 1 and leaf 7) plus an `XGETBV`/`OSXSAVE`
check for OS-enabled AVX state, populating a set of read-only feature flags.
`cpu.simd_detect()` re-runs the probe on demand (it also runs automatically before
`main`); it is idempotent and returns `1`. Each predicate returns `1` if the
feature is present (and, for AVX/AVX2/AVX-512F, OS-enabled) or `0`:

```slag
cpu.simd_detect()   // re-run CPUID feature detection; returns 1
cpu.has_sse()       // SSE
cpu.has_sse2()      // SSE2
cpu.has_sse3()      // SSE3
cpu.has_ssse3()     // SSSE3
cpu.has_sse41()     // SSE4.1
cpu.has_sse42()     // SSE4.2
cpu.has_fma()       // FMA
cpu.has_avx()       // AVX  (OS-enabled)
cpu.has_avx2()      // AVX2 (OS-enabled)
cpu.has_avx512f()   // AVX-512 Foundation (OS-enabled)
```

AVX/AVX2/AVX-512F require both the CPUID feature bit **and** OS support for the
wider register state (`OSXSAVE` set and `XCR0` reporting XMM/YMM — plus opmask and
ZMM for AVX-512), so a predicate returns `0` when the CPU supports the ISA but the
OS has not enabled its register state. Each predicate compiles to a single
zero-extended byte load.

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
net.recv()               // receive one byte -> int; see return contract below
net.send_buf(ptr, len)   // send len bytes from a buffer, looping until done
net.recv_buf(ptr, max)   // receive up to max bytes into a buffer -> count
net.recv_buf_exact(ptr, n) // reassemble exactly n bytes across ticks ->
                         // n (complete), -2 (partial, poll again), -1 (err)
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
> expected byte count has been collected, or use `net.recv_buf_exact`.

**`net.recv_buf_exact(ptr, n)`** does the reassembly for you: bytes accumulate
in an internal 4 KB buffer across calls, and it returns `n` only once the full
message has arrived (copied to `ptr`, accumulator reset), `-2` while still
partial (call again next tick), or `-1` on disconnect/error or when `n` exceeds
the buffer. It performs at most one non-blocking `recv` per call, so it never
blocks the loop. This is the intended way to receive a framed payload that must
be processed as a whole unit — e.g. a fixed-size length prefix, or an encrypted
message (see section 11C) that has to arrive complete before it can be decrypted.

**Return contract for `net.recv()` / `net.recv_buf()`:** a connection
established via `net.connect()` is non-blocking, so these can return three
distinct outcomes: a byte count `>= 0` (data received), `-2` (no data
available this instant, connection still alive — not an error, just poll
again next tick), or `-1` (the peer actually disconnected; connection state
is cleared). A connection established via `net.bind()`/`net.accept()`/
`net.listen()` instead, keeps its original blocking behavior, so `-2` does
not occur on that path — `net.recv_buf` simply waits for data as before.

**`net.connected()`** performs an active check (a non-blocking peek at the
socket) rather than reading a passive flag, so it correctly reflects the
peer disconnecting even if no `net.send`/`net.recv` call has happened
recently. Both `net.connect()`'s and the persistent server's per-client
sockets (see 11B) also enable TCP keepalive, so even a peer that vanishes
silently (power loss, unplugged cable — no FIN/RST ever sent) is detected
within roughly 7-12 seconds rather than never, at which point `net.connected()`
reflects it.

---

## 11B. Persistent Multi-Client Server and LAN Discovery

Built on top of the primitives in 11A, `net.server_*` is a non-blocking,
multi-client TCP server geared toward a game-server model (host authoritative,
several clients connecting concurrently), and `net.discover_*` is a UDP-based
LAN "server browser" so a client can find a running server without already
knowing its address. Both are additive; the single-peer API in 11A is
unaffected and still the right choice for a plain two-machine session.

```
net.server_start(port, name)  // WSAStartup + socket+bind+listen (TCP),
                              // plus opens a UDP discovery listener on a
                              // fixed port (9001) advertising name/port
net.server_accept()           // -> int client slot idx, or -1 if none
                              // pending; non-blocking, call every tick.
                              // Also services one pending discovery query
                              // per call as a side effect (free with the
                              // polling you're already doing).
net.server_send(idx, byte)
net.server_recv(idx)          // -> byte, -2 (no data yet, still connected),
                              // or -1 (peer disconnected -- slot freed)
net.server_send_buf(idx, ptr, len)
net.server_recv_buf(idx, ptr, maxlen)  // same -2 / -1 contract as above
net.server_recv_buf_exact(idx, ptr, n) // per-slot exact-n reassembly ->
                              // n (complete), -2 (partial), -1 (err);
                              // non-blocking, safe in a lockstep loop
net.server_connected(idx)     // -> 1/0: active non-blocking check for one
                              // client slot (same semantics as net.connected())
net.server_stop()             // close every client socket, the listener,
                              // and the discovery socket; WSACleanup

net.discover_send()           // fire one UDP broadcast query ("who's out
                              // there?"); internally rate-limited to once
                              // per second, so it's safe to call every tick
net.discover_poll()           // -> discovered-server slot idx just updated,
                              // or -1; non-blocking, call every tick
net.discover_count()          // -> number of servers currently listed
net.discover_ip(idx)          // -> str, dotted-decimal address
net.discover_port(idx)        // -> int, the server's TCP game port
net.discover_name(idx)        // -> str, the name given to net.server_start
net.discover_max(idx)         // -> int, max simultaneous clients
net.discover_clients(idx)     // -> int, live connected count as of the
                              // server's last reply
```

Client slot capacity is a single compile-time constant in the compiler
(`SERVER_MAX_CLIENTS` in `server_runtime.c`, currently 4, designed to scale
to 64 by changing that one value). A client connects to a `net.server_*`
host using the ordinary single-peer client API (`net.start()` +
`net.connect(ip, port)`, typically with `ip`/`port` read from
`net.discover_ip(0)`/`net.discover_port(0)` after browsing) — `net.server_*`
itself is host-only; there is no client-side "server" call.

Discovery entries persist once found and are **not** removed just because a
server becomes full or a client connects to it — only a server that stops
answering queries entirely (fully offline) is pruned, after roughly 6 seconds
with no fresh reply. A server merely at capacity keeps responding to queries
normally and stays listed.

---

## 11C. Cryptography (ECDH + AES)

Encrypted-P2P primitives backed by Windows CNG (`bcrypt.dll`, linked with
`-lbcrypt`): ECDH P-256 key exchange and AES-256-CBC. They are pure primitives
with **no** automatic dispatch — the script performs the handshake and wraps
each payload itself, over either the single-peer (`net.*`) or multi-client
(`net.server_*`) transport. Only one keypair and one derived session key are
held at a time (one endpoint per process, matching the separate client/host
model).

```
crypto.dh_keygen()                     // generate an ephemeral ECDH P-256
                                       // keypair into the module slot
crypto.dh_pubkey(out_ptr)              // export our public key (72-byte
                                       // BCRYPT_ECCPUBLIC_BLOB) -> len (72)
crypto.dh_derive(peer_ptr, peer_len)   // import peer pubkey, ECDH agreement,
                                       // derive 32-byte AES-256 key (HASH
                                       // KDF, SHA-256) into the module slot
crypto.aes_encrypt(in, in_len, out)    // AES-256-CBC + PKCS7; fresh 16-byte
                                       // IV prepended to out -> total len
                                       // (16 + padded ciphertext)
crypto.aes_decrypt(in, in_len, out)    // expects the 16-byte IV prefix ->
                                       // plaintext len, or -1 on failure
```

`out_ptr` for `crypto.aes_encrypt` must hold `16 + in_len` rounded up to the
next 16-byte block. A typical handshake, identical on either transport:

```
crypto.dh_keygen();
crypto.dh_pubkey(buf);            // send buf (72 bytes) to the peer
crypto.dh_derive(peer_buf, 72);  // peer_buf received from the wire
// both sides now hold the same AES-256 key
n = crypto.aes_encrypt(msg, len, out);   // send out over the wire
crypto.aes_decrypt(in, in_len, plain);   // decrypt what arrives
```

Because TCP may fragment a send, receive framed ciphertext with
`net.recv_buf_exact` / `net.server_recv_buf_exact` (section 11A/11B) so a
message is only decrypted once it has fully arrived.

---

## 12. Windowing and Graphics

### 12.1 Window Lifecycle

```
window.open(width, height, title);            // windowed, on its own thread
window.open(width, height, title, fullscreen);// nonzero 4th arg = borderless fullscreen, w/h ignored
window.is_open()                     // returns 1 if open, 0 if closed
window.close();                      // post WM_CLOSE to the window
window.capture_mouse();              // capture mouse, clip to window, hide cursor
window.release_mouse();              // release capture, show cursor
window.native();                     // returns native resolution as "WxH" string
window.width();                      // current client width  (int; tracks live resize)
window.height();                     // current client height (int; tracks live resize)
```

**Thread-Local Storage (TLS):** Window state is stored per-thread via `TlsAlloc`/`TlsSetValue`, enabling multiple independent windows from a single program. Each thread that calls `window.open()` gets its own window with separate framebuffer, z-buffer, and event handling.

**Dynamic resize:** When the user resizes the window, the framebuffer and z-buffer are recreated at the new client size on the next `window.flush()`, and `window.width()`/`window.height()` report the updated dimensions. Read them each frame and derive all positions from them for resolution-independent rendering.

Mouse capture is useful for FPS-style controls where the cursor should be hidden and constrained to the window. Press ESC or call `release_mouse()` to restore normal cursor behavior.

### 12.2 Pixel Write

```
pixel(x, y, r, g, b);
```

Writes a single BGRA pixel to the framebuffer at `(x, y)`. Out-of-bounds coordinates are silently ignored (bounds-checked against framebuffer dimensions).

### 12.3 Triangle Rasterization

All `fill_triangle*` variants perform backface culling: a triangle whose vertices wind clockwise in screen space is discarded before rasterization. Vertices must be supplied in counter-clockwise winding order, as seen on screen, to be drawn. Since screen y increases downward, this is the opposite sense of CW/CCW in standard math y-up coordinates.

```
fill_triangle(x0, y0, x1, y1, x2, y2, r, g, b);
```

Flat-shaded scanline triangle rasterizer. Writes directly to the framebuffer with no per-pixel call overhead. Bounds-clamped.

All six `fill_triangle*` variants share one dispatch path: calls are deferred into a per-frame queue rather than drawn immediately, and `window.flush()` drains it. The queue starts at 1024 entries and doubles via `HeapReAlloc` whenever it fills, so there is no hard cap on triangles queued per frame. Queues below 64 triangles drain sequentially on the calling thread (thread wake/wait overhead isn't worth it at that scale); queues at or above 64 dispatch across a persistent worker pool — lazily spawned on first use, sized from `cpu.safe_thread_limit()` and capped at 32 threads — which splits the framebuffer into horizontal row bands, one band per worker. This is transparent to Slag source: no syntax changed, output is identical, only the rendering of triangle-heavy scenes gets faster on multi-core machines. SIMD-tier dispatch is implemented for `fill_triangle_pcolor`: on CPUs reporting `cpu.has_avx2()` its scanline fill runs an 8-pixel-wide AVX2 SoA kernel (~2x throughput), falling back to the baseline single-pixel SSE2 path otherwise. The dispatch is transparent to Slag source (identical output). The other five `fill_triangle*` variants currently always use the SSE2 path; AVX-512 tiers are not yet implemented.

`fill_triangle` (the flat-shaded variant) resolves its target window fresh via TLS on every call rather than caching the pointer across calls, so drawing to multiple windows from the same thread always lands on the correct one.

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

```
fill_triangle_affine(x0, y0, u0, v0, x1, y1, u1, v1, x2, y2, u2, v2, tex_ptr, tex_w, tex_h);
```

PS1-style affine texture-mapped triangle. UV coordinates are linearly interpolated without perspective correction. The texture must be in RGB565 format (2 bytes per pixel).

```
fill_triangle_persp(x0, y0, z0, u0, v0, x1, y1, z1, u1, v1, x2, y2, z2, u2, v2, tex_ptr, tex_w, tex_h);
```

PS2-style perspective-correct texture-mapped triangle. Interpolates 1/z, u/z, v/z per scanline. To keep the inner loop division-light, perspective is corrected exactly once every 8 pixels and UV is interpolated affinely between those points (the classic PS2 subdivision) — reducing per-pixel divisions ~20× versus per-pixel correction, with sub-pixel drift that is imperceptible at normal triangle sizes. Each pixel is written as a single 32-bit BGRA store.

Near-plane safety: triangles with any vertex at or behind the camera (z <= 0), or whose projected screen coordinates land far outside the active window's bounds (a generous 8x-window-size margin), are silently discarded before rasterization. This is a cheap safety net against the degenerate 1/z blowup that near-camera geometry produces — not a full geometric clip — so it prevents corrupted/stretched-garbage triangles rather than rendering a correctly-clipped partial triangle. It does not protect against a caller's own perspective-divide-by-zero in Slag script code computing screen coordinates before the call; scripts should still guard `z > 0` themselves before projecting.

```
fill_triangle_pcolor(verts, tex_ptr, tex_w, tex_h);
```

Perspective-correct textured triangle with per-vertex colors. The `verts` parameter points to 24 consecutive int64 values (3 vertices × 8 values each: x, y, z, u, v, r, g, b). Texture color is multiplied by interpolated vertex color, enabling Gouraud-style shading combined with texturing. Unlike `fill_triangle_affine`, the texture must be in 32-bit BGRA format (4 bytes per pixel).

The same near-plane safety net as `fill_triangle_persp` applies here: any vertex at or behind the camera, or projected far outside the window's bounds, silently discards the triangle before rasterization.

### 12.4 Flush

Push the DIB buffer to the window surface:

```
window.flush();
```

Internally calls `BitBlt` to blit the DIB to the window DC. Rendering is uncapped — no frame-rate limiting is applied, so throughput is bounded only by rasterization and blit cost. Call once per frame.

### 12.5 Z-Buffer

```
zbuffer.clear();
```

Resets the depth buffer to a far value prior to rendering a frame.

### 12.6 Timing

`time.now_ms()` returns milliseconds since system start via `GetTickCount` (~15 ms resolution). `time.now_us()` returns microseconds via `QueryPerformanceCounter`/`QueryPerformanceFrequency` for high-resolution benchmarking and precise frame timing.

> **Note:** `time.now_ms()` wraps at ~49.7 days (32-bit overflow). For long-running server processes, use `time.now_us()` which has a much longer overflow period.

`sleep(ms)` busy-waits for approximately `ms` milliseconds by spinning on `QueryPerformanceCounter`. Because it spins rather than yielding the CPU, it stays accurate well below the ~15 ms floor of `Sleep()` — `sleep(1)` waits ~1 ms — at the cost of keeping one core busy for the duration.

```
time.now_ms()    // int — milliseconds since system start (GetTickCount)
time.now_us()    // int — microseconds (QueryPerformanceCounter, high-res)
sleep(ms)        // busy-wait ~ms milliseconds (QueryPerformanceCounter spin)
```

### 12.7 Keyboard Events

```
on key_down(int key) {
    // fires on WM_KEYDOWN
}

on key_up(int key) {
    // fires on WM_KEYUP
}
```

Before a handler runs, the runtime translates the raw Win32 virtual key into
its **character** using the live keyboard state (`GetKeyboardState` + `ToAscii`),
so shift, caps lock, and the active keyboard layout are already applied. A
printable key arrives as its ASCII value — pressing A gives `a`, Shift+A gives
`A`. Keys with no character (arrows, function keys, modifiers, etc.) arrive as
`virtual_key + 256` so they never collide with a real character.

Because of this, handlers compare the parameter against **character or
named-key string literals** rather than raw numbers. The compiler lowers a
one-character literal to its byte (case-sensitive), and a named key to the code
the runtime delivers:

```
on key_down(int key) {
    if (key == "a")   { }   // lower-case a
    if (key == "A")   { }   // Shift+A
    if (key == "esc") { window.close(); }
    if (key == "left"){ }   // arrow key
}
```

Recognized non-character names: `esc` (or `escape`), `enter` (or `return`),
`backspace`, `tab`, `space`, `left`, `right`, `up`, `down`, `home`, `end`,
`pageup`, `pagedown`, `insert`, `delete`, `shift`, `ctrl`, `alt`, and
`f1`–`f12`. Handlers not declared by the program default to no-op stubs.

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

### 12.9 Per-Window Input State

Because event handlers execute in their own stack frames, separate from the main loop, a set of accessor builtins is provided so handlers and the main loop can communicate:

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

This state is stored per-window (in the same TLS-resolved window struct as the framebuffer/z-buffer), not in a single shared global. Each `input.*` call resolves the calling thread's window via TLS before reading or writing, so drag/wheel/last-position/bbox state for one window never leaks into another — each `on mouse_move`/`on mouse_wheel`/etc. handler runs on its owning window's own thread (see 12.1), so `input.*` calls made from within a handler always resolve to that same window.

---

### 12.10 Clear & Text

```
window.clear(r, g, b);
```

Fills the entire DIB framebuffer with a solid BGRA color via `rep stosd`. Typically called once per frame before drawing.

```
window.text(x, y, value, r, g, b);
```

Draws `value` (a `str` or `int`) as text at `(x, y)` using GDI `TextOutA` against the window's memory DC, with `SetBkMode(TRANSPARENT)` and the given RGB color via `SetTextColor`. Renders into the same DIB surface that `window.flush()` blits, so text composites with pixel/triangle output drawn earlier in the frame.

```
window.textbuf(x, y, ptr, len, r, g, b);
```

Draws `len` bytes from the runtime byte buffer at `ptr` as text, using the same
GDI path as `window.text`. Where `window.text` takes a compile-time `str`/`int`,
`window.textbuf` takes a pointer + length, so strings assembled at run time
(keyboard input, composed file paths, formatted output built with `mem.poke8`)
can be rendered directly without a string literal.

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
  - `mem.poke64(ptr, byteoff, val)` / `mem.peek64(ptr, byteoff)` — 8-byte
    store/load at a byte offset
  - `mem.pokef32(ptr, byteoff, floatval)` — store a 32-bit float at a byte
    offset (the float is narrowed from Slag's 64-bit double via `cvtsd2ss`).
    Lets SIMD vec4 buffers be filled with readable float literals, e.g.
    `mem.pokef32(a, 0, 1.0)`, instead of hand-packed IEEE-754 bit patterns
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
local int fixed = bit.shl(3, 16);           // 3 << 16 = 196608 (3.0 in 16.16)
local int product = bit.shr(a * b, 16);     // fixed multiply
local int back = bit.shr(fixed, 16);        // convert back to int = 3
```

These are inlined to single CPU instructions with no function-call overhead.

### 14.3 Matrix Stack — `mat.*`

A 3x4 transformation matrix stack for 3D graphics, using 16.16 fixed-point arithmetic for performance. Pre-computed sin/cos lookup tables (256 entries) enable fast rotation without floating-point trig calls.

**Matrix Operations:**
```
mat.identity()              // reset current matrix to identity
mat.push()                  // push current matrix onto stack (16 levels max)
mat.pop()                   // pop matrix from stack into current
mat.translate(x, y, z)      // multiply translation into current matrix
mat.scale(sx, sy, sz)       // multiply scale into current matrix
mat.rotate_x(angle)         // multiply X rotation (angle 0-255 = 0-360°)
mat.rotate_y(angle)         // multiply Y rotation
mat.rotate_z(angle)         // multiply Z rotation
```

**Point Transformation:**
```
mat.transform_x(x, y, z)    // return transformed X coordinate
mat.transform_y(x, y, z)    // return transformed Y coordinate
mat.transform_z(x, y, z)    // return transformed Z coordinate
```

**Fixed-Point Usage:**
All coordinates use 16.16 fixed-point format. Convert integers with `bit.shl(n, 16)` and convert results back with `bit.shr(result, 16)`.

```
mat.identity();
mat.translate(bit.shl(100, 16), bit.shl(200, 16), 0);
mat.rotate_z(64);  // 90 degrees (64/256 * 360)
local int x = mat.transform_x(bit.shl(50, 16), 0, 0);
local int screen_x = bit.shr(x, 16);
```

### 14.4 SIMD — `simd.*`

SSE2 128-bit vector operations for graphics and bulk data processing. All buffer arguments are pointers (from `mem.alloc`) to 16-byte regions.

**Arithmetic (4 packed single-precision floats):**
```
simd.addf4(dest, a, b)   // dest = a + b element-wise
simd.subf4(dest, a, b)   // dest = a - b
simd.mulf4(dest, a, b)   // dest = a * b
simd.divf4(dest, a, b)   // dest = a / b
```

**Vector operations:**
```
simd.dot4(dest, a, b)      // dot product, result broadcast to all 4 elements
simd.cross3(dest, a, b)    // 3D cross product (w=0)
simd.normalize4(dest, v)   // normalize to unit length
simd.lint4(dest, a, b, t)  // linear interpolation: a + t*(b-a)
```

**Matrix operations (row-major 4×4):**
```
simd.mat4_mul(dest, a, b)   // 4×4 matrix multiply: C = A * B
simd.mat4_vec4(dest, m, v)  // matrix-vector multiply
```

**RGB565 texture operations (8 pixels per 128-bit register):**
```
simd.rgb565_unpack(r, g, b, pixels)  // extract R/G/B channels
simd.rgb565_pack(dest, r, g, b)      // combine to RGB565
simd.rgb565_blend(dest, a, b, alpha) // alpha blend (alpha: 8×16-bit, 0-256)
```


### 14.5 BMP Image Loading — `mesh.*`

Load and query 24-bit uncompressed BMP images:

```
local str data = readfile("image.bmp");
local int ptr = data;
local int w = mesh.bmp_width(ptr);
local int h = mesh.bmp_height(ptr);
local int rgb = mesh.bmp_pixel(ptr, x, y);   // returns 0x00RRGGBB
local int gray = mesh.bmp_gray(ptr, x, y);   // returns 0-255
```

### 14.6 Mesh Management — `mesh.*`

Create and manipulate 3D meshes (vertices are 16.16 fixed-point):

```
local int m = mesh.create(100, 200);           // 100 verts, 200 faces
mesh.set_vertex(m, i, x, y, z);              // set vertex i
mesh.set_face(m, f, v0, v1, v2);             // set face f
local int vx = mesh.get_vertex_x(m, i);        // query vertex
local int vy = mesh.get_vertex_y(m, i);
local int vz = mesh.get_vertex_z(m, i);
local int vi = mesh.get_face(m, f, 0);         // query face (0/1/2)
local int nv = mesh.vertex_count(m);
local int nf = mesh.face_count(m);
mesh.destroy(m);                             // free mesh
```

Generate terrain from heightmap:

```
local int terrain = mesh.from_heightmap(bmp_ptr, scale_xz, scale_y);
```

### 14.7 Procedural Textures — `tex.*`

Generate procedural texture values (returns 0-255):

```
local int c = tex.checker(x, y, 32);           // checkerboard
local int gh = tex.gradient_h(x, 256);         // horizontal gradient
local int gv = tex.gradient_v(y, 256);         // vertical gradient
local int b = tex.brick(x, y, 64, 32, 2);      // brick pattern
local int n = tex.noise2d(x, y, seed);         // hash-based noise
local int p = tex.perlin2d(x, y, freq, seed);  // Perlin noise
local int w = tex.wood(x, y, rings, seed);     // wood grain
local int m = tex.marble(x, y, freq, seed);    // marble veins
```
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
| `fill_triangle_affine(...)`     | PS1-style affine textured triangle (RGB565)        |
| `fill_triangle_persp(...)`      | PS2-style perspective-correct textured triangle    |
| `fill_triangle_pcolor(...)`     | Perspective-correct textured triangle, per-vertex color |
| `window.open(w,h,title[,fs])`   | Open window on its own thread; nonzero `fs` = fullscreen |
| `window.close()`                | Post WM_CLOSE to window                            |
| `window.is_open()`              | Returns 1 if window is open                        |
| `window.clear(r,g,b)`           | Fill framebuffer with a solid color                |
| `window.text(x,y,val,r,g,b)`    | Draw str/int text at (x,y); drains pending fill_triangle* first, so it composites on top |
| `window.textbuf(x,y,ptr,len,r,g,b)` | Draw `len` bytes from a runtime byte buffer as text (for dynamically built strings) |
| `window.flush()`                | Drain deferred fill_triangle* queue, blit to window (uncapped) |
| `window.capture_mouse()`        | Capture mouse, clip to window, hide cursor         |
| `window.release_mouse()`        | Release capture, show cursor                       |
| `window.native()`               | Returns native resolution as "WxH" string          |
| `window.width()` / `window.height()` | Current client size as int (tracks live resize)   |
| `zbuffer.clear()`               | Reset depth buffer                                 |
| `time.now_ms()`                 | Milliseconds since system start (GetTickCount)     |
| `time.now_us()`                 | Microseconds (QueryPerformanceCounter, high-res)   |
| `sleep(ms)`                     | Busy-wait ~ms milliseconds (QueryPerformanceCounter spin) |
| `cpu.physical_cores()`          | Physical core count                                |
| `cpu.logical_cores()`           | Logical processor count                            |
| `cpu.threads_per_core()`        | Logical / physical cores                           |
| `cpu.safe_thread_limit()`       | Logical cores − 1 (min 1)                         |
| `cpu.hyperthreaded()`           | 1 if SMT active, 0 otherwise                       |
| `cpu.simd_detect()`             | Re-run CPUID SIMD detection (auto-run at startup)  |
| `cpu.has_sse/sse2/sse3/ssse3()` | ISA feature flag (1/0)                             |
| `cpu.has_sse41/sse42/fma()`     | ISA feature flag (1/0)                             |
| `cpu.has_avx/avx2/avx512f()`    | ISA feature flag, OS-enabled (1/0)                 |
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
| `mem.poke64(ptr,byteoff,v)`     | Store 8 bytes at ptr + byteoff                     |
| `mem.peek64(ptr,byteoff)`       | Load 8 bytes at ptr + byteoff -> int               |
| `mem.pokef32(ptr,byteoff,f)`    | Store 32-bit float at ptr + byteoff                |
| `file.open(path,mode)`          | Open file; mode 1=read/2=write/3=append -> handle (-1 fail) |
| `file.close(handle)`            | Close a file handle                                |
| `file.read(handle,buf,n)`       | Read n bytes into buf -> bytes read (-1 fail)      |
| `file.write(handle,buf,n)`      | Write n bytes from buf -> bytes written (-1 fail)  |
| `file.seek(handle,off,whence)`  | Seek; whence 0=start/1=current/2=end -> new pos    |
| `file.size(handle)`             | File size in bytes (-1 fail)                       |
| `file.exists(path)`             | 1/0: does path exist                               |
| `file.delete(path)`             | Delete a file -> 1/0 success                       |
| `file.mkdir(path)`              | Create one directory -> 1/0 (fails if it exists or a parent is missing) |
| `file.rmdir(path)`              | Remove an empty directory -> 1/0 success           |
| `file.make(dir,filename)`       | Join dir+"/"+filename, mkdir -p parents, create the file (existing left untouched) -> 1/0 |
| `file.list_open(pattern)`       | Open a directory listing (glob e.g. "dir/*.ext") -> handle (-1 fail) |
| `file.list_next(handle)`        | Advance to next entry -> 1/0 (0 = no more)         |
| `file.list_name(handle)`        | Current entry's filename -> str                    |
| `file.list_close(handle)`       | Close a directory listing handle                   |
| `audio.init(rate,channels,bits)` | Open waveOut device at this exact format -> 1/0    |
| `audio.close()`                 | Stop mixer, close the audio device                 |
| `audio.load(path)`              | Load a WAV (must match audio.init's format) -> handle (-1 fail) |
| `audio.free(handle)`            | Free a loaded sound                                |
| `audio.play(handle)`            | Play once from the start (ignores loop points)     |
| `audio.loop(handle)`            | Play from start, then repeat [loop_start,loop_end) forever |
| `audio.stop(handle)`            | Stop playback                                      |
| `audio.volume(handle,vol)`      | Per-sound volume 0-255                             |
| `audio.master_volume(vol)`      | Master volume 0-255                                |
| `audio.pan(handle,pan)`         | Stereo pan: 0=left, 128=center, 255=right          |
| `audio.is_playing(handle)`      | 1/0: is this sound currently playing                |
| `audio.position(handle)`        | Current byte offset into the sound's PCM data      |
| `bit.shl(val,count)`            | Left shift (inlined to shl instruction)            |
| `bit.shr(val,count)`            | Unsigned right shift (inlined to shr instruction)  |
| `mat.identity()`                | Reset current matrix to identity                   |
| `mat.push()` / `mat.pop()`      | Push/pop matrix stack (16 levels)                  |
| `mat.translate(x,y,z)`          | Multiply translation into current matrix           |
| `mat.scale(sx,sy,sz)`           | Multiply scale into current matrix                 |
| `mat.rotate_x/y/z(angle)`       | Multiply rotation (angle 0-255 = 0-360°)           |
| `mat.transform_x/y/z(x,y,z)`    | Transform point, return single coordinate          |
| `simd.addf4(d,a,b)`             | Add 4 packed floats                                |
| `simd.subf4(d,a,b)`             | Subtract 4 packed floats                           |
| `simd.mulf4(d,a,b)`             | Multiply 4 packed floats                           |
| `simd.divf4(d,a,b)`             | Divide 4 packed floats                             |
| `simd.dot4(d,a,b)`              | Dot product of 4-float vectors                     |
| `simd.cross3(d,a,b)`            | 3D cross product                                   |
| `simd.normalize4(d,v)`          | Normalize vec4 to unit length                      |
| `simd.lint4(d,a,b,t)`           | Linear interpolation a + t*(b-a)                   |
| `simd.mat4_mul(d,a,b)`          | 4×4 matrix multiply                                |
| `simd.mat4_vec4(d,m,v)`         | Matrix-vector multiply                             |
| `simd.rgb565_unpack(r,g,b,px)`  | Extract R/G/B from 8 RGB565 pixels                 |
| `simd.rgb565_pack(d,r,g,b)`     | Pack R/G/B to 8 RGB565 pixels                      |
| `simd.rgb565_blend(d,a,b,α)`    | Alpha blend 8 RGB565 pixels                        |
| `mesh.bmp_width/height(ptr)`    | BMP image dimensions                               |
| `mesh.bmp_pixel(ptr,x,y)`       | BMP pixel as 0x00RRGGBB                            |
| `mesh.bmp_gray(ptr,x,y)`        | BMP grayscale value 0-255                          |
| `mesh.create(v,f)`              | Allocate mesh with v vertices, f faces             |
| `mesh.destroy(h)`               | Free mesh memory                                   |
| `mesh.set/get_vertex_*(h,i,...)` | Vertex access (16.16 fixed-point)                 |
| `mesh.set/get_face(h,i,...)`    | Face vertex indices                                |
| `mesh.from_heightmap(ptr,sx,sy)` | Generate terrain from grayscale BMP               |
| `mesh.vertex/face_count(h)`     | Query mesh size                                    |
| `tex.checker(x,y,size)`         | Checkerboard pattern (0 or 255)                    |
| `tex.gradient_h/v(coord,size)`  | Linear gradient 0-255                              |
| `tex.brick(x,y,bw,bh,m)`        | Brick pattern (0=mortar, 255=brick)                |
| `tex.noise2d(x,y,seed)`         | Hash-based noise 0-255                             |
| `tex.perlin2d(x,y,freq,seed)`   | Perlin noise 0-255                                 |
| `tex.wood/marble(x,y,...)`      | Organic texture patterns                           |
| `net.start()` / `net.end()`     | Begin / end a networking session (ws2_32)          |
| `net.bind(port)`                | Socket + bind + listen (no block)                  |
| `net.accept()`                  | Block for a peer on the bound socket               |
| `net.listen(port)`              | Convenience: bind + listen + accept                |
| `net.connect(host,port)`        | Connect to a peer (host is a string literal)       |
| `net.send(byte)` / `net.recv()` | Send / receive a single byte                       |
| `net.send_buf(ptr,len)`         | Send len bytes from a buffer (loops until done)    |
| `net.recv_buf(ptr,max)`         | Receive up to max bytes -> count                   |
| `net.recv_buf_exact(ptr,n)`     | Reassemble exactly n bytes -> n / -2 partial / -1 err (non-blocking) |
| `net.ack()`                     | 1/0: did the last network op succeed               |
| `net.connected()`               | 1/0: is the active connection alive (active check) |
| `net.server_start(port,name)`   | Host: TCP listen + UDP discovery listener          |
| `net.server_accept()`           | Host: -> client slot idx or -1 (non-blocking)      |
| `net.server_send/recv(idx,...)` | Host: per-client byte send/recv                    |
| `net.server_send/recv_buf(idx,...)` | Host: per-client buffered send/recv            |
| `net.server_recv_buf_exact(idx,ptr,n)` | Host: per-slot exact-n reassembly -> n / -2 / -1 (non-blocking) |
| `net.server_connected(idx)`     | Host: 1/0 active check for one client slot         |
| `net.server_stop()`             | Host: close all client sockets + listener + UDP    |
| `net.discover_send()`          | Client: fire a UDP LAN discovery broadcast          |
| `net.discover_poll()`           | Client: -> discovered slot idx or -1               |
| `net.discover_count/ip/port/name/max/clients(idx)` | Client: read the discovered-server list |
| `crypto.dh_keygen()`            | Generate an ephemeral ECDH P-256 keypair (bcrypt)  |
| `crypto.dh_pubkey(out)`         | Export our public key (72-byte blob) -> len        |
| `crypto.dh_derive(peer,len)`    | ECDH agreement + derive AES-256 key (HASH/SHA-256) |
| `crypto.aes_encrypt(in,len,out)` | AES-256-CBC encrypt, IV prepended -> total len    |
| `crypto.aes_decrypt(in,len,out)` | AES-256-CBC decrypt -> plaintext len (-1 fail)    |

**Audio known limitations:** no pause/resume (`audio.stop` always resets position to 0), each loaded sound has exactly one playback slot (overlapping the same sound with itself requires separate handles), no pitch/rate variation, and `audio.load` reads the entire file into memory (no streaming).

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
    local float radius = 5.0;
    local float area = 3.14159 * radius * radius;
    println(area);
    return;
}
```

### 17.3 File Reading

```
function main() {
    local str content = readfile("data.txt");
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

    local int i = 0;
    while (i < cpu.safe_thread_limit()) {
        thread {
            // parallel work unit
        }
        i = i + 1;
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
| 0.9     | Bit shifts (bit.shl/shr), mouse capture, multi-window TLS   | ✅ Complete |
| 0.10    | Matrix stack (mat.*), affine texture mapping, BMP loading, mesh management, procedural textures (tex.*) | ✅ Complete |
| 0.11    | Perspective-correct texture mapping (fill_triangle_persp, fill_triangle_pcolor) | ✅ Complete |
| 0.12    | Backface culling on all fill_triangle* variants, window.clear/window.text (GDI overlay text), `global`/`local` as the sole declaration syntax (removed `var` and `$((...))`), mem.peek64/poke64 changed to byte-offset addressing | ✅ Complete |
| 0.13    | Runtime SIMD detection (`cpu.simd_detect`/`cpu.has_*` via CPUID+XGETBV), `mem.pokef32`, perspective rasterizer inner-loop optimization (8px UV subdivision, single-store BGRA writes) | ✅ Complete |
| 0.13.1  | File I/O (`file.open/close/read/write/seek/size/exists/delete/mkdir`), per-handle directory listing (`file.list_open/next/name/close`) | ✅ Complete |
| 0.13.2  | Near-plane cull safety net for `fill_triangle_persp`/`fill_triangle_pcolor` (degenerate z<=0 reject + adaptive window-bounds reject); `mem.poke8` register-clobber fix for nested inlined builtin calls | ✅ Complete |
| 0.13.3  | Full audio runtime (`audio.init/close/load/free/play/loop/stop/volume/pan/master_volume/is_playing/position`): 32-slot software mixer over waveOut, RIFF/WAVE parsing with automatic `smpl`-chunk loop-point support | ✅ Complete |
| 0.13.4  | Bilinear texture filtering (4-tap weighted average) for `fill_triangle_persp`/`fill_triangle_pcolor` | ✅ Complete |
| 0.13.5  | `file.make`/`file.rmdir`; file.* path args + `audio.load` accept runtime byte-buffer pointers (not just str literals); `window.textbuf` (draw a runtime byte buffer as text); keyboard handlers deliver translated characters (compare against char/named-key literals like `"a"`/`"esc"`/`"left"`); compiler emits a single `compiled successfully` line. **Bug fix:** `emit_user_call` 16-byte stack-alignment fix for odd-argument-count (1 or 3) user calls that reach a Win32 API | ✅ Complete |
| 0.14    | Full near-plane geometric clipping (Sutherland-Hodgman, distinct from the 0.13.2 cull safety net) | 🔲 Planned  |
| 0.15    | Encrypted P2P: ECDH P-256 key exchange + AES-256-CBC via CNG (`crypto.*`); non-blocking exact-length message reassembly (`net.recv_buf_exact`/`net.server_recv_buf_exact`) for framed/encrypted payloads. **Bug fix:** unescaped `%` in the window runtime's shifted-key table that broke `cg_emit`'s `vfprintf` | ✅ Complete |
| 1.0     | Self-hosting compiler bootstrap                             | 🔲 Planned  |

### PS2-Era Graphics Target (60fps)

PS2-era software rendering at 60fps. Current pipeline status:

**Core rendering (complete):**
- Perspective-correct texture interpolation (1/z correction, 8px UV subdivision)
- Bilinear texture filtering (4-tap) on `fill_triangle_persp`/`fill_triangle_pcolor`
- Gouraud shading (`fill_triangle_gradient`), and Gouraud combined with texturing (`fill_triangle_pcolor` per-vertex color modulation)
- Backface culling (signed-area winding test, all `fill_triangle*` variants)
- Z-buffer depth testing (`fill_triangle_z`, `zbuffer.clear`)

**Performance (complete):**
- Multi-threaded rasterization is fully implemented: `fill_triangle*` calls enqueue into a deferred queue and are drained at `window.flush()` across a persistent worker pool, split into horizontal bands (one per worker thread; worker count scales with CPU cores). Batches below `FT_POOL_THRESHOLD` draw sequentially to avoid wake/wait overhead.
- Uncapped presentation (no frame-rate limiting; `sleep(ms)` available for pacing).

**Performance target:**
- 1,000,000 polygons per frame at 1920x1080 (native 1080p), sub-16ms (60fps). The multi-threaded rasterizer already surpasses earlier 10K-class counts with no SIMD dispatch; SIMD auto-dispatch is the remaining lever toward the 1M-polygon target.

**Planned:**
- Near-plane triangle clipping (Sutherland-Hodgman) — 0.14
- Fog, lighting, and shadows are handled per-vertex in Slag script via `fill_triangle_pcolor` color modulation (e.g. the terrain demo); no built-in fog stage planned — keeps the rasterizer's hot loop free of per-pixel special-casing
- SIMD-vectorized rasterizer inner loops with auto-dispatch on detected CPU features

---

*Slag Language Specification v0.15 — Subject to revision*
