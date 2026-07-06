// file_runtime.c -- handle-based file I/O primitives for Slag.
// Mirrors mem_runtime.c / net_runtime.c: emits NASM asm via cg_emit.
//
// Builtins (wired in codegen.c as file.*):
//   file.open(path, mode)   -> int handle (-1 on fail)
//                              mode: 1=read, 2=write(truncate), 3=append
//   file.close(handle)      -> (void)
//   file.read(handle, buf, n)  -> int bytes read (-1 on fail)
//   file.write(handle, buf, n) -> int bytes written (-1 on fail)
//   file.seek(handle, offset, whence) -> int new position (-1 on fail)
//                              whence: 0=start, 1=current, 2=end
//   file.size(handle)       -> int size in bytes (-1 on fail)
//   file.exists(path)       -> int bool (1/0)
//   file.delete(path)       -> int bool (1/0)
//   file.mkdir(path)        -> int bool (1/0)
//
// Paths are passed as a plain pointer (rcx) and treated as null-terminated
// C strings -- string literals in Slag are always emitted with a trailing
// 0 byte, matching the existing _slag_readfile convention. Non-literal str
// values are not guaranteed null-terminated and should not be used as paths.
//
// Sizes/offsets use the classic 32-bit Win32 file APIs (GetFileSize,
// SetFilePointer), consistent with the existing _slag_readfile -- files
// over ~4GB are not supported, matching that existing limitation.

#include "codegen_internal.h"
#include "file_runtime.h"

#define E(...) cg_emit(cg, __VA_ARGS__)

void emit_file_imports(Codegen *cg) {
    E("extern WriteFile");
    E("extern SetFilePointer");
    E("extern DeleteFileA");
    E("extern GetFileAttributesA");
    E("extern CreateDirectoryA");
    E("extern FindFirstFileA");
    E("extern FindNextFileA");
    E("extern FindClose");
}

void emit_file_bss(Codegen *cg) {
    (void)cg;
}

