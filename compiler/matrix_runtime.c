// matrix_runtime.c — 3D matrix stack for Slag.
// Mirrors mem_runtime.c / window_runtime.c: emits NASM asm via cg_emit.
//
// Design goals:
//   - Performance over all else: fixed-point 16.16, lookup tables, inlined ops
//   - 3x4 matrices (rotation + translation, no perspective column)
//   - Fixed-depth stack (16 levels) for hierarchical transforms
//   - No Win32 dependencies — pure math
//
// Matrix layout (row-major, 3x4):
//   [ m0  m1  m2  m3  ]   // row 0: X axis + tx
//   [ m4  m5  m6  m7  ]   // row 1: Y axis + ty
//   [ m8  m9  m10 m11 ]   // row 2: Z axis + tz
//
// Fixed-point 16.16: upper 16 bits = integer, lower 16 bits = fraction
//   To convert: fixed = int << 16
//   To multiply: (a * b) >> 16
//   To convert back: int = fixed >> 16

#include "codegen_internal.h"
#include "matrix_runtime.h"
#include <math.h>

#define E(...) cg_emit(cg, __VA_ARGS__)

#define MAT_STACK_DEPTH 16
#define MAT_SIZE 12          // 3x4 matrix = 12 values
#define TRIG_TABLE_SIZE 256  // 256 entries = ~1.4 degree resolution
#define FIXED_SHIFT 16
#define FIXED_ONE (1 << FIXED_SHIFT)

void emit_mat_imports(Codegen *cg) {
    // No Win32 imports needed — pure math
    (void)cg;
}

