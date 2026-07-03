// simd_runtime.c — SSE2 SIMD primitives for Slag.
// Provides 128-bit vector operations for graphics and bulk data processing.
//
// Builtins (wired in codegen.c as simd.*):
//   simd.addf4(dest, a, b)  -> dest ptr; adds 4 packed single floats
//   simd.subf4(dest, a, b)  -> dest ptr; subtracts 4 packed single floats
//   simd.mulf4(dest, a, b)  -> dest ptr; multiplies 4 packed single floats
//   simd.divf4(dest, a, b)  -> dest ptr; divides 4 packed single floats
//   simd.dot4(dest, a, b)   -> dest ptr; dot product (result in all 4 elements)
//   simd.cross3(dest, a, b) -> dest ptr; 3D cross product (w=0)
//   simd.normalize4(dest, v) -> dest ptr; normalizes vec4 to unit length
//   simd.lint4(dest, a, b, t) -> dest ptr; linear interp a + t*(b-a)
//   simd.mat4_mul(dest, a, b) -> dest ptr; 4x4 matrix multiply C=A*B
//   simd.mat4_vec4(dest, m, v) -> dest ptr; matrix-vector multiply
//   simd.rgb565_unpack(r, g, b, pixels) -> extract R/G/B from 8 RGB565 pixels
//   simd.rgb565_pack(dest, r, g, b) -> pack R/G/B to 8 RGB565 pixels
//   simd.rgb565_blend(dest, a, b, alpha) -> alpha blend 8 RGB565 pixels
//
// All pointers must be 16-byte aligned for movaps.

#include "codegen_internal.h"
#include "simd_runtime.h"

#define E(...) cg_emit(cg, __VA_ARGS__)

void emit_simd_imports(Codegen *cg) {
    // SSE2 is CPU intrinsic, no DLL imports needed
    (void)cg;
}

void emit_simd_bss(Codegen *cg) {
    // No BSS data needed
    (void)cg;
}

void emit_simd_data(Codegen *cg) {
    // RGB565 masks (16-byte aligned, must be in .data not .bss)
    E("align 16");
    E("_simd_rgb565_mask_r: times 8 dw 0xF800");
    E("_simd_rgb565_mask_g: times 8 dw 0x07E0");
    E("_simd_rgb565_mask_b: times 8 dw 0x001F");
    E("_simd_cross3_wmask: dd 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000");
    E("_simd_rgb565_max_5bit: times 8 dw 31");
    E("_simd_rgb565_max_6bit: times 8 dw 63");
}

