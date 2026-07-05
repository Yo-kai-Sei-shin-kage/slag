// server_runtime.c -- persistent multi-client TCP server runtime for Slag.
// Backs the net.server_start / net.server_accept / net.server_send /
// net.server_recv / net.server_send_buf / net.server_recv_buf /
// net.server_stop builtins.
//
// Deliberately standalone from net_runtime.c (single-peer client/listen
// primitives): its own imports, its own .bss state, its own client-slot
// table. No shared globals between the two runtimes.

#include "codegen_internal.h"
#include "server_runtime.h"

#define E(...) cg_emit(cg, __VA_ARGS__)

// Max simultaneous client connections. Bump this single constant (and
// relink) to scale up to 64 -- every table size below derives from it.
#define SERVER_MAX_CLIENTS 4

// ----- imports (ws2_32) -------------------------------------------------
void emit_server_imports(Codegen *cg) {
    E("extern WSAStartup");
    E("extern WSACleanup");
    E("extern socket");
    E("extern bind");
    E("extern listen");
    E("extern accept");
    E("extern send");
    E("extern recv");
    E("extern closesocket");
    E("extern htons");
    E("extern ioctlsocket");
}

// ----- .bss globals -------------------------------------------------------
void emit_server_bss(Codegen *cg) {
    E("_srv_wsadata:     resb 512   ; WSADATA for server-mode WSAStartup");
    E("_srv_listen_sock: resq 1     ; non-blocking listening socket");
    E("_srv_sockaddr:    resb 16    ; sockaddr_in scratch");
    E("_srv_ioctl_arg:   resd 1     ; FIONBIO mode scratch (1 = non-blocking)");
    E("_srv_bytebuf:     resb 8     ; one-byte send/recv scratch");
    E("_srv_clients:     resq %d    ; client socket handles, 0 = empty slot", SERVER_MAX_CLIENTS);
    E("_srv_last_ok:     resq 1     ; 1 if last net.server_* op succeeded");
}