void emit_mat_data(Codegen *cg) {
    E("; ===================== matrix data (sin/cos tables) =====================");

    // Pre-computed sin table: 256 entries, fixed-point 16.16
    E("_mat_sin_table:");
    for (int i = 0; i < TRIG_TABLE_SIZE; i += 8) {
        E("    dq %d, %d, %d, %d, %d, %d, %d, %d",
            (int)(sin((double)(i+0) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(sin((double)(i+1) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(sin((double)(i+2) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(sin((double)(i+3) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(sin((double)(i+4) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(sin((double)(i+5) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(sin((double)(i+6) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(sin((double)(i+7) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE));
    }
    E("");

    // Pre-computed cos table: 256 entries, fixed-point 16.16
    E("_mat_cos_table:");
    for (int i = 0; i < TRIG_TABLE_SIZE; i += 8) {
        E("    dq %d, %d, %d, %d, %d, %d, %d, %d",
            (int)(cos((double)(i+0) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(cos((double)(i+1) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(cos((double)(i+2) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(cos((double)(i+3) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(cos((double)(i+4) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(cos((double)(i+5) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(cos((double)(i+6) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE),
            (int)(cos((double)(i+7) * 2.0 * 3.14159265358979323846 / TRIG_TABLE_SIZE) * FIXED_ONE));
    }
    E("");

    // Identity matrix constant (fixed-point 16.16)
    E("_mat_identity_const:");
    E("    dq %d, 0, 0, 0", FIXED_ONE);  // row 0: [1, 0, 0, 0]
    E("    dq 0, %d, 0, 0", FIXED_ONE);  // row 1: [0, 1, 0, 0]
    E("    dq 0, 0, %d, 0", FIXED_ONE);  // row 2: [0, 0, 1, 0]
    E("");
}

void emit_mat_bss(Codegen *cg) {
    E("; ===================== matrix bss =====================");
    E("_mat_current:  resq %d   ; current 3x4 matrix (12 qwords)", MAT_SIZE);
    E("_mat_stack:    resq %d   ; matrix stack (%d levels x %d values)",
        MAT_STACK_DEPTH * MAT_SIZE, MAT_STACK_DEPTH, MAT_SIZE);
    E("_mat_sp:       resq 1    ; stack pointer (0 = empty)");
    E("");
}

void emit_mat_runtime(Codegen *cg) {
    E("; ===================== matrix runtime =====================");
    E("");

    // mat.identity() — reset current matrix to identity
    E("; --- _slag_mat_identity ---");
    E("_slag_mat_identity:");
    E("    push rsi");
    E("    push rdi");
    E("    push rcx");
    E("    lea  rsi, [rel _mat_identity_const]");
    E("    lea  rdi, [rel _mat_current]");
    E("    mov  rcx, %d", MAT_SIZE);
    E("    rep  movsq");
    E("    pop  rcx");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    ret");
    E("");

    // mat.push() — push current matrix onto stack
    E("; --- _slag_mat_push ---");
    E("_slag_mat_push:");
    E("    push rsi");
    E("    push rdi");
    E("    push rcx");
    E("    push rax");
    E("    push rbx");
    E("    ; check stack overflow");
    E("    lea  rbx, [rel _mat_sp]");
    E("    mov  rax, [rbx]");
    E("    cmp  rax, %d", MAT_STACK_DEPTH);
    E("    jge  .push_done          ; stack full, ignore");
    E("    ; calculate dest = _mat_stack + sp * %d * 8", MAT_SIZE);
    E("    imul rax, %d", MAT_SIZE * 8);
    E("    lea  rdi, [rel _mat_stack]");
    E("    add  rdi, rax");
    E("    lea  rsi, [rel _mat_current]");
    E("    mov  rcx, %d", MAT_SIZE);
    E("    rep  movsq");
    E("    ; increment stack pointer");
    E("    mov  rax, [rbx]");
    E("    inc  rax");
    E("    mov  [rbx], rax");
    E(".push_done:");
    E("    pop  rbx");
    E("    pop  rax");
    E("    pop  rcx");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    ret");
    E("");

    // mat.pop() — pop matrix from stack into current
    E("; --- _slag_mat_pop ---");
    E("_slag_mat_pop:");
    E("    push rsi");
    E("    push rdi");
    E("    push rcx");
    E("    push rax");
    E("    push rbx");
    E("    ; check stack underflow");
    E("    lea  rbx, [rel _mat_sp]");
    E("    mov  rax, [rbx]");
    E("    test rax, rax");
    E("    jz   .pop_done           ; stack empty, ignore");
    E("    ; decrement stack pointer");
    E("    dec  rax");
    E("    mov  [rbx], rax");
    E("    ; calculate src = _mat_stack + sp * %d * 8", MAT_SIZE);
    E("    imul rax, %d", MAT_SIZE * 8);
    E("    lea  rsi, [rel _mat_stack]");
    E("    add  rsi, rax");
    E("    lea  rdi, [rel _mat_current]");
    E("    mov  rcx, %d", MAT_SIZE);
    E("    rep  movsq");
    E(".pop_done:");
    E("    pop  rbx");
    E("    pop  rax");
    E("    pop  rcx");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    ret");
    E("");

    // mat.translate(x, y, z) — multiply translation into current
    // rcx=x, rdx=y, r8=z (all in fixed-point 16.16)
    E("; --- _slag_mat_translate (rcx=x, rdx=y, r8=z) ---");
    E("_slag_mat_translate:");
    E("    push rbx");
    E("    lea  rbx, [rel _mat_current]");
    E("    add  qword [rbx + 3*8], rcx    ; m3 += x");
    E("    add  qword [rbx + 7*8], rdx    ; m7 += y");
    E("    add  qword [rbx + 11*8], r8    ; m11 += z");
    E("    pop  rbx");
    E("    ret");
    E("");

    // mat.scale(sx, sy, sz) — multiply scale into current
    // rcx=sx, rdx=sy, r8=sz (all in fixed-point 16.16)
    E("; --- _slag_mat_scale (rcx=sx, rdx=sy, r8=sz) ---");
    E("_slag_mat_scale:");
    E("    push rbx");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    lea  rbx, [rel _mat_current]");
    E("    mov  r12, rcx              ; r12 = sx");
    E("    mov  r13, rdx              ; r13 = sy");
    E("    mov  r14, r8               ; r14 = sz");
    E("    ; scale row 0 by sx");
    E("    mov  rax, [rbx + 0*8]");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  [rbx + 0*8], rax");
    E("    mov  rax, [rbx + 1*8]");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  [rbx + 1*8], rax");
    E("    mov  rax, [rbx + 2*8]");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  [rbx + 2*8], rax");
    E("    mov  rax, [rbx + 3*8]");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  [rbx + 3*8], rax");
    E("    ; scale row 1 by sy");
    E("    mov  rax, [rbx + 4*8]");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  [rbx + 4*8], rax");
    E("    mov  rax, [rbx + 5*8]");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  [rbx + 5*8], rax");
    E("    mov  rax, [rbx + 6*8]");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  [rbx + 6*8], rax");
    E("    mov  rax, [rbx + 7*8]");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  [rbx + 7*8], rax");
    E("    ; scale row 2 by sz");
    E("    mov  rax, [rbx + 8*8]");
    E("    imul r14");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  [rbx + 8*8], rax");
    E("    mov  rax, [rbx + 9*8]");
    E("    imul r14");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  [rbx + 9*8], rax");
    E("    mov  rax, [rbx + 10*8]");
    E("    imul r14");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  [rbx + 10*8], rax");
    E("    mov  rax, [rbx + 11*8]");
    E("    imul r14");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  [rbx + 11*8], rax");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbx");
    E("    ret");
    E("");

    // mat.rotate_y(angle) — multiply Y rotation into current
    // rcx = angle (0-255 maps to 0-360 degrees)
    // Y rotation: new_m0 = m0*cos - m2*sin, new_m2 = m0*sin + m2*cos
    E("; --- _slag_mat_rotate_y (rcx=angle 0-255) ---");
    E("_slag_mat_rotate_y:");
    E("    push rbx");
    E("    push r10");
    E("    push r11");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    push r15");
    E("    ; get sin/cos from tables");
    E("    and  rcx, 255");
    E("    lea  rax, [rel _mat_sin_table]");
    E("    mov  r12, [rax + rcx*8]        ; r12 = sin");
    E("    lea  rax, [rel _mat_cos_table]");
    E("    mov  r13, [rax + rcx*8]        ; r13 = cos");
    E("    lea  rbx, [rel _mat_current]");
    E("");
    E("    ; Row 0: m0, m2");
    E("    mov  r14, [rbx + 0*8]          ; r14 = m0");
    E("    mov  r15, [rbx + 2*8]          ; r15 = m2");
    E("    ; new_m0 = m0*cos - m2*sin");
    E("    mov  rax, r14");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r10, rax                  ; r10 = m0*cos");
    E("    mov  rax, r15");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    sub  r10, rax                  ; r10 = m0*cos - m2*sin");
    E("    ; new_m2 = m0*sin + m2*cos");
    E("    mov  rax, r14");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r11, rax                  ; r11 = m0*sin");
    E("    mov  rax, r15");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r11, rax                  ; r11 = m0*sin + m2*cos");
    E("    mov  [rbx + 0*8], r10");
    E("    mov  [rbx + 2*8], r11");
    E("");
    E("    ; Row 1: m4, m6");
    E("    mov  r14, [rbx + 4*8]");
    E("    mov  r15, [rbx + 6*8]");
    E("    mov  rax, r14");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r10, rax");
    E("    mov  rax, r15");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    sub  r10, rax");
    E("    mov  rax, r14");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r11, rax");
    E("    mov  rax, r15");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r11, rax");
    E("    mov  [rbx + 4*8], r10");
    E("    mov  [rbx + 6*8], r11");
    E("");
    E("    ; Row 2: m8, m10");
    E("    mov  r14, [rbx + 8*8]");
    E("    mov  r15, [rbx + 10*8]");
    E("    mov  rax, r14");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r10, rax");
    E("    mov  rax, r15");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    sub  r10, rax");
    E("    mov  rax, r14");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r11, rax");
    E("    mov  rax, r15");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r11, rax");
    E("    mov  [rbx + 8*8], r10");
    E("    mov  [rbx + 10*8], r11");
    E("");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  r11");
    E("    pop  r10");
    E("    pop  rbx");
    E("    ret");
    E("");

    // mat.rotate_x(angle) — multiply X rotation into current
    // X rotation: new_m1 = m1*cos + m2*sin, new_m2 = -m1*sin + m2*cos
    E("; --- _slag_mat_rotate_x (rcx=angle 0-255) ---");
    E("_slag_mat_rotate_x:");
    E("    push rbx");
    E("    push r10");
    E("    push r11");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    push r15");
    E("    ; get sin/cos from tables");
    E("    and  rcx, 255");
    E("    lea  rax, [rel _mat_sin_table]");
    E("    mov  r12, [rax + rcx*8]        ; r12 = sin");
    E("    lea  rax, [rel _mat_cos_table]");
    E("    mov  r13, [rax + rcx*8]        ; r13 = cos");
    E("    lea  rbx, [rel _mat_current]");
    E("");
    E("    ; Row 0: m1, m2");
    E("    mov  r14, [rbx + 1*8]          ; r14 = m1");
    E("    mov  r15, [rbx + 2*8]          ; r15 = m2");
    E("    ; new_m1 = m1*cos + m2*sin");
    E("    mov  rax, r14");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r10, rax                  ; r10 = m1*cos");
    E("    mov  rax, r15");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r10, rax                  ; r10 = m1*cos + m2*sin");
    E("    ; new_m2 = -m1*sin + m2*cos");
    E("    mov  rax, r14");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r11, rax");
    E("    neg  r11                       ; r11 = -m1*sin");
    E("    mov  rax, r15");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r11, rax                  ; r11 = -m1*sin + m2*cos");
    E("    mov  [rbx + 1*8], r10");
    E("    mov  [rbx + 2*8], r11");
    E("");
    E("    ; Row 1: m5, m6");
    E("    mov  r14, [rbx + 5*8]");
    E("    mov  r15, [rbx + 6*8]");
    E("    mov  rax, r14");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r10, rax");
    E("    mov  rax, r15");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r10, rax");
    E("    mov  rax, r14");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r11, rax");
    E("    neg  r11");
    E("    mov  rax, r15");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r11, rax");
    E("    mov  [rbx + 5*8], r10");
    E("    mov  [rbx + 6*8], r11");
    E("");
    E("    ; Row 2: m9, m10");
    E("    mov  r14, [rbx + 9*8]");
    E("    mov  r15, [rbx + 10*8]");
    E("    mov  rax, r14");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r10, rax");
    E("    mov  rax, r15");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r10, rax");
    E("    mov  rax, r14");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r11, rax");
    E("    neg  r11");
    E("    mov  rax, r15");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r11, rax");
    E("    mov  [rbx + 9*8], r10");
    E("    mov  [rbx + 10*8], r11");
    E("");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  r11");
    E("    pop  r10");
    E("    pop  rbx");
    E("    ret");
    E("");

    // mat.rotate_z(angle) — multiply Z rotation into current
    // Z rotation: new_m0 = m0*cos + m1*sin, new_m1 = -m0*sin + m1*cos
    E("; --- _slag_mat_rotate_z (rcx=angle 0-255) ---");
    E("_slag_mat_rotate_z:");
    E("    push rbx");
    E("    push r10");
    E("    push r11");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    push r15");
    E("    ; get sin/cos from tables");
    E("    and  rcx, 255");
    E("    lea  rax, [rel _mat_sin_table]");
    E("    mov  r12, [rax + rcx*8]        ; r12 = sin");
    E("    lea  rax, [rel _mat_cos_table]");
    E("    mov  r13, [rax + rcx*8]        ; r13 = cos");
    E("    lea  rbx, [rel _mat_current]");
    E("");
    E("    ; Row 0: m0, m1");
    E("    mov  r14, [rbx + 0*8]          ; r14 = m0");
    E("    mov  r15, [rbx + 1*8]          ; r15 = m1");
    E("    ; new_m0 = m0*cos + m1*sin");
    E("    mov  rax, r14");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r10, rax                  ; r10 = m0*cos");
    E("    mov  rax, r15");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r10, rax                  ; r10 = m0*cos + m1*sin");
    E("    ; new_m1 = -m0*sin + m1*cos");
    E("    mov  rax, r14");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r11, rax");
    E("    neg  r11                       ; r11 = -m0*sin");
    E("    mov  rax, r15");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r11, rax                  ; r11 = -m0*sin + m1*cos");
    E("    mov  [rbx + 0*8], r10");
    E("    mov  [rbx + 1*8], r11");
    E("");
    E("    ; Row 1: m4, m5");
    E("    mov  r14, [rbx + 4*8]");
    E("    mov  r15, [rbx + 5*8]");
    E("    mov  rax, r14");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r10, rax");
    E("    mov  rax, r15");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r10, rax");
    E("    mov  rax, r14");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r11, rax");
    E("    neg  r11");
    E("    mov  rax, r15");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r11, rax");
    E("    mov  [rbx + 4*8], r10");
    E("    mov  [rbx + 5*8], r11");
    E("");
    E("    ; Row 2: m8, m9");
    E("    mov  r14, [rbx + 8*8]");
    E("    mov  r15, [rbx + 9*8]");
    E("    mov  rax, r14");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r10, rax");
    E("    mov  rax, r15");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r10, rax");
    E("    mov  rax, r14");
    E("    imul r12");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r11, rax");
    E("    neg  r11");
    E("    mov  rax, r15");
    E("    imul r13");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r11, rax");
    E("    mov  [rbx + 8*8], r10");
    E("    mov  [rbx + 9*8], r11");
    E("");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  r11");
    E("    pop  r10");
    E("    pop  rbx");
    E("    ret");
    E("");

    // mat.transform_x(x, y, z) — return transformed X coordinate
    // rcx=x, rdx=y, r8=z -> rax = m0*x + m1*y + m2*z + m3
    E("; --- _slag_mat_transform_x (rcx=x, rdx=y, r8=z) -> rax ---");
    E("_slag_mat_transform_x:");
    E("    push rbx");
    E("    push r12");
    E("    lea  rbx, [rel _mat_current]");
    E("    ; rax = m0*x");
    E("    mov  rax, [rbx + 0*8]");
    E("    imul rcx");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r12, rax");
    E("    ; r12 += m1*y");
    E("    mov  rax, [rbx + 1*8]");
    E("    imul rdx");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r12, rax");
    E("    ; r12 += m2*z");
    E("    mov  rax, [rbx + 2*8]");
    E("    imul r8");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r12, rax");
    E("    ; r12 += m3 (translation)");
    E("    add  r12, [rbx + 3*8]");
    E("    mov  rax, r12");
    E("    pop  r12");
    E("    pop  rbx");
    E("    ret");
    E("");

    // mat.transform_y(x, y, z) — return transformed Y coordinate
    E("; --- _slag_mat_transform_y (rcx=x, rdx=y, r8=z) -> rax ---");
    E("_slag_mat_transform_y:");
    E("    push rbx");
    E("    push r12");
    E("    lea  rbx, [rel _mat_current]");
    E("    ; rax = m4*x");
    E("    mov  rax, [rbx + 4*8]");
    E("    imul rcx");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r12, rax");
    E("    ; r12 += m5*y");
    E("    mov  rax, [rbx + 5*8]");
    E("    imul rdx");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r12, rax");
    E("    ; r12 += m6*z");
    E("    mov  rax, [rbx + 6*8]");
    E("    imul r8");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r12, rax");
    E("    ; r12 += m7 (translation)");
    E("    add  r12, [rbx + 7*8]");
    E("    mov  rax, r12");
    E("    pop  r12");
    E("    pop  rbx");
    E("    ret");
    E("");

    // mat.transform_z(x, y, z) — return transformed Z coordinate
    E("; --- _slag_mat_transform_z (rcx=x, rdx=y, r8=z) -> rax ---");
    E("_slag_mat_transform_z:");
    E("    push rbx");
    E("    push r12");
    E("    lea  rbx, [rel _mat_current]");
    E("    ; rax = m8*x");
    E("    mov  rax, [rbx + 8*8]");
    E("    imul rcx");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    mov  r12, rax");
    E("    ; r12 += m9*y");
    E("    mov  rax, [rbx + 9*8]");
    E("    imul rdx");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r12, rax");
    E("    ; r12 += m10*z");
    E("    mov  rax, [rbx + 10*8]");
    E("    imul r8");
    E("    sar  rax, %d", FIXED_SHIFT);
    E("    add  r12, rax");
    E("    ; r12 += m11 (translation)");
    E("    add  r12, [rbx + 11*8]");
    E("    mov  rax, r12");
    E("    pop  r12");
    E("    pop  rbx");
    E("    ret");
    E("");
}
