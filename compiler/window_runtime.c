// window_runtime.c — Slag Win32 windowing runtime emitter
//
// Emits NASM x86-64 Win64 assembly for the window subsystem.
// Included into the build alongside codegen.c.
//
// Design:
//   - Window lives on its own thread (_slag_window_thread_proc)
//   - Main thread writes pixels directly to DIB memory (_slag_pixel)
//   - window.flush() posts WM_USER+1 to the window thread for BitBlt
//   - window.close() posts WM_CLOSE to the window
//   - _window_open flag is set/cleared by the window thread (volatile qword)
//   - Event handlers are weak-linked — user on key_down { } etc. override them

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "ast.h"
#include "codegen_internal.h"
#include "window_runtime.h"

// Convenience macro — uses cg_emit from codegen_internal
#define E(fmt, ...) cg_emit(cg, fmt, ##__VA_ARGS__)

// ---------------------------------------------------------------------
// Win32 constants emitted as NASM equates
// ---------------------------------------------------------------------

static void emit_window_constants(Codegen *cg) {
    E("; --- Win32 window constants ---");
    E("WS_OVERLAPPEDWINDOW  equ 0x00CF0000");
    E("");
    E("; --- Window state struct offsets (TLS) ---");
    E("WSTATE_HWND        equ 0");
    E("WSTATE_HDC         equ 8");
    E("WSTATE_MEMDC       equ 16");
    E("WSTATE_HBITMAP     equ 24");
    E("WSTATE_PIXELS      equ 32");
    E("WSTATE_ZBUFFER     equ 40");
    E("WSTATE_WIDTH       equ 48");
    E("WSTATE_HEIGHT      equ 56");
    E("WSTATE_ADJ_W       equ 64");
    E("WSTATE_ADJ_H       equ 72");
    E("WSTATE_OPEN        equ 80");
    E("WSTATE_TITLE       equ 88");
    E("WSTATE_READY_EVT   equ 96");
    E("WSTATE_THREAD      equ 104");
    E("WSTATE_MSG         equ 112");
    E("WSTATE_SIZE        equ 160");
    E("WS_VISIBLE           equ 0x10000000");
    E("WS_EX_APPWINDOW      equ 0x00040000");
    E("CS_HREDRAW           equ 0x0002");
    E("CS_VREDRAW           equ 0x0001");
    E("IDC_ARROW            equ 32512");
    E("COLOR_WINDOW         equ 5");
    E("WM_DESTROY           equ 0x0002");
    E("WM_PAINT             equ 0x000F");
    E("WM_KEYDOWN           equ 0x0100");
    E("WM_KEYUP             equ 0x0101");
    E("WM_MOUSEMOVE         equ 0x0200");
    E("WM_LBUTTONDOWN       equ 0x0201");
    E("WM_LBUTTONUP         equ 0x0202");
    E("WM_RBUTTONDOWN       equ 0x0204");
    E("WM_RBUTTONUP         equ 0x0205");
    E("WM_MBUTTONDOWN       equ 0x0207");
    E("WM_MBUTTONUP         equ 0x0208");
    E("WM_MOUSEWHEEL        equ 0x020A");
    E("WM_USER_FLUSH        equ 0x0401  ; WM_USER+1: trigger BitBlt");
    E("WM_SIZE              equ 0x0005");
    E("WM_CLOSE             equ 0x0010");
    E("SW_SHOW              equ 5");
    E("PM_REMOVE            equ 1");
    E("DIB_RGB_COLORS       equ 0");
    E("BI_RGB               equ 0");
    E("SRCCOPY              equ 0x00CC0020");
    E("CREATE_SUSPENDED     equ 0x00000004");
    E("INFINITE             equ 0xFFFFFFFF");
    E("EVENT_AUTO_RESET     equ 0");
    E("EVENT_MANUAL_RESET   equ 1");
    E("MK_LBUTTON           equ 0x0001");
    E("MK_RBUTTON           equ 0x0002");
    E("MK_MBUTTON           equ 0x0010");
    E("");
}

// ---------------------------------------------------------------------
// _slag_window_open(rcx=w, rdx=h, r8=title_ptr, r9=title_len)
//
// TLS-based implementation:
// 1. Initialize TLS slot if first call
// 2. Allocate window state struct via HeapAlloc
// 3. Store params in struct, store struct ptr in TLS
// 4. Spawn window thread with struct ptr as parameter
// 5. Wait for ready event
// ---------------------------------------------------------------------
static void emit_window_open(Codegen *cg) {
    E("; --- _slag_window_open(w, h, title_ptr, title_len) ---");
    E("_slag_window_open:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push r12          ; w");
    E("    push r13          ; h");
    E("    push r14          ; title_ptr");
    E("    push r15          ; struct ptr");
    E("    sub  rsp, 64");
    E("");
    E("    mov  r12, rcx");
    E("    mov  r13, rdx");
    E("    mov  r14, r8");
    E("");
    E("    ; --- Initialize TLS if needed ---");
    E("    cmp  qword [_window_tls_init], 1");
    E("    je   .tls_ready");
    E("    sub  rsp, 32");
    E("    call TlsAlloc");
    E("    add  rsp, 32");
    E("    mov  [_window_tls_index], rax");
    E("    mov  qword [_window_tls_init], 1");
    E(".tls_ready:");
    E("");
    E("    ; --- Allocate window state struct (WSTATE_SIZE bytes) ---");
    E("    sub  rsp, 32");
    E("    call GetProcessHeap");
    E("    add  rsp, 32");
    E("    mov  rcx, rax           ; hHeap");
    E("    mov  rdx, 8             ; HEAP_ZERO_MEMORY");
    E("    mov  r8,  WSTATE_SIZE   ; dwBytes");
    E("    sub  rsp, 32");
    E("    call HeapAlloc");
    E("    add  rsp, 32");
    E("    mov  r15, rax           ; r15 = struct ptr");
    E("");
    E("    ; --- Store params in struct ---");
    E("    mov  [r15 + WSTATE_WIDTH],  r12");
    E("    mov  [r15 + WSTATE_HEIGHT], r13");
    E("    mov  [r15 + WSTATE_TITLE],  r14");
    E("");
    E("    ; --- Store struct ptr in TLS ---");
    E("    mov  rcx, [_window_tls_index]");
    E("    mov  rdx, r15");
    E("    sub  rsp, 32");
    E("    call TlsSetValue");
    E("    add  rsp, 32");
    E("");
    E("    ; --- Create ready event, store in struct ---");
    E("    xor  rcx, rcx          ; lpEventAttributes = NULL");
    E("    mov  rdx, 1            ; bManualReset = TRUE");
    E("    xor  r8,  r8           ; bInitialState = FALSE");
    E("    xor  r9,  r9           ; lpName = NULL");
    E("    sub  rsp, 32");
    E("    call CreateEventA");
    E("    add  rsp, 32");
    E("    mov  [r15 + WSTATE_READY_EVT], rax");
    E("");
    E("    ; --- Spawn window thread with struct ptr as param ---");
    E("    xor  rcx, rcx          ; lpThreadAttributes = NULL");
    E("    xor  rdx, rdx          ; dwStackSize = default");
    E("    lea  r8,  [_slag_window_thread_proc]");
    E("    mov  r9,  r15          ; lpParameter = struct ptr");
    E("    sub  rsp, 48");
    E("    mov  qword [rsp+32], 0 ; dwCreationFlags = 0");
    E("    mov  qword [rsp+40], 0 ; lpThreadId = NULL");
    E("    call CreateThread");
    E("    add  rsp, 48");
    E("    mov  [r15 + WSTATE_THREAD], rax");
    E("");
    E("    ; --- Wait for window to be ready ---");
    E("    mov  rcx, [r15 + WSTATE_READY_EVT]");
    E("    mov  rdx, INFINITE");
    E("    sub  rsp, 32");
    E("    call WaitForSingleObject");
    E("    add  rsp, 32");
    E("");
    E("    lea  rsp, [rbp-32]");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbp");
    E("    ret");
    E("");
}

// ---------------------------------------------------------------------
// _slag_window_thread_proc(rcx = struct_ptr)
//
// Runs on a dedicated thread. Receives window state struct pointer.
// Registers WNDCLASSEX, creates the window, creates the DIB section,
// signals ready event, then runs the Win32 message loop until WM_DESTROY.
// ---------------------------------------------------------------------
static void emit_window_thread_proc(Codegen *cg) {
    E("; --- _slag_window_thread_proc(rcx=struct_ptr) ---");
    E("_slag_window_thread_proc:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx              ; struct ptr (preserved)");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    push r15");
    E("    sub  rsp, 168         ; WNDCLASSEX(80) + MSG(48) + locals + align");
    E("");
    E("    mov  rbx, rcx         ; rbx = struct ptr (callee-saved)");
    E("");
    E("    ; --- Store struct ptr in TLS for this thread ---");
    E("    mov  rcx, [_window_tls_index]");
    E("    mov  rdx, rbx");
    E("    sub  rsp, 32");
    E("    call TlsSetValue");
    E("    add  rsp, 32");
    E("");

    // Register window class
    E("    ; --- RegisterClassEx ---");
    E("    ; WNDCLASSEX at [rsp]");
    E("    mov  dword [rsp+0],  80        ; cbSize = sizeof(WNDCLASSEX)");
    E("    mov  dword [rsp+4],  0x0003    ; style = CS_HREDRAW|CS_VREDRAW");
    E("    lea  rax, [_slag_wndproc]");
    E("    mov  [rsp+8],   rax            ; lpfnWndProc");
    E("    mov  dword [rsp+16], 0         ; cbClsExtra");
    E("    mov  dword [rsp+20], 0         ; cbWndExtra");
    E("    sub  rsp, 32");
    E("    xor  rcx, rcx");
    E("    call GetModuleHandleA          ; rcx=NULL -> hInstance");
    E("    add  rsp, 32");
    E("    mov  r12, rax                  ; save hInstance");
    E("    mov  [rsp+24], rax             ; hInstance");
    E("    mov  qword [rsp+32], 0         ; hIcon = NULL");
    E("    ; load arrow cursor");
    E("    xor  rcx, rcx");
    E("    mov  rdx, IDC_ARROW");
    E("    sub  rsp, 32");
    E("    call LoadCursorA");
    E("    add  rsp, 32");
    E("    mov  [rsp+40], rax             ; hCursor");
    E("    mov  qword [rsp+48], COLOR_WINDOW+1 ; hbrBackground");
    E("    mov  qword [rsp+56], 0         ; lpszMenuName = NULL");
    E("    lea  rax, [_window_class_name]");
    E("    mov  [rsp+64], rax             ; lpszClassName");
    E("    mov  qword [rsp+72], 0         ; hIconSm = NULL");
    E("    lea  rcx, [rsp]");
    E("    sub  rsp, 32");
    E("    call RegisterClassExA");
    E("    add  rsp, 32");
    E("");

    // Adjust window rect for client area
    E("    ; --- AdjustWindowRectEx ---");
    E("    sub  rsp, 48                   ; RECT(16) + shadow(32)");
    E("    mov  dword [rsp+32], 0         ; left = 0");
    E("    mov  dword [rsp+36], 0         ; top = 0");
    E("    mov  eax, [rbx + WSTATE_WIDTH]");
    E("    mov  [rsp+40], eax             ; right = width");
    E("    mov  eax, [rbx + WSTATE_HEIGHT]");
    E("    mov  [rsp+44], eax             ; bottom = height");
    E("    lea  rcx, [rsp+32]             ; lpRect");
    E("    mov  rdx, WS_OVERLAPPEDWINDOW  ; dwStyle");
    E("    xor  r8, r8                    ; bMenu = FALSE");
    E("    mov  r9, WS_EX_APPWINDOW       ; dwExStyle");
    E("    call AdjustWindowRectEx");
    E("    ; Calculate adjusted dimensions");
    E("    mov  eax, [rsp+40]             ; right");
    E("    sub  eax, [rsp+32]             ; right - left");
    E("    mov  [rbx + WSTATE_ADJ_W], eax");
    E("    mov  eax, [rsp+44]             ; bottom");
    E("    sub  eax, [rsp+36]             ; bottom - top");
    E("    mov  [rbx + WSTATE_ADJ_H], eax");
    E("    add  rsp, 48");
    E("");

    // Create window
    E("    ; --- CreateWindowEx ---");
    E("    mov  rcx, WS_EX_APPWINDOW     ; dwExStyle");
    E("    lea  rdx, [_window_class_name] ; lpClassName");
    E("    mov  r8,  [rbx + WSTATE_TITLE] ; lpWindowName");
    E("    mov  r9,  WS_OVERLAPPEDWINDOW  ; dwStyle");
    E("    sub  rsp, 96");
    E("    mov  qword [rsp+32], 100       ; x");
    E("    mov  qword [rsp+40], 100       ; y");
    E("    mov  rax,  [rbx + WSTATE_ADJ_W]");
    E("    mov  [rsp+48], rax             ; nWidth");
    E("    mov  rax,  [rbx + WSTATE_ADJ_H]");
    E("    mov  [rsp+56], rax             ; nHeight");
    E("    mov  qword [rsp+64], 0         ; hWndParent = NULL");
    E("    mov  qword [rsp+72], 0         ; hMenu = NULL");
    E("    mov  [rsp+80], r12             ; hInstance");
    E("    mov  qword [rsp+88], 0         ; lpParam = NULL");
    E("    call CreateWindowExA");
    E("    add  rsp, 96");
    E("    mov  r13, rax                  ; save HWND");
    E("    mov  [rbx + WSTATE_HWND], rax");
    E("    mov  qword [rbx + WSTATE_OPEN], 1");
    E("");

    // Store struct ptr in window's GWLP_USERDATA
    E("    ; --- Store struct ptr in window userdata ---");
    E("    mov  rcx, r13                  ; hwnd");
    E("    mov  rdx, GWLP_USERDATA");
    E("    mov  r8,  rbx                  ; struct ptr");
    E("    sub  rsp, 32");
    E("    call SetWindowLongPtrA");
    E("    add  rsp, 32");
    E("");

    // Create DIB section
    E("    ; --- CreateDIBSection ---");
    E("    ; Build BITMAPINFOHEADER on stack");
    E("    sub  rsp, 48                   ; BITMAPINFOHEADER = 40 bytes + pad");
    E("    mov  dword [rsp+0],  40        ; biSize");
    E("    mov  rax, [rbx + WSTATE_WIDTH]");
    E("    mov  dword [rsp+4],  eax       ; biWidth");
    E("    mov  rax, [rbx + WSTATE_HEIGHT]");
    E("    neg  rax                       ; negative = top-down DIB");
    E("    mov  dword [rsp+8],  eax       ; biHeight");
    E("    mov  word  [rsp+12], 1         ; biPlanes");
    E("    mov  word  [rsp+14], 32        ; biBitCount (BGRA)");
    E("    mov  dword [rsp+16], 0         ; biCompression = BI_RGB");
    E("    mov  dword [rsp+20], 0         ; biSizeImage");
    E("    mov  dword [rsp+24], 0         ; biXPelsPerMeter");
    E("    mov  dword [rsp+28], 0         ; biYPelsPerMeter");
    E("    mov  dword [rsp+32], 0         ; biClrUsed");
    E("    mov  dword [rsp+36], 0         ; biClrImportant");
    E("    ; get window DC");
    E("    mov  rcx, r13");
    E("    sub  rsp, 32");
    E("    call GetDC");
    E("    add  rsp, 32");
    E("    mov  r14, rax                  ; save hDC");
    E("    mov  [rbx + WSTATE_HDC], rax");
    E("    ; CreateCompatibleDC");
    E("    mov  rcx, r14");
    E("    sub  rsp, 32");
    E("    call CreateCompatibleDC");
    E("    add  rsp, 32");
    E("    mov  r15, rax                  ; save memDC");
    E("    mov  [rbx + WSTATE_MEMDC], rax");
    E("    ; CreateDIBSection - need temp for ppvBits");
    E("    mov  rcx, r15                  ; hdc = memDC");
    E("    lea  rdx, [rsp]                ; pbmi (our BITMAPINFOHEADER at rsp)");
    E("    mov  r8,  DIB_RGB_COLORS");
    E("    lea  r9,  [rbp-80]             ; temp for ppvBits");
    E("    sub  rsp, 48");
    E("    mov  qword [rsp+32], 0         ; hSection = NULL");
    E("    mov  qword [rsp+40], 0         ; dwOffset = 0");
    E("    call CreateDIBSection");
    E("    add  rsp, 48");
    E("    mov  [rbx + WSTATE_HBITMAP], rax");
    E("    mov  rax, [rbp-80]             ; get pixels ptr from temp");
    E("    mov  [rbx + WSTATE_PIXELS], rax");
    E("    ; SelectObject(memDC, hbitmap)");
    E("    mov  rcx, r15");
    E("    mov  rdx, [rbx + WSTATE_HBITMAP]");
    E("    sub  rsp, 32");
    E("    call SelectObject");
    E("    add  rsp, 32");
    E("    add  rsp, 48                   ; free BITMAPINFOHEADER space");
    E("");
    E("    ; --- allocate z-buffer: width*height*8 bytes ---");
    E("    mov  rax, [rbx + WSTATE_WIDTH]");
    E("    imul rax, [rbx + WSTATE_HEIGHT]");
    E("    shl  rax, 3                    ; * 8 bytes per double");
    E("    mov  r14, rax                  ; save byte count");
    E("    sub  rsp, 32");
    E("    call GetProcessHeap");
    E("    add  rsp, 32");
    E("    mov  rcx, rax                  ; hHeap");
    E("    xor  rdx, rdx                  ; flags = 0");
    E("    mov  r8,  r14                  ; size in bytes");
    E("    sub  rsp, 32");
    E("    call HeapAlloc");
    E("    add  rsp, 32");
    E("    mov  [rbx + WSTATE_ZBUFFER], rax");
    E("");

    // Show window
    E("    ; ShowWindow");
    E("    mov  rcx, r13");
    E("    mov  rdx, SW_SHOW");
    E("    sub  rsp, 32");
    E("    call ShowWindow");
    E("    add  rsp, 32");
    E("    mov  rcx, r13");
    E("    sub  rsp, 32");
    E("    call UpdateWindow");
    E("    add  rsp, 32");
    E("");

    // Signal ready
    E("    ; signal ready event");
    E("    mov  rcx, [rbx + WSTATE_READY_EVT]");
    E("    sub  rsp, 32");
    E("    call SetEvent");
    E("    add  rsp, 32");
    E("");

    // Message loop - use struct's MSG buffer
    E("    ; --- message loop ---");
    E(".msg_loop:");
    E("    lea  rcx, [rbx + WSTATE_MSG]   ; lpMsg");
    E("    xor  rdx, rdx                  ; hWnd = NULL (all messages)");
    E("    xor  r8,  r8                   ; wMsgFilterMin = 0");
    E("    xor  r9,  r9                   ; wMsgFilterMax = 0");
    E("    sub  rsp, 32");
    E("    call GetMessageA");
    E("    add  rsp, 32");
    E("    test rax, rax");
    E("    jz   .msg_done");
    E("    lea  rcx, [rbx + WSTATE_MSG]");
    E("    sub  rsp, 32");
    E("    call TranslateMessage");
    E("    add  rsp, 32");
    E("    lea  rcx, [rbx + WSTATE_MSG]");
    E("    sub  rsp, 32");
    E("    call DispatchMessageA");
    E("    add  rsp, 32");
    E("    jmp  .msg_loop");
    E(".msg_done:");
    E("    mov  qword [rbx + WSTATE_OPEN], 0");
    E("");
    E("    add  rsp, 168");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    xor  rax, rax");
    E("    ret");
    E("");
}

