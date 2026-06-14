#ifndef WINDOW_RUNTIME_H
#define WINDOW_RUNTIME_H

#include "codegen_internal.h"

// ---------------------------------------------------------------------
// Window runtime emitter
//
// emit_window_runtime(cg) emits all window-related NASM procs and data
// into the output assembly file. Called once from codegen_program()
// before user functions are emitted.
//
// Architecture:
//   - _slag_window_open(w, h, title_ptr, title_len)
//       Spawns the window thread, waits for the window to be created,
//       then returns. Main thread continues immediately.
//
//   - _slag_window_thread_proc(lpParam)
//       Win32 thread proc. Registers window class, creates window,
//       signals creation complete, runs the message loop.
//
//   - _slag_window_flush()
//       Posts WM_USER to the window thread to trigger a BitBlt.
//       Returns immediately — does not wait for blit to complete.
//
//   - _slag_window_close()
//       Posts WM_DESTROY to the window HWND.
//
//   - _slag_pixel(x, y, r, g, b)
//       Writes a single BGRA pixel into the DIB framebuffer.
//       Pure memory write — no locking, no GDI calls.
//
// Shared globals (in .bss / .data):
//   _window_hwnd        — HWND of the created window
//   _window_open        — volatile flag: 1 = open, 0 = closed
//   _window_width       — framebuffer width in pixels
//   _window_height      — framebuffer height in pixels
//   _window_pixels      — pointer to DIB pixel buffer (BGRA, 4 bytes/px)
//   _window_ready_event — Win32 event handle signaled when window is up
//   _window_thread      — thread handle
//   _window_hdc         — window DC
//   _window_hbitmap     — DIB bitmap handle
//   _window_memdc       — memory DC for BitBlt
//
// User event handler hooks (weak labels — user code overrides):
//   _slag_on_key_down(keycode)
//   _slag_on_key_up(keycode)
//   _slag_on_mouse_move(x, y)
//   _slag_on_mouse_down(button, x, y)
//   _slag_on_mouse_up(button, x, y)
// ---------------------------------------------------------------------

void emit_window_runtime(Codegen *cg, const EventHandlerFlags *flags);
void emit_window_data(Codegen *cg);
void emit_window_bss(Codegen *cg);
void emit_window_imports(Codegen *cg);

#endif // WINDOW_RUNTIME_H