void emit_simd_runtime(Codegen *cg) {
    E("; ===================== simd runtime (SSE2) =====================");

    // _slag_simd_addf4(dest=rcx, a=rdx, b=r8) -> rax=dest
    E("; --- _slag_simd_addf4 (rcx=dest, rdx=a, r8=b) -> rax ---");
    E("_slag_simd_addf4:");
    E("    movaps xmm0, [rdx]       ; load 4 floats from a");
    E("    addps  xmm0, [r8]        ; add 4 floats from b");
    E("    movaps [rcx], xmm0       ; store to dest");
    E("    mov    rax, rcx          ; return dest ptr");
    E("    ret");

    // _slag_simd_subf4(dest=rcx, a=rdx, b=r8) -> rax=dest
    E("; --- _slag_simd_subf4 (rcx=dest, rdx=a, r8=b) -> rax ---");
    E("_slag_simd_subf4:");
    E("    movaps xmm0, [rdx]       ; load 4 floats from a");
    E("    subps  xmm0, [r8]        ; subtract 4 floats from b");
    E("    movaps [rcx], xmm0       ; store to dest");
    E("    mov    rax, rcx          ; return dest ptr");
    E("    ret");

    // _slag_simd_mulf4(dest=rcx, a=rdx, b=r8) -> rax=dest
    E("; --- _slag_simd_mulf4 (rcx=dest, rdx=a, r8=b) -> rax ---");
    E("_slag_simd_mulf4:");
    E("    movaps xmm0, [rdx]       ; load 4 floats from a");
    E("    mulps  xmm0, [r8]        ; multiply 4 floats by b");
    E("    movaps [rcx], xmm0       ; store to dest");
    E("    mov    rax, rcx          ; return dest ptr");
    E("    ret");

    // _slag_simd_divf4(dest=rcx, a=rdx, b=r8) -> rax=dest
    E("; --- _slag_simd_divf4 (rcx=dest, rdx=a, r8=b) -> rax ---");
    E("_slag_simd_divf4:");
    E("    movaps xmm0, [rdx]       ; load 4 floats from a");
    E("    divps  xmm0, [r8]        ; divide 4 floats by b");
    E("    movaps [rcx], xmm0       ; store to dest");
    E("    mov    rax, rcx          ; return dest ptr");
    E("    ret");

    // _slag_simd_dot4(dest=rcx, a=rdx, b=r8) -> rax=dest
    // Computes dot product: a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3]
    // Result stored in dest[0] and broadcast to all 4 elements
    E("; --- _slag_simd_dot4 (rcx=dest, rdx=a, r8=b) -> rax ---");
    E("_slag_simd_dot4:");
    E("    movaps xmm0, [rdx]       ; load a");
    E("    mulps  xmm0, [r8]        ; xmm0 = a*b element-wise");
    E("    movaps xmm1, xmm0");
    E("    shufps xmm1, xmm1, 0xB1  ; swap pairs: {1,0,3,2}");
    E("    addps  xmm0, xmm1        ; partial sums in pairs");
    E("    movaps xmm1, xmm0");
    E("    shufps xmm1, xmm1, 0x4E  ; swap halves: {2,3,0,1}");
    E("    addps  xmm0, xmm1        ; final sum broadcast to all");
    E("    movaps [rcx], xmm0       ; store result");
    E("    mov    rax, rcx          ; return dest ptr");
    E("    ret");

    // _slag_simd_cross3(dest=rcx, a=rdx, b=r8) -> rax=dest
    // Computes 3D cross product (w component set to 0):
    // result = {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x, 0}
    E("; --- _slag_simd_cross3 (rcx=dest, rdx=a, r8=b) -> rax ---");
    E("_slag_simd_cross3:");
    E("    movaps xmm0, [rdx]       ; load a");
    E("    movaps xmm1, [r8]        ; load b");
    E("    movaps xmm2, xmm0");
    E("    movaps xmm3, xmm1");
    E("    shufps xmm0, xmm0, 0xC9  ; a -> {a.y, a.z, a.x, a.w}");
    E("    shufps xmm1, xmm1, 0xD2  ; b -> {b.z, b.x, b.y, b.w}");
    E("    mulps  xmm0, xmm1        ; {a.y*b.z, a.z*b.x, a.x*b.y, ...}");
    E("    shufps xmm2, xmm2, 0xD2  ; a -> {a.z, a.x, a.y, a.w}");
    E("    shufps xmm3, xmm3, 0xC9  ; b -> {b.y, b.z, b.x, b.w}");
    E("    mulps  xmm2, xmm3        ; {a.z*b.y, a.x*b.z, a.y*b.x, ...}");
    E("    subps  xmm0, xmm2        ; cross product result");
    E("    andps  xmm0, [_simd_cross3_wmask] ; zero w component");
    E("    movaps [rcx], xmm0       ; store result");
    E("    mov    rax, rcx          ; return dest ptr");
    E("    ret");

    // _slag_simd_normalize4(dest=rcx, v=rdx) -> rax=dest
    // Normalizes a 4-float vector to unit length: v / |v|
    E("; --- _slag_simd_normalize4 (rcx=dest, rdx=v) -> rax ---");
    E("_slag_simd_normalize4:");
    E("    movaps xmm0, [rdx]       ; load v");
    E("    movaps xmm1, xmm0        ; copy for length calc");
    E("    mulps  xmm1, xmm1        ; xmm1 = v*v element-wise");
    E("    ; horizontal sum for squared length");
    E("    movaps xmm2, xmm1");
    E("    shufps xmm2, xmm2, 0xB1  ; swap pairs");
    E("    addps  xmm1, xmm2");
    E("    movaps xmm2, xmm1");
    E("    shufps xmm2, xmm2, 0x4E  ; swap halves");
    E("    addps  xmm1, xmm2        ; xmm1 = squared length (all 4)");
    E("    sqrtps xmm1, xmm1        ; xmm1 = length");
    E("    xorps  xmm2, xmm2        ; zero for comparison");
    E("    cmpps  xmm2, xmm1, 0     ; xmm2 = (length == 0) mask");
    E("    divps  xmm0, xmm1        ; xmm0 = v / length");
    E("    andnps xmm2, xmm0        ; zero result if length was zero");
    E("    movaps [rcx], xmm2       ; store normalized");
    E("    mov    rax, rcx          ; return dest ptr");
    E("    ret");

    // _slag_simd_lint4(dest=rcx, a=rdx, b=r8, t=r9) -> rax=dest
    // Linear interpolation: result = a + t * (b - a)
    E("; --- _slag_simd_lint4 (rcx=dest, rdx=a, r8=b, r9=t) -> rax ---");
    E("_slag_simd_lint4:");
    E("    movaps xmm0, [rdx]       ; load a");
    E("    movaps xmm1, [r8]        ; load b");
    E("    movaps xmm2, [r9]        ; load t");
    E("    subps  xmm1, xmm0        ; xmm1 = b - a");
    E("    mulps  xmm1, xmm2        ; xmm1 = t * (b - a)");
    E("    addps  xmm0, xmm1        ; xmm0 = a + t * (b - a)");
    E("    movaps [rcx], xmm0       ; store result");
    E("    mov    rax, rcx          ; return dest ptr");
    E("    ret");

    // _slag_simd_mat4_vec4(dest=rcx, m=rdx, v=r8) -> rax=dest
    // Matrix-vector multiply: result = M * v (row-major 4x4 matrix)
    // result[i] = dot(row_i, v)
    E("; --- _slag_simd_mat4_vec4 (rcx=dest, rdx=m, r8=v) -> rax ---");
    E("_slag_simd_mat4_vec4:");
    E("    movaps xmm4, [r8]        ; load v");
    E("    ; row 0");
    E("    movaps xmm0, [rdx]       ; load row 0");
    E("    mulps  xmm0, xmm4        ; row0 * v");
    E("    movaps xmm1, xmm0");
    E("    shufps xmm1, xmm1, 0xB1");
    E("    addps  xmm0, xmm1");
    E("    movaps xmm1, xmm0");
    E("    shufps xmm1, xmm1, 0x4E");
    E("    addss  xmm0, xmm1        ; result[0] in xmm0[0]");
    E("    ; row 1");
    E("    movaps xmm1, [rdx+16]    ; load row 1");
    E("    mulps  xmm1, xmm4");
    E("    movaps xmm2, xmm1");
    E("    shufps xmm2, xmm2, 0xB1");
    E("    addps  xmm1, xmm2");
    E("    movaps xmm2, xmm1");
    E("    shufps xmm2, xmm2, 0x4E");
    E("    addss  xmm1, xmm2        ; result[1] in xmm1[0]");
    E("    ; row 2");
    E("    movaps xmm2, [rdx+32]    ; load row 2");
    E("    mulps  xmm2, xmm4");
    E("    movaps xmm3, xmm2");
    E("    shufps xmm3, xmm3, 0xB1");
    E("    addps  xmm2, xmm3");
    E("    movaps xmm3, xmm2");
    E("    shufps xmm3, xmm3, 0x4E");
    E("    addss  xmm2, xmm3        ; result[2] in xmm2[0]");
    E("    ; row 3");
    E("    movaps xmm3, [rdx+48]    ; load row 3");
    E("    mulps  xmm3, xmm4");
    E("    movaps xmm5, xmm3");
    E("    shufps xmm5, xmm5, 0xB1");
    E("    addps  xmm3, xmm5");
    E("    movaps xmm5, xmm3");
    E("    shufps xmm5, xmm5, 0x4E");
    E("    addss  xmm3, xmm5        ; result[3] in xmm3[0]");
    E("    ; combine results into xmm0");
    E("    unpcklps xmm0, xmm1      ; xmm0 = {r0, r1, ?, ?}");
    E("    unpcklps xmm2, xmm3      ; xmm2 = {r2, r3, ?, ?}");
    E("    movlhps  xmm0, xmm2      ; xmm0 = {r0, r1, r2, r3}");
    E("    movaps [rcx], xmm0       ; store result");
    E("    mov    rax, rcx");
    E("    ret");

    // _slag_simd_mat4_mul(dest=rcx, a=rdx, b=r8) -> rax=dest
    // 4x4 matrix multiply: C = A * B (row-major)
    // C_row_i = A[i][0]*B_row_0 + A[i][1]*B_row_1 + A[i][2]*B_row_2 + A[i][3]*B_row_3
    E("; --- _slag_simd_mat4_mul (rcx=dest, rdx=a, r8=b) -> rax ---");
    E("_slag_simd_mat4_mul:");
    E("    ; load all B rows");
    E("    movaps xmm4, [r8]        ; B row 0");
    E("    movaps xmm5, [r8+16]     ; B row 1");
    E("    movaps xmm6, [r8+32]     ; B row 2");
    E("    movaps xmm7, [r8+48]     ; B row 3");
    E("    ; --- C row 0 ---");
    E("    movaps xmm0, [rdx]       ; A row 0");
    E("    movaps xmm1, xmm0");
    E("    shufps xmm1, xmm1, 0x00  ; broadcast A[0][0]");
    E("    mulps  xmm1, xmm4");
    E("    movaps xmm2, xmm0");
    E("    shufps xmm2, xmm2, 0x55  ; broadcast A[0][1]");
    E("    mulps  xmm2, xmm5");
    E("    addps  xmm1, xmm2");
    E("    movaps xmm2, xmm0");
    E("    shufps xmm2, xmm2, 0xAA  ; broadcast A[0][2]");
    E("    mulps  xmm2, xmm6");
    E("    addps  xmm1, xmm2");
    E("    shufps xmm0, xmm0, 0xFF  ; broadcast A[0][3]");
    E("    mulps  xmm0, xmm7");
    E("    addps  xmm1, xmm0");
    E("    movaps [rcx], xmm1       ; store C row 0");
    E("    ; --- C row 1 ---");
    E("    movaps xmm0, [rdx+16]    ; A row 1");
    E("    movaps xmm1, xmm0");
    E("    shufps xmm1, xmm1, 0x00");
    E("    mulps  xmm1, xmm4");
    E("    movaps xmm2, xmm0");
    E("    shufps xmm2, xmm2, 0x55");
    E("    mulps  xmm2, xmm5");
    E("    addps  xmm1, xmm2");
    E("    movaps xmm2, xmm0");
    E("    shufps xmm2, xmm2, 0xAA");
    E("    mulps  xmm2, xmm6");
    E("    addps  xmm1, xmm2");
    E("    shufps xmm0, xmm0, 0xFF");
    E("    mulps  xmm0, xmm7");
    E("    addps  xmm1, xmm0");
    E("    movaps [rcx+16], xmm1    ; store C row 1");
    E("    ; --- C row 2 ---");
    E("    movaps xmm0, [rdx+32]    ; A row 2");
    E("    movaps xmm1, xmm0");
    E("    shufps xmm1, xmm1, 0x00");
    E("    mulps  xmm1, xmm4");
    E("    movaps xmm2, xmm0");
    E("    shufps xmm2, xmm2, 0x55");
    E("    mulps  xmm2, xmm5");
    E("    addps  xmm1, xmm2");
    E("    movaps xmm2, xmm0");
    E("    shufps xmm2, xmm2, 0xAA");
    E("    mulps  xmm2, xmm6");
    E("    addps  xmm1, xmm2");
    E("    shufps xmm0, xmm0, 0xFF");
    E("    mulps  xmm0, xmm7");
    E("    addps  xmm1, xmm0");
    E("    movaps [rcx+32], xmm1    ; store C row 2");
    E("    ; --- C row 3 ---");
    E("    movaps xmm0, [rdx+48]    ; A row 3");
    E("    movaps xmm1, xmm0");
    E("    shufps xmm1, xmm1, 0x00");
    E("    mulps  xmm1, xmm4");
    E("    movaps xmm2, xmm0");
    E("    shufps xmm2, xmm2, 0x55");
    E("    mulps  xmm2, xmm5");
    E("    addps  xmm1, xmm2");
    E("    movaps xmm2, xmm0");
    E("    shufps xmm2, xmm2, 0xAA");
    E("    mulps  xmm2, xmm6");
    E("    addps  xmm1, xmm2");
    E("    shufps xmm0, xmm0, 0xFF");
    E("    mulps  xmm0, xmm7");
    E("    addps  xmm1, xmm0");
    E("    movaps [rcx+48], xmm1    ; store C row 3");
    E("    mov    rax, rcx");
    E("    ret");

    // _slag_simd_rgb565_unpack(r=rcx, g=rdx, b=r8, pixels=r9)
    // Unpacks 8 RGB565 pixels to separate R(5-bit), G(6-bit), B(5-bit) arrays
    E("; --- _slag_simd_rgb565_unpack (rcx=r, rdx=g, r8=b, r9=pixels) ---");
    E("_slag_simd_rgb565_unpack:");
    E("    movdqu xmm0, [r9]              ; load 8 RGB565 pixels");
    E("    movdqa xmm1, xmm0              ; copy for G");
    E("    movdqa xmm2, xmm0              ; copy for B");
    E("    ; extract R: (pixel & 0xF800) >> 11");
    E("    pand   xmm0, [_simd_rgb565_mask_r]");
    E("    psrlw  xmm0, 11");
    E("    movdqu [rcx], xmm0             ; store R");
    E("    ; extract G: (pixel & 0x07E0) >> 5");
    E("    pand   xmm1, [_simd_rgb565_mask_g]");
    E("    psrlw  xmm1, 5");
    E("    movdqu [rdx], xmm1             ; store G");
    E("    ; extract B: pixel & 0x001F");
    E("    pand   xmm2, [_simd_rgb565_mask_b]");
    E("    movdqu [r8], xmm2              ; store B");
    E("    mov    rax, rcx                ; return dest ptr");
    E("    ret");

    // _slag_simd_rgb565_pack(dest=rcx, r=rdx, g=r8, b=r9)
    // Packs R(5-bit), G(6-bit), B(5-bit) arrays to 8 RGB565 pixels
    E("; --- _slag_simd_rgb565_pack (rcx=dest, rdx=r, r8=g, r9=b) ---");
    E("_slag_simd_rgb565_pack:");
    E("    movdqu xmm0, [rdx]             ; load R (5-bit values)");
    E("    movdqu xmm1, [r8]              ; load G (6-bit values)");
    E("    movdqu xmm2, [r9]              ; load B (5-bit values)");
    E("    psllw  xmm0, 11                ; R << 11");
    E("    psllw  xmm1, 5                 ; G << 5");
    E("    por    xmm0, xmm1              ; R | G");
    E("    por    xmm0, xmm2              ; R | G | B");
    E("    movdqu [rcx], xmm0             ; store result");
    E("    mov    rax, rcx");
    E("    ret");

    // _slag_simd_rgb565_blend(dest=rcx, a=rdx, b=r8, alpha=r9)
    // Alpha blends 8 RGB565 pixels: result = a + alpha*(b-a)/256
    // alpha is ptr to 8x16-bit values (0-256 range)
    E("; --- _slag_simd_rgb565_blend (rcx=dest, rdx=a, r8=b, r9=alpha) ---");
    E("_slag_simd_rgb565_blend:");
    E("    push   rbx                     ; save callee-saved");
    E("    mov    rbx, rcx                ; save dest in rbx");
    E("    sub    rsp, 112                ; temp space (16-byte aligned)");
    E("    ; unpack a");
    E("    movdqu xmm0, [rdx]");
    E("    movdqa xmm1, xmm0");
    E("    movdqa xmm2, xmm0");
    E("    pand   xmm0, [_simd_rgb565_mask_r]");
    E("    psrlw  xmm0, 11");
    E("    movdqa [rsp], xmm0             ; a_r at rsp+0");
    E("    pand   xmm1, [_simd_rgb565_mask_g]");
    E("    psrlw  xmm1, 5");
    E("    movdqa [rsp+16], xmm1          ; a_g at rsp+16");
    E("    pand   xmm2, [_simd_rgb565_mask_b]");
    E("    movdqa [rsp+32], xmm2          ; a_b at rsp+32");
    E("    ; unpack b");
    E("    movdqu xmm0, [r8]");
    E("    movdqa xmm1, xmm0");
    E("    movdqa xmm2, xmm0");
    E("    pand   xmm0, [_simd_rgb565_mask_r]");
    E("    psrlw  xmm0, 11");
    E("    movdqa [rsp+48], xmm0          ; b_r at rsp+48");
    E("    pand   xmm1, [_simd_rgb565_mask_g]");
    E("    psrlw  xmm1, 5");
    E("    movdqa [rsp+64], xmm1          ; b_g at rsp+64");
    E("    pand   xmm2, [_simd_rgb565_mask_b]");
    E("    movdqa [rsp+80], xmm2          ; b_b at rsp+80");
    E("    ; load alpha");
    E("    movdqu xmm3, [r9]              ; alpha (0-256)");
    E("    pxor   xmm7, xmm7               ; zero for clamping");
    E("    ; blend R: a_r + ((b_r - a_r) * alpha) >> 8");
    E("    movdqa xmm0, [rsp]             ; a_r");
    E("    movdqa xmm1, [rsp+48]          ; b_r");
    E("    psubw  xmm1, xmm0              ; b_r - a_r");
    E("    pmullw xmm1, xmm3              ; * alpha");
    E("    psraw  xmm1, 8                 ; >> 8");
    E("    paddw  xmm0, xmm1              ; a_r + diff");
    E("    pmaxsw xmm0, xmm7              ; clamp >= 0");
    E("    pminsw xmm0, [_simd_rgb565_max_5bit] ; clamp <= 31");
    E("    movdqa xmm4, xmm0              ; save blended R");
    E("    ; blend G");
    E("    movdqa xmm0, [rsp+16]          ; a_g");
    E("    movdqa xmm1, [rsp+64]          ; b_g");
    E("    psubw  xmm1, xmm0");
    E("    pmullw xmm1, xmm3");
    E("    psraw  xmm1, 8");
    E("    paddw  xmm0, xmm1");
    E("    pmaxsw xmm0, xmm7              ; clamp >= 0");
    E("    pminsw xmm0, [_simd_rgb565_max_6bit] ; clamp <= 63");
    E("    movdqa xmm5, xmm0              ; save blended G");
    E("    ; blend B");
    E("    movdqa xmm0, [rsp+32]          ; a_b");
    E("    movdqa xmm1, [rsp+80]          ; b_b");
    E("    psubw  xmm1, xmm0");
    E("    pmullw xmm1, xmm3");
    E("    psraw  xmm1, 8");
    E("    paddw  xmm0, xmm1");
    E("    pmaxsw xmm0, xmm7              ; clamp >= 0");
    E("    pminsw xmm0, [_simd_rgb565_max_5bit] ; clamp <= 31");
    E("    movdqa xmm6, xmm0              ; save blended B");
    E("    ; pack result");
    E("    psllw  xmm4, 11                ; R << 11");
    E("    psllw  xmm5, 5                 ; G << 5");
    E("    por    xmm4, xmm5");
    E("    por    xmm4, xmm6");
    E("    movdqu [rbx], xmm4             ; store to dest");
    E("    add    rsp, 112");
    E("    mov    rax, rbx");
    E("    pop    rbx");
    E("    ret");
}