// ----- runtime procs -------------------------------------------------------
void emit_server_runtime(Codegen *cg) {
    E("; ================= persistent server runtime (ws2_32) =================");

    // _slag_server_start(port in rcx): WSAStartup + socket+bind+listen,
    // then flips the listen socket into non-blocking mode so
    // _slag_server_accept can be polled once per game-loop tick.
    E("; --- _slag_server_start (rcx = port) ---");
    E("_slag_server_start:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48");
    E("    mov  [rbp-8], rcx          ; save port");
    E("    mov  rcx, 0x0202           ; MAKEWORD(2,2)");
    E("    lea  rdx, [_srv_wsadata]");
    E("    call WSAStartup");
    E("    test eax, eax");
    E("    jnz  .svs_fail");
    E("    mov  rcx, 2                ; AF_INET");
    E("    mov  rdx, 1                ; SOCK_STREAM");
    E("    xor  r8, r8");
    E("    call socket");
    E("    mov  [_srv_listen_sock], rax");
    E("    cmp  rax, -1");
    E("    je   .svs_fail");
    E("    mov  word [_srv_sockaddr], 2      ; AF_INET");
    E("    mov  rcx, [rbp-8]");
    E("    call htons");
    E("    mov  word [_srv_sockaddr+2], ax   ; sin_port");
    E("    mov  dword [_srv_sockaddr+4], 0   ; INADDR_ANY");
    E("    mov  dword [_srv_sockaddr+8], 0");
    E("    mov  dword [_srv_sockaddr+12], 0");
    E("    mov  rcx, [_srv_listen_sock]");
    E("    lea  rdx, [_srv_sockaddr]");
    E("    mov  r8, 16");
    E("    call bind");
    E("    test eax, eax");
    E("    jnz  .svs_fail");
    E("    mov  rcx, [_srv_listen_sock]");
    E("    mov  rdx, %d", SERVER_MAX_CLIENTS);
    E("    call listen");
    E("    test eax, eax");
    E("    jnz  .svs_fail");
    E("    ; ioctlsocket(s, FIONBIO, &mode=1) -- non-blocking accept");
    E("    mov  dword [_srv_ioctl_arg], 1");
    E("    mov  rcx, [_srv_listen_sock]");
    E("    mov  rdx, 0x8004667E");
    E("    lea  r8, [_srv_ioctl_arg]");
    E("    call ioctlsocket");
    E("    ; clear the client slot table");
    E("    mov  rcx, %d", SERVER_MAX_CLIENTS);
    E("    lea  rdi, [_srv_clients]");
    E(".svs_clear:");
    E("    mov  qword [rdi], 0");
    E("    add  rdi, 8");
    E("    dec  rcx");
    E("    jnz  .svs_clear");
    E("    mov  qword [_srv_last_ok], 1");
    E("    jmp  .svs_done");
    E(".svs_fail:");
    E("    mov  qword [_srv_last_ok], 0");
    E(".svs_done:");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_server_accept() -> rax = new client slot index, or -1 if no
    // pending connection (non-blocking) or the table is full.
    E("; --- _slag_server_accept -> rax (slot idx or -1) ---");
    E("_slag_server_accept:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    mov  rcx, [_srv_listen_sock]");
    E("    test rcx, rcx");
    E("    jz   .sva_fail");
    E("    xor  rdx, rdx");
    E("    xor  r8, r8");
    E("    call accept");
    E("    cmp  rax, -1");
    E("    je   .sva_fail             ; nothing pending, or error");
    E("    mov  r10, rax              ; new client socket");
    E("    xor  r11, r11              ; scan index");
    E("    lea  r9, [_srv_clients]");
    E(".sva_scan:");
    E("    cmp  r11, %d", SERVER_MAX_CLIENTS);
    E("    jge  .sva_full");
    E("    cmp  qword [r9 + r11*8], 0");
    E("    jne  .sva_next");
    E("    mov  [r9 + r11*8], r10");
    E("    mov  rax, r11");
    E("    mov  qword [_srv_last_ok], 1");
    E("    jmp  .sva_done");
    E(".sva_next:");
    E("    inc  r11");
    E("    jmp  .sva_scan");
    E(".sva_full:");
    E("    mov  rcx, r10              ; table full: refuse the connection");
    E("    call closesocket");
    E(".sva_fail:");
    E("    mov  rax, -1");
    E(".sva_done:");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_server_send(idx in rcx, byte value in rdx)
    E("; --- _slag_server_send (rcx = slot idx, rdx = byte) ---");
    E("_slag_server_send:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    cmp  rcx, 0");
    E("    jl   .svsd_fail");
    E("    cmp  rcx, %d", SERVER_MAX_CLIENTS);
    E("    jge  .svsd_fail");
    E("    lea  r9, [_srv_clients]");
    E("    mov  r10, [r9 + rcx*8]");
    E("    test r10, r10");
    E("    jz   .svsd_fail");
    E("    mov  byte [_srv_bytebuf], dl");
    E("    mov  rcx, r10");
    E("    lea  rdx, [_srv_bytebuf]");
    E("    mov  r8, 1");
    E("    xor  r9, r9");
    E("    call send");
    E("    cmp  rax, 1");
    E("    sete al");
    E("    movzx rax, al");
    E("    mov  [_srv_last_ok], rax");
    E("    jmp  .svsd_done");
    E(".svsd_fail:");
    E("    mov  qword [_srv_last_ok], 0");
    E(".svsd_done:");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_server_recv(idx in rcx) -> rax = byte, or -1 on fail/disconnect
    E("; --- _slag_server_recv (rcx = slot idx) -> rax ---");
    E("_slag_server_recv:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 32");
    E("    mov  rbx, rcx              ; save idx across the `call recv`");
    E("    cmp  rbx, 0");
    E("    jl   .svrv_fail");
    E("    cmp  rbx, %d", SERVER_MAX_CLIENTS);
    E("    jge  .svrv_fail");
    E("    lea  r9, [_srv_clients]");
    E("    mov  r10, [r9 + rbx*8]");
    E("    test r10, r10");
    E("    jz   .svrv_fail");
    E("    mov  rcx, r10");
    E("    lea  rdx, [_srv_bytebuf]");
    E("    mov  r8, 1");
    E("    xor  r9, r9");
    E("    call recv");
    E("    cmp  rax, 1");
    E("    jne  .svrv_closed");
    E("    mov  qword [_srv_last_ok], 1");
    E("    movzx rax, byte [_srv_bytebuf]");
    E("    jmp  .svrv_done");
    E(".svrv_closed:");
    E("    lea  r9, [_srv_clients]");
    E("    mov  rcx, [r9 + rbx*8]");
    E("    call closesocket");
    E("    lea  r9, [_srv_clients]");
    E("    mov  qword [r9 + rbx*8], 0");
    E("    mov  qword [_srv_last_ok], 0");
    E("    mov  rax, -1");
    E("    jmp  .svrv_done");
    E(".svrv_fail:");
    E("    mov  qword [_srv_last_ok], 0");
    E("    mov  rax, -1");
    E(".svrv_done:");
    E("    add  rsp, 32");
    E("    pop  rbx");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_server_send_buf(idx=rcx, ptr=rdx, len=r8): loops until all
    // sent. On error/close, closes and frees the slot.
    E("; --- _slag_server_send_buf (rcx=idx, rdx=ptr, r8=len) ---");
    E("_slag_server_send_buf:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    push r12");
    E("    sub  rsp, 32");
    E("    mov  rbx, rcx              ; idx");
    E("    mov  rsi, rdx              ; buf ptr");
    E("    mov  rdi, r8               ; remaining len");
    E("    xor  r12, r12              ; bytes-sent offset");
    E("    cmp  rbx, 0");
    E("    jl   .svsb_fail");
    E("    cmp  rbx, %d", SERVER_MAX_CLIENTS);
    E("    jge  .svsb_fail");
    E("    lea  r9, [_srv_clients]");
    E("    cmp  qword [r9 + rbx*8], 0");
    E("    je   .svsb_fail");
    E(".svsb_loop:");
    E("    test rdi, rdi");
    E("    jz   .svsb_ok");
    E("    lea  r9, [_srv_clients]");
    E("    mov  rcx, [r9 + rbx*8]");
    E("    lea  rdx, [rsi + r12]");
    E("    mov  r8, rdi");
    E("    xor  r9, r9");
    E("    call send");
    E("    cmp  rax, 0");
    E("    jle  .svsb_disc");
    E("    add  r12, rax");
    E("    sub  rdi, rax");
    E("    jmp  .svsb_loop");
    E(".svsb_ok:");
    E("    mov  qword [_srv_last_ok], 1");
    E("    jmp  .svsb_done");
    E(".svsb_disc:");
    E("    lea  r9, [_srv_clients]");
    E("    mov  rcx, [r9 + rbx*8]");
    E("    call closesocket");
    E("    lea  r9, [_srv_clients]");
    E("    mov  qword [r9 + rbx*8], 0");
    E("    mov  qword [_srv_last_ok], 0");
    E("    jmp  .svsb_done");
    E(".svsb_fail:");
    E("    mov  qword [_srv_last_ok], 0");
    E(".svsb_done:");
    E("    add  rsp, 32");
    E("    pop  r12");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_server_recv_buf(idx=rcx, ptr=rdx, maxlen=r8) -> rax = bytes
    // received. 0/negative => peer closed/error; closes + frees the slot.
    E("; --- _slag_server_recv_buf (rcx=idx, rdx=ptr, r8=maxlen) -> rax ---");
    E("_slag_server_recv_buf:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 32");
    E("    mov  rbx, rcx              ; idx");
    E("    cmp  rbx, 0");
    E("    jl   .svrb_fail");
    E("    cmp  rbx, %d", SERVER_MAX_CLIENTS);
    E("    jge  .svrb_fail");
    E("    lea  r9, [_srv_clients]");
    E("    mov  r10, [r9 + rbx*8]");
    E("    test r10, r10");
    E("    jz   .svrb_fail");
    E("    mov  rcx, r10              ; socket (rdx=ptr, r8=maxlen untouched)");
    E("    xor  r9, r9");
    E("    call recv");
    E("    cmp  rax, 0");
    E("    jg   .svrb_ok");
    E("    lea  r9, [_srv_clients]");
    E("    mov  rcx, [r9 + rbx*8]");
    E("    call closesocket");
    E("    lea  r9, [_srv_clients]");
    E("    mov  qword [r9 + rbx*8], 0");
    E("    mov  qword [_srv_last_ok], 0");
    E("    jmp  .svrb_done");
    E(".svrb_ok:");
    E("    mov  qword [_srv_last_ok], 1");
    E("    jmp  .svrb_done");
    E(".svrb_fail:");
    E("    mov  qword [_srv_last_ok], 0");
    E("    mov  rax, -1");
    E(".svrb_done:");
    E("    add  rsp, 32");
    E("    pop  rbx");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_server_stop(): close every client socket + the listen socket,
    // then WSACleanup. Full teardown -- net.server_start must be called
    // again to open a new server session.
    E("; --- _slag_server_stop ---");
    E("_slag_server_stop:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 32");
    E("    xor  rbx, rbx");
    E(".svst_loop:");
    E("    cmp  rbx, %d", SERVER_MAX_CLIENTS);
    E("    jge  .svst_clients_done");
    E("    lea  rax, [_srv_clients]");
    E("    mov  rcx, [rax + rbx*8]");
    E("    test rcx, rcx");
    E("    jz   .svst_next");
    E("    call closesocket");
    E("    lea  rax, [_srv_clients]");
    E("    mov  qword [rax + rbx*8], 0");
    E(".svst_next:");
    E("    inc  rbx");
    E("    jmp  .svst_loop");
    E(".svst_clients_done:");
    E("    mov  rcx, [_srv_listen_sock]");
    E("    test rcx, rcx");
    E("    jz   .svst_clean");
    E("    call closesocket");
    E("    mov  qword [_srv_listen_sock], 0");
    E(".svst_clean:");
    E("    call WSACleanup");
    E("    mov  qword [_srv_last_ok], 1");
    E("    add  rsp, 32");
    E("    pop  rbx");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");
}
