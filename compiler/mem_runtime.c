// mem_runtime.c — raw memory/buffer primitives for Slag.
// Mirrors window_runtime.c / net_runtime.c: emits NASM asm via cg_emit.
//
// Builtins (wired in codegen.c as mem.*):
//   mem.alloc(nbytes)            -> int pointer (0 on failure)
//   mem.free(ptr)                -> (void)
//   mem.poke8(ptr, byteoff, v)   -> store low byte of v at ptr[byteoff]
//   mem.peek8(ptr, byteoff)      -> int, zero-extended byte at ptr[byteoff]
//   mem.poke64(ptr, wordoff, v)  -> store 8-byte v at ptr + wordoff*8
//   mem.peek64(ptr, wordoff)     -> int, 8 bytes at ptr + wordoff*8
//
// Pointers are plain 64-bit ints (the heap address). Accessors are
// unchecked — a single mov each — for maximum speed on CPU-bound work.
// alloc returns 0 on failure so callers can test cheaply.

#include "codegen_internal.h"
#include "mem_runtime.h"

#define E(...) cg_emit(cg, __VA_ARGS__)

void emit_mem_imports(Codegen *cg) {
    // GetProcessHeap / HeapAlloc / HeapFree may already be imported by the
    // window runtime (zbuffer uses them). Re-declaring extern is harmless in
    // NASM only if not duplicated; to be safe we DO NOT re-emit them here and
    // rely on the window runtime's imports. If the window runtime is ever
    // removed, add them here.
    (void)cg;
}

void emit_mem_bss(Codegen *cg) {
    E("_mem_heap:   resq 1   ; cached process heap handle (lazy-init)");
}

void emit_mem_runtime(Codegen *cg) {
    E("; ===================== mem runtime (heap buffers) =====================");

    // _slag_mem_alloc(nbytes in rcx) -> rax pointer (0 on fail)
    E("; --- _slag_mem_alloc (rcx = nbytes) -> rax ---");
    E("_slag_mem_alloc:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48");
    E("    mov  [rbp-8], rcx           ; save nbytes");
    E("    ; lazily fetch + cache the process heap handle");
    E("    mov  rax, [_mem_heap]");
    E("    test rax, rax");
    E("    jnz  .ma_have_heap");
    E("    sub  rsp, 32");
    E("    call GetProcessHeap");
    E("    add  rsp, 32");
    E("    mov  [_mem_heap], rax");
    E(".ma_have_heap:");
    E("    ; HeapAlloc(heap, HEAP_ZERO_MEMORY=8, nbytes)");
    E("    mov  rcx, [_mem_heap]");
    E("    mov  rdx, 8                 ; HEAP_ZERO_MEMORY");
    E("    mov  r8,  [rbp-8]");
    E("    sub  rsp, 32");
    E("    call HeapAlloc");
    E("    add  rsp, 32");
    E("    ; rax = pointer (0 on failure) -> returned as-is");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_mem_free(ptr in rcx)
    E("; --- _slag_mem_free (rcx = ptr) ---");
    E("_slag_mem_free:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    test rcx, rcx");
    E("    jz   .mf_done               ; free(0) is a no-op");
    E("    mov  r8, rcx                ; lpMem");
    E("    mov  rcx, [_mem_heap]        ; hHeap");
    E("    xor  rdx, rdx               ; dwFlags = 0");
    E("    sub  rsp, 32");
    E("    call HeapFree");
    E("    add  rsp, 32");
    E(".mf_done:");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_mem_poke8(ptr=rcx, byteoff=rdx, val=r8)
    E("; --- _slag_mem_poke8 (rcx=ptr, rdx=byteoff, r8=val) ---");
    E("_slag_mem_poke8:");
    E("    mov  byte [rcx + rdx], r8b");
    E("    ret");
    E("");

    // _slag_mem_peek8(ptr=rcx, byteoff=rdx) -> rax (zero-extended byte)
    E("; --- _slag_mem_peek8 (rcx=ptr, rdx=byteoff) -> rax ---");
    E("_slag_mem_peek8:");
    E("    movzx rax, byte [rcx + rdx]");
    E("    ret");
    E("");

    // _slag_mem_poke64(ptr=rcx, wordoff=rdx, val=r8)
    E("; --- _slag_mem_poke64 (rcx=ptr, rdx=wordoff, r8=val) ---");
    E("_slag_mem_poke64:");
    E("    mov  [rcx + rdx*8], r8");
    E("    ret");
    E("");

    // _slag_mem_peek64(ptr=rcx, wordoff=rdx) -> rax
    E("; --- _slag_mem_peek64 (rcx=ptr, rdx=wordoff) -> rax ---");
    E("_slag_mem_peek64:");
    E("    mov  rax, [rcx + rdx*8]");
    E("    ret");
    E("");
}
