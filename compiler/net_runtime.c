// net_runtime.c — Winsock (ws2_32) networking runtime for Slag.
// Mirrors window_runtime.c: emits NASM asm via cg_emit. The bulky procs
// live here; codegen.c only holds thin dispatch arms that evaluate args
// and `call` these labels.
//
// Connection-layer + single-byte transfer. Full buffered send/recv waits
// on the memory/buffer primitives (mem_runtime.c).

#include "codegen_internal.h"
#include "net_runtime.h"

#define E(...) cg_emit(cg, __VA_ARGS__)

// ----- imports (ws2_32) -------------------------------------------------
void emit_net_imports(Codegen *cg) {
    E("extern WSAStartup");
    E("extern WSACleanup");
    E("extern socket");
    E("extern bind");
    E("extern listen");
    E("extern accept");
    E("extern connect");
    E("extern send");
    E("extern recv");
    E("extern closesocket");
    E("extern htons");
    E("extern inet_addr");
}

// ----- .bss globals -----------------------------------------------------
void emit_net_bss(Codegen *cg) {
    E("_net_wsadata:   resb 512   ; WSADATA (actually ~408 bytes, padded)");
    E("_net_listen_sock: resq 1   ; listening socket");
    E("_net_conn_sock:   resq 1   ; active connection socket");
    E("_net_last_ok:     resq 1   ; 1 if last net op succeeded, else 0");
    E("_net_sockaddr:  resb 16    ; sockaddr_in scratch (16 bytes)");
    E("_net_bytebuf:   resb 8     ; one-byte send/recv scratch");
    E("_net_connected:   resq 1   ; 1 while active connection is alive");
}