void emit_file_runtime(Codegen *cg) {
    E("; ===================== file runtime (handle-based I/O) =====================");

    E("; --- _slag_file_open (rcx=path, rdx=mode) -> rax ---");
    E("_slag_file_open:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push r12");
    E("    push r13");
    E("    mov  r12, rcx           ; path ptr");
    E("    mov  r13, rdx           ; mode");
    E("");
    E("    cmp  r13, 1");
    E("    je   .fopen_read");
    E("    cmp  r13, 2");
    E("    je   .fopen_write");
    E("    cmp  r13, 3");
    E("    je   .fopen_append");
    E("    mov  rax, -1            ; invalid mode");
    E("    jmp  .fopen_done");
    E("");
    E(".fopen_read:");
    E("    mov  rcx, r12");
    E("    mov  rdx, 0x80000000    ; GENERIC_READ");
    E("    mov  r8,  1             ; FILE_SHARE_READ");
    E("    xor  r9,  r9            ; lpSecurityAttributes = NULL");
    E("    sub  rsp, 64            ; shadow(32) + 3 stack args(24) + pad(8)");
    E("    mov  qword [rsp+32], 3  ; OPEN_EXISTING");
    E("    mov  qword [rsp+40], 0x80 ; FILE_ATTRIBUTE_NORMAL");
    E("    mov  qword [rsp+48], 0  ; hTemplateFile = NULL");
    E("    call CreateFileA");
    E("    add  rsp, 64");
    E("    jmp  .fopen_done");
    E("");
    E(".fopen_write:");
    E("    mov  rcx, r12");
    E("    mov  rdx, 0x40000000    ; GENERIC_WRITE");
    E("    xor  r8,  r8            ; dwShareMode = 0 (exclusive)");
    E("    xor  r9,  r9            ; lpSecurityAttributes = NULL");
    E("    sub  rsp, 64");
    E("    mov  qword [rsp+32], 2  ; CREATE_ALWAYS");
    E("    mov  qword [rsp+40], 0x80 ; FILE_ATTRIBUTE_NORMAL");
    E("    mov  qword [rsp+48], 0  ; hTemplateFile = NULL");
    E("    call CreateFileA");
    E("    add  rsp, 64");
    E("    jmp  .fopen_done");
    E("");
    E(".fopen_append:");
    E("    mov  rcx, r12");
    E("    mov  rdx, 0x40000000    ; GENERIC_WRITE");
    E("    xor  r8,  r8            ; dwShareMode = 0 (exclusive)");
    E("    xor  r9,  r9            ; lpSecurityAttributes = NULL");
    E("    sub  rsp, 64");
    E("    mov  qword [rsp+32], 4  ; OPEN_ALWAYS");
    E("    mov  qword [rsp+40], 0x80 ; FILE_ATTRIBUTE_NORMAL");
    E("    mov  qword [rsp+48], 0  ; hTemplateFile = NULL");
    E("    call CreateFileA");
    E("    add  rsp, 64");
    E("    cmp  rax, -1            ; INVALID_HANDLE_VALUE");
    E("    je   .fopen_done");
    E("    mov  r12, rax           ; save handle");
    E("    mov  rcx, r12");
    E("    xor  rdx, rdx");
    E("    xor  r8,  r8");
    E("    mov  r9,  2             ; FILE_END");
    E("    sub  rsp, 32");
    E("    call SetFilePointer");
    E("    add  rsp, 32");
    E("    mov  rax, r12           ; return handle regardless of seek result");
    E("");
    E(".fopen_done:");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbp");
    E("    ret");
    E("");

    E("; --- _slag_file_close (rcx=handle) ---");
    E("_slag_file_close:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32            ; shadow space");
    E("    call CloseHandle");
    E("    add  rsp, 32");
    E("    pop  rbp");
    E("    ret");
    E("");

    E("; --- _slag_file_read (rcx=handle, rdx=buf, r8=nbytes) -> rax ---");
    E("_slag_file_read:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48");
    E("    lea  r9,  [rsp+32]");
    E("    mov  qword [rsp+32], 0");
    E("    mov  qword [rsp+40], 0  ; lpOverlapped = NULL");
    E("    call ReadFile");
    E("    test rax, rax");
    E("    jz   .fread_fail");
    E("    mov  eax, [rsp+32]      ; bytesRead, zero-extended to rax");
    E("    add  rsp, 48");
    E("    pop  rbp");
    E("    ret");
    E(".fread_fail:");
    E("    add  rsp, 48");
    E("    mov  rax, -1");
    E("    pop  rbp");
    E("    ret");
    E("");

    E("; --- _slag_file_write (rcx=handle, rdx=buf, r8=nbytes) -> rax ---");
    E("_slag_file_write:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48");
    E("    lea  r9,  [rsp+32]");
    E("    mov  qword [rsp+32], 0");
    E("    mov  qword [rsp+40], 0  ; lpOverlapped = NULL");
    E("    call WriteFile");
    E("    test rax, rax");
    E("    jz   .fwrite_fail");
    E("    mov  eax, [rsp+32]      ; bytesWritten, zero-extended to rax");
    E("    add  rsp, 48");
    E("    pop  rbp");
    E("    ret");
    E(".fwrite_fail:");
    E("    add  rsp, 48");
    E("    mov  rax, -1");
    E("    pop  rbp");
    E("    ret");
    E("");

    E("; --- _slag_file_seek (rcx=handle, rdx=offset, r8=whence) -> rax ---");
    E("_slag_file_seek:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push r12");
    E("    sub  rsp, 8             ; alignment pad");
    E("    mov  r12, r8            ; save whence (r8 gets overwritten below)");
    E("    xor  r8,  r8            ; lpDistanceToMoveHigh = NULL");
    E("    mov  r9,  r12           ; dwMoveMethod = whence");
    E("    sub  rsp, 32");
    E("    call SetFilePointer");
    E("    add  rsp, 32");
    E("    mov  eax, eax           ; zero-extend 32-bit result to 64-bit");
    E("    cmp  eax, 0xFFFFFFFF    ; INVALID_SET_FILE_POINTER");
    E("    je   .fseek_fail");
    E("    jmp  .fseek_done");
    E(".fseek_fail:");
    E("    mov  rax, -1");
    E(".fseek_done:");
    E("    add  rsp, 8");
    E("    pop  r12");
    E("    pop  rbp");
    E("    ret");
    E("");

    E("; --- _slag_file_size (rcx=handle) -> rax ---");
    E("_slag_file_size:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    xor  rdx, rdx           ; lpFileSizeHigh = NULL");
    E("    call GetFileSize");
    E("    add  rsp, 32");
    E("    mov  eax, eax           ; zero-extend 32-bit result to 64-bit");
    E("    cmp  eax, 0xFFFFFFFF    ; INVALID_FILE_SIZE");
    E("    je   .fsize_fail");
    E("    jmp  .fsize_done");
    E(".fsize_fail:");
    E("    mov  rax, -1");
    E(".fsize_done:");
    E("    pop  rbp");
    E("    ret");
    E("");

    E("; --- _slag_file_exists (rcx=path) -> rax ---");
    E("_slag_file_exists:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    call GetFileAttributesA");
    E("    add  rsp, 32");
    E("    cmp  eax, 0xFFFFFFFF    ; INVALID_FILE_ATTRIBUTES");
    E("    je   .fexists_false");
    E("    mov  rax, 1");
    E("    jmp  .fexists_done");
    E(".fexists_false:");
    E("    xor  rax, rax");
    E(".fexists_done:");
    E("    pop  rbp");
    E("    ret");
    E("");

    E("; --- _slag_file_delete (rcx=path) -> rax ---");
    E("_slag_file_delete:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    call DeleteFileA");
    E("    add  rsp, 32");
    E("    test rax, rax");
    E("    jz   .fdelete_fail");
    E("    mov  rax, 1");
    E("    jmp  .fdelete_done");
    E(".fdelete_fail:");
    E("    xor  rax, rax");
    E(".fdelete_done:");
    E("    pop  rbp");
    E("    ret");
    E("");

    E("; --- _slag_file_mkdir (rcx=path) -> rax ---");
    E("_slag_file_mkdir:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    xor  rdx, rdx           ; lpSecurityAttributes = NULL");
    E("    call CreateDirectoryA");
    E("    add  rsp, 32");
    E("    test rax, rax");
    E("    jz   .fmkdir_fail");
    E("    mov  rax, 1");
    E("    jmp  .fmkdir_done");
    E(".fmkdir_fail:");
    E("    xor  rax, rax");
    E(".fmkdir_done:");
    E("    pop  rbp");
    E("    ret");
    E("");
    // Per-handle directory listing. Each search's state (the real Win32
    // search HANDLE plus a WIN32_FIND_DATAA buffer) is heap-allocated
    // independently per file.list_open() call -- mirrors window_runtime.c
    // giving each window its own heap-allocated state struct -- so multiple
    // listings can be open/interleaved without clobbering each other.
    //
    // Struct layout (LISTSTATE_SIZE bytes, HeapAlloc'd):
    //   +0   : real Win32 search HANDLE (8 bytes)
    //   +8   : WIN32_FIND_DATAA (326 bytes; cFileName sits at +52 within
    //          the struct, i.e. offset 8+44 -- the fixed, ABI-stable
    //          WIN32_FIND_DATAA layout: dwFileAttributes(4) + 3x
    //          FILETIME(8 each) + nFileSizeHigh/Low(4+4) + 2x
    //          dwReserved(4+4) = 44 bytes before cFileName)
    E("LISTSTATE_HANDLE    equ 0");
    E("LISTSTATE_FINDDATA  equ 8");
    E("LISTSTATE_CFILENAME equ 52");
    E("LISTSTATE_SIZE      equ 336");
    E("");

    // _slag_file_list_open(rcx=pattern ptr) -> rax = handle (-1 on fail)
    // pattern is a glob like "dir/*" (Win32 FindFirstFileA semantics).
    E("; --- _slag_file_list_open (rcx=pattern) -> rax ---");
    E("_slag_file_list_open:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push r12");
    E("    push r13");
    E("    mov  r12, rcx           ; pattern ptr");
    E("");
    E("    sub  rsp, 32");
    E("    call GetProcessHeap");
    E("    add  rsp, 32");
    E("    mov  rcx, rax           ; heap handle");
    E("    xor  rdx, rdx           ; dwFlags = 0");
    E("    mov  r8,  LISTSTATE_SIZE");
    E("    sub  rsp, 32");
    E("    call HeapAlloc");
    E("    add  rsp, 32");
    E("    test rax, rax");
    E("    jz   .flopen_fail_noalloc");
    E("    mov  r13, rax           ; r13 = our search-state struct ptr");
    E("");
    E("    ; FindFirstFileA(pattern, &struct[LISTSTATE_FINDDATA])");
    E("    mov  rcx, r12");
    E("    lea  rdx, [r13 + LISTSTATE_FINDDATA]");
    E("    sub  rsp, 32");
    E("    call FindFirstFileA");
    E("    add  rsp, 32");
    E("    cmp  rax, -1            ; INVALID_HANDLE_VALUE");
    E("    je   .flopen_fail_free");
    E("    mov  [r13 + LISTSTATE_HANDLE], rax");
    E("    mov  rax, r13           ; return struct ptr as the handle");
    E("    jmp  .flopen_done");
    E("");
    E(".flopen_fail_free:");
    E("    sub  rsp, 32");
    E("    call GetProcessHeap");
    E("    add  rsp, 32");
    E("    mov  rcx, rax");
    E("    xor  rdx, rdx");
    E("    mov  r8,  r13");
    E("    sub  rsp, 32");
    E("    call HeapFree");
    E("    add  rsp, 32");
    E("    mov  rax, -1");
    E("    jmp  .flopen_done");
    E("");
    E(".flopen_fail_noalloc:");
    E("    mov  rax, -1");
    E("");
    E(".flopen_done:");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_file_list_next(rcx=struct ptr) -> rax = 1 (advanced) / 0 (no more)
    E("; --- _slag_file_list_next (rcx=handle) -> rax ---");
    E("_slag_file_list_next:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push r12");
    E("    sub  rsp, 8             ; alignment pad");
    E("    mov  r12, rcx           ; struct ptr");
    E("    mov  rcx, [r12 + LISTSTATE_HANDLE]");
    E("    lea  rdx, [r12 + LISTSTATE_FINDDATA]");
    E("    sub  rsp, 32");
    E("    call FindNextFileA");
    E("    add  rsp, 32");
    E("    test rax, rax");
    E("    jz   .flnext_false");
    E("    mov  rax, 1");
    E("    jmp  .flnext_done");
    E(".flnext_false:");
    E("    xor  rax, rax");
    E(".flnext_done:");
    E("    add  rsp, 8");
    E("    pop  r12");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_file_list_name(rcx=struct ptr) -> rax=ptr, rdx=len (str)
    // No Win32 call needed -- just points at cFileName and measures it.
    E("; --- _slag_file_list_name (rcx=handle) -> rax=ptr, rdx=len ---");
    E("_slag_file_list_name:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    lea  rax, [rcx + LISTSTATE_CFILENAME]");
    E("    mov  r8,  rax           ; scan pointer");
    E("    xor  rdx, rdx           ; length counter");
    E(".flname_loop:");
    E("    cmp  byte [r8], 0");
    E("    je   .flname_done");
    E("    inc  r8");
    E("    inc  rdx");
    E("    jmp  .flname_loop");
    E(".flname_done:");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_file_list_close(rcx=struct ptr)
    E("; --- _slag_file_list_close (rcx=handle) ---");
    E("_slag_file_list_close:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push r12");
    E("    push r13");
    E("    mov  r12, rcx           ; struct ptr");
    E("    mov  rcx, [r12 + LISTSTATE_HANDLE]");
    E("    sub  rsp, 32");
    E("    call FindClose");
    E("    add  rsp, 32");
    E("");
    E("    sub  rsp, 32");
    E("    call GetProcessHeap");
    E("    add  rsp, 32");
    E("    mov  r13, rax           ; heap handle");
    E("    mov  rcx, r13");
    E("    xor  rdx, rdx");
    E("    mov  r8,  r12");
    E("    sub  rsp, 32");
    E("    call HeapFree");
    E("    add  rsp, 32");
    E("");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbp");
    E("    ret");
    E("");

}