// ---------------------------------------------------------------------
// _slag_wndproc(HWND, UINT msg, WPARAM, LPARAM)
//
// Gets window state struct from GWLP_USERDATA.
// Handles:
//   WM_DESTROY   — post quit, clear open flag
//   WM_PAINT     — BitBlt DIB to window
//   WM_USER_FLUSH— BitBlt DIB to window (from window.flush())
//   WM_KEYDOWN/UP, WM_MOUSE* — call event handlers
// ---------------------------------------------------------------------
static void emit_wndproc(Codegen *cg) {
    E("; --- _slag_wndproc ---");
    E("_slag_wndproc:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx              ; struct ptr");
    E("    push r12              ; hwnd");
    E("    push r13              ; msg");
    E("    push r14              ; wParam");
    E("    push r15              ; lParam");
    E("    sub  rsp, 72");
    E("");
    E("    mov  r12, rcx         ; hwnd");
    E("    mov  r13, rdx         ; msg");
    E("    mov  r14, r8          ; wParam");
    E("    mov  r15, r9          ; lParam");
    E("");
    E("    ; --- Get struct ptr from window userdata ---");
    E("    mov  rcx, r12         ; hwnd");
    E("    mov  rdx, GWLP_USERDATA");
    E("    sub  rsp, 32");
    E("    call GetWindowLongPtrA");
    E("    add  rsp, 32");
    E("    mov  rbx, rax         ; rbx = struct ptr");
    E("    test rbx, rbx");
    E("    jz   .wndproc_default ; no struct yet, use default");
    E("");

    // Dispatch on message
    E("    cmp  r13, WM_DESTROY");
    E("    je   .wndproc_destroy");
    E("    cmp  r13, WM_PAINT");
    E("    je   .wndproc_paint");
    E("    cmp  r13, WM_USER_FLUSH");
    E("    je   .wndproc_flush");
    E("    cmp  r13, WM_KEYDOWN");
    E("    je   .wndproc_keydown");
    E("    cmp  r13, WM_KEYUP");
    E("    je   .wndproc_keyup");
    E("    cmp  r13, WM_MOUSEMOVE");
    E("    je   .wndproc_mousemove");
    E("    cmp  r13, WM_LBUTTONDOWN");
    E("    je   .wndproc_lbuttondown");
    E("    cmp  r13, WM_LBUTTONUP");
    E("    je   .wndproc_lbuttonup");
    E("    cmp  r13, WM_RBUTTONDOWN");
    E("    je   .wndproc_rbuttondown");
    E("    cmp  r13, WM_RBUTTONUP");
    E("    je   .wndproc_rbuttonup");
    E("    cmp  r13, WM_MBUTTONDOWN");
    E("    je   .wndproc_mbuttondown");
    E("    cmp  r13, WM_MBUTTONUP");
    E("    je   .wndproc_mbuttonup");
    E("    cmp  r13, WM_MOUSEWHEEL");
    E("    je   .wndproc_mousewheel");
    E("    cmp  r13, WM_CLOSE");
    E("    je   .wndproc_close");
    E("    jmp  .wndproc_default");
    E("");

    // WM_DESTROY
    E(".wndproc_destroy:");
    E("    mov  qword [rbx + WSTATE_OPEN], 0");
    E("    xor  rcx, rcx");
    E("    sub  rsp, 32");
    E("    call PostQuitMessage");
    E("    add  rsp, 32");
    E("    xor  rax, rax");
    E("    jmp  .wndproc_ret");
    E("");

    // WM_PAINT and WM_USER_FLUSH — BitBlt DIB to window
    E(".wndproc_paint:");
    E(".wndproc_flush:");
    E("    ; BitBlt(hdc, 0, 0, w, h, memdc, 0, 0, SRCCOPY)");
    E("    mov  rcx, [rbx + WSTATE_HDC]");
    E("    xor  rdx, rdx              ; x=0");
    E("    xor  r8,  r8               ; y=0");
    E("    mov  r9,  [rbx + WSTATE_WIDTH]");
    E("    sub  rsp, 80");
    E("    mov  rax, [rbx + WSTATE_HEIGHT]");
    E("    mov  [rsp+32], rax         ; nHeight");
    E("    mov  rax, [rbx + WSTATE_MEMDC]");
    E("    mov  [rsp+40], rax         ; hdcSrc");
    E("    mov  qword [rsp+48], 0     ; xSrc=0");
    E("    mov  qword [rsp+56], 0     ; ySrc=0");
    E("    mov  qword [rsp+64], SRCCOPY");
    E("    call BitBlt");
    E("    add  rsp, 80");
    E("    ; ValidateRect for WM_PAINT");
    E("    mov  rcx, r12");
    E("    xor  rdx, rdx");
    E("    sub  rsp, 32");
    E("    call ValidateRect");
    E("    add  rsp, 32");
    E("    xor  rax, rax");
    E("    jmp  .wndproc_ret");
    E("");

    // WM_KEYDOWN
    E(".wndproc_keydown:");
    E("    mov  rcx, r14              ; keycode = wParam");
    E("    sub  rsp, 32");
    E("    call _slag_on_key_down");
    E("    add  rsp, 32");
    E("    xor  rax, rax");
    E("    jmp  .wndproc_ret");
    E("");

    // WM_KEYUP
    E(".wndproc_keyup:");
    E("    mov  rcx, r14");
    E("    sub  rsp, 32");
    E("    call _slag_on_key_up");
    E("    add  rsp, 32");
    E("    xor  rax, rax");
    E("    jmp  .wndproc_ret");
    E("");

    // WM_MOUSEMOVE — lParam: low word = x, high word = y
    E(".wndproc_mousemove:");
    E("    mov  rcx, r15");
    E("    movsx rdx, cx              ; low 16 = x");
    E("    shr  rcx, 16");
    E("    movsx rcx, cx              ; high 16 = y");
    E("    xchg rcx, rdx              ; reorder for call(x,y)");
    E("    sub  rsp, 32");
    E("    call _slag_on_mouse_move");
    E("    add  rsp, 32");
    E("    xor  rax, rax");
    E("    jmp  .wndproc_ret");
    E("");

    // WM_LBUTTONDOWN
    E(".wndproc_lbuttondown:");
    E("    xor  rcx, rcx              ; button = 0 (left)");
    E("    mov  rdx, r15");
    E("    movsx r8, dx               ; x");
    E("    shr  rdx, 16");
    E("    movsx r9, dx               ; y");
    E("    mov  rdx, r8               ; shift args: rcx=btn, rdx=x, r8=y");
    E("    mov  r8,  r9");
    E("    sub  rsp, 32");
    E("    call _slag_on_mouse_down");
    E("    add  rsp, 32");
    E("    xor  rax, rax");
    E("    jmp  .wndproc_ret");
    E("");

    // WM_LBUTTONUP
    E(".wndproc_lbuttonup:");
    E("    xor  rcx, rcx");
    E("    mov  rdx, r15");
    E("    movsx r8, dx");
    E("    shr  rdx, 16");
    E("    movsx r9, dx");
    E("    mov  rdx, r8");
    E("    mov  r8,  r9");
    E("    sub  rsp, 32");
    E("    call _slag_on_mouse_up");
    E("    add  rsp, 32");
    E("    xor  rax, rax");
    E("    jmp  .wndproc_ret");
    E("");

    // WM_RBUTTONDOWN
    E(".wndproc_rbuttondown:");
    E("    mov  rcx, 1                ; button = 1 (right)");
    E("    mov  rdx, r15");
    E("    movsx r8, dx");
    E("    shr  rdx, 16");
    E("    movsx r9, dx");
    E("    mov  rdx, r8");
    E("    mov  r8,  r9");
    E("    sub  rsp, 32");
    E("    call _slag_on_mouse_down");
    E("    add  rsp, 32");
    E("    xor  rax, rax");
    E("    jmp  .wndproc_ret");
    E("");

    // WM_RBUTTONUP
    E(".wndproc_rbuttonup:");
    E("    mov  rcx, 1");
    E("    mov  rdx, r15");
    E("    movsx r8, dx");
    E("    shr  rdx, 16");
    E("    movsx r9, dx");
    E("    mov  rdx, r8");
    E("    mov  r8,  r9");
    E("    sub  rsp, 32");
    E("    call _slag_on_mouse_up");
    E("    add  rsp, 32");
    E("    xor  rax, rax");
    E("    jmp  .wndproc_ret");
    E("");

    // WM_MBUTTONDOWN
    E(".wndproc_mbuttondown:");
    E("    mov  rcx, 2                ; button = 2 (middle)");
    E("    mov  rdx, r15");
    E("    movsx r8, dx");
    E("    shr  rdx, 16");
    E("    movsx r9, dx");
    E("    mov  rdx, r8");
    E("    mov  r8,  r9");
    E("    sub  rsp, 32");
    E("    call _slag_on_mouse_down");
    E("    add  rsp, 32");
    E("    xor  rax, rax");
    E("    jmp  .wndproc_ret");
    E("");

    // WM_MBUTTONUP
    E(".wndproc_mbuttonup:");
    E("    mov  rcx, 2                ; button = 2 (middle)");
    E("    mov  rdx, r15");
    E("    movsx r8, dx");
    E("    shr  rdx, 16");
    E("    movsx r9, dx");
    E("    mov  rdx, r8");
    E("    mov  r8,  r9");
    E("    sub  rsp, 32");
    E("    call _slag_on_mouse_up");
    E("    add  rsp, 32");
    E("    xor  rax, rax");
    E("    jmp  .wndproc_ret");
    E("");

    // WM_MOUSEWHEEL — wParam high word = signed delta (multiples of 120)
    E(".wndproc_mousewheel:");
    E("    mov  rcx, r14              ; wParam");
    E("    shr  rcx, 16");
    E("    movsx rcx, cx              ; delta");
    E("    sub  rsp, 32");
    E("    call _slag_on_mouse_wheel");
    E("    add  rsp, 32");
    E("    xor  rax, rax");
    E("    jmp  .wndproc_ret");
    E("");
    E(".wndproc_close:");
    E("    mov  rcx, r12");
    E("    sub  rsp, 32");
    E("    call DestroyWindow");
    E("    add  rsp, 32");
    E("    xor  rax, rax");
    E("    jmp  .wndproc_ret");
    E("");
    E(".wndproc_default:");
    E("    mov  rcx, r12");
    E("    mov  rdx, r13");
    E("    mov  r8,  r14");
    E("    mov  r9,  r15");
    E("    sub  rsp, 32");
    E("    call DefWindowProcA");
    E("    add  rsp, 32");
    E("    jmp  .wndproc_ret");
    E("");

    E(".wndproc_ret:");
    E("    add  rsp, 72");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");
}