// ----- runtime procs ----------------------------------------------------
void emit_net_runtime(Codegen *cg) {
    E("; ===================== net runtime (ws2_32) =====================");

    // _slag_net_start() : WSAStartup(MAKEWORD(2,2), &wsadata)
    // sets _net_last_ok = 1 on success (0 return), else 0
    E("; --- _slag_net_start ---");
    E("_slag_net_start:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    mov  rcx, 0x0202          ; MAKEWORD(2,2) = version 2.2");
    E("    lea  rdx, [_net_wsadata]");
    E("    call WSAStartup");
    E("    test eax, eax             ; 0 == success");
    E("    sete al");
    E("    movzx rax, al");
    E("    mov  [_net_last_ok], rax");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_net_listen(port in rcx) : create+bind+listen+accept, store conn
    // Blocks on accept until a peer connects. Sets _net_last_ok.
    E("; --- _slag_net_listen (rcx = port) ---");
    E("_slag_net_listen:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48");
    E("    mov  [rbp-8], rcx          ; save port");
    E("    ; socket(AF_INET=2, SOCK_STREAM=1, 0)");
    E("    mov  rcx, 2");
    E("    mov  rdx, 1");
    E("    xor  r8, r8");
    E("    call socket");
    E("    mov  [_net_listen_sock], rax");
    E("    cmp  rax, -1               ; INVALID_SOCKET");
    E("    je   .nl_fail");
    E("    ; build sockaddr_in: family=AF_INET, port=htons(port), addr=INADDR_ANY");
    E("    mov  word [_net_sockaddr], 2      ; AF_INET");
    E("    mov  rcx, [rbp-8]");
    E("    call htons");
    E("    mov  word [_net_sockaddr+2], ax   ; sin_port");
    E("    mov  dword [_net_sockaddr+4], 0   ; INADDR_ANY");
    E("    mov  dword [_net_sockaddr+8], 0");
    E("    mov  dword [_net_sockaddr+12], 0");
    E("    ; bind(s, &sockaddr, 16)");
    E("    mov  rcx, [_net_listen_sock]");
    E("    lea  rdx, [_net_sockaddr]");
    E("    mov  r8, 16");
    E("    call bind");
    E("    test eax, eax");
    E("    jnz  .nl_fail");
    E("    ; listen(s, SOMAXCONN=5)");
    E("    mov  rcx, [_net_listen_sock]");
    E("    mov  rdx, 5");
    E("    call listen");
    E("    test eax, eax");
    E("    jnz  .nl_fail");
    E("    ; accept(s, NULL, NULL) -- blocks until peer connects");
    E("    mov  rcx, [_net_listen_sock]");
    E("    xor  rdx, rdx");
    E("    xor  r8, r8");
    E("    call accept");
    E("    cmp  rax, -1");
    E("    je   .nl_fail");
    E("    mov  [_net_conn_sock], rax");
    E("    mov  qword [_net_connected], 1");
    E("    mov  qword [_net_last_ok], 1");
    E("    jmp  .nl_done");
    E(".nl_fail:");
    E("    mov  qword [_net_last_ok], 0");
    E(".nl_done:");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_net_bind(port in rcx) : socket + bind + listen (no accept, no block)
    E("; --- _slag_net_bind (rcx = port) ---");
    E("_slag_net_bind:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48");
    E("    mov  [rbp-8], rcx          ; save port");
    E("    mov  rcx, 2                ; AF_INET");
    E("    mov  rdx, 1                ; SOCK_STREAM");
    E("    xor  r8, r8");
    E("    call socket");
    E("    mov  [_net_listen_sock], rax");
    E("    cmp  rax, -1");
    E("    je   .nb_fail");
    E("    mov  word [_net_sockaddr], 2");
    E("    mov  rcx, [rbp-8]");
    E("    call htons");
    E("    mov  word [_net_sockaddr+2], ax");
    E("    mov  dword [_net_sockaddr+4], 0   ; INADDR_ANY");
    E("    mov  dword [_net_sockaddr+8], 0");
    E("    mov  dword [_net_sockaddr+12], 0");
    E("    mov  rcx, [_net_listen_sock]");
    E("    lea  rdx, [_net_sockaddr]");
    E("    mov  r8, 16");
    E("    call bind");
    E("    test eax, eax");
    E("    jnz  .nb_fail");
    E("    mov  rcx, [_net_listen_sock]");
    E("    mov  rdx, 5");
    E("    call listen");
    E("    test eax, eax");
    E("    jnz  .nb_fail");
    E("    mov  qword [_net_last_ok], 1");
    E("    jmp  .nb_done");
    E(".nb_fail:");
    E("    mov  qword [_net_last_ok], 0");
    E(".nb_done:");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_net_accept() : block until a peer connects to the bound socket
    E("; --- _slag_net_accept (uses _net_listen_sock) ---");
    E("_slag_net_accept:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    mov  rcx, [_net_listen_sock]");
    E("    test rcx, rcx");
    E("    jz   .na_fail            ; not bound");
    E("    xor  rdx, rdx");
    E("    xor  r8, r8");
    E("    call accept");
    E("    cmp  rax, -1");
    E("    je   .na_fail");
    E("    mov  [_net_conn_sock], rax");
    E("    mov  qword [_net_connected], 1");
    E("    mov  qword [_net_last_ok], 1");
    E("    jmp  .na_done");
    E(".na_fail:");
    E("    mov  qword [_net_last_ok], 0");
    E(".na_done:");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_net_connect(host_ptr in rcx, port in rdx) : connect to peer
    E("; --- _slag_net_connect (rcx=host cstr ptr, rdx=port) ---");
    E("_slag_net_connect:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48");
    E("    mov  [rbp-8], rcx          ; host ptr");
    E("    mov  [rbp-16], rdx         ; port");
    E("    mov  rcx, 2");
    E("    mov  rdx, 1");
    E("    xor  r8, r8");
    E("    call socket");
    E("    mov  [_net_conn_sock], rax");
    E("    cmp  rax, -1");
    E("    je   .nc_fail");
    E("    mov  word [_net_sockaddr], 2");
    E("    mov  rcx, [rbp-16]");
    E("    call htons");
    E("    mov  word [_net_sockaddr+2], ax");
    E("    ; inet_addr(host)");
    E("    mov  rcx, [rbp-8]");
    E("    call inet_addr");
    E("    mov  dword [_net_sockaddr+4], eax");
    E("    mov  dword [_net_sockaddr+8], 0");
    E("    mov  dword [_net_sockaddr+12], 0");
    E("    ; connect(s, &sockaddr, 16)");
    E("    mov  rcx, [_net_conn_sock]");
    E("    lea  rdx, [_net_sockaddr]");
    E("    mov  r8, 16");
    E("    call connect");
    E("    test eax, eax");
    E("    jnz  .nc_fail");
    E("    mov  qword [_net_last_ok], 1");
    E("    mov  qword [_net_connected], 1");
    E("    jmp  .nc_done");
    E(".nc_fail:");
    E("    mov  qword [_net_last_ok], 0");
    E(".nc_done:");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_net_send_byte(value in rcx) : send one byte over conn socket
    E("; --- _slag_net_send_byte (rcx = byte value) ---");
    E("_slag_net_send_byte:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    mov  byte [_net_bytebuf], cl");
    E("    mov  rcx, [_net_conn_sock]");
    E("    lea  rdx, [_net_bytebuf]");
    E("    mov  r8, 1");
    E("    xor  r9, r9");
    E("    call send");
    E("    cmp  rax, 1");
    E("    sete al");
    E("    movzx rax, al");
    E("    mov  [_net_last_ok], rax");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_net_recv_byte() -> received byte in rax (or -1 on fail)
    E("; --- _slag_net_recv_byte -> rax ---");
    E("_slag_net_recv_byte:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    mov  rcx, [_net_conn_sock]");
    E("    lea  rdx, [_net_bytebuf]");
    E("    mov  r8, 1");
    E("    xor  r9, r9");
    E("    call recv");
    E("    cmp  rax, 1");
    E("    jne  .nrb_fail");
    E("    mov  qword [_net_last_ok], 1");
    E("    movzx rax, byte [_net_bytebuf]");
    E("    jmp  .nrb_done");
    E(".nrb_fail:");
    E("    mov  qword [_net_last_ok], 0");
    E("    mov  qword [_net_connected], 0");
    E("    mov  rax, -1");
    E(".nrb_done:");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_net_send_buf(ptr=rcx, len=rdx) : send len bytes from ptr.
    // Loops until all sent. Sets _net_last_ok=1 on full send, 0 on error.
    E("; --- _slag_net_send_buf (rcx=ptr, rdx=len) ---");
    E("_slag_net_send_buf:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    sub  rsp, 40");
    E("    mov  rsi, rcx              ; buf ptr");
    E("    mov  rdi, rdx              ; remaining len");
    E("    xor  rbx, rbx             ; bytes-sent offset");
    E(".nsb_loop:");
    E("    test rdi, rdi");
    E("    jz   .nsb_ok              ; nothing left -> success");
    E("    mov  rcx, [_net_conn_sock]");
    E("    lea  rdx, [rsi + rbx]      ; buf + offset");
    E("    mov  r8,  rdi              ; remaining length");
    E("    xor  r9,  r9              ; flags = 0");
    E("    call send");
    E("    cmp  rax, 0");
    E("    jle  .nsb_fail            ; <=0 -> error / closed");
    E("    add  rbx, rax             ; advance offset");
    E("    sub  rdi, rax             ; reduce remaining");
    E("    jmp  .nsb_loop");
    E(".nsb_ok:");
    E("    mov  qword [_net_last_ok], 1");
    E("    jmp  .nsb_done");
    E(".nsb_fail:");
    E("    mov  qword [_net_last_ok], 0");
    E("    mov  qword [_net_connected], 0");
    E(".nsb_done:");
    E("    add  rsp, 40");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_net_recv_buf(ptr=rcx, maxlen=rdx) -> rax = bytes received.
    // One recv call (may return fewer than maxlen). 0 => peer closed
    // (clears _net_connected). -1/negative => error. Sets _net_last_ok.
    E("; --- _slag_net_recv_buf (rcx=ptr, rdx=maxlen) -> rax bytes ---");
    E("_slag_net_recv_buf:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    mov  r8,  rdx              ; len");
    E("    mov  rdx, rcx              ; buf ptr");
    E("    mov  rcx, [_net_conn_sock]");
    E("    xor  r9,  r9              ; flags = 0");
    E("    call recv");
    E("    cmp  rax, 0");
    E("    jg   .nrcb_ok             ; >0 bytes -> success");
    E("    ; 0 (closed) or <0 (error): mark disconnected");
    E("    mov  qword [_net_last_ok], 0");
    E("    mov  qword [_net_connected], 0");
    E("    jmp  .nrcb_done");
    E(".nrcb_ok:");
    E("    mov  qword [_net_last_ok], 1");
    E(".nrcb_done:");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_net_end() : close sockets + WSACleanup
    E("; --- _slag_net_end ---");
    E("_slag_net_end:");
    E("    mov  qword [_net_connected], 0");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("    mov  rcx, [_net_conn_sock]");
    E("    test rcx, rcx");
    E("    jz   .ne_l");
    E("    call closesocket");
    E("    mov  qword [_net_conn_sock], 0");
    E(".ne_l:");
    E("    mov  rcx, [_net_listen_sock]");
    E("    test rcx, rcx");
    E("    jz   .ne_clean");
    E("    call closesocket");
    E("    mov  qword [_net_listen_sock], 0");
    E(".ne_clean:");
    E("    call WSACleanup");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");
    // _slag_net_connected() -> rax : 1 if active connection alive
    E("; --- _slag_net_connected -> rax ---");
    E("_slag_net_connected:");
    E("    mov  rax, [_net_connected]");
    E("    ret");
    E("");
}