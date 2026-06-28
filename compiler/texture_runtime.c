// texture_runtime.c — procedural texture generation for Slag.
// Emits NASM asm via cg_emit for noise/pattern generation.
//
// All functions return grayscale 0-255 values suitable for:
//   - Direct use as color components
//   - Blending with rgb565 via simd.rgb565_blend
//   - Height/displacement maps

#include "codegen_internal.h"
#include "texture_runtime.h"

#define E(...) cg_emit(cg, __VA_ARGS__)

void emit_tex_imports(Codegen *cg) {
    // No external Win32 imports needed - pure math
    (void)cg;
}

void emit_tex_bss(Codegen *cg) {
    // Scratch space for noise calculations
    E("_tex_scratch: resq 4   ; temp storage for noise ops");
}

void emit_tex_data(Codegen *cg) {
    // Permutation table for noise (256 entries, doubled for wrap)
    E("; Permutation table for Perlin noise");
    E("_tex_perm:");
    E("    db 151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225");
    E("    db 140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148");
    E("    db 247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32");
    E("    db 57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175");
    E("    db 74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122");
    E("    db 60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54");
    E("    db 65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169");
    E("    db 200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64");
    E("    db 52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212");
    E("    db 207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213");
    E("    db 119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9");
    E("    db 129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104");
    E("    db 218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241");
    E("    db 81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157");
    E("    db 184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93");
    E("    db 222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180");
    // Doubled for wrap-around
    E("    db 151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225");
    E("    db 140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148");
    E("    db 247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32");
    E("    db 57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175");
    E("    db 74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122");
    E("    db 60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54");
    E("    db 65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169");
    E("    db 200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64");
    E("    db 52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212");
    E("    db 207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213");
    E("    db 119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9");
    E("    db 129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104");
    E("    db 218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241");
    E("    db 81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157");
    E("    db 184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93");
    E("    db 222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180");
    E("");
}

void emit_tex_runtime(Codegen *cg) {
    E("; ===================== texture runtime (procedural generation) =====================");
    E("");

    // tex.checker(x=rcx, y=rdx, size=r8) -> rax (0 or 255)
    E("; --- _slag_tex_checker (rcx=x, rdx=y, r8=size) -> rax ---");
    E("_slag_tex_checker:");
    E("    mov  r9, rdx           ; save y in r9 (cqo clobbers rdx)");
    E("    mov  rax, rcx          ; rax = x");
    E("    cqo");
    E("    idiv r8                ; rax = x / size");
    E("    mov  r10, rax          ; r10 = x_cell");
    E("    mov  rax, r9           ; rax = y");
    E("    cqo");
    E("    idiv r8                ; rax = y / size");
    E("    xor  rax, r10          ; xor cell coords");
    E("    and  rax, 1            ; get low bit");
    E("    neg  rax               ; 0 or -1");
    E("    and  rax, 255          ; 0 or 255");
    E("    ret");
    E("");

    // tex.gradient_h(x=rcx, width=rdx) -> rax (0-255)
    E("; --- _slag_tex_gradient_h (rcx=x, rdx=width) -> rax ---");
    E("_slag_tex_gradient_h:");
    E("    mov  r9, rdx           ; save width (cqo clobbers rdx)");
    E("    mov  rax, rcx");
    E("    imul rax, 255");
    E("    cqo");
    E("    idiv r9               ; rax = x * 255 / width");
    E("    ; clamp to 0-255");
    E("    test rax, rax");
    E("    jns  .gh_pos");
    E("    xor  rax, rax");
    E("    ret");
    E(".gh_pos:");
    E("    cmp  rax, 255");
    E("    jle  .gh_done");
    E("    mov  rax, 255");
    E(".gh_done:");
    E("    ret");
    E("");

    // tex.gradient_v(y=rcx, height=rdx) -> rax (0-255)
    E("; --- _slag_tex_gradient_v (rcx=y, rdx=height) -> rax ---");
    E("_slag_tex_gradient_v:");
    E("    mov  r9, rdx           ; save height (cqo clobbers rdx)");
    E("    mov  rax, rcx");
    E("    imul rax, 255");
    E("    cqo");
    E("    idiv r9               ; rax = y * 255 / height");
    E("    ; clamp to 0-255");
    E("    test rax, rax");
    E("    jns  .gv_pos");
    E("    xor  rax, rax");
    E("    ret");
    E(".gv_pos:");
    E("    cmp  rax, 255");
    E("    jle  .gv_done");
    E("    mov  rax, 255");
    E(".gv_done:");
    E("    ret");
    E("");

    // tex.brick(x=rcx, y=rdx, bw=r8, bh=r9, mortar=[rsp+40]) -> rax
    E("; --- _slag_tex_brick (rcx=x, rdx=y, r8=bw, r9=bh, [rsp+40]=mortar) -> rax ---");
    E("_slag_tex_brick:");
    E("    push rbx");
    E("    push r12");
    E("    push r13");
    E("    mov  r12, rcx         ; x");
    E("    mov  r13, rdx         ; y");
    E("    ; row = y / bh");
    E("    mov  rax, rdx");
    E("    cqo");
    E("    idiv r9");
    E("    mov  rbx, rax         ; rbx = row");
    E("    ; offset x by half brick on odd rows");
    E("    test rbx, 1");
    E("    jz   .br_even");
    E("    mov  rax, r8");
    E("    shr  rax, 1");
    E("    add  r12, rax         ; x += bw/2");
    E(".br_even:");
    E("    ; check mortar: x % bw < mortar || y % bh < mortar");
    E("    mov  rax, r12");
    E("    cqo");
    E("    idiv r8");
    E("    mov  rcx, rdx         ; rcx = x % bw");
    E("    mov  rax, r13");
    E("    cqo");
    E("    idiv r9");
    E("    ; rdx = y % bh");
    E("    mov  r10, [rsp+64]    ; mortar (3 pushes + ret = 32, +32 shadow = 64)");
    E("    cmp  rcx, r10");
    E("    jl   .br_mortar");
    E("    cmp  rdx, r10");
    E("    jl   .br_mortar");
    E("    mov  rax, 255         ; brick");
    E("    jmp  .br_done");
    E(".br_mortar:");
    E("    xor  rax, rax         ; mortar");
    E(".br_done:");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbx");
    E("    ret");
    E("");

    // tex.noise2d(x=rcx, y=rdx, seed=r8) -> rax (0-255 hash noise)
    E("; --- _slag_tex_noise2d (rcx=x, rdx=y, r8=seed) -> rax ---");
    E("_slag_tex_noise2d:");
    E("    ; Simple hash-based noise: hash(x ^ seed, y) & 255");
    E("    xor  rcx, r8          ; mix seed into x");
    E("    imul rcx, 374761393");
    E("    add  rcx, rdx");
    E("    imul rcx, 668265263");
    E("    mov  rax, rcx");
    E("    shr  rax, 15");
    E("    xor  rcx, rax");
    E("    mov  rax, 2246822519");
    E("    imul rcx, rax");
    E("    mov  rax, rcx");
    E("    shr  rax, 13");
    E("    xor  rcx, rax");
    E("    mov  rax, 3266489917");
    E("    imul rcx, rax");
    E("    mov  rax, rcx");
    E("    shr  rax, 16");
    E("    xor  rax, rcx");
    E("    and  rax, 255");
    E("    ret");
    E("");

    // TODO: tex.perlin2d - requires floating point interpolation
    // TODO: tex.wood - requires sin approximation
    // TODO: tex.marble - requires perlin + sin
}