// ---------------------------------------------------------------------
// _slag_window_flush() — posts WM_USER_FLUSH to window thread
// _slag_window_close() — posts WM_CLOSE to window
// _slag_pixel(x, y, r, g, b) — writes BGRA pixel to DIB buffer
// All use TLS to get current thread's window state struct.
// ---------------------------------------------------------------------
static void emit_window_utils(Codegen *cg) {
    // Helper: _slag_get_window_state — gets struct ptr from TLS into rax
    E("; --- _slag_get_window_state -> rax ---");
    E("_slag_get_window_state:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    add  rsp, 32");
    E("    pop  rbp");
    E("    ret");
    E("");

    // window.flush()
    E("; --- _slag_window_flush ---");
    E("_slag_window_flush:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 40");
    E("    ; get struct ptr");
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    mov  rbx, rax");
    E("    ; PostMessage(hwnd, WM_USER_FLUSH, 0, 0)");
    E("    mov  rcx, [rbx + WSTATE_HWND]");
    E("    mov  rdx, WM_USER_FLUSH");
    E("    xor  r8,  r8");
    E("    xor  r9,  r9");
    E("    sub  rsp, 32");
    E("    call PostMessageA");
    E("    add  rsp, 32");
    E("    ; sleep ~16ms for frame rate cap");
    E("    mov  rcx, 16");
    E("    sub  rsp, 32");
    E("    call Sleep");
    E("    add  rsp, 32");
    E("    add  rsp, 40");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");

    // window.close()
    E("; --- _slag_window_close ---");
    E("_slag_window_close:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 40");
    E("    ; get struct ptr");
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    mov  rbx, rax");
    E("    ; PostMessage(hwnd, WM_CLOSE, 0, 0)");
    E("    mov  rcx, [rbx + WSTATE_HWND]");
    E("    mov  rdx, WM_CLOSE");
    E("    xor  r8,  r8");
    E("    xor  r9,  r9");
    E("    sub  rsp, 32");
    E("    call PostMessageA");
    E("    add  rsp, 32");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_window_capture_mouse() - capture, clip, and hide cursor
    // Stack layout: RECT(16) + POINT(8) + padding
    E("; --- _slag_window_capture_mouse ---");
    E("_slag_window_capture_mouse:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    push r12");
    E("    sub  rsp, 64");
    E("");
    E("    ; get struct ptr from TLS");
    E("    sub  rsp, 32");
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    add  rsp, 32");
    E("    mov  rbx, rax");
    E("");
    E("    ; SetCapture(hwnd)");
    E("    mov  rcx, [rbx + WSTATE_HWND]");
    E("    sub  rsp, 32");
    E("    call SetCapture");
    E("    add  rsp, 32");
    E("");
    E("    ; GetClientRect(hwnd, &rect) - rect at [rbp-48]");
    E("    mov  rcx, [rbx + WSTATE_HWND]");
    E("    lea  rdx, [rbp-48]");
    E("    sub  rsp, 32");
    E("    call GetClientRect");
    E("    add  rsp, 32");
    E("");
    E("    ; Save right/bottom before converting top-left");
    E("    mov  r12d, [rbp-40]          ; save right (offset 8)");
    E("    mov  eax, [rbp-36]           ; save bottom (offset 12)");
    E("    mov  [rbp-24], eax           ; store bottom temporarily");
    E("");
    E("    ; ClientToScreen top-left point (left,top at offset 0,4)");
    E("    mov  rcx, [rbx + WSTATE_HWND]");
    E("    lea  rdx, [rbp-48]           ; point to left,top");
    E("    sub  rsp, 32");
    E("    call ClientToScreen");
    E("    add  rsp, 32");
    E("");
    E("    ; Build point2 with saved right,bottom at [rbp-32]");
    E("    mov  [rbp-32], r12d          ; x = right");
    E("    mov  eax, [rbp-24]");
    E("    mov  [rbp-28], eax           ; y = bottom");
    E("");
    E("    ; ClientToScreen bottom-right point");
    E("    mov  rcx, [rbx + WSTATE_HWND]");
    E("    lea  rdx, [rbp-32]");
    E("    sub  rsp, 32");
    E("    call ClientToScreen");
    E("    add  rsp, 32");
    E("");
    E("    ; Build final RECT: [rbp-48] has screen left,top; [rbp-32] has screen right,bottom");
    E("    mov  eax, [rbp-32]           ; screen right");
    E("    mov  [rbp-40], eax           ; rect.right");
    E("    mov  eax, [rbp-28]           ; screen bottom");
    E("    mov  [rbp-36], eax           ; rect.bottom");
    E("");
    E("    ; ClipCursor(&rect)");
    E("    lea  rcx, [rbp-48]");
    E("    sub  rsp, 32");
    E("    call ClipCursor");
    E("    add  rsp, 32");
    E("");
    E("    ; ShowCursor(FALSE)");
    E("    xor  rcx, rcx");
    E("    sub  rsp, 32");
    E("    call ShowCursor");
    E("    add  rsp, 32");
    E("");
    E("    add  rsp, 64");
    E("    pop  r12");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_window_release_mouse() - release and show cursor
    E("; --- _slag_window_release_mouse ---");
    E("_slag_window_release_mouse:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("");
    E("    ; ReleaseCapture()");
    E("    call ReleaseCapture");
    E("");
    E("    ; ClipCursor(NULL)");
    E("    xor  rcx, rcx");
    E("    call ClipCursor");
    E("");
    E("    ; ShowCursor(TRUE)");
    E("    mov  rcx, 1");
    E("    call ShowCursor");
    E("");
    E("    add  rsp, 32");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_window_center_cursor() - move cursor to window center
    E("; --- _slag_window_center_cursor ---");
    E("_slag_window_center_cursor:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    push r12");
    E("    push r13");
    E("    sub  rsp, 64");
    E("");
    E("    ; get struct ptr from TLS");
    E("    sub  rsp, 32");
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    add  rsp, 32");
    E("    mov  rbx, rax");
    E("");
    E("    ; GetClientRect(hwnd, &rect) - rect at [rbp-48]");
    E("    mov  rcx, [rbx + WSTATE_HWND]");
    E("    lea  rdx, [rbp-48]");
    E("    sub  rsp, 32");
    E("    call GetClientRect");
    E("    add  rsp, 32");
    E("");
    E("    ; Calculate center: cx = right/2, cy = bottom/2");
    E("    mov  eax, [rbp-40]              ; right");
    E("    shr  eax, 1");
    E("    mov  r12d, eax                  ; r12 = center_x");
    E("    mov  eax, [rbp-36]              ; bottom");
    E("    shr  eax, 1");
    E("    mov  r13d, eax                  ; r13 = center_y");
    E("");
    E("    ; Store center point at [rbp-48] for ClientToScreen");
    E("    mov  [rbp-48], r12d             ; point.x = center_x");
    E("    mov  [rbp-44], r13d             ; point.y = center_y");
    E("");
    E("    ; ClientToScreen(hwnd, &point)");
    E("    mov  rcx, [rbx + WSTATE_HWND]");
    E("    lea  rdx, [rbp-48]");
    E("    sub  rsp, 32");
    E("    call ClientToScreen");
    E("    add  rsp, 32");
    E("");
    E("    ; SetCursorPos(screen_x, screen_y)");
    E("    mov  ecx, [rbp-48]              ; screen_x");
    E("    mov  edx, [rbp-44]              ; screen_y");
    E("    sub  rsp, 32");
    E("    call SetCursorPos");
    E("    add  rsp, 32");
    E("");
    E("    add  rsp, 64");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_window_native() - returns "WxH" string for native screen resolution
    E("; --- _slag_window_native ---");
    E("_slag_window_native:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    push r12");
    E("    sub  rsp, 48");
    E("");
    E("    ; GetSystemMetrics(SM_CXSCREEN=0) -> width");
    E("    xor  ecx, ecx");
    E("    sub  rsp, 32");
    E("    call GetSystemMetrics");
    E("    add  rsp, 32");
    E("    mov  r12, rax                 ; r12 = width");
    E("");
    E("    ; GetSystemMetrics(SM_CYSCREEN=1) -> height");
    E("    mov  ecx, 1");
    E("    sub  rsp, 32");
    E("    call GetSystemMetrics");
    E("    add  rsp, 32");
    E("    mov  rbx, rax                 ; rbx = height");
    E("");
    E("    ; Build string \"WxH\" in _native_res_buf");
    E("    lea  rdi, [_native_res_buf]");
    E("");
    E("    ; Convert width to decimal string");
    E("    mov  rax, r12");
    E("    call .int_to_str");
    E("");
    E("    ; Append 'x'");
    E("    mov  byte [rdi], 'x'");
    E("    inc  rdi");
    E("");
    E("    ; Convert height to decimal string");
    E("    mov  rax, rbx");
    E("    call .int_to_str");
    E("");
    E("    ; Null terminate");
    E("    mov  byte [rdi], 0");
    E("");
    E("    ; Return pointer to buffer");
    E("    lea  rax, [_native_res_buf]");
    E("");
    E("    add  rsp, 48");
    E("    pop  r12");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");
    E("    ; Local helper: convert rax to decimal at [rdi], advance rdi");
    E(".int_to_str:");
    E("    push rbx");
    E("    push rcx");
    E("    push rdx");
    E("    mov  rbx, rdi                 ; save start");
    E("    mov  rcx, 10");
    E("    test rax, rax");
    E("    jnz  .its_nonzero");
    E("    mov  byte [rdi], '0'");
    E("    inc  rdi");
    E("    jmp  .its_done");
    E(".its_nonzero:");
    E("    ; Push digits in reverse");
    E("    xor  r8, r8                   ; digit count");
    E(".its_loop:");
    E("    test rax, rax");
    E("    jz   .its_reverse");
    E("    xor  edx, edx");
    E("    div  rcx                      ; rax=quot, rdx=rem");
    E("    add  dl, '0'");
    E("    push rdx");
    E("    inc  r8");
    E("    jmp  .its_loop");
    E(".its_reverse:");
    E("    test r8, r8");
    E("    jz   .its_done");
    E("    pop  rdx");
    E("    mov  [rdi], dl");
    E("    inc  rdi");
    E("    dec  r8");
    E("    jmp  .its_reverse");
    E(".its_done:");
    E("    pop  rdx");
    E("    pop  rcx");
    E("    pop  rbx");
    E("    ret");
    E("");

    // pixel(x, y, r, g, b)
    // rcx=x, rdx=y, r8=r, r9=g, [rsp+32]=b
    // BGRA format: byte order in memory is B, G, R, A
    // offset = (y * width + x) * 4
    // TLS-based: gets struct ptr from TLS, uses WSTATE_* offsets
    E("; --- _slag_pixel(x, y, r, g, b) ---");
    E("_slag_pixel:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    push r15");
    E("    sub  rsp, 56");
    E("");
    E("    ; save args");
    E("    mov  r12, rcx              ; x");
    E("    mov  r13, rdx              ; y");
    E("    mov  r14, r8               ; r");
    E("    mov  r15, r9               ; g");
    E("");
    E("    ; get struct ptr from TLS");
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    mov  rbx, rax              ; rbx = struct ptr");
    E("");
    E("    ; bounds check: 0 <= x < width, 0 <= y < height");
    E("    cmp  r12, 0");
    E("    jl   .pixel_done");
    E("    cmp  r12, [rbx + WSTATE_WIDTH]");
    E("    jge  .pixel_done");
    E("    cmp  r13, 0");
    E("    jl   .pixel_done");
    E("    cmp  r13, [rbx + WSTATE_HEIGHT]");
    E("    jge  .pixel_done");
    E("");
    E("    ; compute offset = (y * width + x) * 4");
    E("    mov  rax, r13                   ; rax = y");
    E("    imul rax, [rbx + WSTATE_WIDTH]  ; rax = y * width");
    E("    add  rax, r12                   ; rax = y * width + x");
    E("    shl  rax, 2                     ; rax *= 4 (BGRA)");
    E("");
    E("    mov  r10, [rbx + WSTATE_PIXELS] ; base ptr");
    E("    add  r10, rax                   ; ptr to pixel");
    E("");
    E("    ; pack color: 0xFF000000 | R<<16 | G<<8 | B");
    E("    mov  eax, 0xFF000000");
    E("    mov  ecx, r14d             ; R");
    E("    shl  ecx, 16");
    E("    or   eax, ecx");
    E("    mov  ecx, r15d             ; G");
    E("    shl  ecx, 8");
    E("    or   eax, ecx");
    E("    mov  ecx, [rbp+48]         ; B (5th arg)");
    E("    or   eax, ecx");
    E("    mov  dword [r10], eax      ; write packed pixel");
    E("");
    E(".pixel_done:");
    E("    add  rsp, 56");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");

    // -------------------------------------------------------------
    // _slag_fill_triangle(x0,y0,x1,y1,x2,y2,r,g,b)
    // Flat-shaded scanline triangle rasterizer. Writes directly into
    // _window_pixels, bounds-clamped to the framebuffer.
    //
    // Args: rcx=x0 rdx=y0 r8=x1 r9=y1
    //       [rbp+48]=x2 [rbp+56]=y2 [rbp+64]=r [rbp+72]=g [rbp+80]=b
    //
    // Local frame layout (all qwords, [rbp-N]):
    //   -8  x0   -16 y0   -24 x1   -32 y1   -40 x2   -48 y2
    //   -56 r    -64 g    -72 b
    //   -80 ya   -88 yb   -96 yc   (sorted y, ascending: ya<=yb<=yc)
    //  -104 xa  -112 xb  -120 xc   (x corresponding to ya/yb/yc)
    //   -128 y         (current scanline)
    //   -136 xleft     -144 xright
    //   -152 struct_ptr (TLS window state)
    // TLS-based: gets struct ptr from TLS, uses WSTATE_* offsets
    // -------------------------------------------------------------
    // DDA-based fill_triangle: compute slopes once, step incrementally
    // Uses 16.16 fixed-point for sub-pixel accuracy
    // Stack layout (expanded for DDA):
    //   -8   x0      -16  y0      -24  x1      -32  y1
    //   -40  x2      -48  y2      -56  r       -64  g       -72  b
    //   -80  ya      -88  yb      -96  yc
    //   -104 xa      -112 xb      -120 xc
    //   -128 y (current scanline)
    // _slag_render_begin() - cache TLS pointer for fast rendering
    // Call once per frame before drawing triangles
    E("; --- _slag_render_begin ---");
    E("_slag_render_begin:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    mov  [_window_cached_state], rax");
    E("    add  rsp, 32");
    E("    pop  rbp");
    E("    ret");
    E("");

    //   -136 x_long (16.16 fixed)   -144 x_short (16.16 fixed)
    //   -152 dx_long (16.16 slope)  -160 dx_short (16.16 slope)
    //   -168 packed_color
    E("; --- _slag_fill_triangle DDA optimized ---");
    E("_slag_fill_triangle:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    push r15");
    E("    sub  rsp, 200");
    E("");
    E("    ; use cached TLS pointer (lazy init if null)");
    E("    mov  rbx, [_window_cached_state]");
    E("    test rbx, rbx");
    E("    jnz  .ft_have_cache");
    E("    mov  [rbp-8],  rcx");
    E("    mov  [rbp-16], rdx");
    E("    mov  [rbp-24], r8");
    E("    mov  [rbp-32], r9");
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    mov  rbx, rax");
    E("    mov  [_window_cached_state], rbx");
    E("    mov  rcx, [rbp-8]");
    E("    mov  rdx, [rbp-16]");
    E("    mov  r8,  [rbp-24]");
    E("    mov  r9,  [rbp-32]");
    E(".ft_have_cache:");
    E("    mov  [rbp-8],  rcx");
    E("    mov  [rbp-16], rdx");
    E("    mov  [rbp-24], r8");
    E("    mov  [rbp-32], r9");
    E("    mov  rax, [rbp+48]");
    E("    mov  [rbp-40], rax");
    E("    mov  rax, [rbp+56]");
    E("    mov  [rbp-48], rax");
    E("    mov  rax, [rbp+64]");
    E("    mov  [rbp-56], rax");
    E("    mov  rax, [rbp+72]");
    E("    mov  [rbp-64], rax");
    E("    mov  rax, [rbp+80]");
    E("    mov  [rbp-72], rax");
    E("");
    E("    ; copy to sortable slots");
    E("    mov  rax, [rbp-8]");
    E("    mov  [rbp-104], rax");
    E("    mov  rax, [rbp-16]");
    E("    mov  [rbp-80], rax");
    E("    mov  rax, [rbp-24]");
    E("    mov  [rbp-112], rax");
    E("    mov  rax, [rbp-32]");
    E("    mov  [rbp-88], rax");
    E("    mov  rax, [rbp-40]");
    E("    mov  [rbp-120], rax");
    E("    mov  rax, [rbp-48]");
    E("    mov  [rbp-96], rax");
    E("");
    E("    ; bubble sort by y");
    E("    mov  rax, [rbp-80]");
    E("    cmp  rax, [rbp-88]");
    E("    jle  .ft_s1");
    E("    mov  rcx, [rbp-88]");
    E("    mov  [rbp-80], rcx");
    E("    mov  [rbp-88], rax");
    E("    mov  rax, [rbp-104]");
    E("    mov  rcx, [rbp-112]");
    E("    mov  [rbp-104], rcx");
    E("    mov  [rbp-112], rax");
    E(".ft_s1:");
    E("    mov  rax, [rbp-88]");
    E("    cmp  rax, [rbp-96]");
    E("    jle  .ft_s2");
    E("    mov  rcx, [rbp-96]");
    E("    mov  [rbp-88], rcx");
    E("    mov  [rbp-96], rax");
    E("    mov  rax, [rbp-112]");
    E("    mov  rcx, [rbp-120]");
    E("    mov  [rbp-112], rcx");
    E("    mov  [rbp-120], rax");
    E(".ft_s2:");
    E("    mov  rax, [rbp-80]");
    E("    cmp  rax, [rbp-88]");
    E("    jle  .ft_s3");
    E("    mov  rcx, [rbp-88]");
    E("    mov  [rbp-80], rcx");
    E("    mov  [rbp-88], rax");
    E("    mov  rax, [rbp-104]");
    E("    mov  rcx, [rbp-112]");
    E("    mov  [rbp-104], rcx");
    E("    mov  [rbp-112], rax");
    E(".ft_s3:");
    E("");
    E("    ; early exit if degenerate (ya == yc)");
    E("    mov  rax, [rbp-80]");
    E("    cmp  rax, [rbp-96]");
    E("    je   .ft_done");
    E("");
    E("    ; pack color once");
    E("    mov  eax, 0xFF000000");
    E("    mov  ecx, [rbp-56]");
    E("    shl  ecx, 16");
    E("    or   eax, ecx");
    E("    mov  ecx, [rbp-64]");
    E("    shl  ecx, 8");
    E("    or   eax, ecx");
    E("    mov  ecx, [rbp-72]");
    E("    or   eax, ecx");
    E("    mov  [rbp-168], eax");
    E("    movd xmm1, eax");
    E("    pshufd xmm1, xmm1, 0");
    E("");
    E("    ; compute dx_long = ((xc-xa) << 16) / (yc-ya)");
    E("    mov  rax, [rbp-120]");
    E("    sub  rax, [rbp-104]");
    E("    shl  rax, 16");
    E("    mov  rcx, [rbp-96]");
    E("    sub  rcx, [rbp-80]");
    E("    cqo");
    E("    idiv rcx");
    E("    mov  [rbp-152], rax");
    E("");
    E("    ; x_long = xa << 16");
    E("    mov  rax, [rbp-104]");
    E("    shl  rax, 16");
    E("    mov  r12, rax");
    E("");
    E("    ; r13 = yb, r14 = yc, r15 = max_y (clamped)");
    E("    mov  r13, [rbp-88]");
    E("    mov  r14, [rbp-96]");
    E("    mov  r15, [rbx + WSTATE_HEIGHT]");
    E("    dec  r15");
    E("");
    E("    ; === UPPER HALF: ya to yb ===");
    E("    mov  rax, [rbp-80]");
    E("    cmp  rax, r13");
    E("    jge  .ft_lower_half");
    E("");
    E("    ; compute dx_upper = ((xb-xa) << 16) / (yb-ya)");
    E("    mov  rax, [rbp-112]");
    E("    sub  rax, [rbp-104]");
    E("    shl  rax, 16");
    E("    mov  rcx, r13");
    E("    sub  rcx, [rbp-80]");
    E("    cmp  rcx, 0");
    E("    je   .ft_lower_half");
    E("    cqo");
    E("    idiv rcx");
    E("    mov  [rbp-160], rax");
    E("");
    E("    ; x_short = xa << 16");
    E("    mov  rax, [rbp-104]");
    E("    shl  rax, 16");
    E("    mov  [rbp-144], rax");
    E("");
    E("    mov  rax, [rbp-80]");
    E("    mov  [rbp-128], rax");
    E("");
    E(".ft_upper_loop:");
    E("    mov  rax, [rbp-128]");
    E("    cmp  rax, r13");
    E("    jge  .ft_lower_half");
    E("    cmp  rax, r15");
    E("    jg   .ft_done");
    E("    cmp  rax, 0");
    E("    jl   .ft_upper_next");
    E("");
    E("    ; get integer x values");
    E("    mov  rax, r12");
    E("    sar  rax, 16");
    E("    mov  rcx, [rbp-144]");
    E("    sar  rcx, 16");
    E("    cmp  rax, rcx");
    E("    jle  .ft_upper_ordered");
    E("    xchg rax, rcx");
    E(".ft_upper_ordered:");
    E("    ; rax=xleft, rcx=xright - clamp to screen");
    E("    cmp  rax, 0");
    E("    jge  .ft_ul_clamp1");
    E("    xor  eax, eax");
    E(".ft_ul_clamp1:");
    E("    mov  r8, [rbx + WSTATE_WIDTH]");
    E("    dec  r8");
    E("    cmp  rcx, r8");
    E("    jle  .ft_ul_clamp2");
    E("    mov  rcx, r8");
    E(".ft_ul_clamp2:");
    E("    cmp  rax, rcx");
    E("    jg   .ft_upper_next");
    E("");
    E("    ; draw span");
    E("    mov  r8, [rbp-128]");
    E("    imul r8, [rbx + WSTATE_WIDTH]");
    E("    add  r8, rax");
    E("    shl  r8, 2");
    E("    add  r8, [rbx + WSTATE_PIXELS]");
    E("    sub  rcx, rax");
    E("    inc  rcx");
    E(".ft_upper_simd16:");
    E("    cmp  rcx, 16");
    E("    jl   .ft_upper_simd4");
    E("    movdqu [r8], xmm1");
    E("    movdqu [r8+16], xmm1");
    E("    movdqu [r8+32], xmm1");
    E("    movdqu [r8+48], xmm1");
    E("    add  r8, 64");
    E("    sub  rcx, 16");
    E("    jmp  .ft_upper_simd16");
    E(".ft_upper_simd4:");
    E("    cmp  rcx, 4");
    E("    jl   .ft_upper_scalar");
    E("    movdqu [r8], xmm1");
    E("    add  r8, 16");
    E("    sub  rcx, 4");
    E("    jmp  .ft_upper_simd4");
    E(".ft_upper_scalar:");
    E("    cmp  rcx, 0");
    E("    jle  .ft_upper_next");
    E("    mov  eax, [rbp-168]");
    E("    mov  dword [r8], eax");
    E("    add  r8, 4");
    E("    dec  rcx");
    E("    jmp  .ft_upper_scalar");
    E("");
    E(".ft_upper_next:");
    E("    add  r12, [rbp-152]");
    E("    mov  rax, [rbp-144]");
    E("    add  rax, [rbp-160]");
    E("    mov  [rbp-144], rax");
    E("    mov  rax, [rbp-128]");
    E("    inc  rax");
    E("    mov  [rbp-128], rax");
    E("    jmp  .ft_upper_loop");
    E("");
    E(".ft_lower_half:");
    E("    ; === LOWER HALF: yb to yc ===");
    E("    cmp  r13, r14");
    E("    jge  .ft_done");
    E("");
    E("    ; compute dx_lower = ((xc-xb) << 16) / (yc-yb)");
    E("    mov  rax, [rbp-120]");
    E("    sub  rax, [rbp-112]");
    E("    shl  rax, 16");
    E("    mov  rcx, r14");
    E("    sub  rcx, r13");
    E("    cmp  rcx, 0");
    E("    je   .ft_done");
    E("    cqo");
    E("    idiv rcx");
    E("    mov  [rbp-160], rax");
    E("");
    E("    ; x_short = xb << 16");
    E("    mov  rax, [rbp-112]");
    E("    shl  rax, 16");
    E("    mov  [rbp-144], rax");
    E("");
    E("    mov  rax, r13");
    E("    mov  [rbp-128], rax");
    E("");
    E(".ft_lower_loop:");
    E("    mov  rax, [rbp-128]");
    E("    cmp  rax, r14");
    E("    jg   .ft_done");
    E("    cmp  rax, r15");
    E("    jg   .ft_done");
    E("    cmp  rax, 0");
    E("    jl   .ft_lower_next");
    E("");
    E("    mov  rax, r12");
    E("    sar  rax, 16");
    E("    mov  rcx, [rbp-144]");
    E("    sar  rcx, 16");
    E("    cmp  rax, rcx");
    E("    jle  .ft_lower_ordered");
    E("    xchg rax, rcx");
    E(".ft_lower_ordered:");
    E("    cmp  rax, 0");
    E("    jge  .ft_ll_clamp1");
    E("    xor  eax, eax");
    E(".ft_ll_clamp1:");
    E("    mov  r8, [rbx + WSTATE_WIDTH]");
    E("    dec  r8");
    E("    cmp  rcx, r8");
    E("    jle  .ft_ll_clamp2");
    E("    mov  rcx, r8");
    E(".ft_ll_clamp2:");
    E("    cmp  rax, rcx");
    E("    jg   .ft_lower_next");
    E("");
    E("    mov  r8, [rbp-128]");
    E("    imul r8, [rbx + WSTATE_WIDTH]");
    E("    add  r8, rax");
    E("    shl  r8, 2");
    E("    add  r8, [rbx + WSTATE_PIXELS]");
    E("    sub  rcx, rax");
    E("    inc  rcx");
    E(".ft_lower_simd16:");
    E("    cmp  rcx, 16");
    E("    jl   .ft_lower_simd4");
    E("    movdqu [r8], xmm1");
    E("    movdqu [r8+16], xmm1");
    E("    movdqu [r8+32], xmm1");
    E("    movdqu [r8+48], xmm1");
    E("    add  r8, 64");
    E("    sub  rcx, 16");
    E("    jmp  .ft_lower_simd16");
    E(".ft_lower_simd4:");
    E("    cmp  rcx, 4");
    E("    jl   .ft_lower_scalar");
    E("    movdqu [r8], xmm1");
    E("    add  r8, 16");
    E("    sub  rcx, 4");
    E("    jmp  .ft_lower_simd4");
    E(".ft_lower_scalar:");
    E("    cmp  rcx, 0");
    E("    jle  .ft_lower_next");
    E("    mov  eax, [rbp-168]");
    E("    mov  dword [r8], eax");
    E("    add  r8, 4");
    E("    dec  rcx");
    E("    jmp  .ft_lower_scalar");
    E("");
    E(".ft_lower_next:");
    E("    add  r12, [rbp-152]");
    E("    mov  rax, [rbp-144]");
    E("    add  rax, [rbp-160]");
    E("    mov  [rbp-144], rax");
    E("    mov  rax, [rbp-128]");
    E("    inc  rax");
    E("    mov  [rbp-128], rax");
    E("    jmp  .ft_lower_loop");
    E("");
    E(".ft_done:");
    E("    add  rsp, 200");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");

    // -------------------------------------------------------------
    // _slag_zbuffer_clear() : fill the depth buffer with a large
    // "far" depth value so any real fragment passes the first test.
    // No-ops safely if the window/buffer hasn't been created.
    // TLS-based: gets struct ptr from TLS
    // -------------------------------------------------------------
    E("; --- _slag_zbuffer_clear() ---");
    E("_slag_zbuffer_clear:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 40");
    E("");
    E("    ; get struct ptr from TLS");
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    mov  rbx, rax");
    E("");
    E("    mov  r10, [rbx + WSTATE_ZBUFFER]");
    E("    test r10, r10");
    E("    jz   .zbc_done             ; no buffer yet -> skip safely");
    E("    mov  rcx, [rbx + WSTATE_WIDTH]");
    E("    imul rcx, [rbx + WSTATE_HEIGHT] ; rcx = pixel count");
    E("    test rcx, rcx");
    E("    jle  .zbc_done");
    E("    mov  rax, 0x44A0000000000000 ; ~1.0e22 as double bits (far depth)");
    E("    xor  r8, r8                ; index = 0");
    E(".zbc_loop:");
    E("    cmp  r8, rcx");
    E("    jge  .zbc_done");
    E("    mov  [r10 + r8*8], rax");
    E("    inc  r8");
    E("    jmp  .zbc_loop");
    E(".zbc_done:");
    E("    add  rsp, 40");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");

    // -------------------------------------------------------------
    // _slag_fill_triangle_z(x0,y0,z0,x1,y1,z1,x2,y2,z2,r,g,b)
    // Flat-shaded scanline triangle rasterizer with per-pixel depth
    // testing against _zbuffer_ptr (one double per pixel, "far" wins).
    //
    // Win64 positional arg/reg assignment (ints -> rcx/rdx/r8/r9 by
    // position, floats -> xmm0-3 by position; same position can't be
    // used by both register files, so a float arg "steals" its slot's
    // integer register, e.g. arg2 (z0, float) lives in xmm2, not r8):
    //   arg0 x0 (int)   -> rcx
    //   arg1 y0 (int)   -> rdx
    //   arg2 z0 (float) -> xmm2
    //   arg3 x1 (int)   -> r9
    //   arg4 y1 (int)   -> [rbp+48]
    //   arg5 z1 (float) -> [rbp+56]
    //   arg6 x2 (int)   -> [rbp+64]
    //   arg7 y2 (int)   -> [rbp+72]
    //   arg8 z2 (float) -> [rbp+80]
    //   arg9 r  (int)   -> [rbp+88]
    //   arg10 g (int)   -> [rbp+96]
    //   arg11 b (int)   -> [rbp+104]
    //
    // Local frame layout ([rbp-N], all qwords unless noted "f" = double):
    //   -8  x0   -16 y0   -24 z0(f)  -32 x1   -40 y1   -48 z1(f)
    //   -56 x2   -64 y2   -72 z2(f)  -80 r    -88 g    -96 b
    //  -104 ya  -112 yb  -120 yc        (sorted y, ascending)
    //  -128 xa  -136 xb  -144 xc        (x at sorted verts)
    //  -152 za(f) -160 zb(f) -168 zc(f) (z at sorted verts)
    //  -176 y                            (current scanline)
    //  -184 xleft   -192 xright
    //  -200 zleft(f) -208 zright(f)      (interpolated z at span ends)
    //  -216 zLong(f) -224 zShort(f)      (scratch, before left/right sort)
    // -------------------------------------------------------------
    E("; --- _slag_fill_triangle_z(x0,y0,z0,x1,y1,z1,x2,y2,z2,r,g,b) ---");
    E("_slag_fill_triangle_z:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 280");
    E("    mov  [rbp-240], r12       ; save callee-saved regs (ABI)");
    E("    mov  [rbp-248], r13");
    E("    mov  [rbp-256], r14");
    E("    mov  [rbp-264], r15");
    E("    mov  [rbp-8],  rcx        ; x0");
    E("    mov  [rbp-16], rdx        ; y0");
    E("    movsd [rbp-24], xmm2      ; z0");
    E("    mov  [rbp-32], r9         ; x1");
    E("    mov  rax, [rbp+48]");
    E("    mov  [rbp-40], rax        ; y1");
    E("    movsd xmm0, [rbp+56]");
    E("    movsd [rbp-48], xmm0      ; z1");
    E("    mov  rax, [rbp+64]");
    E("    mov  [rbp-56], rax        ; x2");
    E("    mov  rax, [rbp+72]");
    E("    mov  [rbp-64], rax        ; y2");
    E("    movsd xmm0, [rbp+80]");
    E("    movsd [rbp-72], xmm0      ; z2");
    E("    mov  rax, [rbp+88]");
    E("    mov  [rbp-80], rax        ; r");
    E("    mov  rax, [rbp+96]");
    E("    mov  [rbp-88], rax        ; g");
    E("    mov  rax, [rbp+104]");
    E("    mov  [rbp-96], rax        ; b");
    E("");
    E("    ; get struct ptr from TLS");
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    mov  rbx, rax              ; rbx = struct ptr for entire function");
    E("");
    E("    ; bail out safely if there is no depth buffer yet");
    E("    mov  r10, [rbx + WSTATE_ZBUFFER]");
    E("    test r10, r10");
    E("    jz   .ftz_done");
    E("");
    E("    ; --- load (x,y,z) into a,b,c then sort by y ascending ---");
    E("    mov  rax, [rbp-8]");
    E("    mov  [rbp-128], rax       ; xa = x0");
    E("    mov  rax, [rbp-16]");
    E("    mov  [rbp-104], rax       ; ya = y0");
    E("    movsd xmm0, [rbp-24]");
    E("    movsd [rbp-152], xmm0     ; za = z0");
    E("    mov  rax, [rbp-32]");
    E("    mov  [rbp-136], rax       ; xb = x1");
    E("    mov  rax, [rbp-40]");
    E("    mov  [rbp-112], rax       ; yb = y1");
    E("    movsd xmm0, [rbp-48]");
    E("    movsd [rbp-160], xmm0     ; zb = z1");
    E("    mov  rax, [rbp-56]");
    E("    mov  [rbp-144], rax       ; xc = x2");
    E("    mov  rax, [rbp-64]");
    E("    mov  [rbp-120], rax       ; yc = y2");
    E("    movsd xmm0, [rbp-72]");
    E("    movsd [rbp-168], xmm0     ; zc = z2");
    E("");
    E("    ; if ya > yb: swap (a,b) including z");
    E("    mov  rax, [rbp-104]");
    E("    cmp  rax, [rbp-112]");
    E("    jle  .ftz_sort_ab_done");
    E("    mov  rax, [rbp-104]");
    E("    mov  rcx, [rbp-112]");
    E("    mov  [rbp-104], rcx");
    E("    mov  [rbp-112], rax");
    E("    mov  rax, [rbp-128]");
    E("    mov  rcx, [rbp-136]");
    E("    mov  [rbp-128], rcx");
    E("    mov  [rbp-136], rax");
    E("    movsd xmm0, [rbp-152]");
    E("    movsd xmm1, [rbp-160]");
    E("    movsd [rbp-152], xmm1");
    E("    movsd [rbp-160], xmm0");
    E(".ftz_sort_ab_done:");
    E("    ; if yb > yc: swap (b,c) including z");
    E("    mov  rax, [rbp-112]");
    E("    cmp  rax, [rbp-120]");
    E("    jle  .ftz_sort_bc_done");
    E("    mov  rax, [rbp-112]");
    E("    mov  rcx, [rbp-120]");
    E("    mov  [rbp-112], rcx");
    E("    mov  [rbp-120], rax");
    E("    mov  rax, [rbp-136]");
    E("    mov  rcx, [rbp-144]");
    E("    mov  [rbp-136], rcx");
    E("    mov  [rbp-144], rax");
    E("    movsd xmm0, [rbp-160]");
    E("    movsd xmm1, [rbp-168]");
    E("    movsd [rbp-160], xmm1");
    E("    movsd [rbp-168], xmm0");
    E(".ftz_sort_bc_done:");
    E("    ; if ya > yb (again): swap (a,b) including z");
    E("    mov  rax, [rbp-104]");
    E("    cmp  rax, [rbp-112]");
    E("    jle  .ftz_sort_ab2_done");
    E("    mov  rax, [rbp-104]");
    E("    mov  rcx, [rbp-112]");
    E("    mov  [rbp-104], rcx");
    E("    mov  [rbp-112], rax");
    E("    mov  rax, [rbp-128]");
    E("    mov  rcx, [rbp-136]");
    E("    mov  [rbp-128], rcx");
    E("    mov  [rbp-136], rax");
    E("    movsd xmm0, [rbp-152]");
    E("    movsd xmm1, [rbp-160]");
    E("    movsd [rbp-152], xmm1");
    E("    movsd [rbp-160], xmm0");
    E(".ftz_sort_ab2_done:");
    E("");
    E("    ; clamp scanline range to framebuffer height");
    E("    mov  rax, [rbp-104]       ; ya");
    E("    cmp  rax, 0");
    E("    jge  .ftz_ystart_ok");
    E("    mov  rax, 0");
    E(".ftz_ystart_ok:");
    E("    mov  [rbp-176], rax       ; y = max(ya, 0)");
    E("");
    E("    mov  r12, [rbp-120]       ; yc");
    E("    mov  rax, [rbx + WSTATE_HEIGHT]");
    E("    dec  rax");
    E("    cmp  r12, rax");
    E("    jle  .ftz_yend_ok");
    E("    mov  r12, rax");
    E(".ftz_yend_ok:");
    E("");
    E(".ftz_scanline_loop:");
    E("    mov  rax, [rbp-176]");
    E("    cmp  rax, r12");
    E("    jg   .ftz_done");
    E("");
    E("    ; --- xLong/zLong: edge (xa,ya,za)-(xc,yc,zc) at current y ---");
    E("    mov  rax, [rbp-120]");
    E("    sub  rax, [rbp-104]       ; denom = yc - ya");
    E("    cmp  rax, 0");
    E("    je   .ftz_long_eq");
    E("    cvtsi2sd xmm3, rax        ; xmm3 = denom (f)");
    E("    mov  rcx, [rbp-144]");
    E("    sub  rcx, [rbp-128]       ; xc - xa (int)");
    E("    mov  r10, [rbp-176]");
    E("    sub  r10, [rbp-104]       ; y - ya (int)");
    E("    imul rcx, r10");
    E("    mov  r11, rax             ; save int denom for idiv");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r11");
    E("    add  rax, [rbp-128]       ; xLong = xa + result");
    E("    ; t = (y-ya)/(yc-ya) as double, for z interpolation");
    E("    mov  r10, [rbp-176]");
    E("    sub  r10, [rbp-104]");
    E("    cvtsi2sd xmm0, r10        ; (y-ya)");
    E("    divsd xmm0, xmm3          ; t");
    E("    movsd xmm1, [rbp-168]     ; zc");
    E("    movsd xmm2, [rbp-152]     ; za");
    E("    subsd xmm1, xmm2          ; zc - za");
    E("    mulsd xmm1, xmm0          ; (zc-za)*t");
    E("    addsd xmm1, xmm2          ; za + (zc-za)*t");
    E("    movsd [rbp-216], xmm1     ; zLong");
    E("    jmp  .ftz_long_done");
    E(".ftz_long_eq:");
    E("    mov  rax, [rbp-128]       ; degenerate: xLong = xa");
    E("    movsd xmm1, [rbp-152]");
    E("    movsd [rbp-216], xmm1     ; zLong = za");
    E(".ftz_long_done:");
    E("    mov  [rbp-184], rax       ; stash xLong in xleft slot");
    E("");
    E("    ; --- xShort/zShort: upper edge (a-b) or lower edge (b-c) ---");
    E("    mov  rax, [rbp-176]");
    E("    cmp  rax, [rbp-112]");
    E("    jl   .ftz_short_upper");
    E("");
    E("    ; lower segment: edge (xb,yb,zb)-(xc,yc,zc)");
    E("    mov  rax, [rbp-120]");
    E("    sub  rax, [rbp-112]       ; denom = yc - yb");
    E("    cmp  rax, 0");
    E("    je   .ftz_short_lower_eq");
    E("    cvtsi2sd xmm3, rax");
    E("    mov  rcx, [rbp-144]");
    E("    sub  rcx, [rbp-136]       ; xc - xb");
    E("    mov  r10, [rbp-176]");
    E("    sub  r10, [rbp-112]       ; y - yb");
    E("    imul rcx, r10");
    E("    mov  r11, rax");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r11");
    E("    add  rax, [rbp-136]       ; xShort = xb + result");
    E("    mov  r10, [rbp-176]");
    E("    sub  r10, [rbp-112]");
    E("    cvtsi2sd xmm0, r10");
    E("    divsd xmm0, xmm3          ; t");
    E("    movsd xmm1, [rbp-168]     ; zc");
    E("    movsd xmm2, [rbp-160]     ; zb");
    E("    subsd xmm1, xmm2");
    E("    mulsd xmm1, xmm0");
    E("    addsd xmm1, xmm2          ; zb + (zc-zb)*t");
    E("    movsd [rbp-224], xmm1     ; zShort");
    E("    jmp  .ftz_short_done");
    E(".ftz_short_lower_eq:");
    E("    mov  rax, [rbp-136]");
    E("    movsd xmm1, [rbp-160]");
    E("    movsd [rbp-224], xmm1");
    E("    jmp  .ftz_short_done");
    E("");
    E(".ftz_short_upper:");
    E("    ; upper segment: edge (xa,ya,za)-(xb,yb,zb)");
    E("    mov  rax, [rbp-112]");
    E("    sub  rax, [rbp-104]       ; denom = yb - ya");
    E("    cmp  rax, 0");
    E("    je   .ftz_short_upper_eq");
    E("    cvtsi2sd xmm3, rax");
    E("    mov  rcx, [rbp-136]");
    E("    sub  rcx, [rbp-128]       ; xb - xa");
    E("    mov  r10, [rbp-176]");
    E("    sub  r10, [rbp-104]       ; y - ya");
    E("    imul rcx, r10");
    E("    mov  r11, rax");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r11");
    E("    add  rax, [rbp-128]       ; xShort = xa + result");
    E("    mov  r10, [rbp-176]");
    E("    sub  r10, [rbp-104]");
    E("    cvtsi2sd xmm0, r10");
    E("    divsd xmm0, xmm3          ; t");
    E("    movsd xmm1, [rbp-160]     ; zb");
    E("    movsd xmm2, [rbp-152]     ; za");
    E("    subsd xmm1, xmm2");
    E("    mulsd xmm1, xmm0");
    E("    addsd xmm1, xmm2          ; za + (zb-za)*t");
    E("    movsd [rbp-224], xmm1     ; zShort");
    E("    jmp  .ftz_short_done");
    E(".ftz_short_upper_eq:");
    E("    mov  rax, [rbp-128]");
    E("    movsd xmm1, [rbp-152]");
    E("    movsd [rbp-224], xmm1");
    E(".ftz_short_done:");
    E("    mov  [rbp-192], rax       ; xShort");
    E("");
    E("    ; xleft = min(xLong,xShort) with matching z; xright = max with z");
    E("    mov  rax, [rbp-184]       ; xLong");
    E("    mov  rcx, [rbp-192]       ; xShort");
    E("    movsd xmm0, [rbp-216]     ; zLong");
    E("    movsd xmm1, [rbp-224]     ; zShort");
    E("    cmp  rax, rcx");
    E("    jle  .ftz_minmax_ok");
    E("    ; swap so rax=min x (->left), rcx=max x (->right); swap z to match");
    E("    mov  r10, rax");
    E("    mov  rax, rcx");
    E("    mov  rcx, r10");
    E("    movsd xmm2, xmm0");
    E("    movsd xmm0, xmm1");
    E("    movsd xmm1, xmm2");
    E(".ftz_minmax_ok:");
    E("    mov  [rbp-184], rax       ; xleft");
    E("    mov  [rbp-192], rcx       ; xright");
    E("    movsd [rbp-200], xmm0     ; zleft");
    E("    movsd [rbp-208], xmm1     ; zright");
    E("");
    E("    ; clamp x range to [0, width-1]");
    E("    mov  rax, [rbp-184]");
    E("    cmp  rax, 0");
    E("    jge  .ftz_xleft_ok");
    E("    mov  rax, 0");
    E("    mov  [rbp-184], rax");
    E(".ftz_xleft_ok:");
    E("    mov  rax, [rbp-192]");
    E("    mov  rcx, [rbx + WSTATE_WIDTH]");
    E("    dec  rcx");
    E("    cmp  rax, rcx");
    E("    jle  .ftz_xright_ok");
    E("    mov  [rbp-192], rcx");
    E(".ftz_xright_ok:");
    E("");
    E("    ; fill span [xleft,xright] on row y, interpolating z per pixel");
    E("    ; and depth-testing against zbuffer");
    E("    mov  rax, [rbp-176]       ; y");
    E("    imul rax, [rbx + WSTATE_WIDTH]");
    E("    mov  r13, rax             ; r13 = y*width (pixel row base)");
    E("");
    E("    ; span_dx = xright - xleft (int, used as the z-lerp denominator)");
    E("    mov  rax, [rbp-192]");
    E("    sub  rax, [rbp-184]");
    E("    mov  r14, rax             ; r14 = span_dx");
    E("");
    E("    mov  rax, [rbp-184]       ; x = xleft");
    E("");
    E(".ftz_span_loop:");
    E("    mov  rcx, [rbp-192]       ; xright");
    E("    cmp  rax, rcx");
    E("    jg   .ftz_span_done");
    E("");
    E("    ; interpolate z at this x: t = (x-xleft)/span_dx (0 if span_dx==0)");
    E("    mov  rcx, rax");
    E("    sub  rcx, [rbp-184]       ; x - xleft");
    E("    movsd xmm0, [rbp-200]     ; zleft");
    E("    test r14, r14");
    E("    jz   .ftz_pix_z_done      ; degenerate span: z = zleft");
    E("    cvtsi2sd xmm1, rcx");
    E("    cvtsi2sd xmm2, r14");
    E("    divsd xmm1, xmm2          ; t");
    E("    movsd xmm3, [rbp-208]     ; zright");
    E("    subsd xmm3, xmm0          ; zright - zleft");
    E("    mulsd xmm3, xmm1");
    E("    addsd xmm0, xmm3          ; z = zleft + (zright-zleft)*t");
    E(".ftz_pix_z_done:");
    E("    ; xmm0 = interpolated z for this pixel");
    E("");
    E("    ; depth test: pixel_index = r13 + x; compare against zbuffer");
    E("    mov  r10, r13");
    E("    add  r10, rax             ; pixel index = y*width + x");
    E("    mov  r15, [rbx + WSTATE_ZBUFFER]");
    E("    movsd xmm1, [r15 + r10*8]");
    E("    comisd xmm0, xmm1");
    E("    jae  .ftz_pix_skip        ; new z >= stored z -> farther/equal, skip");
    E("");
    E("    ; passes depth test: write z, then write pixel color");
    E("    movsd [r15 + r10*8], xmm0");
    E("    shl  r10, 2");
    E("    add  r10, [rbx + WSTATE_PIXELS]");
    E("    mov  rcx, [rbp-96]        ; b");
    E("    mov  byte [r10+0], cl");
    E("    mov  rcx, [rbp-88]        ; g");
    E("    mov  byte [r10+1], cl");
    E("    mov  rcx, [rbp-80]        ; r");
    E("    mov  byte [r10+2], cl");
    E("    mov  byte [r10+3], 0xFF");
    E(".ftz_pix_skip:");
    E("");
    E("    inc  rax");
    E("    jmp  .ftz_span_loop");
    E(".ftz_span_done:");
    E("");
    E("    mov  rax, [rbp-176]");
    E("    inc  rax");
    E("    mov  [rbp-176], rax");
    E("    jmp  .ftz_scanline_loop");
    E("");
    E(".ftz_done:");
    E("    mov  r12, [rbp-240]       ; restore callee-saved regs");
    E("    mov  r13, [rbp-248]");
    E("    mov  r14, [rbp-256]");
    E("    mov  r15, [rbp-264]");
    E("    add  rsp, 280");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");

    // -------------------------------------------------------------


    // -------------------------------------------------------------
    // _slag_fill_triangle_gradient(x0,y0,r0,g0,b0,x1,y1,r1,g1,b1,x2,y2,r2,g2,b2)
    // Per-vertex color scanline rasterizer with linear interpolation
    // along edges and across spans.
    //
    // Args: rcx=x0 rdx=y0 r8=r0 r9=g0
    //       [rbp+48]=b0  [rbp+56]=x1  [rbp+64]=y1  [rbp+72]=r1
    //       [rbp+80]=g1  [rbp+88]=b1  [rbp+96]=x2  [rbp+104]=y2
    //       [rbp+112]=r2 [rbp+120]=g2 [rbp+128]=b2
    //
    // Local layout (qwords, [rbp-N]):
    //   sorted vertices A (lowest y), B (middle y), C (highest y),
    //   each as 5 fields: x,y,r,g,b
    //   A: -8 x  -16 y  -24 r  -32 g  -40 b
    //   B: -48 x -56 y  -64 r  -72 g  -80 b
    //   C: -88 x -96 y -104 r -112 g -120 b
    //   -128 y (current scanline)
    //   -136 xleft  -144 xright
    //   -152 rleft -160 gleft -168 bleft
    //   -176 rright -184 gright -192 bright
    // TLS-based: gets struct ptr from TLS
    // -------------------------------------------------------------
    E("; --- _slag_fill_triangle_gradient(...) ---");
    E("_slag_fill_triangle_gradient:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 264");
    E("");
    E("    ; save first 4 args before TLS call");
    E("    mov  [rbp-8],  rcx   ; A.x = x0");
    E("    mov  [rbp-16], rdx   ; A.y = y0");
    E("    mov  [rbp-24], r8    ; A.r = r0");
    E("    mov  [rbp-32], r9    ; A.g = g0");
    E("");
    E("    ; get struct ptr from TLS");
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    mov  rbx, rax              ; rbx = struct ptr for entire function");
    E("");
    E("    ; load all 3 vertices into A/B/C slots (unsorted initially)");
    E("    ; (first 4 args already saved above)");
    E("    mov  rax, [rbp+48]");
    E("    mov  [rbp-40], rax   ; A.b = b0");
    E("    mov  rax, [rbp+56]");
    E("    mov  [rbp-48], rax   ; B.x = x1");
    E("    mov  rax, [rbp+64]");
    E("    mov  [rbp-56], rax   ; B.y = y1");
    E("    mov  rax, [rbp+72]");
    E("    mov  [rbp-64], rax   ; B.r = r1");
    E("    mov  rax, [rbp+80]");
    E("    mov  [rbp-72], rax   ; B.g = g1");
    E("    mov  rax, [rbp+88]");
    E("    mov  [rbp-80], rax   ; B.b = b1");
    E("    mov  rax, [rbp+96]");
    E("    mov  [rbp-88], rax   ; C.x = x2");
    E("    mov  rax, [rbp+104]");
    E("    mov  [rbp-96], rax   ; C.y = y2");
    E("    mov  rax, [rbp+112]");
    E("    mov  [rbp-104], rax  ; C.r = r2");
    E("    mov  rax, [rbp+120]");
    E("    mov  [rbp-112], rax  ; C.g = g2");
    E("    mov  rax, [rbp+128]");
    E("    mov  [rbp-120], rax  ; C.b = b2");
    E("");
    E("    ; sort A,B,C by y ascending (compare-and-swap network, swap all 5 fields)");
    E("    ; if A.y > B.y: swap A,B");
    E("    mov  rax, [rbp-16]");
    E("    cmp  rax, [rbp-56]");
    E("    jle  .ftg_sort_ab_done");
    for (int i = 0; i < 5; i++) {
        E("    mov  rax, [rbp-%d]", 8 + i*8);
        E("    mov  rcx, [rbp-%d]", 48 + i*8);
        E("    mov  [rbp-%d], rcx", 8 + i*8);
        E("    mov  [rbp-%d], rax", 48 + i*8);
    }
    E(".ftg_sort_ab_done:");
    E("    ; if B.y > C.y: swap B,C");
    E("    mov  rax, [rbp-56]");
    E("    cmp  rax, [rbp-96]");
    E("    jle  .ftg_sort_bc_done");
    for (int i = 0; i < 5; i++) {
        E("    mov  rax, [rbp-%d]", 48 + i*8);
        E("    mov  rcx, [rbp-%d]", 88 + i*8);
        E("    mov  [rbp-%d], rcx", 48 + i*8);
        E("    mov  [rbp-%d], rax", 88 + i*8);
    }
    E(".ftg_sort_bc_done:");
    E("    ; if A.y > B.y (again): swap A,B");
    E("    mov  rax, [rbp-16]");
    E("    cmp  rax, [rbp-56]");
    E("    jle  .ftg_sort_ab2_done");
    for (int i = 0; i < 5; i++) {
        E("    mov  rax, [rbp-%d]", 8 + i*8);
        E("    mov  rcx, [rbp-%d]", 48 + i*8);
        E("    mov  [rbp-%d], rcx", 8 + i*8);
        E("    mov  [rbp-%d], rax", 48 + i*8);
    }
    E(".ftg_sort_ab2_done:");
    E("");
    E("    ; clamp scanline range to framebuffer height");
    E("    mov  rax, [rbp-16]   ; A.y");
    E("    cmp  rax, 0");
    E("    jge  .ftg_ystart_ok");
    E("    mov  rax, 0");
    E(".ftg_ystart_ok:");
    E("    mov  [rbp-128], rax");
    E("");
    E("    mov  r12, [rbp-96]   ; C.y");
    E("    mov  rax, [rbx + WSTATE_HEIGHT]");
    E("    dec  rax");
    E("    cmp  r12, rax");
    E("    jle  .ftg_yend_ok");
    E("    mov  r12, rax");
    E(".ftg_yend_ok:");
    E("");
    E(".ftg_scanline_loop:");
    E("    mov  rax, [rbp-128]");
    E("    cmp  rax, r12");
    E("    jg   .ftg_done");
    E("");
    E("    ; --- long edge A-C: interpolate x,r,g,b at current y ---");
    E("    mov  rax, [rbp-96]");
    E("    sub  rax, [rbp-16]   ; denom = C.y - A.y");
    E("    mov  r14, rax        ; r14 = denomAC (0 if degenerate)");
    E("    mov  r10, [rbp-128]");
    E("    sub  r10, [rbp-16]   ; r10 = y - A.y");
    E("");
    E("    ; xLong");
    E("    cmp  r14, 0");
    E("    je   .ftg_long_x_eq");
    E("    mov  rcx, [rbp-88]");
    E("    sub  rcx, [rbp-8]    ; C.x - A.x");
    E("    imul rcx, r10");
    E("    mov  r11, r14");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r11");
    E("    add  rax, [rbp-8]");
    E("    jmp  .ftg_long_x_done");
    E(".ftg_long_x_eq:");
    E("    mov  rax, [rbp-8]");
    E(".ftg_long_x_done:");
    E("    mov  [rbp-136], rax  ; xLong");
    E("");
    E("    ; rLong, gLong, bLong (fields at offset 16,24,32 from x, i.e. A.r/g/b at -24/-32/-40, C.r/g/b at -104/-112/-120)");
    for (int f = 0; f < 3; f++) {
        int a_off = 24 + f*8;   // A.r=-24, A.g=-32, A.b=-40
        int c_off = 104 + f*8;  // C.r=-104, C.g=-112, C.b=-120
        int dst_off = 152 + f*8; // rleft/gleft/bleft at -152/-160/-168
        E("    cmp  r14, 0");
        E("    je   .ftg_long_c%d_eq", f);
        E("    mov  rcx, [rbp-%d]", c_off);
        E("    sub  rcx, [rbp-%d]", a_off);
        E("    imul rcx, r10");
        E("    mov  r11, r14");
        E("    mov  rax, rcx");
        E("    cqo");
        E("    idiv r11");
        E("    add  rax, [rbp-%d]", a_off);
        E("    jmp  .ftg_long_c%d_done", f);
        E(".ftg_long_c%d_eq:", f);
        E("    mov  rax, [rbp-%d]", a_off);
        E(".ftg_long_c%d_done:", f);
        E("    mov  [rbp-%d], rax  ; long edge channel %d (left)", dst_off, f);
    }
    E("");
    E("    ; --- short edge: A-B for y<B.y, else B-C ---");
    E("    mov  rax, [rbp-128]");
    E("    cmp  rax, [rbp-56]   ; y < B.y ?");
    E("    jl   .ftg_short_ab");
    E("");
    E("    ; B-C segment");
    E("    mov  rax, [rbp-96]");
    E("    sub  rax, [rbp-56]   ; denom = C.y - B.y");
    E("    mov  r14, rax");
    E("    mov  r10, [rbp-128]");
    E("    sub  r10, [rbp-56]   ; y - B.y");
    E("    cmp  r14, 0");
    E("    je   .ftg_short_x_bc_eq");
    E("    mov  rcx, [rbp-88]");
    E("    sub  rcx, [rbp-48]   ; C.x - B.x");
    E("    imul rcx, r10");
    E("    mov  r11, r14");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r11");
    E("    add  rax, [rbp-48]");
    E("    jmp  .ftg_short_x_bc_done");
    E(".ftg_short_x_bc_eq:");
    E("    mov  rax, [rbp-48]");
    E(".ftg_short_x_bc_done:");
    E("    mov  [rbp-144], rax  ; xShort");
    for (int f = 0; f < 3; f++) {
        int b_off = 64 + f*8;   // B.r=-64, B.g=-72, B.b=-80
        int c_off = 104 + f*8;  // C.r=-104, C.g=-112, C.b=-120
        int dst_off = 176 + f*8; // rright/gright/bright at -176/-184/-192
        E("    cmp  r14, 0");
        E("    je   .ftg_short_bc_c%d_eq", f);
        E("    mov  rcx, [rbp-%d]", c_off);
        E("    sub  rcx, [rbp-%d]", b_off);
        E("    imul rcx, r10");
        E("    mov  r11, r14");
        E("    mov  rax, rcx");
        E("    cqo");
        E("    idiv r11");
        E("    add  rax, [rbp-%d]", b_off);
        E("    jmp  .ftg_short_bc_c%d_done", f);
        E(".ftg_short_bc_c%d_eq:", f);
        E("    mov  rax, [rbp-%d]", b_off);
        E(".ftg_short_bc_c%d_done:", f);
        E("    mov  [rbp-%d], rax  ; short edge channel %d (right) BC", dst_off, f);
    }
    E("    jmp  .ftg_short_done");
    E("");
    E(".ftg_short_ab:");
    E("    ; A-B segment");
    E("    mov  rax, [rbp-56]");
    E("    sub  rax, [rbp-16]   ; denom = B.y - A.y");
    E("    mov  r14, rax");
    E("    mov  r10, [rbp-128]");
    E("    sub  r10, [rbp-16]   ; y - A.y");
    E("    cmp  r14, 0");
    E("    je   .ftg_short_x_ab_eq");
    E("    mov  rcx, [rbp-48]");
    E("    sub  rcx, [rbp-8]    ; B.x - A.x");
    E("    imul rcx, r10");
    E("    mov  r11, r14");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r11");
    E("    add  rax, [rbp-8]");
    E("    jmp  .ftg_short_x_ab_done");
    E(".ftg_short_x_ab_eq:");
    E("    mov  rax, [rbp-8]");
    E(".ftg_short_x_ab_done:");
    E("    mov  [rbp-144], rax  ; xShort");
    for (int f = 0; f < 3; f++) {
        int a_off = 24 + f*8;   // A.r/g/b
        int b_off = 64 + f*8;   // B.r/g/b
        int dst_off = 176 + f*8; // rright/gright/bright
        E("    cmp  r14, 0");
        E("    je   .ftg_short_ab_c%d_eq", f);
        E("    mov  rcx, [rbp-%d]", b_off);
        E("    sub  rcx, [rbp-%d]", a_off);
        E("    imul rcx, r10");
        E("    mov  r11, r14");
        E("    mov  rax, rcx");
        E("    cqo");
        E("    idiv r11");
        E("    add  rax, [rbp-%d]", a_off);
        E("    jmp  .ftg_short_ab_c%d_done", f);
        E(".ftg_short_ab_c%d_eq:", f);
        E("    mov  rax, [rbp-%d]", a_off);
        E(".ftg_short_ab_c%d_done:", f);
        E("    mov  [rbp-%d], rax  ; short edge channel %d (right) AB", dst_off, f);
    }
    E(".ftg_short_done:");
    E("");
    E("    ; --- order xleft/xright (and matching colors) so xleft <= xright ---");
    E("    mov  rax, [rbp-136]  ; xLong");
    E("    mov  rcx, [rbp-144]  ; xShort");
    E("    cmp  rax, rcx");
    E("    jle  .ftg_minmax_ok");
    E("    mov  r10, rax");
    E("    mov  rax, rcx");
    E("    mov  rcx, r10");
    E("    mov  [rbp-136], rax");
    E("    mov  [rbp-144], rcx");
    for (int f = 0; f < 3; f++) {
        int left_off = 152 + f*8;
        int right_off = 176 + f*8;
        E("    mov  rax, [rbp-%d]", left_off);
        E("    mov  rcx, [rbp-%d]", right_off);
        E("    mov  [rbp-%d], rcx", left_off);
        E("    mov  [rbp-%d], rax", right_off);
    }
    E("    jmp  .ftg_xorder_done");
    E(".ftg_minmax_ok:");
    E("    mov  [rbp-136], rax");
    E("    mov  [rbp-144], rcx");
    E(".ftg_xorder_done:");
    E("");
    E("    ; clamp xleft/xright to [0, width-1]");
    E("    mov  rax, [rbp-136]");
    E("    cmp  rax, 0");
    E("    jge  .ftg_xleft_ok");
    E("    mov  rax, 0");
    E("    mov  [rbp-136], rax");
    E(".ftg_xleft_ok:");
    E("    mov  rax, [rbp-144]");
    E("    mov  rcx, [rbx + WSTATE_WIDTH]");
    E("    dec  rcx");
    E("    cmp  rax, rcx");
    E("    jle  .ftg_xright_ok");
    E("    mov  [rbp-144], rcx");
    E(".ftg_xright_ok:");
    E("");
    E("    ; --- fill span, interpolating color per pixel ---");
    E("    mov  rax, [rbp-144]");
    E("    sub  rax, [rbp-136]");
    E("    mov  [rbp-200], rax  ; span_w");
    E("");
    E("    mov  rax, [rbp-128]  ; y");
    E("    imul rax, [rbx + WSTATE_WIDTH]");
    E("    mov  [rbp-208], rax  ; y*width");
    E("");
    E("    mov  rax, [rbp-136]  ; x = xleft");
    E("    mov  [rbp-216], rax");
    E("");
    E(".ftg_span_loop:");
    E("    mov  rax, [rbp-216]");
    E("    cmp  rax, [rbp-144]");
    E("    jg   .ftg_span_done");
    E("");
    E("    mov  rax, [rbp-216]");
    E("    sub  rax, [rbp-136]  ; t_num = x - xleft");
    E("    mov  r10, [rbp-200]  ; t_den = span_w");
    E("    cmp  r10, 0");
    E("    jne  .ftg_pix_den_ok");
    E("    mov  r10, 1");
    E(".ftg_pix_den_ok:");
    E("");
    for (int f = 0; f < 3; f++) {
        int left_off = 152 + f*8;
        int right_off = 176 + f*8;
        int dst_off = 224 + f*8;
        E("    mov  rcx, [rbp-%d]", right_off);
        E("    sub  rcx, [rbp-%d]", left_off);
        E("    imul rcx, rax");
        E("    mov  r11, r10");
        E("    push rax");
        E("    push r10");
        E("    mov  rax, rcx");
        E("    cqo");
        E("    idiv r11");
        E("    add  rax, [rbp-%d]", left_off);
        E("    mov  [rbp-%d], rax  ; per-pixel channel %d", dst_off, f);
        E("    pop  r10");
        E("    pop  rax");
    }
    E("");
    E("    mov  r10, [rbp-208]  ; y*width");
    E("    add  r10, [rbp-216]  ; + x");
    E("    shl  r10, 2");
    E("    add  r10, [rbx + WSTATE_PIXELS]");
    E("    ; pack color into single dword: 0xFF | R<<16 | G<<8 | B");
    E("    mov  eax, 0xFF000000");
    E("    mov  ecx, [rbp-224]  ; R");
    E("    shl  ecx, 16");
    E("    or   eax, ecx");
    E("    mov  ecx, [rbp-232]  ; G");
    E("    shl  ecx, 8");
    E("    or   eax, ecx");
    E("    mov  ecx, [rbp-240]  ; B");
    E("    or   eax, ecx");
    E("    mov  dword [r10], eax");
    E("");
    E("    mov  rax, [rbp-216]");
    E("    inc  rax");
    E("    mov  [rbp-216], rax");
    E("    jmp  .ftg_span_loop");
    E(".ftg_span_done:");
    E("");
    E("    mov  rax, [rbp-128]");
    E("    inc  rax");
    E("    mov  [rbp-128], rax");
    E("    jmp  .ftg_scanline_loop");
    E("");
    E(".ftg_done:");
    E("    add  rsp, 264");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_fill_triangle_affine(x0,y0,u0,v0,x1,y1,u1,v1,x2,y2,u2,v2,tex_ptr,tex_w,tex_h)
    // Affine textured triangle rasterizer (PS1-style, no perspective correction).
    // Texture is RGB565 format, 2 bytes per pixel.
    //
    // Win64 arg assignment:
    //   arg0  x0       -> rcx
    //   arg1  y0       -> rdx
    //   arg2  u0       -> r8
    //   arg3  v0       -> r9
    //   arg4  x1       -> [rbp+48]
    //   arg5  y1       -> [rbp+56]
    //   arg6  u1       -> [rbp+64]
    //   arg7  v1       -> [rbp+72]
    //   arg8  x2       -> [rbp+80]
    //   arg9  y2       -> [rbp+88]
    //   arg10 u2       -> [rbp+96]
    //   arg11 v2       -> [rbp+104]
    //   arg12 tex_ptr  -> [rbp+112]
    //   arg13 tex_w    -> [rbp+120]
    //   arg14 tex_h    -> [rbp+128]
    //
    // Local frame ([rbp-N]):
    //   -8  x0   -16 y0   -24 u0   -32 v0
    //   -40 x1   -48 y1   -56 u1   -64 v1
    //   -72 x2   -80 y2   -88 u2   -96 v2
    //  -104 tex_ptr  -112 tex_w  -120 tex_h
    //  -128 ya  -136 yb  -144 yc  (sorted y ascending)
    //  -152 xa  -160 xb  -168 xc
    //  -176 ua  -184 ub  -192 uc
    //  -200 va  -208 vb  -216 vc
    //  -224 y (current scanline)
    //  -232 xleft  -240 xright
    //  -248 uleft  -256 vleft  -264 uright  -272 vright
    //  -280 r12_save  -288 r13_save  -296 r14_save  -304 r15_save
    // -------------------------------------------------------------
    E("; --- _slag_fill_triangle_affine(x0,y0,u0,v0,...,tex_ptr,tex_w,tex_h) ---");
    E("_slag_fill_triangle_affine:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 312");
    E("    mov  [rbp-280], r12");
    E("    mov  [rbp-288], r13");
    E("    mov  [rbp-296], r14");
    E("    mov  [rbp-304], r15");
    E("    ; copy args to local frame");
    E("    mov  [rbp-8],  rcx        ; x0");
    E("    mov  [rbp-16], rdx        ; y0");
    E("    mov  [rbp-24], r8         ; u0");
    E("    mov  [rbp-32], r9         ; v0");
    E("    mov  rax, [rbp+48]");
    E("    mov  [rbp-40], rax        ; x1");
    E("    mov  rax, [rbp+56]");
    E("    mov  [rbp-48], rax        ; y1");
    E("    mov  rax, [rbp+64]");
    E("    mov  [rbp-56], rax        ; u1");
    E("    mov  rax, [rbp+72]");
    E("    mov  [rbp-64], rax        ; v1");
    E("    mov  rax, [rbp+80]");
    E("    mov  [rbp-72], rax        ; x2");
    E("    mov  rax, [rbp+88]");
    E("    mov  [rbp-80], rax        ; y2");
    E("    mov  rax, [rbp+96]");
    E("    mov  [rbp-88], rax        ; u2");
    E("    mov  rax, [rbp+104]");
    E("    mov  [rbp-96], rax        ; v2");
    E("    mov  rax, [rbp+112]");
    E("    mov  [rbp-104], rax       ; tex_ptr");
    E("    mov  rax, [rbp+120]");
    E("    mov  [rbp-112], rax       ; tex_w");
    E("    mov  rax, [rbp+128]");
    E("    mov  [rbp-120], rax       ; tex_h");
    E("");
    E("    ; get TLS window state");
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    mov  rbx, rax             ; rbx = window state ptr");
    E("");
    E("    ; --- copy to a,b,c then sort by y ascending ---");
    E("    mov  rax, [rbp-8]");
    E("    mov  [rbp-152], rax       ; xa = x0");
    E("    mov  rax, [rbp-16]");
    E("    mov  [rbp-128], rax       ; ya = y0");
    E("    mov  rax, [rbp-24]");
    E("    mov  [rbp-176], rax       ; ua = u0");
    E("    mov  rax, [rbp-32]");
    E("    mov  [rbp-200], rax       ; va = v0");
    E("    mov  rax, [rbp-40]");
    E("    mov  [rbp-160], rax       ; xb = x1");
    E("    mov  rax, [rbp-48]");
    E("    mov  [rbp-136], rax       ; yb = y1");
    E("    mov  rax, [rbp-56]");
    E("    mov  [rbp-184], rax       ; ub = u1");
    E("    mov  rax, [rbp-64]");
    E("    mov  [rbp-208], rax       ; vb = v1");
    E("    mov  rax, [rbp-72]");
    E("    mov  [rbp-168], rax       ; xc = x2");
    E("    mov  rax, [rbp-80]");
    E("    mov  [rbp-144], rax       ; yc = y2");
    E("    mov  rax, [rbp-88]");
    E("    mov  [rbp-192], rax       ; uc = u2");
    E("    mov  rax, [rbp-96]");
    E("    mov  [rbp-216], rax       ; vc = v2");
    E("");
    E("    ; bubble sort by y: if ya > yb swap(a,b)");
    E("    mov  rax, [rbp-128]");
    E("    cmp  rax, [rbp-136]");
    E("    jle  .fta_sort1_done");
    E("    ; swap ya/yb");
    E("    mov  rcx, [rbp-136]");
    E("    mov  [rbp-128], rcx");
    E("    mov  [rbp-136], rax");
    E("    ; swap xa/xb");
    E("    mov  rax, [rbp-152]");
    E("    mov  rcx, [rbp-160]");
    E("    mov  [rbp-152], rcx");
    E("    mov  [rbp-160], rax");
    E("    ; swap ua/ub");
    E("    mov  rax, [rbp-176]");
    E("    mov  rcx, [rbp-184]");
    E("    mov  [rbp-176], rcx");
    E("    mov  [rbp-184], rax");
    E("    ; swap va/vb");
    E("    mov  rax, [rbp-200]");
    E("    mov  rcx, [rbp-208]");
    E("    mov  [rbp-200], rcx");
    E("    mov  [rbp-208], rax");
    E(".fta_sort1_done:");
    E("    ; if yb > yc swap(b,c)");
    E("    mov  rax, [rbp-136]");
    E("    cmp  rax, [rbp-144]");
    E("    jle  .fta_sort2_done");
    E("    mov  rcx, [rbp-144]");
    E("    mov  [rbp-136], rcx");
    E("    mov  [rbp-144], rax");
    E("    mov  rax, [rbp-160]");
    E("    mov  rcx, [rbp-168]");
    E("    mov  [rbp-160], rcx");
    E("    mov  [rbp-168], rax");
    E("    mov  rax, [rbp-184]");
    E("    mov  rcx, [rbp-192]");
    E("    mov  [rbp-184], rcx");
    E("    mov  [rbp-192], rax");
    E("    mov  rax, [rbp-208]");
    E("    mov  rcx, [rbp-216]");
    E("    mov  [rbp-208], rcx");
    E("    mov  [rbp-216], rax");
    E(".fta_sort2_done:");
    E("    ; if ya > yb swap(a,b) again");
    E("    mov  rax, [rbp-128]");
    E("    cmp  rax, [rbp-136]");
    E("    jle  .fta_sort3_done");
    E("    mov  rcx, [rbp-136]");
    E("    mov  [rbp-128], rcx");
    E("    mov  [rbp-136], rax");
    E("    mov  rax, [rbp-152]");
    E("    mov  rcx, [rbp-160]");
    E("    mov  [rbp-152], rcx");
    E("    mov  [rbp-160], rax");
    E("    mov  rax, [rbp-176]");
    E("    mov  rcx, [rbp-184]");
    E("    mov  [rbp-176], rcx");
    E("    mov  [rbp-184], rax");
    E("    mov  rax, [rbp-200]");
    E("    mov  rcx, [rbp-208]");
    E("    mov  [rbp-200], rcx");
    E("    mov  [rbp-208], rax");
    E(".fta_sort3_done:");
    E("");
    E("    ; clamp y range to [0, height-1]");
    E("    mov  rax, [rbp-128]       ; ya");
    E("    cmp  rax, 0");
    E("    jge  .fta_ystart_ok");
    E("    xor  rax, rax");
    E(".fta_ystart_ok:");
    E("    mov  [rbp-224], rax       ; y = max(ya, 0)");
    E("    mov  r12, [rbp-144]       ; yc (max y)");
    E("    mov  rax, [rbx + WSTATE_HEIGHT]");
    E("    dec  rax");
    E("    cmp  r12, rax");
    E("    jle  .fta_yend_ok");
    E("    mov  r12, rax");
    E(".fta_yend_ok:");
    E("");
    E(".fta_scanline:");
    E("    mov  rax, [rbp-224]");
    E("    cmp  rax, r12");
    E("    jg   .fta_done");
    E("");
    E("    ; --- compute xLong, uLong, vLong on edge a-c ---");
    E("    mov  rax, [rbp-144]");
    E("    sub  rax, [rbp-128]       ; denom = yc - ya");
    E("    mov  r14, rax             ; save denom");
    E("    cmp  rax, 0");
    E("    je   .fta_long_degen");
    E("    ; xLong = xa + (xc - xa) * (y - ya) / denom");
    E("    mov  rcx, [rbp-168]");
    E("    sub  rcx, [rbp-152]       ; xc - xa");
    E("    mov  r10, [rbp-224]");
    E("    sub  r10, [rbp-128]       ; y - ya");
    E("    imul rcx, r10");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r14");
    E("    add  rax, [rbp-152]       ; xLong");
    E("    mov  r13, rax             ; r13 = xLong");
    E("    ; uLong = ua + (uc - ua) * (y - ya) / denom");
    E("    mov  rcx, [rbp-192]");
    E("    sub  rcx, [rbp-176]       ; uc - ua");
    E("    mov  r10, [rbp-224]");
    E("    sub  r10, [rbp-128]");
    E("    imul rcx, r10");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r14");
    E("    add  rax, [rbp-176]");
    E("    mov  [rbp-248], rax       ; uLong -> uleft temp");
    E("    ; vLong = va + (vc - va) * (y - ya) / denom");
    E("    mov  rcx, [rbp-216]");
    E("    sub  rcx, [rbp-200]       ; vc - va");
    E("    mov  r10, [rbp-224]");
    E("    sub  r10, [rbp-128]");
    E("    imul rcx, r10");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r14");
    E("    add  rax, [rbp-200]");
    E("    mov  [rbp-256], rax       ; vLong -> vleft temp");
    E("    jmp  .fta_long_done");
    E(".fta_long_degen:");
    E("    mov  r13, [rbp-152]       ; xLong = xa");
    E("    mov  rax, [rbp-176]");
    E("    mov  [rbp-248], rax       ; uLong = ua");
    E("    mov  rax, [rbp-200]");
    E("    mov  [rbp-256], rax       ; vLong = va");
    E(".fta_long_done:");
    E("");
    E("    ; --- compute xShort, uShort, vShort on edge a-b or b-c ---");
    E("    mov  rax, [rbp-224]");
    E("    cmp  rax, [rbp-136]       ; y < yb?");
    E("    jl   .fta_short_upper");
    E("");
    E("    ; lower segment: edge b-c");
    E("    mov  rax, [rbp-144]");
    E("    sub  rax, [rbp-136]       ; denom = yc - yb");
    E("    mov  r14, rax");
    E("    cmp  rax, 0");
    E("    je   .fta_short_lower_degen");
    E("    mov  rcx, [rbp-168]");
    E("    sub  rcx, [rbp-160]       ; xc - xb");
    E("    mov  r10, [rbp-224]");
    E("    sub  r10, [rbp-136]       ; y - yb");
    E("    imul rcx, r10");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r14");
    E("    add  rax, [rbp-160]       ; xShort");
    E("    mov  r15, rax");
    E("    mov  rcx, [rbp-192]");
    E("    sub  rcx, [rbp-184]       ; uc - ub");
    E("    mov  r10, [rbp-224]");
    E("    sub  r10, [rbp-136]");
    E("    imul rcx, r10");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r14");
    E("    add  rax, [rbp-184]");
    E("    mov  [rbp-264], rax       ; uShort -> uright temp");
    E("    mov  rcx, [rbp-216]");
    E("    sub  rcx, [rbp-208]       ; vc - vb");
    E("    mov  r10, [rbp-224]");
    E("    sub  r10, [rbp-136]");
    E("    imul rcx, r10");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r14");
    E("    add  rax, [rbp-208]");
    E("    mov  [rbp-272], rax       ; vShort -> vright temp");
    E("    jmp  .fta_short_done");
    E(".fta_short_lower_degen:");
    E("    mov  r15, [rbp-160]       ; xShort = xb");
    E("    mov  rax, [rbp-184]");
    E("    mov  [rbp-264], rax");
    E("    mov  rax, [rbp-208]");
    E("    mov  [rbp-272], rax");
    E("    jmp  .fta_short_done");
    E("");
    E(".fta_short_upper:");
    E("    ; upper segment: edge a-b");
    E("    mov  rax, [rbp-136]");
    E("    sub  rax, [rbp-128]       ; denom = yb - ya");
    E("    mov  r14, rax");
    E("    cmp  rax, 0");
    E("    je   .fta_short_upper_degen");
    E("    mov  rcx, [rbp-160]");
    E("    sub  rcx, [rbp-152]       ; xb - xa");
    E("    mov  r10, [rbp-224]");
    E("    sub  r10, [rbp-128]       ; y - ya");
    E("    imul rcx, r10");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r14");
    E("    add  rax, [rbp-152]       ; xShort");
    E("    mov  r15, rax");
    E("    mov  rcx, [rbp-184]");
    E("    sub  rcx, [rbp-176]       ; ub - ua");
    E("    mov  r10, [rbp-224]");
    E("    sub  r10, [rbp-128]");
    E("    imul rcx, r10");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r14");
    E("    add  rax, [rbp-176]");
    E("    mov  [rbp-264], rax       ; uShort");
    E("    mov  rcx, [rbp-208]");
    E("    sub  rcx, [rbp-200]       ; vb - va");
    E("    mov  r10, [rbp-224]");
    E("    sub  r10, [rbp-128]");
    E("    imul rcx, r10");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r14");
    E("    add  rax, [rbp-200]");
    E("    mov  [rbp-272], rax       ; vShort");
    E("    jmp  .fta_short_done");
    E(".fta_short_upper_degen:");
    E("    mov  r15, [rbp-152]       ; xShort = xa");
    E("    mov  rax, [rbp-176]");
    E("    mov  [rbp-264], rax");
    E("    mov  rax, [rbp-200]");
    E("    mov  [rbp-272], rax");
    E(".fta_short_done:");
    E("");
    E("    ; sort so xleft <= xright with corresponding u,v");
    E("    ; r13 = xLong, r15 = xShort");
    E("    ; [rbp-248] = uLong, [rbp-256] = vLong");
    E("    ; [rbp-264] = uShort, [rbp-272] = vShort");
    E("    cmp  r13, r15");
    E("    jle  .fta_lr_ok");
    E("    ; swap x");
    E("    mov  rax, r13");
    E("    mov  r13, r15");
    E("    mov  r15, rax");
    E("    ; swap u");
    E("    mov  rax, [rbp-248]");
    E("    mov  rcx, [rbp-264]");
    E("    mov  [rbp-248], rcx");
    E("    mov  [rbp-264], rax");
    E("    ; swap v");
    E("    mov  rax, [rbp-256]");
    E("    mov  rcx, [rbp-272]");
    E("    mov  [rbp-256], rcx");
    E("    mov  [rbp-272], rax");
    E(".fta_lr_ok:");
    E("    mov  [rbp-232], r13       ; xleft");
    E("    mov  [rbp-240], r15       ; xright");
    E("");
    E("    ; clamp x to [0, width-1]");
    E("    mov  rax, [rbp-232]");
    E("    cmp  rax, 0");
    E("    jge  .fta_xleft_ok");
    E("    xor  rax, rax");
    E("    mov  [rbp-232], rax");
    E(".fta_xleft_ok:");
    E("    mov  rax, [rbp-240]");
    E("    mov  rcx, [rbx + WSTATE_WIDTH]");
    E("    dec  rcx");
    E("    cmp  rax, rcx");
    E("    jle  .fta_xright_ok");
    E("    mov  [rbp-240], rcx");
    E(".fta_xright_ok:");
    E("");
    E("    ; compute row base offset");
    E("    mov  rax, [rbp-224]       ; y");
    E("    imul rax, [rbx + WSTATE_WIDTH]");
    E("    mov  r14, rax             ; r14 = y * width");
    E("");
    E("    ; span width for u,v interpolation");
    E("    mov  rax, [rbp-240]");
    E("    sub  rax, [rbp-232]       ; xright - xleft");
    E("    mov  r13, rax             ; r13 = span_dx (reuse, xLong done)");
    E("");
    E("    ; pixel loop: use r15 as current x");
    E("    mov  r15, [rbp-232]       ; r15 = x = xleft");
    E(".fta_span:");
    E("    cmp  r15, [rbp-240]");
    E("    jg   .fta_span_done");
    E("");
    E("    ; interpolate u,v at this x");
    E("    mov  rcx, r15");
    E("    sub  rcx, [rbp-232]       ; rcx = x - xleft");
    E("    cmp  r13, 0");
    E("    je   .fta_uv_degen");
    E("    ; u = uleft + (uright - uleft) * (x - xleft) / span_dx");
    E("    mov  r8, [rbp-264]");
    E("    sub  r8, [rbp-248]        ; uright - uleft");
    E("    imul r8, rcx");
    E("    mov  rax, r8");
    E("    cqo");
    E("    idiv r13");
    E("    add  rax, [rbp-248]");
    E("    mov  r8, rax              ; r8 = u");
    E("    ; v = vleft + (vright - vleft) * (x - xleft) / span_dx");
    E("    mov  r9, [rbp-272]");
    E("    sub  r9, [rbp-256]        ; vright - vleft");
    E("    imul r9, rcx");
    E("    mov  rax, r9");
    E("    cqo");
    E("    idiv r13");
    E("    add  rax, [rbp-256]");
    E("    mov  r9, rax              ; r9 = v");
    E("    jmp  .fta_uv_done");
    E(".fta_uv_degen:");
    E("    mov  r8, [rbp-248]        ; u = uleft");
    E("    mov  r9, [rbp-256]        ; v = vleft");
    E(".fta_uv_done:");
    E("");
    E("    ; clamp u to [0, tex_w-1]");
    E("    cmp  r8, 0");
    E("    jge  .fta_u_min_ok");
    E("    xor  r8, r8");
    E(".fta_u_min_ok:");
    E("    mov  rax, [rbp-112]       ; tex_w");
    E("    dec  rax");
    E("    cmp  r8, rax");
    E("    jle  .fta_u_max_ok");
    E("    mov  r8, rax");
    E(".fta_u_max_ok:");
    E("    ; clamp v to [0, tex_h-1]");
    E("    cmp  r9, 0");
    E("    jge  .fta_v_min_ok");
    E("    xor  r9, r9");
    E(".fta_v_min_ok:");
    E("    mov  rax, [rbp-120]       ; tex_h");
    E("    dec  rax");
    E("    cmp  r9, rax");
    E("    jle  .fta_v_max_ok");
    E("    mov  r9, rax");
    E(".fta_v_max_ok:");
    E("");
    E("    ; sample texture: addr = tex_ptr + (v * tex_w + u) * 2");
    E("    mov  rax, r9");
    E("    imul rax, [rbp-112]       ; v * tex_w");
    E("    add  rax, r8              ; + u");
    E("    shl  rax, 1               ; * 2 bytes");
    E("    add  rax, [rbp-104]       ; + tex_ptr");
    E("    movzx r10, word [rax]     ; r10 = RGB565 pixel");
    E("");
    E("    ; convert RGB565 to RGB888 using shifts (faster than div)");
    E("    ; R = ((pix >> 11) & 0x1F) << 3  (5-bit to 8-bit)");
    E("    ; G = ((pix >> 5) & 0x3F) << 2   (6-bit to 8-bit)");
    E("    ; B = (pix & 0x1F) << 3          (5-bit to 8-bit)");
    E("    mov  rax, r10");
    E("    shr  rax, 11");
    E("    and  rax, 0x1F");
    E("    shl  rax, 3");
    E("    mov  r11, rax             ; r11 = R");
    E("    mov  rax, r10");
    E("    shr  rax, 5");
    E("    and  rax, 0x3F");
    E("    shl  rax, 2");
    E("    mov  rcx, rax             ; rcx = G");
    E("    mov  rax, r10");
    E("    and  rax, 0x1F");
    E("    shl  rax, 3");
    E("    mov  rdx, rax             ; rdx = B");
    E("");
    E("    ; write pixel (BGRA format)");
    E("    mov  rax, r14             ; y * width");
    E("    add  rax, r15             ; + x");
    E("    shl  rax, 2               ; * 4 bytes per pixel");
    E("    add  rax, [rbx + WSTATE_PIXELS]");
    E("    mov  byte [rax+0], dl     ; B");
    E("    mov  byte [rax+1], cl     ; G");
    E("    mov  byte [rax+2], r11b   ; R");
    E("    mov  byte [rax+3], 0xFF   ; A");
    E("");
    E("    inc  r15");
    E("    jmp  .fta_span");
    E(".fta_span_done:");
    E("");
    E("    mov  rax, [rbp-224]");
    E("    inc  rax");
    E("    mov  [rbp-224], rax");
    E("    jmp  .fta_scanline");
    E("");
    E(".fta_done:");
    E("    mov  r12, [rbp-280]");
    E("    mov  r13, [rbp-288]");
    E("    mov  r14, [rbp-296]");
    E("    mov  r15, [rbp-304]");
    E("    add  rsp, 312");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_fill_triangle_persp(x0,y0,z0,u0,v0,x1,y1,z1,u1,v1,x2,y2,z2,u2,v2,tex_ptr,tex_w,tex_h)
    // Perspective-correct textured triangle rasterizer.
    E("; --- _slag_fill_triangle_persp ---");
    E("_slag_fill_triangle_persp:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 400");
    E("    mov  [rbp-344], r12");
    E("    mov  [rbp-352], r13");
    E("    mov  [rbp-360], r14");
    E("    mov  [rbp-368], r15");
    E("    mov  rax, 0x3FF0000000000000");
    E("    mov  [rbp-384], rax");
    E("    mov  [rbp-8], rcx");
    E("    mov  [rbp-16], rdx");
    E("    mov  [rbp-24], r8");
    E("    mov  [rbp-32], r9");
    E("    mov  rax, [rbp+48]");
    E("    mov  [rbp-40], rax");
    E("    mov  rax, [rbp+56]");
    E("    mov  [rbp-48], rax");
    E("    mov  rax, [rbp+64]");
    E("    mov  [rbp-56], rax");
    E("    mov  rax, [rbp+72]");
    E("    mov  [rbp-64], rax");
    E("    mov  rax, [rbp+80]");
    E("    mov  [rbp-72], rax");
    E("    mov  rax, [rbp+88]");
    E("    mov  [rbp-80], rax");
    E("    mov  rax, [rbp+96]");
    E("    mov  [rbp-88], rax");
    E("    mov  rax, [rbp+104]");
    E("    mov  [rbp-96], rax");
    E("    mov  rax, [rbp+112]");
    E("    mov  [rbp-104], rax");
    E("    mov  rax, [rbp+120]");
    E("    mov  [rbp-112], rax");
    E("    mov  rax, [rbp+128]");
    E("    mov  [rbp-120], rax");
    E("    mov  rax, [rbp+136]");
    E("    mov  [rbp-128], rax");
    E("    mov  rax, [rbp+144]");
    E("    mov  [rbp-136], rax");
    E("    mov  rax, [rbp+152]");
    E("    mov  [rbp-144], rax");
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    mov  rbx, rax");
    E("    cvtsi2sd xmm0, qword [rbp-24]");
    E("    movsd xmm1, [rbp-384]");
    E("    divsd xmm1, xmm0");
    E("    movsd [rbp-200], xmm1");
    E("    cvtsi2sd xmm2, qword [rbp-32]");
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-224], xmm2");
    E("    cvtsi2sd xmm3, qword [rbp-40]");
    E("    mulsd xmm3, xmm1");
    E("    movsd [rbp-248], xmm3");
    E("    mov rax, [rbp-8]");
    E("    mov [rbp-176], rax");
    E("    mov rax, [rbp-16]");
    E("    mov [rbp-152], rax");
    E("    cvtsi2sd xmm0, qword [rbp-64]");
    E("    movsd xmm1, [rbp-384]");
    E("    divsd xmm1, xmm0");
    E("    movsd [rbp-208], xmm1");
    E("    cvtsi2sd xmm2, qword [rbp-72]");
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-232], xmm2");
    E("    cvtsi2sd xmm3, qword [rbp-80]");
    E("    mulsd xmm3, xmm1");
    E("    movsd [rbp-256], xmm3");
    E("    mov rax, [rbp-48]");
    E("    mov [rbp-184], rax");
    E("    mov rax, [rbp-56]");
    E("    mov [rbp-160], rax");
    E("    cvtsi2sd xmm0, qword [rbp-104]");
    E("    movsd xmm1, [rbp-384]");
    E("    divsd xmm1, xmm0");
    E("    movsd [rbp-216], xmm1");
    E("    cvtsi2sd xmm2, qword [rbp-112]");
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-240], xmm2");
    E("    cvtsi2sd xmm3, qword [rbp-120]");
    E("    mulsd xmm3, xmm1");
    E("    movsd [rbp-264], xmm3");
    E("    mov rax, [rbp-88]");
    E("    mov [rbp-192], rax");
    E("    mov rax, [rbp-96]");
    E("    mov [rbp-168], rax");
    E("    mov rax, [rbp-152]");
    E("    cmp rax, [rbp-160]");
    E("    jle .ftp_s1");
    E("    mov rcx, [rbp-160]");
    E("    mov [rbp-152], rcx");
    E("    mov [rbp-160], rax");
    E("    mov rax, [rbp-176]");
    E("    mov rcx, [rbp-184]");
    E("    mov [rbp-176], rcx");
    E("    mov [rbp-184], rax");
    E("    movsd xmm0, [rbp-200]");
    E("    movsd xmm1, [rbp-208]");
    E("    movsd [rbp-200], xmm1");
    E("    movsd [rbp-208], xmm0");
    E("    movsd xmm0, [rbp-224]");
    E("    movsd xmm1, [rbp-232]");
    E("    movsd [rbp-224], xmm1");
    E("    movsd [rbp-232], xmm0");
    E("    movsd xmm0, [rbp-248]");
    E("    movsd xmm1, [rbp-256]");
    E("    movsd [rbp-248], xmm1");
    E("    movsd [rbp-256], xmm0");
    E(".ftp_s1:");
    E("    mov rax, [rbp-160]");
    E("    cmp rax, [rbp-168]");
    E("    jle .ftp_s2");
    E("    mov rcx, [rbp-168]");
    E("    mov [rbp-160], rcx");
    E("    mov [rbp-168], rax");
    E("    mov rax, [rbp-184]");
    E("    mov rcx, [rbp-192]");
    E("    mov [rbp-184], rcx");
    E("    mov [rbp-192], rax");
    E("    movsd xmm0, [rbp-208]");
    E("    movsd xmm1, [rbp-216]");
    E("    movsd [rbp-208], xmm1");
    E("    movsd [rbp-216], xmm0");
    E("    movsd xmm0, [rbp-232]");
    E("    movsd xmm1, [rbp-240]");
    E("    movsd [rbp-232], xmm1");
    E("    movsd [rbp-240], xmm0");
    E("    movsd xmm0, [rbp-256]");
    E("    movsd xmm1, [rbp-264]");
    E("    movsd [rbp-256], xmm1");
    E("    movsd [rbp-264], xmm0");
    E(".ftp_s2:");
    E("    mov rax, [rbp-152]");
    E("    cmp rax, [rbp-160]");
    E("    jle .ftp_s3");
    E("    mov rcx, [rbp-160]");
    E("    mov [rbp-152], rcx");
    E("    mov [rbp-160], rax");
    E("    mov rax, [rbp-176]");
    E("    mov rcx, [rbp-184]");
    E("    mov [rbp-176], rcx");
    E("    mov [rbp-184], rax");
    E("    movsd xmm0, [rbp-200]");
    E("    movsd xmm1, [rbp-208]");
    E("    movsd [rbp-200], xmm1");
    E("    movsd [rbp-208], xmm0");
    E("    movsd xmm0, [rbp-224]");
    E("    movsd xmm1, [rbp-232]");
    E("    movsd [rbp-224], xmm1");
    E("    movsd [rbp-232], xmm0");
    E("    movsd xmm0, [rbp-248]");
    E("    movsd xmm1, [rbp-256]");
    E("    movsd [rbp-248], xmm1");
    E("    movsd [rbp-256], xmm0");
    E(".ftp_s3:");
    E("    mov rax, [rbp-152]");
    E("    cmp rax, 0");
    E("    jge .ftp_ys");
    E("    xor rax, rax");
    E(".ftp_ys:");
    E("    mov [rbp-272], rax");
    E("    mov r12, [rbp-168]");
    E("    mov rax, [rbx + WSTATE_HEIGHT]");
    E("    dec rax");
    E("    cmp r12, rax");
    E("    jle .ftp_ye");
    E("    mov r12, rax");
    E(".ftp_ye:");
    E(".ftp_scan:");
    E("    mov rax, [rbp-272]");
    E("    cmp rax, r12");
    E("    jg .ftp_done");
    E("    mov rax, [rbp-168]");
    E("    sub rax, [rbp-152]");
    E("    cvtsi2sd xmm7, rax");
    E("    xorpd xmm6, xmm6");
    E("    ucomisd xmm7, xmm6");
    E("    je .ftp_ld");
    E("    mov rcx, [rbp-272]");
    E("    sub rcx, [rbp-152]");
    E("    cvtsi2sd xmm5, rcx");
    E("    divsd xmm5, xmm7");
    E("    mov rcx, [rbp-192]");
    E("    sub rcx, [rbp-176]");
    E("    cvtsi2sd xmm0, rcx");
    E("    mulsd xmm0, xmm5");
    E("    cvtsi2sd xmm1, qword [rbp-176]");
    E("    addsd xmm0, xmm1");
    E("    cvttsd2si r13, xmm0");
    E("    movsd xmm0, [rbp-216]");
    E("    subsd xmm0, [rbp-200]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp-200]");
    E("    movsd [rbp-296], xmm0");
    E("    movsd xmm0, [rbp-240]");
    E("    subsd xmm0, [rbp-224]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp-224]");
    E("    movsd [rbp-312], xmm0");
    E("    movsd xmm0, [rbp-264]");
    E("    subsd xmm0, [rbp-248]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp-248]");
    E("    movsd [rbp-328], xmm0");
    E("    jmp .ftp_lx");
    E(".ftp_ld:");
    E("    mov r13, [rbp-176]");
    E("    movsd xmm0, [rbp-200]");
    E("    movsd [rbp-296], xmm0");
    E("    movsd xmm0, [rbp-224]");
    E("    movsd [rbp-312], xmm0");
    E("    movsd xmm0, [rbp-248]");
    E("    movsd [rbp-328], xmm0");
    E(".ftp_lx:");
    E("    mov rax, [rbp-272]");
    E("    cmp rax, [rbp-160]");
    E("    jl .ftp_su");
    E("    mov rax, [rbp-168]");
    E("    sub rax, [rbp-160]");
    E("    cvtsi2sd xmm7, rax");
    E("    xorpd xmm6, xmm6");
    E("    ucomisd xmm7, xmm6");
    E("    je .ftp_sld");
    E("    mov rcx, [rbp-272]");
    E("    sub rcx, [rbp-160]");
    E("    cvtsi2sd xmm5, rcx");
    E("    divsd xmm5, xmm7");
    E("    mov rcx, [rbp-192]");
    E("    sub rcx, [rbp-184]");
    E("    cvtsi2sd xmm0, rcx");
    E("    mulsd xmm0, xmm5");
    E("    cvtsi2sd xmm1, qword [rbp-184]");
    E("    addsd xmm0, xmm1");
    E("    cvttsd2si r15, xmm0");
    E("    movsd xmm0, [rbp-216]");
    E("    subsd xmm0, [rbp-208]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp-208]");
    E("    movsd [rbp-304], xmm0");
    E("    movsd xmm0, [rbp-240]");
    E("    subsd xmm0, [rbp-232]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp-232]");
    E("    movsd [rbp-320], xmm0");
    E("    movsd xmm0, [rbp-264]");
    E("    subsd xmm0, [rbp-256]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp-256]");
    E("    movsd [rbp-336], xmm0");
    E("    jmp .ftp_sx");
    E(".ftp_sld:");
    E("    mov r15, [rbp-184]");
    E("    movsd xmm0, [rbp-208]");
    E("    movsd [rbp-304], xmm0");
    E("    movsd xmm0, [rbp-232]");
    E("    movsd [rbp-320], xmm0");
    E("    movsd xmm0, [rbp-256]");
    E("    movsd [rbp-336], xmm0");
    E("    jmp .ftp_sx");
    E(".ftp_su:");
    E("    mov rax, [rbp-160]");
    E("    sub rax, [rbp-152]");
    E("    cvtsi2sd xmm7, rax");
    E("    xorpd xmm6, xmm6");
    E("    ucomisd xmm7, xmm6");
    E("    je .ftp_sud");
    E("    mov rcx, [rbp-272]");
    E("    sub rcx, [rbp-152]");
    E("    cvtsi2sd xmm5, rcx");
    E("    divsd xmm5, xmm7");
    E("    mov rcx, [rbp-184]");
    E("    sub rcx, [rbp-176]");
    E("    cvtsi2sd xmm0, rcx");
    E("    mulsd xmm0, xmm5");
    E("    cvtsi2sd xmm1, qword [rbp-176]");
    E("    addsd xmm0, xmm1");
    E("    cvttsd2si r15, xmm0");
    E("    movsd xmm0, [rbp-208]");
    E("    subsd xmm0, [rbp-200]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp-200]");
    E("    movsd [rbp-304], xmm0");
    E("    movsd xmm0, [rbp-232]");
    E("    subsd xmm0, [rbp-224]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp-224]");
    E("    movsd [rbp-320], xmm0");
    E("    movsd xmm0, [rbp-256]");
    E("    subsd xmm0, [rbp-248]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp-248]");
    E("    movsd [rbp-336], xmm0");
    E("    jmp .ftp_sx");
    E(".ftp_sud:");
    E("    mov r15, [rbp-176]");
    E("    movsd xmm0, [rbp-200]");
    E("    movsd [rbp-304], xmm0");
    E("    movsd xmm0, [rbp-224]");
    E("    movsd [rbp-320], xmm0");
    E("    movsd xmm0, [rbp-248]");
    E("    movsd [rbp-336], xmm0");
    E(".ftp_sx:");
    E("    cmp r13, r15");
    E("    jle .ftp_lr");
    E("    mov rax, r13");
    E("    mov r13, r15");
    E("    mov r15, rax");
    E("    movsd xmm0, [rbp-296]");
    E("    movsd xmm1, [rbp-304]");
    E("    movsd [rbp-296], xmm1");
    E("    movsd [rbp-304], xmm0");
    E("    movsd xmm0, [rbp-312]");
    E("    movsd xmm1, [rbp-320]");
    E("    movsd [rbp-312], xmm1");
    E("    movsd [rbp-320], xmm0");
    E("    movsd xmm0, [rbp-328]");
    E("    movsd xmm1, [rbp-336]");
    E("    movsd [rbp-328], xmm1");
    E("    movsd [rbp-336], xmm0");
    E(".ftp_lr:");
    E("    mov [rbp-280], r13");
    E("    mov [rbp-288], r15");
    E("    mov rax, [rbp-280]");
    E("    cmp rax, 0");
    E("    jge .ftp_xl");
    E("    xor rax, rax");
    E("    mov [rbp-280], rax");
    E(".ftp_xl:");
    E("    mov rax, [rbp-288]");
    E("    mov rcx, [rbx + WSTATE_WIDTH]");
    E("    dec rcx");
    E("    cmp rax, rcx");
    E("    jle .ftp_xr");
    E("    mov [rbp-288], rcx");
    E(".ftp_xr:");
    E("    mov rax, [rbp-272]");
    E("    imul rax, [rbx + WSTATE_WIDTH]");
    E("    mov r14, rax");
    E("    mov rax, [rbp-288]");
    E("    sub rax, [rbp-280]");
    E("    cvtsi2sd xmm7, rax");
    E("    mov r15, [rbp-280]");
    E(".ftp_px:");
    E("    cmp r15, [rbp-288]");
    E("    jg .ftp_pxd");
    E("    mov rcx, r15");
    E("    sub rcx, [rbp-280]");
    E("    cvtsi2sd xmm5, rcx");
    E("    xorpd xmm6, xmm6");
    E("    ucomisd xmm7, xmm6");
    E("    je .ftp_uvd");
    E("    divsd xmm5, xmm7");
    E("    movsd xmm0, [rbp-304]");
    E("    subsd xmm0, [rbp-296]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp-296]");
    E("    movsd xmm1, [rbp-320]");
    E("    subsd xmm1, [rbp-312]");
    E("    mulsd xmm1, xmm5");
    E("    addsd xmm1, [rbp-312]");
    E("    movsd xmm2, [rbp-336]");
    E("    subsd xmm2, [rbp-328]");
    E("    mulsd xmm2, xmm5");
    E("    addsd xmm2, [rbp-328]");
    E("    divsd xmm1, xmm0");
    E("    divsd xmm2, xmm0");
    E("    cvttsd2si r8, xmm1");
    E("    cvttsd2si r9, xmm2");
    E("    jmp .ftp_uvx");
    E(".ftp_uvd:");
    E("    movsd xmm0, [rbp-296]");
    E("    movsd xmm1, [rbp-312]");
    E("    movsd xmm2, [rbp-328]");
    E("    divsd xmm1, xmm0");
    E("    divsd xmm2, xmm0");
    E("    cvttsd2si r8, xmm1");
    E("    cvttsd2si r9, xmm2");
    E(".ftp_uvx:");
    E("    cmp r8, 0");
    E("    jge .ftp_u0");
    E("    xor r8, r8");
    E(".ftp_u0:");
    E("    mov rax, [rbp-136]");
    E("    dec rax");
    E("    cmp r8, rax");
    E("    jle .ftp_u1");
    E("    mov r8, rax");
    E(".ftp_u1:");
    E("    cmp r9, 0");
    E("    jge .ftp_v0");
    E("    xor r9, r9");
    E(".ftp_v0:");
    E("    mov rax, [rbp-144]");
    E("    dec rax");
    E("    cmp r9, rax");
    E("    jle .ftp_v1");
    E("    mov r9, rax");
    E(".ftp_v1:");
    E("    mov rax, r9");
    E("    imul rax, [rbp-136]");
    E("    add rax, r8");
    E("    shl rax, 1");
    E("    add rax, [rbp-128]");
    E("    movzx r10, word [rax]");
    E("    mov rax, r10");
    E("    shr rax, 11");
    E("    and rax, 0x1F");
    E("    shl rax, 3");
    E("    mov r11, rax");
    E("    mov rax, r10");
    E("    shr rax, 5");
    E("    and rax, 0x3F");
    E("    shl rax, 2");
    E("    mov rcx, rax");
    E("    mov rax, r10");
    E("    and rax, 0x1F");
    E("    shl rax, 3");
    E("    mov rdx, rax");
    E("    mov rax, r14");
    E("    add rax, r15");
    E("    shl rax, 2");
    E("    add rax, [rbx + WSTATE_PIXELS]");
    E("    mov byte [rax], dl");
    E("    mov byte [rax+1], cl");
    E("    mov byte [rax+2], r11b");
    E("    mov byte [rax+3], 0xFF");
    E("    inc r15");
    E("    jmp .ftp_px");
    E(".ftp_pxd:");
    E("    mov rax, [rbp-272]");
    E("    inc rax");
    E("    mov [rbp-272], rax");
    E("    jmp .ftp_scan");
    E(".ftp_done:");
    E("    mov r12, [rbp-344]");
    E("    mov r13, [rbp-352]");
    E("    mov r14, [rbp-360]");
    E("    mov r15, [rbp-368]");
    E("    add rsp, 400");
    E("    pop rbx");
    E("    pop rbp");
    E("    ret");
    E("");
}

