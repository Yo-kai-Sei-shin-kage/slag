// mesh_runtime.c — mesh management and BMP parsing for Slag.
// Emits NASM asm via cg_emit.
//
// BMP format (24-bit uncompressed):
//   offset 0:  'BM' signature
//   offset 10: pixel data offset (4 bytes LE)
//   offset 18: width (4 bytes LE)
//   offset 22: height (4 bytes LE)
//   Rows stored bottom-up, 3 bytes BGR per pixel, padded to 4-byte boundary

#include "codegen_internal.h"
#include "mesh_runtime.h"

#define E(...) cg_emit(cg, __VA_ARGS__)

void emit_mesh_imports(Codegen *cg) {
    // Uses HeapAlloc/HeapFree from window_runtime
    (void)cg;
}

void emit_mesh_bss(Codegen *cg) {
    (void)cg;
    // No BSS needed
}

void emit_mesh_data(Codegen *cg) {
    (void)cg;
    // No data constants needed
}

void emit_mesh_runtime(Codegen *cg) {
    E("; ===================== mesh runtime =====================");
    E("");

    // mesh.bmp_width(ptr=rcx) -> rax
    E("; --- _slag_mesh_bmp_width (rcx=ptr) -> rax ---");
    E("_slag_mesh_bmp_width:");
    E("    movzx eax, word [rcx+18]  ; width at offset 18 (LE, 4 bytes but usually <64k)");
    E("    movzx edx, word [rcx+20]");
    E("    shl  rdx, 16");
    E("    or   rax, rdx");
    E("    ret");
    E("");

    // mesh.bmp_height(ptr=rcx) -> rax
    E("; --- _slag_mesh_bmp_height (rcx=ptr) -> rax ---");
    E("_slag_mesh_bmp_height:");
    E("    movzx eax, word [rcx+22]  ; height at offset 22");
    E("    movzx edx, word [rcx+24]");
    E("    shl  rdx, 16");
    E("    or   rax, rdx");
    E("    ret");
    E("");

    // mesh.bmp_pixel(ptr=rcx, x=rdx, y=r8) -> rax (0x00RRGGBB)
    E("; --- _slag_mesh_bmp_pixel (rcx=ptr, rdx=x, r8=y) -> rax ---");
    E("_slag_mesh_bmp_pixel:");
    E("    push rbx");
    E("    push r12");
    E("    push r13");
    E("    mov  rbx, rcx             ; rbx = bmp ptr");
    E("    mov  r12, rdx             ; r12 = x");
    E("    mov  r13, r8              ; r13 = y");
    E("    ; get width");
    E("    movzx eax, word [rbx+18]");
    E("    movzx ecx, word [rbx+20]");
    E("    shl  rcx, 16");
    E("    or   rax, rcx");
    E("    mov  r8, rax              ; r8 = width");
    E("    ; get height");
    E("    movzx eax, word [rbx+22]");
    E("    movzx ecx, word [rbx+24]");
    E("    shl  rcx, 16");
    E("    or   rax, rcx");
    E("    mov  r9, rax              ; r9 = height");
    E("    ; get pixel data offset");
    E("    movzx eax, word [rbx+10]");
    E("    movzx ecx, word [rbx+12]");
    E("    shl  rcx, 16");
    E("    or   rax, rcx");
    E("    mov  r10, rax             ; r10 = data offset");
    E("    ; row stride = (width*3 + 3) & ~3 (align to 4)");
    E("    mov  rax, r8");
    E("    imul rax, 3");
    E("    add  rax, 3");
    E("    and  rax, ~3");
    E("    mov  r11, rax             ; r11 = row stride");
    E("    ; BMP is bottom-up: flip y");
    E("    mov  rax, r9");
    E("    dec  rax");
    E("    sub  rax, r13             ; rax = (height-1) - y");
    E("    ; pixel addr = ptr + data_offset + flipped_y * stride + x * 3");
    E("    imul rax, r11");
    E("    mov  rcx, r12");
    E("    imul rcx, 3");
    E("    add  rax, rcx");
    E("    add  rax, r10");
    E("    add  rax, rbx             ; rax = pixel address");
    E("    ; read BGR, return RGB");
    E("    movzx ecx, byte [rax+0]   ; B");
    E("    movzx edx, byte [rax+1]   ; G");
    E("    movzx eax, byte [rax+2]   ; R");
    E("    shl  eax, 16              ; R << 16");
    E("    shl  edx, 8               ; G << 8");
    E("    or   eax, edx");
    E("    or   eax, ecx             ; 0x00RRGGBB");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbx");
    E("    ret");
    E("");

    // mesh.bmp_gray(ptr=rcx, x=rdx, y=r8) -> rax (0-255)
    E("; --- _slag_mesh_bmp_gray (rcx=ptr, rdx=x, r8=y) -> rax ---");
    E("_slag_mesh_bmp_gray:");
    E("    push rbx");
    E("    push r12");
    E("    push r13");
    E("    mov  rbx, rcx");
    E("    mov  r12, rdx");
    E("    mov  r13, r8");
    E("    ; get width");
    E("    movzx eax, word [rbx+18]");
    E("    movzx ecx, word [rbx+20]");
    E("    shl  rcx, 16");
    E("    or   rax, rcx");
    E("    mov  r8, rax");
    E("    ; get height");
    E("    movzx eax, word [rbx+22]");
    E("    movzx ecx, word [rbx+24]");
    E("    shl  rcx, 16");
    E("    or   rax, rcx");
    E("    mov  r9, rax");
    E("    ; get data offset");
    E("    movzx eax, word [rbx+10]");
    E("    movzx ecx, word [rbx+12]");
    E("    shl  rcx, 16");
    E("    or   rax, rcx");
    E("    mov  r10, rax");
    E("    ; row stride");
    E("    mov  rax, r8");
    E("    imul rax, 3");
    E("    add  rax, 3");
    E("    and  rax, ~3");
    E("    mov  r11, rax");
    E("    ; flip y");
    E("    mov  rax, r9");
    E("    dec  rax");
    E("    sub  rax, r13");
    E("    ; pixel addr");
    E("    imul rax, r11");
    E("    mov  rcx, r12");
    E("    imul rcx, 3");
    E("    add  rax, rcx");
    E("    add  rax, r10");
    E("    add  rax, rbx");
    E("    ; read BGR, compute grayscale: (R + G + B) / 3");
    E("    movzx ecx, byte [rax+0]   ; B");
    E("    movzx edx, byte [rax+1]   ; G");
    E("    movzx eax, byte [rax+2]   ; R");
    E("    add  eax, edx");
    E("    add  eax, ecx");
    E("    mov  ecx, 3");
    E("    xor  edx, edx");
    E("    div  ecx                  ; rax = (R+G+B)/3");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbx");
    E("    ret");
    E("");

    // mesh.create(verts=rcx, faces=rdx) -> rax (handle)
    E("; --- _slag_mesh_create (rcx=verts, rdx=faces) -> rax ---");
    E("_slag_mesh_create:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 64");
    E("    mov  [rbp-8], rcx         ; vertex_count");
    E("    mov  [rbp-16], rdx        ; face_count");
    E("    ; allocate mesh struct (32 bytes)");
    E("    mov  rcx, [_mem_heap]");
    E("    test rcx, rcx");
    E("    jnz  .mc_have_heap");
    E("    call GetProcessHeap");
    E("    mov  [_mem_heap], rax");
    E("    mov  rcx, rax");
    E(".mc_have_heap:");
    E("    mov  rdx, 8               ; HEAP_ZERO_MEMORY");
    E("    mov  r8, 32               ; struct size");
    E("    call HeapAlloc");
    E("    test rax, rax");
    E("    jz   .mc_fail");
    E("    mov  [rbp-24], rax        ; mesh ptr");
    E("    ; store counts");
    E("    mov  rcx, [rbp-8]");
    E("    mov  [rax], rcx           ; vertex_count");
    E("    mov  rcx, [rbp-16]");
    E("    mov  [rax+8], rcx         ; face_count");
    E("    ; allocate vertices: count * 3 * 8 bytes");
    E("    mov  rcx, [_mem_heap]");
    E("    mov  rdx, 8");
    E("    mov  r8, [rbp-8]");
    E("    imul r8, 24               ; 3 coords * 8 bytes");
    E("    call HeapAlloc");
    E("    mov  rcx, [rbp-24]");
    E("    mov  [rcx+16], rax        ; vertices_ptr");
    E("    ; allocate faces: count * 3 * 8 bytes");
    E("    mov  rcx, [_mem_heap]");
    E("    mov  rdx, 8");
    E("    mov  r8, [rbp-16]");
    E("    imul r8, 24               ; 3 indices * 8 bytes");
    E("    call HeapAlloc");
    E("    mov  rcx, [rbp-24]");
    E("    mov  [rcx+24], rax        ; faces_ptr");
    E("    mov  rax, [rbp-24]        ; return handle");
    E("    jmp  .mc_done");
    E(".mc_fail:");
    E("    xor  rax, rax");
    E(".mc_done:");
    E("    add  rsp, 64");
    E("    pop  rbp");
    E("    ret");
    E("");

    // mesh.free(handle=rcx)
    E("; --- _slag_mesh_free (rcx=handle) ---");
    E("_slag_mesh_free:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48");
    E("    mov  [rbp-8], rcx         ; handle");
    E("    test rcx, rcx");
    E("    jz   .mf_done");
    E("    ; free vertices");
    E("    mov  rax, [rcx+16]");
    E("    test rax, rax");
    E("    jz   .mf_no_verts");
    E("    mov  rcx, [_mem_heap]");
    E("    xor  rdx, rdx");
    E("    mov  r8, rax");
    E("    call HeapFree");
    E(".mf_no_verts:");
    E("    ; free faces");
    E("    mov  rcx, [rbp-8]");
    E("    mov  rax, [rcx+24]");
    E("    test rax, rax");
    E("    jz   .mf_no_faces");
    E("    mov  rcx, [_mem_heap]");
    E("    xor  rdx, rdx");
    E("    mov  r8, rax");
    E("    call HeapFree");
    E(".mf_no_faces:");
    E("    ; free struct");
    E("    mov  rcx, [_mem_heap]");
    E("    xor  rdx, rdx");
    E("    mov  r8, [rbp-8]");
    E("    call HeapFree");
    E(".mf_done:");
    E("    add  rsp, 48");
    E("    pop  rbp");
    E("    ret");
    E("");

    // mesh.vertex_count(handle=rcx) -> rax
    E("; --- _slag_mesh_vertex_count (rcx=handle) -> rax ---");
    E("_slag_mesh_vertex_count:");
    E("    mov  rax, [rcx]");
    E("    ret");
    E("");

    // mesh.face_count(handle=rcx) -> rax
    E("; --- _slag_mesh_face_count (rcx=handle) -> rax ---");
    E("_slag_mesh_face_count:");
    E("    mov  rax, [rcx+8]");
    E("    ret");
    E("");

    // mesh.set_vertex(handle=rcx, i=rdx, x=r8, y=r9, z=[rsp+40])
    E("; --- _slag_mesh_set_vertex (rcx=h, rdx=i, r8=x, r9=y, [rsp+40]=z) ---");
    E("_slag_mesh_set_vertex:");
    E("    mov  rax, [rcx+16]        ; vertices_ptr");
    E("    imul rdx, 24              ; i * 24 bytes");
    E("    add  rax, rdx");
    E("    mov  [rax], r8            ; x");
    E("    mov  [rax+8], r9          ; y");
    E("    mov  rcx, [rsp+40]        ; z (5th arg)");
    E("    mov  [rax+16], rcx");
    E("    ret");
    E("");

    // mesh.get_vertex_x(handle=rcx, i=rdx) -> rax
    E("; --- _slag_mesh_get_vertex_x (rcx=h, rdx=i) -> rax ---");
    E("_slag_mesh_get_vertex_x:");
    E("    mov  rax, [rcx+16]");
    E("    imul rdx, 24");
    E("    mov  rax, [rax+rdx]");
    E("    ret");
    E("");

    // mesh.get_vertex_y(handle=rcx, i=rdx) -> rax
    E("; --- _slag_mesh_get_vertex_y (rcx=h, rdx=i) -> rax ---");
    E("_slag_mesh_get_vertex_y:");
    E("    mov  rax, [rcx+16]");
    E("    imul rdx, 24");
    E("    mov  rax, [rax+rdx+8]");
    E("    ret");
    E("");

    // mesh.get_vertex_z(handle=rcx, i=rdx) -> rax
    E("; --- _slag_mesh_get_vertex_z (rcx=h, rdx=i) -> rax ---");
    E("_slag_mesh_get_vertex_z:");
    E("    mov  rax, [rcx+16]");
    E("    imul rdx, 24");
    E("    mov  rax, [rax+rdx+16]");
    E("    ret");
    E("");

    // mesh.set_face(handle=rcx, i=rdx, v0=r8, v1=r9, v2=[rsp+40])
    E("; --- _slag_mesh_set_face (rcx=h, rdx=i, r8=v0, r9=v1, [rsp+40]=v2) ---");
    E("_slag_mesh_set_face:");
    E("    mov  rax, [rcx+24]        ; faces_ptr");
    E("    imul rdx, 24              ; i * 24 bytes");
    E("    add  rax, rdx");
    E("    mov  [rax], r8            ; v0");
    E("    mov  [rax+8], r9          ; v1");
    E("    mov  rcx, [rsp+40]");
    E("    mov  [rax+16], rcx        ; v2");
    E("    ret");
    E("");

    // mesh.get_face(handle=rcx, i=rdx, c=r8) -> rax (vertex index)
    E("; --- _slag_mesh_get_face (rcx=h, rdx=i, r8=c) -> rax ---");
    E("_slag_mesh_get_face:");
    E("    mov  rax, [rcx+24]        ; faces_ptr");
    E("    imul rdx, 24");
    E("    add  rax, rdx");
    E("    imul r8, 8                ; c * 8");
    E("    mov  rax, [rax+r8]");
    E("    ret");
    E("");

    // mesh.from_heightmap(bmp_ptr=rcx, scale_xz=rdx, scale_y=r8) -> rax (handle)
    // Creates a grid mesh from grayscale BMP
    E("; --- _slag_mesh_from_heightmap (rcx=bmp, rdx=scale_xz, r8=scale_y) -> rax ---");
    E("_slag_mesh_from_heightmap:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 128");
    E("    push rbx");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    push r15");
    E("    mov  [rbp-8], rcx         ; bmp_ptr");
    E("    mov  [rbp-16], rdx        ; scale_xz");
    E("    mov  [rbp-24], r8         ; scale_y");
    E("    ; get dimensions");
    E("    movzx eax, word [rcx+18]");
    E("    mov  [rbp-32], rax        ; width");
    E("    movzx eax, word [rcx+22]");
    E("    mov  [rbp-40], rax        ; height");
    E("    ; vertex count = width * height");
    E("    mov  rax, [rbp-32]");
    E("    imul rax, [rbp-40]");
    E("    mov  [rbp-48], rax        ; vertex_count");
    E("    ; face count = (width-1) * (height-1) * 2");
    E("    mov  rax, [rbp-32]");
    E("    dec  rax");
    E("    mov  rcx, [rbp-40]");
    E("    dec  rcx");
    E("    imul rax, rcx");
    E("    shl  rax, 1               ; * 2 triangles per quad");
    E("    mov  [rbp-56], rax        ; face_count");
    E("    ; create mesh");
    E("    mov  rcx, [rbp-48]");
    E("    mov  rdx, [rbp-56]");
    E("    call _slag_mesh_create");
    E("    test rax, rax");
    E("    jz   .mfh_fail");
    E("    mov  [rbp-64], rax        ; mesh handle");
    E("    ; fill vertices: loop y, x");
    E("    xor  r12, r12             ; y = 0");
    E(".mfh_vert_y:");
    E("    cmp  r12, [rbp-40]");
    E("    jge  .mfh_vert_done");
    E("    xor  r13, r13             ; x = 0");
    E(".mfh_vert_x:");
    E("    cmp  r13, [rbp-32]");
    E("    jge  .mfh_vert_x_done");
    E("    ; vertex index = y * width + x");
    E("    mov  rax, r12");
    E("    imul rax, [rbp-32]");
    E("    add  rax, r13");
    E("    mov  r14, rax             ; r14 = vertex index");
    E("    ; get grayscale height");
    E("    mov  rcx, [rbp-8]");
    E("    mov  rdx, r13");
    E("    mov  r8, r12");
    E("    call _slag_mesh_bmp_gray");
    E("    mov  r15, rax             ; r15 = gray (0-255)");
    E("    ; vx = x * scale_xz (16.16 fixed)");
    E("    mov  rax, r13");
    E("    shl  rax, 16              ; convert to 16.16");
    E("    imul rax, [rbp-16]");
    E("    sar  rax, 16              ; normalize");
    E("    mov  r8, rax              ; r8 = vx");
    E("    ; vy = gray * scale_y / 255 (16.16 fixed)");
    E("    mov  rax, r15");
    E("    shl  rax, 16");
    E("    imul rax, [rbp-24]");
    E("    mov  rcx, 255");
    E("    cqo");
    E("    idiv rcx");
    E("    mov  r9, rax              ; r9 = vy");
    E("    ; vz = y * scale_xz");
    E("    mov  rax, r12");
    E("    shl  rax, 16");
    E("    imul rax, [rbp-16]");
    E("    sar  rax, 16");
    E("    mov  [rbp-72], rax        ; vz (push to stack for 5th arg)");
    E("    ; set_vertex(handle, index, vx, vy, vz)");
    E("    mov  rcx, [rbp-64]");
    E("    mov  rdx, r14");
    E("    ; r8, r9 already set");
    E("    mov  rax, [rbp-72]");
    E("    mov  [rsp+32], rax        ; 5th arg");
    E("    call _slag_mesh_set_vertex");
    E("    inc  r13");
    E("    jmp  .mfh_vert_x");
    E(".mfh_vert_x_done:");
    E("    inc  r12");
    E("    jmp  .mfh_vert_y");
    E(".mfh_vert_done:");
    E("    ; fill faces: 2 triangles per quad");
    E("    xor  r12, r12             ; y = 0");
    E("    xor  rbx, rbx             ; face_index = 0");
    E(".mfh_face_y:");
    E("    mov  rax, [rbp-40]");
    E("    dec  rax");
    E("    cmp  r12, rax");
    E("    jge  .mfh_face_done");
    E("    xor  r13, r13             ; x = 0");
    E(".mfh_face_x:");
    E("    mov  rax, [rbp-32]");
    E("    dec  rax");
    E("    cmp  r13, rax");
    E("    jge  .mfh_face_x_done");
    E("    ; quad corners: TL, TR, BL, BR");
    E("    ; TL = y * width + x");
    E("    mov  rax, r12");
    E("    imul rax, [rbp-32]");
    E("    add  rax, r13");
    E("    mov  r14, rax             ; TL");
    E("    mov  r15, rax");
    E("    inc  r15                  ; TR = TL + 1");
    E("    mov  r8, r14");
    E("    add  r8, [rbp-32]         ; BL = TL + width");
    E("    mov  r9, r8");
    E("    inc  r9                   ; BR = BL + 1");
    E("    ; triangle 1: TL, BL, TR");
    E("    mov  rcx, [rbp-64]");
    E("    mov  rdx, rbx");
    E("    ; r8 = BL (already), need to reorder: TL, BL, TR");
    E("    mov  [rbp-80], r14        ; save TL");
    E("    mov  [rbp-88], r8         ; save BL");
    E("    mov  [rbp-96], r15        ; save TR");
    E("    mov  r8, r14              ; v0 = TL");
    E("    mov  r9, [rbp-88]         ; v1 = BL");
    E("    mov  rax, [rbp-96]");
    E("    mov  [rsp+32], rax        ; v2 = TR");
    E("    call _slag_mesh_set_face");
    E("    inc  rbx");
    E("    ; triangle 2: TR, BL, BR");
    E("    mov  rcx, [rbp-64]");
    E("    mov  rdx, rbx");
    E("    mov  r8, [rbp-96]         ; v0 = TR");
    E("    mov  r9, [rbp-88]         ; v1 = BL");
    E("    mov  [rsp+32], r9");
    E("    inc  qword [rsp+32]       ; v2 = BR = BL + 1");
    E("    call _slag_mesh_set_face");
    E("    inc  rbx");
    E("    inc  r13");
    E("    jmp  .mfh_face_x");
    E(".mfh_face_x_done:");
    E("    inc  r12");
    E("    jmp  .mfh_face_y");
    E(".mfh_face_done:");
    E("    mov  rax, [rbp-64]        ; return handle");
    E("    jmp  .mfh_exit");
    E(".mfh_fail:");
    E("    xor  rax, rax");
    E(".mfh_exit:");
    E("    pop  r15");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbx");
    E("    add  rsp, 128");
    E("    pop  rbp");
    E("    ret");
    E("");
}
