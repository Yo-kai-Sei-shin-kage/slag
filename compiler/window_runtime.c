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
// 1. Saves w/h into _window_width/_window_height
// 2. Creates a Win32 manual-reset event (_window_ready_event)
// 3. Spawns _slag_window_thread_proc on a new thread
// 4. Waits on _window_ready_event (window is up and DIB is allocated)
// 5. Returns
// ---------------------------------------------------------------------
static void emit_window_open(Codegen *cg) {
    E("; --- _slag_window_open(w, h, title_ptr, title_len) ---");
    E("_slag_window_open:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push r12          ; w");
    E("    push r13          ; h");
    E("    push r14          ; title_ptr");
    E("    push r15          ; title_len");
    E("    sub  rsp, 64");
    E("");
    E("    mov  r12, rcx");
    E("    mov  r13, rdx");
    E("    mov  r14, r8");
    E("    mov  r15, r9");
    E("");
    E("    ; store dimensions");
    E("    mov  [_window_width],  r12");
    E("    mov  [_window_height], r13");
    E("    mov  [_window_title],  r14");
    E("");
    E("    ; create manual-reset event (initially unsignaled)");
    E("    xor  rcx, rcx          ; lpEventAttributes = NULL");
    E("    mov  rdx, 1            ; bManualReset = TRUE");
    E("    xor  r8,  r8           ; bInitialState = FALSE");
    E("    xor  r9,  r9           ; lpName = NULL");
    E("    sub  rsp, 32");
    E("    call CreateEventA");
    E("    add  rsp, 32");
    E("    mov  [_window_ready_event], rax");
    E("");
    E("    ; spawn window thread");
    E("    xor  rcx, rcx          ; lpThreadAttributes = NULL");
    E("    xor  rdx, rdx          ; dwStackSize = default");
    E("    lea  r8,  [_slag_window_thread_proc]");
    E("    xor  r9,  r9           ; lpParameter = NULL");
    E("    sub  rsp, 48");
    E("    mov  qword [rsp+32], 0 ; dwCreationFlags = 0 (run immediately)");
    E("    mov  qword [rsp+40], 0 ; lpThreadId = NULL");
    E("    call CreateThread");
    E("    add  rsp, 48");
    E("    mov  [_window_thread], rax");
    E("");
    E("    ; wait for window to be ready");
    E("    mov  rcx, [_window_ready_event]");
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
// _slag_window_thread_proc
//
// Runs on a dedicated thread. Registers WNDCLASSEX, creates the window,
// creates the DIB section, signals _window_ready_event, then runs the
// Win32 message loop until WM_DESTROY.
// ---------------------------------------------------------------------
static void emit_window_thread_proc(Codegen *cg) {
    E("; --- _slag_window_thread_proc ---");
    E("_slag_window_thread_proc:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    push r15");
    E("    sub  rsp, 160          ; WNDCLASSEX(80) + MSG(48) + locals");
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

    // Create window
    E("    ; --- CreateWindowEx ---");
    E("    mov  rcx, WS_EX_APPWINDOW     ; dwExStyle");
    E("    lea  rdx, [_window_class_name] ; lpClassName");
    E("    mov  r8,  [_window_title]      ; lpWindowName");
    E("    mov  r9,  WS_OVERLAPPEDWINDOW  ; dwStyle");
    E("    sub  rsp, 96");
    E("    mov  qword [rsp+32], 100       ; x");
    E("    mov  qword [rsp+40], 100       ; y");
    E("    mov  rax,  [_window_width]");
    E("    mov  [rsp+48], rax             ; nWidth");
    E("    mov  rax,  [_window_height]");
    E("    mov  [rsp+56], rax             ; nHeight");
    E("    mov  qword [rsp+64], 0         ; hWndParent = NULL");
    E("    mov  qword [rsp+72], 0         ; hMenu = NULL");
    E("    mov  [rsp+80], r12             ; hInstance");
    E("    mov  qword [rsp+88], 0         ; lpParam = NULL");
    E("    call CreateWindowExA");
    E("    add  rsp, 96");
    E("    mov  r13, rax                  ; save HWND");
    E("    mov  [_window_hwnd], rax");
    E("    mov  qword [_window_open], 1");
    E("");

    // Create DIB section
    E("    ; --- CreateDIBSection ---");
    E("    ; Build BITMAPINFOHEADER on stack");
    E("    sub  rsp, 48                   ; BITMAPINFOHEADER = 40 bytes + pad");
    E("    mov  dword [rsp+0],  40        ; biSize");
    E("    mov  rax, [_window_width]");
    E("    mov  dword [rsp+4],  eax       ; biWidth");
    E("    mov  rax, [_window_height]");
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
    E("    mov  [_window_hdc], rax");
    E("    ; CreateCompatibleDC");
    E("    mov  rcx, r14");
    E("    sub  rsp, 32");
    E("    call CreateCompatibleDC");
    E("    add  rsp, 32");
    E("    mov  r15, rax                  ; save memDC");
    E("    mov  [_window_memdc], rax");
    E("    ; CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &pixels, NULL, 0)");
    E("    mov  rcx, r15                  ; hdc = memDC");
    E("    lea  rdx, [rsp]                ; pbmi (our BITMAPINFOHEADER at rsp)");
    E("    mov  r8,  DIB_RGB_COLORS");
    E("    lea  r9,  [_window_pixels]     ; ppvBits — filled by CreateDIBSection");
    E("    sub  rsp, 48");
    E("    mov  qword [rsp+32], 0         ; hSection = NULL");
    E("    mov  qword [rsp+40], 0         ; dwOffset = 0");
    E("    call CreateDIBSection");
    E("    add  rsp, 48");
    E("    mov  [_window_hbitmap], rax");
    E("    ; SelectObject(memDC, hbitmap)");
    E("    mov  rcx, r15");
    E("    mov  rdx, rax");
    E("    sub  rsp, 32");
    E("    call SelectObject");
    E("    add  rsp, 32");
    E("    add  rsp, 48                   ; free BITMAPINFOHEADER space");
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
    E("    ; signal _window_ready_event");
    E("    mov  rcx, [_window_ready_event]");
    E("    sub  rsp, 32");
    E("    call SetEvent");
    E("    add  rsp, 32");
    E("");

    // Message loop
    E("    ; --- message loop ---");
    E(".msg_loop:");
    E("    lea  rcx, [_window_msg]        ; lpMsg");
    E("    xor  rdx, rdx                  ; hWnd = NULL (all messages)");
    E("    xor  r8,  r8                   ; wMsgFilterMin = 0");
    E("    xor  r9,  r9                   ; wMsgFilterMax = 0");
    E("    sub  rsp, 32");
    E("    call GetMessageA");
    E("    add  rsp, 32");
    E("    test rax, rax");
    E("    jz   .msg_done");
    E("    lea  rcx, [_window_msg]");
    E("    sub  rsp, 32");
    E("    call TranslateMessage");
    E("    add  rsp, 32");
    E("    lea  rcx, [_window_msg]");
    E("    sub  rsp, 32");
    E("    call DispatchMessageA");
    E("    add  rsp, 32");
    E("    jmp  .msg_loop");
    E(".msg_done:");
    E("    mov  qword [_window_open], 0");
    E("");
    E("    lea  rsp, [rbp-32]");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbp");
    E("    xor  rax, rax");
    E("    ret");
    E("");
}

// ---------------------------------------------------------------------
// _slag_wndproc(HWND, UINT msg, WPARAM, LPARAM)
//
// Handles:
//   WM_DESTROY   — post quit, clear _window_open
//   WM_PAINT     — BitBlt DIB to window
//   WM_USER_FLUSH— BitBlt DIB to window (from window.flush())
//   WM_KEYDOWN   — call _slag_on_key_down(wParam)
//   WM_KEYUP     — call _slag_on_key_up(wParam)
//   WM_MOUSEMOVE — call _slag_on_mouse_move(x, y)
//   WM_LBUTTONDOWN etc. — call _slag_on_mouse_down/up(button, x, y)
// ---------------------------------------------------------------------
static void emit_wndproc(Codegen *cg) {
    E("; --- _slag_wndproc ---");
    E("_slag_wndproc:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push r12          ; hwnd");
    E("    push r13          ; msg");
    E("    push r14          ; wParam");
    E("    push r15          ; lParam");
    E("    sub  rsp, 64");
    E("");
    E("    mov  r12, rcx     ; hwnd");
    E("    mov  r13, rdx     ; msg");
    E("    mov  r14, r8      ; wParam");
    E("    mov  r15, r9      ; lParam");
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
    E("    jmp  .wndproc_default");
    E("");

    // WM_DESTROY
    E(".wndproc_destroy:");
    E("    mov  qword [_window_open], 0");
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
    E("    mov  rcx, [_window_hdc]");
    E("    xor  rdx, rdx              ; x=0");
    E("    xor  r8,  r8               ; y=0");
    E("    mov  r9,  [_window_width]");
    E("    sub  rsp, 80");
    E("    mov  rax, [_window_height]");
    E("    mov  [rsp+32], rax         ; nHeight");
    E("    mov  rax, [_window_memdc]");
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
    E("    movsx rcx, word [rbp-24]   ; x = low word of lParam (r15)");
    E("    movsx rdx, word [rbp-22]   ; y = high word of lParam");
    E("    ; extract from r15 directly");
    E("    mov  rcx, r15");
    E("    movsx rdx, cx              ; low 16 = x");
    E("    shr  rcx, 16");
    E("    movsx rcx, cx              ; high 16 = y");
    E("    ; swap: rdx=x, rcx=y — reorder for call(x,y)");
    E("    xchg rcx, rdx");
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
// _slag_window_flush() — posts WM_USER_FLUSH to window thread
// _slag_window_close() — posts WM_CLOSE to window
// _slag_pixel(x, y, r, g, b) — writes BGRA pixel to DIB buffer
// ---------------------------------------------------------------------
static void emit_window_utils(Codegen *cg) {
    // window.flush()
    E("; --- _slag_window_flush ---");
    E("_slag_window_flush:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48");
    E("    mov  rcx, [_window_hwnd]");
    E("    mov  rdx, WM_USER_FLUSH");
    E("    xor  r8,  r8");
    E("    xor  r9,  r9");
    E("    sub  rsp, 32");
    E("    call PostMessageA");
    E("    add  rsp, 32");
    E("    ; sleep ~16ms to yield to window thread and cap frame rate (~60fps)");
    E("    mov  rcx, 16");
    E("    sub  rsp, 32");
    E("    call Sleep");
    E("    add  rsp, 32");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // window.close()
    E("; --- _slag_window_close ---");
    E("_slag_window_close:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48");
    E("    mov  rcx, [_window_hwnd]");
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

    // pixel(x, y, r, g, b)
    // rcx=x, rdx=y, r8=r, r9=g, [rsp+32]=b
    // BGRA format: byte order in memory is B, G, R, A
    // offset = (y * width + x) * 4
    E("; --- _slag_pixel(x, y, r, g, b) ---");
    E("_slag_pixel:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48");
    E("");
    E("    ; bounds check: 0 <= x < width, 0 <= y < height");
    E("    cmp  rcx, 0");
    E("    jl   .pixel_done");
    E("    cmp  rcx, [_window_width]");
    E("    jge  .pixel_done");
    E("    cmp  rdx, 0");
    E("    jl   .pixel_done");
    E("    cmp  rdx, [_window_height]");
    E("    jge  .pixel_done");
    E("");
    E("    ; compute offset = (y * width + x) * 4");
    E("    mov  rax, rdx              ; rax = y");
    E("    imul rax, [_window_width]  ; rax = y * width");
    E("    add  rax, rcx              ; rax = y * width + x");
    E("    shl  rax, 2                ; rax *= 4 (BGRA)");
    E("");
    E("    mov  r10, [_window_pixels] ; base ptr");
    E("    add  r10, rax              ; ptr to pixel");
    E("");
    E("    ; b = 5th arg, placed at [rbp+48] by caller (after 32-byte shadow space)");
    E("    mov  rax, [rbp+48]         ; b (5th arg)");
    E("    mov  byte [r10+0], al      ; B");
    E("    mov  byte [r10+1], r9b     ; G");
    E("    mov  byte [r10+2], r8b     ; R");
    E("    mov  byte [r10+3], 0xFF    ; A = opaque");
    E("");
    E(".pixel_done:");
    E("    mov  rsp, rbp");
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
    // -------------------------------------------------------------
    E("; --- _slag_fill_triangle(x0,y0,x1,y1,x2,y2,r,g,b) ---");
    E("_slag_fill_triangle:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 160");
    E("    mov  [rbp-8],  rcx   ; x0");
    E("    mov  [rbp-16], rdx   ; y0");
    E("    mov  [rbp-24], r8    ; x1");
    E("    mov  [rbp-32], r9    ; y1");
    E("    mov  rax, [rbp+48]");
    E("    mov  [rbp-40], rax   ; x2");
    E("    mov  rax, [rbp+56]");
    E("    mov  [rbp-48], rax   ; y2");
    E("    mov  rax, [rbp+64]");
    E("    mov  [rbp-56], rax   ; r");
    E("    mov  rax, [rbp+72]");
    E("    mov  [rbp-64], rax   ; g");
    E("    mov  rax, [rbp+80]");
    E("    mov  [rbp-72], rax   ; b");
    E("");
    E("    ; --- sort the 3 (x,y) pairs by y ascending: (xa,ya) (xb,yb) (xc,yc) ---");
    E("    mov  rax, [rbp-8]");
    E("    mov  [rbp-104], rax  ; xa = x0");
    E("    mov  rax, [rbp-16]");
    E("    mov  [rbp-80], rax   ; ya = y0");
    E("    mov  rax, [rbp-24]");
    E("    mov  [rbp-112], rax  ; xb = x1");
    E("    mov  rax, [rbp-32]");
    E("    mov  [rbp-88], rax   ; yb = y1");
    E("    mov  rax, [rbp-40]");
    E("    mov  [rbp-120], rax  ; xc = x2");
    E("    mov  rax, [rbp-48]");
    E("    mov  [rbp-96], rax   ; yc = y2");
    E("");
    E("    ; sort pairs (a,b,c) by y using simple compare-and-swap network");
    E("    ; if ya > yb: swap a,b");
    E("    mov  rax, [rbp-80]");
    E("    cmp  rax, [rbp-88]");
    E("    jle  .ft_sort_ab_done");
    E("    mov  rax, [rbp-80]");
    E("    mov  rcx, [rbp-88]");
    E("    mov  [rbp-80], rcx");
    E("    mov  [rbp-88], rax");
    E("    mov  rax, [rbp-104]");
    E("    mov  rcx, [rbp-112]");
    E("    mov  [rbp-104], rcx");
    E("    mov  [rbp-112], rax");
    E(".ft_sort_ab_done:");
    E("    ; if yb > yc: swap b,c");
    E("    mov  rax, [rbp-88]");
    E("    cmp  rax, [rbp-96]");
    E("    jle  .ft_sort_bc_done");
    E("    mov  rax, [rbp-88]");
    E("    mov  rcx, [rbp-96]");
    E("    mov  [rbp-88], rcx");
    E("    mov  [rbp-96], rax");
    E("    mov  rax, [rbp-112]");
    E("    mov  rcx, [rbp-120]");
    E("    mov  [rbp-112], rcx");
    E("    mov  [rbp-120], rax");
    E(".ft_sort_bc_done:");
    E("    ; if ya > yb (again, after possible swap): swap a,b");
    E("    mov  rax, [rbp-80]");
    E("    cmp  rax, [rbp-88]");
    E("    jle  .ft_sort_ab2_done");
    E("    mov  rax, [rbp-80]");
    E("    mov  rcx, [rbp-88]");
    E("    mov  [rbp-80], rcx");
    E("    mov  [rbp-88], rax");
    E("    mov  rax, [rbp-104]");
    E("    mov  rcx, [rbp-112]");
    E("    mov  [rbp-104], rcx");
    E("    mov  [rbp-112], rax");
    E(".ft_sort_ab2_done:");
    E("");
    E("    ; clamp scanline range to framebuffer height");
    E("    mov  rax, [rbp-80]   ; ya");
    E("    cmp  rax, 0");
    E("    jge  .ft_ystart_ok");
    E("    mov  rax, 0");
    E(".ft_ystart_ok:");
    E("    mov  [rbp-128], rax  ; y = max(ya, 0)");
    E("");
    E("    mov  r12, [rbp-96]   ; yc (loop end, exclusive of clamp below)");
    E("    mov  rax, [_window_height]");
    E("    dec  rax");
    E("    cmp  r12, rax");
    E("    jle  .ft_yend_ok");
    E("    mov  r12, rax");
    E(".ft_yend_ok:");
    E("");
    E(".ft_scanline_loop:");
    E("    mov  rax, [rbp-128]");
    E("    cmp  rax, r12");
    E("    jg   .ft_done");
    E("");
    E("    ; --- compute xLong: x along edge (xa,ya)-(xc,yc) at current y ---");
    E("    ; xLong = xa + (xc-xa) * (y-ya) / (yc-ya)   (guard yc==ya)");
    E("    mov  rax, [rbp-96]");
    E("    sub  rax, [rbp-80]   ; denom = yc - ya");
    E("    cmp  rax, 0");
    E("    je   .ft_long_eq");
    E("    mov  rcx, [rbp-120]");
    E("    sub  rcx, [rbp-104]  ; xc - xa");
    E("    mov  r10, [rbp-128]");
    E("    sub  r10, [rbp-80]   ; y - ya");
    E("    imul rcx, r10        ; (xc-xa)*(y-ya)");
    E("    mov  r11, rax        ; save denom in r11 for idiv");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r11");
    E("    add  rax, [rbp-104]  ; xLong = xa + result");
    E("    jmp  .ft_long_done");
    E(".ft_long_eq:");
    E("    mov  rax, [rbp-104]  ; degenerate: xLong = xa");
    E(".ft_long_done:");
    E("    mov  [rbp-136], rax  ; stash xLong temporarily in xleft slot");
    E("");
    E("    ; --- compute xShort: depends on whether y < yb (upper) or >= yb (lower) ---");
    E("    mov  rax, [rbp-128]");
    E("    cmp  rax, [rbp-88]");
    E("    jl   .ft_short_upper");
    E("");
    E("    ; lower segment: edge (xb,yb)-(xc,yc)");
    E("    mov  rax, [rbp-96]");
    E("    sub  rax, [rbp-88]   ; denom = yc - yb");
    E("    cmp  rax, 0");
    E("    je   .ft_short_lower_eq");
    E("    mov  rcx, [rbp-120]");
    E("    sub  rcx, [rbp-112]  ; xc - xb");
    E("    mov  r10, [rbp-128]");
    E("    sub  r10, [rbp-88]   ; y - yb");
    E("    imul rcx, r10");
    E("    mov  r11, rax");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r11");
    E("    add  rax, [rbp-112]  ; xShort = xb + result");
    E("    jmp  .ft_short_done");
    E(".ft_short_lower_eq:");
    E("    mov  rax, [rbp-112]");
    E("    jmp  .ft_short_done");
    E("");
    E(".ft_short_upper:");
    E("    ; upper segment: edge (xa,ya)-(xb,yb)");
    E("    mov  rax, [rbp-88]");
    E("    sub  rax, [rbp-80]   ; denom = yb - ya");
    E("    cmp  rax, 0");
    E("    je   .ft_short_upper_eq");
    E("    mov  rcx, [rbp-112]");
    E("    sub  rcx, [rbp-104]  ; xb - xa");
    E("    mov  r10, [rbp-128]");
    E("    sub  r10, [rbp-80]   ; y - ya");
    E("    imul rcx, r10");
    E("    mov  r11, rax");
    E("    mov  rax, rcx");
    E("    cqo");
    E("    idiv r11");
    E("    add  rax, [rbp-104]  ; xShort = xa + result");
    E("    jmp  .ft_short_done");
    E(".ft_short_upper_eq:");
    E("    mov  rax, [rbp-104]");
    E(".ft_short_done:");
    E("    mov  [rbp-144], rax  ; xShort");
    E("");
    E("    ; xleft = min(xLong, xShort), xright = max(...)");
    E("    mov  rax, [rbp-136]  ; xLong");
    E("    mov  rcx, [rbp-144]  ; xShort");
    E("    cmp  rax, rcx");
    E("    jle  .ft_minmax_ok");
    E("    ; swap so rax=min, rcx=max");
    E("    mov  r10, rax");
    E("    mov  rax, rcx");
    E("    mov  rcx, r10");
    E(".ft_minmax_ok:");
    E("    mov  [rbp-136], rax  ; xleft");
    E("    mov  [rbp-144], rcx  ; xright");
    E("");
    E("    ; clamp y to [0, height-1] for the pixel write (already loop-bounded,");
    E("    ; but guard anyway) and clamp x range to [0, width-1]");
    E("    mov  rax, [rbp-136]");
    E("    cmp  rax, 0");
    E("    jge  .ft_xleft_ok");
    E("    mov  rax, 0");
    E("    mov  [rbp-136], rax");
    E(".ft_xleft_ok:");
    E("    mov  rax, [rbp-144]");
    E("    mov  rcx, [_window_width]");
    E("    dec  rcx");
    E("    cmp  rax, rcx");
    E("    jle  .ft_xright_ok");
    E("    mov  [rbp-144], rcx");
    E(".ft_xright_ok:");
    E("");
    E("    ; fill span [xleft, xright] on row y");
    E("    ; row_offset = y * width * 4 ; pixel_offset = row_offset + x*4");
    E("    mov  rax, [rbp-128]  ; y");
    E("    imul rax, [_window_width]");
    E("    mov  r13, rax        ; r13 = y*width");
    E("    mov  rax, [rbp-136]  ; x = xleft");
    E("");
    E(".ft_span_loop:");
    E("    mov  rcx, [rbp-144]  ; xright");
    E("    cmp  rax, rcx");
    E("    jg   .ft_span_done");
    E("");
    E("    mov  r10, r13");
    E("    add  r10, rax        ; y*width + x");
    E("    shl  r10, 2");
    E("    add  r10, [_window_pixels]");
    E("    mov  rcx, [rbp-72]   ; b");
    E("    mov  byte [r10+0], cl");
    E("    mov  rcx, [rbp-64]   ; g");
    E("    mov  byte [r10+1], cl");
    E("    mov  rcx, [rbp-56]   ; r");
    E("    mov  byte [r10+2], cl");
    E("    mov  byte [r10+3], 0xFF");
    E("");
    E("    inc  rax");
    E("    jmp  .ft_span_loop");
    E(".ft_span_done:");
    E("");
    E("    mov  rax, [rbp-128]");
    E("    inc  rax");
    E("    mov  [rbp-128], rax");
    E("    jmp  .ft_scanline_loop");
    E("");
    E(".ft_done:");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");


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
    // -------------------------------------------------------------
    E("; --- _slag_fill_triangle_gradient(...) ---");
    E("_slag_fill_triangle_gradient:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 256");
    E("");
    E("    ; load all 3 vertices into A/B/C slots (unsorted initially)");
    E("    mov  [rbp-8],  rcx   ; A.x = x0");
    E("    mov  [rbp-16], rdx   ; A.y = y0");
    E("    mov  [rbp-24], r8    ; A.r = r0");
    E("    mov  [rbp-32], r9    ; A.g = g0");
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
    E("    mov  rax, [_window_height]");
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
    E("    mov  rcx, [_window_width]");
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
    E("    imul rax, [_window_width]");
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
    E("    add  r10, [_window_pixels]");
    E("    mov  rax, [rbp-240]");
    E("    mov  byte [r10+0], al  ; B");
    E("    mov  rax, [rbp-232]");
    E("    mov  byte [r10+1], al  ; G");
    E("    mov  rax, [rbp-224]");
    E("    mov  byte [r10+2], al  ; R");
    E("    mov  byte [r10+3], 0xFF");
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
    E("    mov  rsp, rbp");
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
    E("_window_title:      dq 0   ; set by _slag_window_open");
    E("_window_width:      dq 0");
    E("_window_height:     dq 0");
    E("_window_open:       dq 0   ; volatile: 1=open, 0=closed");
    E("");
}

// ---------------------------------------------------------------------
// .bss additions for window subsystem
// ---------------------------------------------------------------------
void emit_window_bss(Codegen *cg) {
    E("_window_hwnd:        resq 1");
    E("_window_hdc:         resq 1");
    E("_window_memdc:       resq 1");
    E("_window_hbitmap:     resq 1");
    E("_window_pixels:      resq 1   ; ptr filled by CreateDIBSection");
    E("_window_ready_event: resq 1");
    E("_window_thread:      resq 1");
    E("_window_msg:         resb 48  ; MSG struct");
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
}

// ---------------------------------------------------------------------
// Import additions for window subsystem
// ---------------------------------------------------------------------
void emit_window_imports(Codegen *cg) {
    E("extern GetModuleHandleA");
    E("extern RegisterClassExA");
    E("extern CreateWindowExA");
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
    emit_default_event_handlers(cg, flags);
}