// ---------------------------------------------------------------------
// fill_triangle_pcolor: perspective-correct textured triangle with vertex colors
// Parameters: rcx=verts, rdx=tex_ptr, r8=tex_w, r9=tex_h
// verts points to 24 int64s: 3 vertices x (x,y,z,u,v,r,g,b)
// ---------------------------------------------------------------------
static void emit_fill_triangle_pcolor(Codegen *cg) {
    E("; --- _slag_fill_triangle_pcolor ---");
    E("_slag_fill_triangle_pcolor:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    push r15");
    E("    sub  rsp, 576");

    // Save parameters
    E("    mov  [rbp-8], rcx");    // verts ptr
    E("    mov  [rbp-16], rdx");   // tex_ptr
    E("    mov  [rbp-24], r8");    // tex_w
    E("    mov  [rbp-32], r9");    // tex_h

    // Load 1.0 constant
    E("    mov  rax, 0x3FF0000000000000");
    E("    mov  [rbp-40], rax");

    // Get TLS window state
    E("    mov  rcx, [_window_tls_index]");
    E("    call TlsGetValue");
    E("    mov  rbx, rax");

    // Load vertex data from buffer
    // Vertex 0: x0=[rbp-48], y0=[rbp-56], z0=[rbp-64], u0=[rbp-72], v0=[rbp-80], r0=[rbp-88], g0=[rbp-96], b0=[rbp-104]
    E("    mov  rsi, [rbp-8]");
    E("    mov  rax, [rsi]");
    E("    mov  [rbp-48], rax");   // x0
    E("    mov  rax, [rsi+8]");
    E("    mov  [rbp-56], rax");   // y0
    E("    mov  rax, [rsi+16]");
    E("    mov  [rbp-64], rax");   // z0
    E("    mov  rax, [rsi+24]");
    E("    mov  [rbp-72], rax");   // u0
    E("    mov  rax, [rsi+32]");
    E("    mov  [rbp-80], rax");   // v0
    E("    mov  rax, [rsi+40]");
    E("    mov  [rbp-88], rax");   // r0
    E("    mov  rax, [rsi+48]");
    E("    mov  [rbp-96], rax");   // g0
    E("    mov  rax, [rsi+56]");
    E("    mov  [rbp-104], rax");  // b0

    // Vertex 1: x1=[rbp-112], y1=[rbp-120], z1=[rbp-128], u1=[rbp-136], v1=[rbp-144], r1=[rbp-152], g1=[rbp-160], b1=[rbp-168]
    E("    mov  rax, [rsi+64]");
    E("    mov  [rbp-112], rax");  // x1
    E("    mov  rax, [rsi+72]");
    E("    mov  [rbp-120], rax");  // y1
    E("    mov  rax, [rsi+80]");
    E("    mov  [rbp-128], rax");  // z1
    E("    mov  rax, [rsi+88]");
    E("    mov  [rbp-136], rax");  // u1
    E("    mov  rax, [rsi+96]");
    E("    mov  [rbp-144], rax");  // v1
    E("    mov  rax, [rsi+104]");
    E("    mov  [rbp-152], rax");  // r1
    E("    mov  rax, [rsi+112]");
    E("    mov  [rbp-160], rax");  // g1
    E("    mov  rax, [rsi+120]");
    E("    mov  [rbp-168], rax");  // b1

    // Vertex 2: x2=[rbp-176], y2=[rbp-184], z2=[rbp-192], u2=[rbp-200], v2=[rbp-208], r2=[rbp-216], g2=[rbp-224], b2=[rbp-232]
    E("    mov  rax, [rsi+128]");
    E("    mov  [rbp-176], rax");  // x2
    E("    mov  rax, [rsi+136]");
    E("    mov  [rbp-184], rax");  // y2
    E("    mov  rax, [rsi+144]");
    E("    mov  [rbp-192], rax");  // z2
    E("    mov  rax, [rsi+152]");
    E("    mov  [rbp-200], rax");  // u2
    E("    mov  rax, [rsi+160]");
    E("    mov  [rbp-208], rax");  // v2
    E("    mov  rax, [rsi+168]");
    E("    mov  [rbp-216], rax");  // r2
    E("    mov  rax, [rsi+176]");
    E("    mov  [rbp-224], rax");  // g2
    E("    mov  rax, [rsi+184]");
    E("    mov  [rbp-232], rax");  // b2

    // Compute 1/z, u/z, v/z, r/z, g/z, b/z for vertex 0
    // [rbp-240]=1/z0, [rbp-248]=u/z0, [rbp-256]=v/z0, [rbp-264]=r/z0, [rbp-272]=g/z0, [rbp-280]=b/z0
    E("    cvtsi2sd xmm0, qword [rbp-64]");   // z0
    E("    movsd xmm1, [rbp-40]");            // 1.0
    E("    divsd xmm1, xmm0");                // 1/z0
    E("    movsd [rbp-240], xmm1");
    E("    cvtsi2sd xmm2, qword [rbp-72]");   // u0
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-248], xmm2");           // u/z0
    E("    cvtsi2sd xmm2, qword [rbp-80]");   // v0
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-256], xmm2");           // v/z0
    E("    cvtsi2sd xmm2, qword [rbp-88]");   // r0
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-264], xmm2");           // r/z0
    E("    cvtsi2sd xmm2, qword [rbp-96]");   // g0
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-272], xmm2");           // g/z0
    E("    cvtsi2sd xmm2, qword [rbp-104]");  // b0
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-280], xmm2");           // b/z0

    // Compute 1/z, u/z, v/z, r/z, g/z, b/z for vertex 1
    // [rbp-288]=1/z1, [rbp-296]=u/z1, [rbp-304]=v/z1, [rbp-312]=r/z1, [rbp-320]=g/z1, [rbp-328]=b/z1
    E("    cvtsi2sd xmm0, qword [rbp-128]");  // z1
    E("    movsd xmm1, [rbp-40]");            // 1.0
    E("    divsd xmm1, xmm0");                // 1/z1
    E("    movsd [rbp-288], xmm1");
    E("    cvtsi2sd xmm2, qword [rbp-136]");  // u1
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-296], xmm2");           // u/z1
    E("    cvtsi2sd xmm2, qword [rbp-144]");  // v1
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-304], xmm2");           // v/z1
    E("    cvtsi2sd xmm2, qword [rbp-152]");  // r1
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-312], xmm2");           // r/z1
    E("    cvtsi2sd xmm2, qword [rbp-160]");  // g1
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-320], xmm2");           // g/z1
    E("    cvtsi2sd xmm2, qword [rbp-168]");  // b1
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-328], xmm2");           // b/z1

    // Compute 1/z, u/z, v/z, r/z, g/z, b/z for vertex 2
    // [rbp-336]=1/z2, [rbp-344]=u/z2, [rbp-352]=v/z2, [rbp-360]=r/z2, [rbp-368]=g/z2, [rbp-376]=b/z2
    E("    cvtsi2sd xmm0, qword [rbp-192]");  // z2
    E("    movsd xmm1, [rbp-40]");            // 1.0
    E("    divsd xmm1, xmm0");                // 1/z2
    E("    movsd [rbp-336], xmm1");
    E("    cvtsi2sd xmm2, qword [rbp-200]");  // u2
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-344], xmm2");           // u/z2
    E("    cvtsi2sd xmm2, qword [rbp-208]");  // v2
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-352], xmm2");           // v/z2
    E("    cvtsi2sd xmm2, qword [rbp-216]");  // r2
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-360], xmm2");           // r/z2
    E("    cvtsi2sd xmm2, qword [rbp-224]");  // g2
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-368], xmm2");           // g/z2
    E("    cvtsi2sd xmm2, qword [rbp-232]");  // b2
    E("    mulsd xmm2, xmm1");
    E("    movsd [rbp-376], xmm2");           // b/z2

    // Sort vertices by y (bubble sort: v0.y <= v1.y <= v2.y)
    // Working copies: y0=[rbp-384], y1=[rbp-392], y2=[rbp-400]
    // x0=[rbp-408], x1=[rbp-416], x2=[rbp-424]
    E("    mov  rax, [rbp-56]");
    E("    mov  [rbp-384], rax");  // y0
    E("    mov  rax, [rbp-120]");
    E("    mov  [rbp-392], rax");  // y1
    E("    mov  rax, [rbp-184]");
    E("    mov  [rbp-400], rax");  // y2
    E("    mov  rax, [rbp-48]");
    E("    mov  [rbp-408], rax");  // x0
    E("    mov  rax, [rbp-112]");
    E("    mov  [rbp-416], rax");  // x1
    E("    mov  rax, [rbp-176]");
    E("    mov  [rbp-424], rax");  // x2

    // Attribute pointers for sorted vertices
    // [rbp-432]=attr0_base (offset into rbp for 1/z,u/z,v/z,r/z,g/z,b/z)
    // [rbp-440]=attr1_base
    // [rbp-448]=attr2_base
    E("    mov  qword [rbp-432], 240");  // vertex 0 attrs start at rbp-240
    E("    mov  qword [rbp-440], 288");  // vertex 1 attrs start at rbp-288
    E("    mov  qword [rbp-448], 336");  // vertex 2 attrs start at rbp-336

    // Sort pass 1: if y0 > y1, swap
    E(".ftpc_s1:");
    E("    mov  rax, [rbp-384]");
    E("    cmp  rax, [rbp-392]");
    E("    jle  .ftpc_s2");
    E("    mov  rcx, [rbp-392]");
    E("    mov  [rbp-384], rcx");
    E("    mov  [rbp-392], rax");
    E("    mov  rax, [rbp-408]");
    E("    mov  rcx, [rbp-416]");
    E("    mov  [rbp-408], rcx");
    E("    mov  [rbp-416], rax");
    E("    mov  rax, [rbp-432]");
    E("    mov  rcx, [rbp-440]");
    E("    mov  [rbp-432], rcx");
    E("    mov  [rbp-440], rax");

    // Sort pass 2: if y1 > y2, swap
    E(".ftpc_s2:");
    E("    mov  rax, [rbp-392]");
    E("    cmp  rax, [rbp-400]");
    E("    jle  .ftpc_s3");
    E("    mov  rcx, [rbp-400]");
    E("    mov  [rbp-392], rcx");
    E("    mov  [rbp-400], rax");
    E("    mov  rax, [rbp-416]");
    E("    mov  rcx, [rbp-424]");
    E("    mov  [rbp-416], rcx");
    E("    mov  [rbp-424], rax");
    E("    mov  rax, [rbp-440]");
    E("    mov  rcx, [rbp-448]");
    E("    mov  [rbp-440], rcx");
    E("    mov  [rbp-448], rax");

    // Sort pass 3: if y0 > y1, swap (again)
    E(".ftpc_s3:");
    E("    mov  rax, [rbp-384]");
    E("    cmp  rax, [rbp-392]");
    E("    jle  .ftpc_sorted");
    E("    mov  rcx, [rbp-392]");
    E("    mov  [rbp-384], rcx");
    E("    mov  [rbp-392], rax");
    E("    mov  rax, [rbp-408]");
    E("    mov  rcx, [rbp-416]");
    E("    mov  [rbp-408], rcx");
    E("    mov  [rbp-416], rax");
    E("    mov  rax, [rbp-432]");
    E("    mov  rcx, [rbp-440]");
    E("    mov  [rbp-432], rcx");
    E("    mov  [rbp-440], rax");

    E(".ftpc_sorted:");
    // Clamp y range to screen
    // [rbp-456]=y_start, [rbp-464]=y_end
    E("    mov  rax, [rbp-384]");
    E("    cmp  rax, 0");
    E("    jge  .ftpc_ys");
    E("    xor  rax, rax");
    E(".ftpc_ys:");
    E("    mov  [rbp-456], rax");
    E("    mov  r12, [rbp-400]");
    E("    mov  rax, [rbx + WSTATE_HEIGHT]");
    E("    dec  rax");
    E("    cmp  r12, rax");
    E("    jle  .ftpc_ye");
    E("    mov  r12, rax");
    E(".ftpc_ye:");

    // Scanline loop
    E(".ftpc_scan:");
    E("    mov  rax, [rbp-456]");
    E("    cmp  rax, r12");
    E("    jg   .ftpc_done");

    // Interpolate along long edge (v0 to v2)
    E("    mov  rax, [rbp-400]");
    E("    sub  rax, [rbp-384]");
    E("    cvtsi2sd xmm7, rax");
    E("    xorpd xmm6, xmm6");
    E("    ucomisd xmm7, xmm6");
    E("    je   .ftpc_longd");
    E("    mov  rcx, [rbp-456]");
    E("    sub  rcx, [rbp-384]");
    E("    cvtsi2sd xmm5, rcx");
    E("    divsd xmm5, xmm7");  // t = (y - y0) / (y2 - y0)

    // x_long = x0 + t * (x2 - x0)
    E("    mov  rcx, [rbp-424]");
    E("    sub  rcx, [rbp-408]");
    E("    cvtsi2sd xmm0, rcx");
    E("    mulsd xmm0, xmm5");
    E("    cvtsi2sd xmm1, qword [rbp-408]");
    E("    addsd xmm0, xmm1");
    E("    cvttsd2si r13, xmm0");  // x_long in r13

    // Interpolate attributes along long edge
    // Load attr0 base and attr2 base
    E("    mov  rdi, [rbp-432]");  // attr0 offset
    E("    neg  rdi");
    E("    mov  rsi, [rbp-448]");  // attr2 offset
    E("    neg  rsi");

    // 1/z_long
    E("    movsd xmm0, [rbp+rsi]");     // 1/z2
    E("    subsd xmm0, [rbp+rdi]");     // 1/z2 - 1/z0
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp+rdi]");
    E("    movsd [rbp-472], xmm0");     // 1/z_long

    // u/z_long
    E("    movsd xmm0, [rbp+rsi-8]");
    E("    subsd xmm0, [rbp+rdi-8]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp+rdi-8]");
    E("    movsd [rbp-480], xmm0");

    // v/z_long
    E("    movsd xmm0, [rbp+rsi-16]");
    E("    subsd xmm0, [rbp+rdi-16]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp+rdi-16]");
    E("    movsd [rbp-488], xmm0");

    // r/z_long
    E("    movsd xmm0, [rbp+rsi-24]");
    E("    subsd xmm0, [rbp+rdi-24]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp+rdi-24]");
    E("    movsd [rbp-496], xmm0");

    // g/z_long
    E("    movsd xmm0, [rbp+rsi-32]");
    E("    subsd xmm0, [rbp+rdi-32]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp+rdi-32]");
    E("    movsd [rbp-504], xmm0");

    // b/z_long
    E("    movsd xmm0, [rbp+rsi-40]");
    E("    subsd xmm0, [rbp+rdi-40]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp+rdi-40]");
    E("    movsd [rbp-512], xmm0");
    E("    jmp  .ftpc_short");

    E(".ftpc_longd:");  // degenerate long edge
    E("    mov  r13, [rbp-408]");
    E("    mov  rdi, [rbp-432]");
    E("    neg  rdi");
    E("    movsd xmm0, [rbp+rdi]");
    E("    movsd [rbp-472], xmm0");
    E("    movsd xmm0, [rbp+rdi-8]");
    E("    movsd [rbp-480], xmm0");
    E("    movsd xmm0, [rbp+rdi-16]");
    E("    movsd [rbp-488], xmm0");
    E("    movsd xmm0, [rbp+rdi-24]");
    E("    movsd [rbp-496], xmm0");
    E("    movsd xmm0, [rbp+rdi-32]");
    E("    movsd [rbp-504], xmm0");
    E("    movsd xmm0, [rbp+rdi-40]");
    E("    movsd [rbp-512], xmm0");

    // Interpolate along short edges (v0-v1 or v1-v2)
    E(".ftpc_short:");
    E("    mov  rax, [rbp-456]");
    E("    cmp  rax, [rbp-392]");
    E("    jl   .ftpc_upper");

    // Lower half: v1 to v2
    E("    mov  rax, [rbp-400]");
    E("    sub  rax, [rbp-392]");
    E("    cvtsi2sd xmm7, rax");
    E("    xorpd xmm6, xmm6");
    E("    ucomisd xmm7, xmm6");
    E("    je   .ftpc_shortd");
    E("    mov  rcx, [rbp-456]");
    E("    sub  rcx, [rbp-392]");
    E("    cvtsi2sd xmm5, rcx");
    E("    divsd xmm5, xmm7");

    E("    mov  rcx, [rbp-424]");
    E("    sub  rcx, [rbp-416]");
    E("    cvtsi2sd xmm0, rcx");
    E("    mulsd xmm0, xmm5");
    E("    cvtsi2sd xmm1, qword [rbp-416]");
    E("    addsd xmm0, xmm1");
    E("    cvttsd2si r15, xmm0");  // x_short in r15

    E("    mov  rdi, [rbp-440]");
    E("    neg  rdi");
    E("    mov  rsi, [rbp-448]");
    E("    neg  rsi");
    E("    jmp  .ftpc_shortattr");

    // Upper half: v0 to v1
    E(".ftpc_upper:");
    E("    mov  rax, [rbp-392]");
    E("    sub  rax, [rbp-384]");
    E("    cvtsi2sd xmm7, rax");
    E("    xorpd xmm6, xmm6");
    E("    ucomisd xmm7, xmm6");
    E("    je   .ftpc_shortd");
    E("    mov  rcx, [rbp-456]");
    E("    sub  rcx, [rbp-384]");
    E("    cvtsi2sd xmm5, rcx");
    E("    divsd xmm5, xmm7");

    E("    mov  rcx, [rbp-416]");
    E("    sub  rcx, [rbp-408]");
    E("    cvtsi2sd xmm0, rcx");
    E("    mulsd xmm0, xmm5");
    E("    cvtsi2sd xmm1, qword [rbp-408]");
    E("    addsd xmm0, xmm1");
    E("    cvttsd2si r15, xmm0");

    E("    mov  rdi, [rbp-432]");
    E("    neg  rdi");
    E("    mov  rsi, [rbp-440]");
    E("    neg  rsi");

    E(".ftpc_shortattr:");
    // Interpolate short edge attributes
    E("    movsd xmm0, [rbp+rsi]");
    E("    subsd xmm0, [rbp+rdi]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp+rdi]");
    E("    movsd [rbp-520], xmm0");  // 1/z_short
    E("    movsd xmm0, [rbp+rsi-8]");
    E("    subsd xmm0, [rbp+rdi-8]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp+rdi-8]");
    E("    movsd [rbp-528], xmm0");  // u/z_short
    E("    movsd xmm0, [rbp+rsi-16]");
    E("    subsd xmm0, [rbp+rdi-16]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp+rdi-16]");
    E("    movsd [rbp-536], xmm0");  // v/z_short
    E("    movsd xmm0, [rbp+rsi-24]");
    E("    subsd xmm0, [rbp+rdi-24]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp+rdi-24]");
    E("    movsd [rbp-544], xmm0");  // r/z_short
    E("    movsd xmm0, [rbp+rsi-32]");
    E("    subsd xmm0, [rbp+rdi-32]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp+rdi-32]");
    E("    movsd [rbp-552], xmm0");  // g/z_short
    E("    movsd xmm0, [rbp+rsi-40]");
    E("    subsd xmm0, [rbp+rdi-40]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp+rdi-40]");
    E("    movsd [rbp-560], xmm0");  // b/z_short
    E("    jmp  .ftpc_xsetup");

    E(".ftpc_shortd:");  // degenerate short edge
    E("    mov  r15, [rbp-408]");
    E("    mov  rdi, [rbp-432]");
    E("    neg  rdi");
    E("    movsd xmm0, [rbp+rdi]");
    E("    movsd [rbp-520], xmm0");
    E("    movsd xmm0, [rbp+rdi-8]");
    E("    movsd [rbp-528], xmm0");
    E("    movsd xmm0, [rbp+rdi-16]");
    E("    movsd [rbp-536], xmm0");
    E("    movsd xmm0, [rbp+rdi-24]");
    E("    movsd [rbp-544], xmm0");
    E("    movsd xmm0, [rbp+rdi-32]");
    E("    movsd [rbp-552], xmm0");
    E("    movsd xmm0, [rbp+rdi-40]");
    E("    movsd [rbp-560], xmm0");

    E(".ftpc_xsetup:");
    // Ensure x_left <= x_right
    E("    cmp  r13, r15");
    E("    jle  .ftpc_xok");
    E("    xchg r13, r15");
    // Swap all the /z values
    E("    movsd xmm0, [rbp-472]");
    E("    movsd xmm1, [rbp-520]");
    E("    movsd [rbp-472], xmm1");
    E("    movsd [rbp-520], xmm0");
    E("    movsd xmm0, [rbp-480]");
    E("    movsd xmm1, [rbp-528]");
    E("    movsd [rbp-480], xmm1");
    E("    movsd [rbp-528], xmm0");
    E("    movsd xmm0, [rbp-488]");
    E("    movsd xmm1, [rbp-536]");
    E("    movsd [rbp-488], xmm1");
    E("    movsd [rbp-536], xmm0");
    E("    movsd xmm0, [rbp-496]");
    E("    movsd xmm1, [rbp-544]");
    E("    movsd [rbp-496], xmm1");
    E("    movsd [rbp-544], xmm0");
    E("    movsd xmm0, [rbp-504]");
    E("    movsd xmm1, [rbp-552]");
    E("    movsd [rbp-504], xmm1");
    E("    movsd [rbp-552], xmm0");
    E("    movsd xmm0, [rbp-512]");
    E("    movsd xmm1, [rbp-560]");
    E("    movsd [rbp-512], xmm1");
    E("    movsd [rbp-560], xmm0");

    E(".ftpc_xok:");
    // Clamp x to screen
    E("    cmp  r13, 0");
    E("    jge  .ftpc_xl");
    E("    xor  r13, r13");
    E(".ftpc_xl:");
    E("    mov  rax, [rbx + WSTATE_WIDTH]");
    E("    dec  rax");
    E("    cmp  r15, rax");
    E("    jle  .ftpc_xr");
    E("    mov  r15, rax");
    E(".ftpc_xr:");

    // Compute row offset
    E("    mov  rax, [rbp-456]");
    E("    imul rax, [rbx + WSTATE_WIDTH]");
    E("    mov  r14, rax");  // row offset in r14

    // x span width
    E("    mov  rax, r15");
    E("    sub  rax, r13");
    E("    cvtsi2sd xmm7, rax");  // span width

    // Pixel loop
    E("    mov  [rbp-568], r13");  // current x
    E(".ftpc_px:");
    E("    mov  rax, [rbp-568]");
    E("    cmp  rax, r15");
    E("    jg   .ftpc_pxd");

    // Interpolate across scanline
    E("    mov  rcx, [rbp-568]");
    E("    sub  rcx, r13");
    E("    cvtsi2sd xmm5, rcx");
    E("    xorpd xmm6, xmm6");
    E("    ucomisd xmm7, xmm6");
    E("    je   .ftpc_uvd");
    E("    divsd xmm5, xmm7");  // t = (x - x_left) / span

    // Interpolate 1/z
    E("    movsd xmm0, [rbp-520]");
    E("    subsd xmm0, [rbp-472]");
    E("    mulsd xmm0, xmm5");
    E("    addsd xmm0, [rbp-472]");  // 1/z

    // Interpolate u/z, v/z and divide by 1/z
    E("    movsd xmm1, [rbp-528]");
    E("    subsd xmm1, [rbp-480]");
    E("    mulsd xmm1, xmm5");
    E("    addsd xmm1, [rbp-480]");
    E("    divsd xmm1, xmm0");  // u
    E("    movsd xmm2, [rbp-536]");
    E("    subsd xmm2, [rbp-488]");
    E("    mulsd xmm2, xmm5");
    E("    addsd xmm2, [rbp-488]");
    E("    divsd xmm2, xmm0");  // v

    // Interpolate r/z, g/z, b/z and divide by 1/z
    E("    movsd xmm3, [rbp-544]");
    E("    subsd xmm3, [rbp-496]");
    E("    mulsd xmm3, xmm5");
    E("    addsd xmm3, [rbp-496]");
    E("    divsd xmm3, xmm0");  // r
    E("    movsd xmm4, [rbp-552]");
    E("    subsd xmm4, [rbp-504]");
    E("    mulsd xmm4, xmm5");
    E("    addsd xmm4, [rbp-504]");
    E("    divsd xmm4, xmm0");  // g
    E("    movsd xmm8, [rbp-560]");
    E("    subsd xmm8, [rbp-512]");
    E("    mulsd xmm8, xmm5");
    E("    addsd xmm8, [rbp-512]");
    E("    divsd xmm8, xmm0");  // b

    // Convert to int
    E("    cvttsd2si r8, xmm1");   // u
    E("    cvttsd2si r9, xmm2");   // v
    E("    cvttsd2si r10, xmm3");  // r
    E("    cvttsd2si r11, xmm4");  // g
    E("    cvttsd2si rdi, xmm8");  // b (use rdi temp)
    E("    jmp  .ftpc_sample");

    E(".ftpc_uvd:");  // degenerate span
    E("    movsd xmm0, [rbp-472]");
    E("    movsd xmm1, [rbp-480]");
    E("    divsd xmm1, xmm0");
    E("    movsd xmm2, [rbp-488]");
    E("    divsd xmm2, xmm0");
    E("    movsd xmm3, [rbp-496]");
    E("    divsd xmm3, xmm0");
    E("    movsd xmm4, [rbp-504]");
    E("    divsd xmm4, xmm0");
    E("    movsd xmm8, [rbp-512]");
    E("    divsd xmm8, xmm0");
    E("    cvttsd2si r8, xmm1");
    E("    cvttsd2si r9, xmm2");
    E("    cvttsd2si r10, xmm3");
    E("    cvttsd2si r11, xmm4");
    E("    cvttsd2si rdi, xmm8");

    E(".ftpc_sample:");
    // Clamp UV
    E("    cmp  r8, 0");
    E("    jge  .ftpc_u0");
    E("    xor  r8, r8");
    E(".ftpc_u0:");
    E("    mov  rax, [rbp-24]");
    E("    dec  rax");
    E("    cmp  r8, rax");
    E("    jle  .ftpc_u1");
    E("    mov  r8, rax");
    E(".ftpc_u1:");
    E("    cmp  r9, 0");
    E("    jge  .ftpc_v0");
    E("    xor  r9, r9");
    E(".ftpc_v0:");
    E("    mov  rax, [rbp-32]");
    E("    dec  rax");
    E("    cmp  r9, rax");
    E("    jle  .ftpc_v1");
    E("    mov  r9, rax");
    E(".ftpc_v1:");

    // Clamp vertex colors to 0-255
    E("    cmp  r10, 0");
    E("    jge  .ftpc_r0");
    E("    xor  r10, r10");
    E(".ftpc_r0:");
    E("    cmp  r10, 255");
    E("    jle  .ftpc_r1");
    E("    mov  r10, 255");
    E(".ftpc_r1:");
    E("    cmp  r11, 0");
    E("    jge  .ftpc_g0");
    E("    xor  r11, r11");
    E(".ftpc_g0:");
    E("    cmp  r11, 255");
    E("    jle  .ftpc_g1");
    E("    mov  r11, 255");
    E(".ftpc_g1:");
    E("    cmp  rdi, 0");
    E("    jge  .ftpc_b0");
    E("    xor  rdi, rdi");
    E(".ftpc_b0:");
    E("    cmp  rdi, 255");
    E("    jle  .ftpc_b1");
    E("    mov  rdi, 255");
    E(".ftpc_b1:");

    // Sample texture (RGB565)
    E("    mov  rax, r9");
    E("    imul rax, [rbp-24]");
    E("    add  rax, r8");
    E("    shl  rax, 1");
    E("    add  rax, [rbp-16]");
    E("    movzx rsi, word [rax]");  // RGB565 pixel

    // Extract and expand RGB565 to 8-bit
    E("    mov  rax, rsi");
    E("    shr  rax, 11");
    E("    and  rax, 0x1F");
    E("    shl  rax, 3");        // tex_r (0-248)
    E("    mov  rcx, rsi");
    E("    shr  rcx, 5");
    E("    and  rcx, 0x3F");
    E("    shl  rcx, 2");        // tex_g (0-252)
    E("    mov  rdx, rsi");
    E("    and  rdx, 0x1F");
    E("    shl  rdx, 3");        // tex_b (0-248)

    // Multiply texture by vertex color: (tex * vert) / 256
    E("    imul rax, r10");
    E("    shr  rax, 8");        // final_r
    E("    imul rcx, r11");
    E("    shr  rcx, 8");        // final_g
    E("    imul rdx, rdi");
    E("    shr  rdx, 8");        // final_b

    // Write pixel (BGRA)
    E("    mov  rsi, r14");
    E("    add  rsi, [rbp-568]");
    E("    shl  rsi, 2");
    E("    add  rsi, [rbx + WSTATE_PIXELS]");
    E("    mov  byte [rsi], dl");     // B
    E("    mov  byte [rsi+1], cl");   // G
    E("    mov  byte [rsi+2], al");   // R
    E("    mov  byte [rsi+3], 0xFF"); // A

    E("    inc  qword [rbp-568]");
    E("    jmp  .ftpc_px");

    E(".ftpc_pxd:");
    E("    inc  qword [rbp-456]");
    E("    jmp  .ftpc_scan");

    E(".ftpc_done:");
    E("    add  rsp, 576");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbx");
    E("    pop  rbp");
    E("    ret");
    E("");
}

// ---------------------------------------------------------------------
// Default (stub) event handlers — user on { } blocks override these
// ---------------------------------------------------------------------
static void emit_default_event_handlers(Codegen *cg, const EventHandlerFlags *flags) {
    E("; --- default event handler stubs ---");
    E("; Only emitted for handlers the user program does not define.");
    if (!flags->has_key_down) {
        E("_slag_on_key_down:");
        E("    ret");
    }
    if (!flags->has_key_up) {
        E("_slag_on_key_up:");
        E("    ret");
    }
    if (!flags->has_mouse_move) {
        E("_slag_on_mouse_move:");
        E("    ret");
    }
    if (!flags->has_mouse_down) {
        E("_slag_on_mouse_down:");
        E("    ret");
    }
    if (!flags->has_mouse_up) {
        E("_slag_on_mouse_up:");
        E("    ret");
    }
    if (!flags->has_mouse_wheel) {
        E("_slag_on_mouse_wheel:");
        E("    ret");
    }
    E("");
}

// ---------------------------------------------------------------------
// .data additions for window subsystem
// ---------------------------------------------------------------------
void emit_window_data(Codegen *cg) {
    E("_window_class_name: db \"SlagWindow\", 0");
    E("_window_tls_index:  dq 0   ; TLS slot for per-thread window state");
    E("_window_tls_init:   dq 0   ; 1 if TLS has been initialized");
    E("");
}

// ---------------------------------------------------------------------
// .bss additions for window subsystem
// ---------------------------------------------------------------------
void emit_window_bss(Codegen *cg) {
    E("; Per-window state is now in TLS-allocated structs");
    E("; Only shared input state remains global");
    E("");
    E("; --- TLS cache for fast rendering (set by render_begin) ---");
    E("_window_cached_state: resq 1   ; cached TLS struct ptr for rendering");
    E("");
    E("; --- shared input state (written by event handlers, read by user code) ---");
    E("_input_drag_x:       resq 1   ; accumulated drag offset x");
    E("_input_drag_y:       resq 1   ; accumulated drag offset y");
    E("_input_dragging:     resq 1   ; 1 while left button held");
    E("_input_last_x:       resq 1   ; last mouse x (for delta calc)");
    E("_input_last_y:       resq 1   ; last mouse y");
    E("_input_wheel:        resq 1   ; accumulated wheel delta");
    E("_input_bbox_minx:    resq 1");
    E("_input_bbox_miny:    resq 1");
    E("_input_bbox_maxx:    resq 1");
    E("_input_bbox_maxy:    resq 1");
    E("");
    E("; --- native resolution string buffer ---");
    E("_native_res_buf:     resb 24");
    E("");
}

// ---------------------------------------------------------------------
// Import additions for window subsystem
// ---------------------------------------------------------------------
void emit_window_imports(Codegen *cg) {
    E("extern GetModuleHandleA");
    E("extern RegisterClassExA");
    E("extern CreateWindowExA");
    E("extern AdjustWindowRectEx");
    E("extern ShowWindow");
    E("extern UpdateWindow");
    E("extern GetMessageA");
    E("extern TranslateMessage");
    E("extern DispatchMessageA");
    E("extern DefWindowProcA");
    E("extern PostQuitMessage");
    E("extern PostMessageA");
    E("extern LoadCursorA");
    E("extern DestroyWindow");
    E("extern Sleep");
    E("extern GetTickCount");
    E("extern GetDC");
    E("extern CreateCompatibleDC");
    E("extern CreateDIBSection");
    E("extern SelectObject");
    E("extern BitBlt");
    E("extern ValidateRect");
    E("extern CreateEventA");
    E("extern SetEvent");
    E("extern WaitForSingleObject");
    E("; --- TLS functions ---");
    E("extern TlsAlloc");
    E("extern TlsGetValue");
    E("extern TlsSetValue");
    E("; --- Heap functions ---");
    E("extern GetProcessHeap");
    E("extern HeapAlloc");
    E("; --- Window user data ---");
    E("extern SetWindowLongPtrA");
    E("extern GetWindowLongPtrA");
    E("GWLP_USERDATA equ -21");
    E("; --- Mouse capture ---");
    E("extern SetCapture");
    E("extern ReleaseCapture");
    E("extern ClipCursor");
    E("extern ShowCursor");
    E("extern GetClientRect");
    E("extern ClientToScreen");
    E("extern SetCursorPos");
    E("; --- Screen info ---");
    E("extern GetSystemMetrics");
    E("");
}

// ---------------------------------------------------------------------
// Top-level emitter
// ---------------------------------------------------------------------
void emit_window_runtime(Codegen *cg, const EventHandlerFlags *flags) {
    emit_window_constants(cg);
    emit_window_open(cg);
    emit_window_thread_proc(cg);
    emit_wndproc(cg);
    emit_window_utils(cg);
    emit_fill_triangle_pcolor(cg);
    emit_default_event_handlers(cg, flags);
}
