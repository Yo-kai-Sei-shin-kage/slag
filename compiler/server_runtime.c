// server_runtime.c -- persistent multi-client TCP server runtime for Slag,
// plus UDP LAN discovery (server browser) for the same game-server model.
//
// Backs: net.server_start / net.server_accept / net.server_send /
// net.server_recv / net.server_send_buf / net.server_recv_buf /
// net.server_stop (TCP, reliable) and net.discover_send / discover_poll /
// discover_count / discover_ip / discover_port / discover_name /
// discover_max / discover_clients (UDP, connectionless lobby browsing).
//
// Deliberately standalone from net_runtime.c: its own imports, its own
// .bss state, its own client-slot table. No shared globals between the
// two runtimes.

#include "codegen_internal.h"
#include "server_runtime.h"

#define E(...) cg_emit(cg, __VA_ARGS__)

// Max simultaneous client connections. Bump this single constant (and
// relink) to scale up to 64 -- every table size below derives from it.
#define SERVER_MAX_CLIENTS 4

// Max servers tracked in a client's discovered-server list.
#define DISCOVER_MAX 8

// Fixed UDP port used for the discovery query/reply protocol. Separate
// from the TCP game port (set per-server via net.server_start) and from
// any future lockstep-input UDP channel -- discovery never shares a port
// with actual game traffic.
#define DISCOVERY_PORT 9001

// Max bytes copied from a server name into the discovery reply payload.
#define SERVER_NAME_MAX 32

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
    E("extern sendto");
    E("extern recvfrom");
    E("extern setsockopt");
    E("extern closesocket");
    E("extern htons");
    E("extern ioctlsocket");
    E("extern GetTickCount");
    E("extern WSAGetLastError");
    E("extern WSAIoctl");
}

// ----- .bss globals -------------------------------------------------------
void emit_server_bss(Codegen *cg) {
    E("_srv_wsadata:     resb 512   ; WSADATA for server-mode WSAStartup");
    E("_srv_listen_sock: resq 1     ; non-blocking TCP listening socket");
    E("_srv_sockaddr:    resb 16    ; sockaddr_in scratch (TCP)");
    E("_srv_ioctl_arg:   resd 1     ; FIONBIO mode scratch (1 = non-blocking)");
    E("_srv_bytebuf:     resb 8     ; one-byte send/recv scratch");
    E("_srv_clients:     resq %d    ; client socket handles, 0 = empty slot", SERVER_MAX_CLIENTS);
    E("_srv_last_ok:     resq 1     ; 1 if last net.server_* op succeeded");

    E("_srv_tcp_port:    resq 1     ; TCP game port, echoed back in discovery replies");
    E("_srv_name:        resb %d    ; server name, set by net.server_start", SERVER_NAME_MAX);
    E("_srv_name_len:    resq 1");
    E("_srv_disc_sock:   resq 1     ; non-blocking UDP discovery socket");
    E("_srv_disc_addr:   resb 16    ; sockaddr_in scratch (discovery bind/recv-from/send-to)");
    E("_srv_disc_addrlen: resd 1    ; in/out addrlen for recvfrom");
    E("_srv_disc_qbuf:   resb 8     ; scratch for the tiny incoming query packet");
    E("_srv_disc_rbuf:   resb 52    ; scratch for the outgoing reply packet");
    E("_srv_keepalive:   resb 12    ; tcp_keepalive{onoff,time,interval} for new client sockets");
    E("_srv_keepalive_ret: resd 1   ; WSAIoctl's required (unused) bytes-returned out-param");

    E("_cln_disc_sock:      resq 1     ; non-blocking UDP socket used to browse for servers");
    E("_cln_disc_wsadata:   resb 512");
    E("_cln_disc_bcast_opt: resd 1     ; SO_BROADCAST option value scratch");
    E("_cln_disc_ioctl_arg: resd 1");
    E("_cln_disc_addr:      resb 16    ; sockaddr_in scratch (broadcast dest / reply source)");
    E("_cln_disc_addrlen:   resd 1");
    E("_cln_disc_qbuf:      resb 8     ; the outgoing \"SDSQ\" query bytes");
    E("_cln_disc_rbuf:      resb 52    ; scratch for an incoming reply packet");
    E("_cln_disc_last_ms:   resq 1     ; GetTickCount() at the last actual broadcast send");

    E("_disc_ip:        resd %d", DISCOVER_MAX);
    E("_disc_port:      resq %d", DISCOVER_MAX);
    E("_disc_max:       resq %d", DISCOVER_MAX);
    E("_disc_cur:       resq %d", DISCOVER_MAX);
    E("_disc_namelen:   resq %d", DISCOVER_MAX);
    E("_disc_name:      resb %d", DISCOVER_MAX * SERVER_NAME_MAX);
    E("_disc_ipstrbuf:  resb 16    ; scratch \"a.b.c.d\" formatting buffer");
}

// ----- runtime procs -------------------------------------------------------
void emit_server_runtime(Codegen *cg) {
    E("; ================= persistent server runtime (ws2_32) =================");

    // _slag_server_start(port in rcx, name_ptr in rdx, name_len in r8):
    // WSAStartup + socket+bind+listen for the TCP game port (flipped
    // non-blocking so _slag_server_accept can be polled), plus opens a
    // UDP discovery socket bound to DISCOVERY_PORT so this server shows
    // up for net.discover_poll() on other machines.
    E("; --- _slag_server_start (rcx=port, rdx=name_ptr, r8=name_len) ---");
    E("_slag_server_start:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    push rsi");
    E("    push rdi");
    E("    sub  rsp, 40               ; 4 pushes -> need subamount = 8 (mod 16)");
    E("    mov  rbx, rcx              ; port (survives calls)");
    E("    mov  rsi, rdx              ; name ptr");
    E("    mov  rdi, r8               ; name len");
    E("    mov  qword [_srv_tcp_port], rbx");
    E("    cmp  rdi, %d", SERVER_NAME_MAX);
    E("    jle  .svs_namelen_ok");
    E("    mov  rdi, %d", SERVER_NAME_MAX);
    E(".svs_namelen_ok:");
    E("    mov  qword [_srv_name_len], rdi");
    E("    xor  r10, r10");
    E("    lea  r9, [_srv_name]");
    E(".svs_namecopy:");
    E("    cmp  r10, rdi");
    E("    jge  .svs_namecopy_done");
    E("    movzx eax, byte [rsi + r10]");
    E("    mov  [r9 + r10], al");
    E("    inc  r10");
    E("    jmp  .svs_namecopy");
    E(".svs_namecopy_done:");
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
    E("    mov  word [_srv_sockaddr], 2");
    E("    mov  rcx, rbx");
    E("    call htons");
    E("    mov  word [_srv_sockaddr+2], ax");
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
    E("    mov  dword [_srv_ioctl_arg], 1");
    E("    mov  rcx, [_srv_listen_sock]");
    E("    mov  rdx, 0x8004667E       ; FIONBIO");
    E("    lea  r8, [_srv_ioctl_arg]");
    E("    call ioctlsocket");
    E("    mov  r10, %d", SERVER_MAX_CLIENTS);
    E("    lea  r11, [_srv_clients]");
    E(".svs_clear:");
    E("    mov  qword [r11], 0");
    E("    add  r11, 8");
    E("    dec  r10");
    E("    jnz  .svs_clear");
    E("    mov  rcx, 2                ; AF_INET");
    E("    mov  rdx, 2                ; SOCK_DGRAM");
    E("    xor  r8, r8");
    E("    call socket");
    E("    mov  [_srv_disc_sock], rax");
    E("    cmp  rax, -1");
    E("    je   .svs_fail");
    E("    mov  word [_srv_disc_addr], 2");
    E("    mov  rcx, %d", DISCOVERY_PORT);
    E("    call htons");
    E("    mov  word [_srv_disc_addr+2], ax");
    E("    mov  dword [_srv_disc_addr+4], 0   ; INADDR_ANY");
    E("    mov  dword [_srv_disc_addr+8], 0");
    E("    mov  dword [_srv_disc_addr+12], 0");
    E("    mov  rcx, [_srv_disc_sock]");
    E("    lea  rdx, [_srv_disc_addr]");
    E("    mov  r8, 16");
    E("    call bind");
    E("    test eax, eax");
    E("    jnz  .svs_fail");
    E("    mov  dword [_srv_ioctl_arg], 1");
    E("    mov  rcx, [_srv_disc_sock]");
    E("    mov  rdx, 0x8004667E       ; FIONBIO");
    E("    lea  r8, [_srv_ioctl_arg]");
    E("    call ioctlsocket");
    E("    mov  qword [_srv_last_ok], 1");
    E("    jmp  .svs_done");
    E(".svs_fail:");
    E("    mov  qword [_srv_last_ok], 0");
    E(".svs_done:");
    E("    add  rsp, 40");
    E("    pop  rdi");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_server_accept() -> rax = new client slot index, or -1 if no
    // pending connection (non-blocking) or the table is full. As a side
    // effect, also services one pending UDP discovery query per call
    // (non-blocking) -- callers already poll this once per game-loop
    // tick, so discovery replies ride along for free.
    E("; --- _slag_server_accept -> rax (slot idx or -1) ---");
    E("_slag_server_accept:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx                  ; holds the accept result across discovery servicing");
    E("    sub  rsp, 72               ; 2 pushes -> need subamount = 8 (mod 16), >=72 for WSAIoctl's 5 stack args");
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
    E("    mov  [r9 + r11*8], r10      ; store new client socket");
    E("    mov  rbx, r11               ; save slot idx (non-volatile, survives the call below)");
    E("    mov  qword [_srv_last_ok], 1");
    E("    ; flip the new client socket non-blocking too, so server_recv/recv_buf");
    E("    ; never hang the game loop waiting on one idle client");
    E("    mov  dword [_srv_ioctl_arg], 1");
    E("    mov  rcx, r10");
    E("    mov  rdx, 0x8004667E        ; FIONBIO");
    E("    lea  r8, [_srv_ioctl_arg]");
    E("    call ioctlsocket");
    E("    ; enable TCP keepalive on the new client socket too, so a");
    E("    ; silent disconnect (power loss, cable pull -- no FIN/RST ever");
    E("    ; sent) is still detected within a few seconds instead of never");
    E("    mov  dword [_srv_keepalive], 1        ; onoff");
    E("    mov  dword [_srv_keepalive+4], 2000    ; keepalivetime (ms)");
    E("    mov  dword [_srv_keepalive+8], 1000    ; keepaliveinterval (ms)");
    E("    lea  r9, [_srv_clients]");
    E("    mov  rcx, [r9 + rbx*8]                 ; reload client socket");
    E("    mov  rdx, 0x98000004                   ; SIO_KEEPALIVE_VALS");
    E("    lea  r8, [_srv_keepalive]");
    E("    mov  r9, 12                            ; cbInBuffer");
    E("    mov  qword [rsp+32], 0                 ; lpvOutBuffer = NULL");
    E("    mov  qword [rsp+40], 0                 ; cbOutBuffer = 0");
    E("    lea  rax, [_srv_keepalive_ret]");
    E("    mov  [rsp+48], rax                     ; lpcbBytesReturned (required, non-null)");
    E("    mov  qword [rsp+56], 0                 ; lpOverlapped = NULL");
    E("    mov  qword [rsp+64], 0                 ; lpCompletionRoutine = NULL");
    E("    call WSAIoctl");
    E("    jmp  .sva_service_disc");
    E(".sva_next:");
    E("    inc  r11");
    E("    jmp  .sva_scan");
    E(".sva_full:");
    E("    mov  rcx, r10              ; table full: refuse the connection");
    E("    call closesocket");
    E(".sva_fail:");
    E("    mov  rbx, -1");
    E(".sva_service_disc:");
    E("    mov  rcx, [_srv_disc_sock]");
    E("    test rcx, rcx");
    E("    jz   .sva_disc_done        ; discovery socket not up (server_start didn't run)");
    E("    mov  dword [_srv_disc_addrlen], 16");
    E("    lea  rdx, [_srv_disc_qbuf]");
    E("    mov  r8, 8");
    E("    xor  r9, r9");
    E("    lea  r10, [_srv_disc_addr]");
    E("    mov  [rsp+32], r10");
    E("    lea  r10, [_srv_disc_addrlen]");
    E("    mov  [rsp+40], r10");
    E("    call recvfrom");
    E("    movsxd rax, eax            ; sign-extend 32-bit recvfrom() result");
    E("    cmp  rax, 4");
    E("    jl   .sva_disc_done        ; nothing pending, or too short to be a query");
    E("    cmp  byte [_srv_disc_qbuf+0], 'S'");
    E("    jne  .sva_disc_done");
    E("    cmp  byte [_srv_disc_qbuf+1], 'D'");
    E("    jne  .sva_disc_done");
    E("    cmp  byte [_srv_disc_qbuf+2], 'S'");
    E("    jne  .sva_disc_done");
    E("    cmp  byte [_srv_disc_qbuf+3], 'Q'");
    E("    jne  .sva_disc_done");
    E("    mov  byte [_srv_disc_rbuf+0], 'S'");
    E("    mov  byte [_srv_disc_rbuf+1], 'D'");
    E("    mov  byte [_srv_disc_rbuf+2], 'S'");
    E("    mov  byte [_srv_disc_rbuf+3], 'R'");
    E("    mov  eax, dword [_srv_tcp_port]");
    E("    mov  [_srv_disc_rbuf+4], eax");
    E("    mov  dword [_srv_disc_rbuf+8], %d", SERVER_MAX_CLIENTS);
    E("    xor  r10, r10              ; current client count accumulator");
    E("    xor  r11, r11");
    E("    lea  r9, [_srv_clients]");
    E(".sva_disc_count:");
    E("    cmp  r11, %d", SERVER_MAX_CLIENTS);
    E("    jge  .sva_disc_count_done");
    E("    cmp  qword [r9 + r11*8], 0");
    E("    je   .sva_disc_count_next");
    E("    inc  r10");
    E(".sva_disc_count_next:");
    E("    inc  r11");
    E("    jmp  .sva_disc_count");
    E(".sva_disc_count_done:");
    E("    mov  [_srv_disc_rbuf+12], r10d");
    E("    mov  eax, dword [_srv_name_len]");
    E("    mov  [_srv_disc_rbuf+16], eax");
    E("    xor  r11, r11");
    E("    mov  r10d, dword [_srv_name_len]");
    E("    lea  r8, [_srv_name]");
    E("    lea  r9, [_srv_disc_rbuf + 20]");
    E(".sva_disc_namecopy:");
    E("    cmp  r11, r10");
    E("    jge  .sva_disc_namecopy_done");
    E("    movzx eax, byte [r8 + r11]");
    E("    mov  [r9 + r11], al");
    E("    inc  r11");
    E("    jmp  .sva_disc_namecopy");
    E(".sva_disc_namecopy_done:");
    E("    mov  rcx, [_srv_disc_sock]");
    E("    lea  rdx, [_srv_disc_rbuf]");
    E("    mov  r8, 52");
    E("    xor  r9, r9");
    E("    lea  r10, [_srv_disc_addr]");
    E("    mov  [rsp+32], r10");
    E("    mov  qword [rsp+40], 16");
    E("    call sendto");
    E(".sva_disc_done:");
    E("    mov  rax, rbx");
    E("    add  rsp, 56");
    E("    pop  rbx");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_server_send(idx in rcx, byte value in rdx) -> _srv_last_ok
    // reflects success; a WOULDBLOCK failure does NOT disconnect the
    // slot (still connected, just couldn't send this instant); a real
    // error/disconnect closes and clears the slot.
    E("; --- _slag_server_send (rcx = slot idx, rdx = byte) ---");
    E("_slag_server_send:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 40               ; 2 pushes -> need subamount = 8 (mod 16)");
    E("    mov  rbx, rcx              ; save idx across the calls below");
    E("    cmp  rbx, 0");
    E("    jl   .svsd_fail");
    E("    cmp  rbx, %d", SERVER_MAX_CLIENTS);
    E("    jge  .svsd_fail");
    E("    lea  r9, [_srv_clients]");
    E("    mov  r10, [r9 + rbx*8]");
    E("    test r10, r10");
    E("    jz   .svsd_fail");
    E("    mov  byte [_srv_bytebuf], dl");
    E("    mov  rcx, r10");
    E("    lea  rdx, [_srv_bytebuf]");
    E("    mov  r8, 1");
    E("    xor  r9, r9");
    E("    call send");
    E("    movsxd rax, eax            ; sign-extend 32-bit send() result");
    E("    cmp  rax, 1");
    E("    je   .svsd_ok");
    E("    call WSAGetLastError");
    E("    cmp  eax, 10035            ; WSAEWOULDBLOCK -- still connected, just backed up");
    E("    je   .svsd_wouldblock");
    E("    jmp  .svsd_closed          ; any other error -> treat as disconnect");
    E(".svsd_ok:");
    E("    mov  qword [_srv_last_ok], 1");
    E("    jmp  .svsd_done");
    E(".svsd_wouldblock:");
    E("    mov  qword [_srv_last_ok], 0");
    E("    jmp  .svsd_done");
    E(".svsd_closed:");
    E("    lea  r9, [_srv_clients]");
    E("    mov  rcx, [r9 + rbx*8]");
    E("    call closesocket");
    E("    lea  r9, [_srv_clients]");
    E("    mov  qword [r9 + rbx*8], 0");
    E("    mov  qword [_srv_last_ok], 0");
    E("    jmp  .svsd_done");
    E(".svsd_fail:");
    E("    mov  qword [_srv_last_ok], 0");
    E(".svsd_done:");
    E("    add  rsp, 40");
    E("    pop  rbx");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");
    // _slag_server_recv(idx in rcx) -> rax = byte (0-255) received, -2 if
    // no data is available right now (still connected -- non-blocking
    // socket), or -1 on real disconnect/error (slot closed and cleared).
    E("; --- _slag_server_recv (rcx = slot idx) -> rax ---");
    E("_slag_server_recv:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 40               ; 2 pushes -> need subamount = 8 (mod 16)");
    E("    mov  rbx, rcx              ; save idx across the calls below");
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
    E("    movsxd rax, eax            ; sign-extend 32-bit recv() result");
    E("    cmp  rax, 1");
    E("    je   .svrv_ok");
    E("    cmp  rax, 0");
    E("    je   .svrv_closed          ; recv()==0 -> peer gracefully closed");
    E("    call WSAGetLastError");
    E("    cmp  eax, 10035            ; WSAEWOULDBLOCK -- no data, still connected");
    E("    je   .svrv_wouldblock");
    E("    jmp  .svrv_closed          ; any other error -> treat as disconnect");
    E(".svrv_ok:");
    E("    mov  qword [_srv_last_ok], 1");
    E("    movzx rax, byte [_srv_bytebuf]");
    E("    jmp  .svrv_done");
    E(".svrv_wouldblock:");
    E("    mov  qword [_srv_last_ok], 1");
    E("    mov  rax, -2");
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
    E("    add  rsp, 40");
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
    E("    movsxd rax, eax            ; sign-extend 32-bit send() result");
    E("    cmp  rax, 0");
    E("    jg   .svsb_advance");
    E("    call WSAGetLastError");
    E("    cmp  eax, 10035            ; WSAEWOULDBLOCK -- still connected, just backed up");
    E("    je   .svsb_wouldblock");
    E("    jmp  .svsb_disc            ; any other error -> treat as disconnect");
    E(".svsb_advance:");
    E("    add  r12, rax");
    E("    sub  rdi, rax");
    E("    jmp  .svsb_loop");
    E(".svsb_ok:");
    E("    mov  qword [_srv_last_ok], 1");
    E("    jmp  .svsb_done");
    E(".svsb_wouldblock:");
    E("    mov  qword [_srv_last_ok], 0    ; incomplete send, but still connected");
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
    // received (>=0), -2 if no data is available right now (still
    // connected -- non-blocking socket), or -1 on real disconnect/error
    // (slot closed and cleared).
    E("; --- _slag_server_recv_buf (rcx=idx, rdx=ptr, r8=maxlen) -> rax ---");
    E("_slag_server_recv_buf:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 40               ; 2 pushes -> need subamount = 8 (mod 16)");
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
    E("    movsxd rax, eax            ; sign-extend 32-bit recv() result");
    E("    cmp  rax, 0");
    E("    jg   .svrb_ok");
    E("    je   .svrb_closed          ; recv()==0 -> peer gracefully closed");
    E("    call WSAGetLastError");
    E("    cmp  eax, 10035            ; WSAEWOULDBLOCK -- no data, still connected");
    E("    je   .svrb_wouldblock");
    E("    jmp  .svrb_closed          ; any other error -> treat as disconnect");
    E(".svrb_ok:");
    E("    mov  qword [_srv_last_ok], 1");
    E("    jmp  .svrb_done");
    E(".svrb_wouldblock:");
    E("    mov  qword [_srv_last_ok], 1");
    E("    mov  rax, -2");
    E("    jmp  .svrb_done");
    E(".svrb_closed:");
    E("    lea  r9, [_srv_clients]");
    E("    mov  rcx, [r9 + rbx*8]");
    E("    call closesocket");
    E("    lea  r9, [_srv_clients]");
    E("    mov  qword [r9 + rbx*8], 0");
    E("    mov  qword [_srv_last_ok], 0");
    E("    mov  rax, -1");
    E("    jmp  .svrb_done");
    E(".svrb_fail:");
    E("    mov  qword [_srv_last_ok], 0");
    E("    mov  rax, -1");
    E(".svrb_done:");
    E("    add  rsp, 40");
    E("    pop  rbx");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");
    // _slag_server_connected(idx in rcx) -> rax = 1 if this client slot
    // is actively connected, 0 otherwise. Uses a non-blocking MSG_PEEK
    // recv to actively detect a closed/reset connection even when no
    // real data is flowing (doesn't consume any pending data).
    E("; --- _slag_server_connected (rcx = slot idx) -> rax (1/0) ---");
    E("_slag_server_connected:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 40               ; 2 pushes -> need subamount = 8 (mod 16)");
    E("    mov  rbx, rcx              ; idx");
    E("    cmp  rbx, 0");
    E("    jl   .svc_false");
    E("    cmp  rbx, %d", SERVER_MAX_CLIENTS);
    E("    jge  .svc_false");
    E("    lea  r9, [_srv_clients]");
    E("    mov  r10, [r9 + rbx*8]");
    E("    test r10, r10");
    E("    jz   .svc_false            ; empty slot -> not connected");
    E("    mov  rcx, r10");
    E("    lea  rdx, [_srv_bytebuf]");
    E("    mov  r8, 1");
    E("    mov  r9, 2                 ; MSG_PEEK");
    E("    call recv");
    E("    movsxd rax, eax            ; sign-extend 32-bit recv() result");
    E("    cmp  rax, 0");
    E("    jg   .svc_true             ; data pending -> still connected");
    E("    je   .svc_closed           ; recv()==0 -> peer gracefully closed");
    E("    call WSAGetLastError");
    E("    cmp  eax, 10035            ; WSAEWOULDBLOCK -- no data, still connected");
    E("    je   .svc_true");
    E(".svc_closed:");
    E("    lea  r9, [_srv_clients]");
    E("    mov  rcx, [r9 + rbx*8]");
    E("    call closesocket");
    E("    lea  r9, [_srv_clients]");
    E("    mov  qword [r9 + rbx*8], 0");
    E("    jmp  .svc_false");
    E(".svc_true:");
    E("    mov  rax, 1");
    E("    jmp  .svc_done");
    E(".svc_false:");
    E("    xor  rax, rax");
    E(".svc_done:");
    E("    add  rsp, 40");
    E("    pop  rbx");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_server_stop(): close every client socket, the TCP listen
    // socket, and the UDP discovery socket, then WSACleanup.
    E("; --- _slag_server_stop ---");
    E("_slag_server_stop:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    sub  rsp, 40               ; 2 pushes -> need subamount = 8 (mod 16)");
    E("    xor  rbx, rbx");
    E(".sstop_loop:");
    E("    cmp  rbx, %d", SERVER_MAX_CLIENTS);
    E("    jge  .sstop_clients_done");
    E("    lea  rax, [_srv_clients]");
    E("    mov  rcx, [rax + rbx*8]");
    E("    test rcx, rcx");
    E("    jz   .sstop_next");
    E("    call closesocket");
    E("    lea  rax, [_srv_clients]");
    E("    mov  qword [rax + rbx*8], 0");
    E(".sstop_next:");
    E("    inc  rbx");
    E("    jmp  .sstop_loop");
    E(".sstop_clients_done:");
    E("    mov  rcx, [_srv_listen_sock]");
    E("    test rcx, rcx");
    E("    jz   .sstop_disc");
    E("    call closesocket");
    E("    mov  qword [_srv_listen_sock], 0");
    E(".sstop_disc:");
    E("    mov  rcx, [_srv_disc_sock]");
    E("    test rcx, rcx");
    E("    jz   .sstop_clean");
    E("    call closesocket");
    E("    mov  qword [_srv_disc_sock], 0");
    E(".sstop_clean:");
    E("    call WSACleanup");
    E("    mov  qword [_srv_last_ok], 1");
    E("    add  rsp, 40");
    E("    pop  rbx");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    E("; ================= client-side LAN discovery (ws2_32) =================");

    // _slag_discover_send(): lazily creates+configures a broadcast-capable
    // non-blocking UDP socket, then fires one "is anyone out there" query.
    // Callers re-call this every so often (no sleep() builtin exists, so
    // pacing is the caller's job) to pick up servers that start up later.
    E("; --- _slag_discover_send ---");
    E("_slag_discover_send:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48               ; 1 push -> need subamount = 0 (mod 16), >=48 for setsockopt's 5th arg");
    E("    ; --- rate limit: no-op if called again within 1000ms ---");
    E("    call GetTickCount");
    E("    mov  eax, eax              ; zero-extend 32-bit tick count");
    E("    mov  r10, rax              ; now");
    E("    mov  r11, [_cln_disc_last_ms]");
    E("    add  r11, 1000");
    E("    cmp  r10, r11");
    E("    jl   .cds_done             ; too soon since the last actual send");
    E("    mov  [_cln_disc_last_ms], r10");
    E("    mov  rax, [_cln_disc_sock]");
    E("    test rax, rax");
    E("    jnz  .cds_have_sock");
    E("    mov  rcx, 0x0202");
    E("    lea  rdx, [_cln_disc_wsadata]");
    E("    call WSAStartup");
    E("    mov  rcx, 2                ; AF_INET");
    E("    mov  rdx, 2                ; SOCK_DGRAM");
    E("    xor  r8, r8");
    E("    call socket");
    E("    mov  [_cln_disc_sock], rax");
    E("    cmp  rax, -1");
    E("    je   .cds_done             ; couldn't create the socket, bail");
    E("    mov  dword [_cln_disc_bcast_opt], 1");
    E("    mov  rcx, [_cln_disc_sock]");
    E("    mov  rdx, 0xffff           ; SOL_SOCKET");
    E("    mov  r8,  0x0020           ; SO_BROADCAST");
    E("    lea  r9,  [_cln_disc_bcast_opt]");
    E("    mov  dword [rsp+32], 4     ; optlen");
    E("    call setsockopt");
    E("    mov  dword [_cln_disc_ioctl_arg], 1");
    E("    mov  rcx, [_cln_disc_sock]");
    E("    mov  rdx, 0x8004667E       ; FIONBIO");
    E("    lea  r8,  [_cln_disc_ioctl_arg]");
    E("    call ioctlsocket");
    E(".cds_have_sock:");
    E("    mov  byte [_cln_disc_qbuf+0], 'S'");
    E("    mov  byte [_cln_disc_qbuf+1], 'D'");
    E("    mov  byte [_cln_disc_qbuf+2], 'S'");
    E("    mov  byte [_cln_disc_qbuf+3], 'Q'");
    E("    mov  word [_cln_disc_addr], 2      ; AF_INET");
    E("    mov  rcx, %d", DISCOVERY_PORT);
    E("    call htons");
    E("    mov  word [_cln_disc_addr+2], ax");
    E("    mov  dword [_cln_disc_addr+4], 0xFFFFFFFF   ; 255.255.255.255");
    E("    mov  dword [_cln_disc_addr+8], 0");
    E("    mov  dword [_cln_disc_addr+12], 0");
    E("    mov  rcx, [_cln_disc_sock]");
    E("    lea  rdx, [_cln_disc_qbuf]");
    E("    mov  r8,  4");
    E("    xor  r9,  r9");
    E("    lea  rax, [_cln_disc_addr]");
    E("    mov  [rsp+32], rax");
    E("    mov  qword [rsp+40], 16");
    E("    call sendto");
    E(".cds_done:");
    E("    add  rsp, 48");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_discover_poll() -> rax = discovered-server slot index that was
    // just inserted/updated, or -1 if no reply is pending this call.
    // Non-blocking; entries are never auto-removed (a server stays listed
    // even after clients connect to it).
    E("; --- _slag_discover_poll -> rax (slot idx or -1) ---");
    E("_slag_discover_poll:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx                  ; matched/insert slot index");
    E("    sub  rsp, 56               ; 2 pushes -> need subamount = 8 (mod 16), >=48 for recvfrom stack args");
    E("    mov  rax, [_cln_disc_sock]");
    E("    test rax, rax");
    E("    jz   .cdp_none             ; discover_send() never called yet");
    E("    mov  dword [_cln_disc_addrlen], 16");
    E("    mov  rcx, rax");
    E("    lea  rdx, [_cln_disc_rbuf]");
    E("    mov  r8,  52");
    E("    xor  r9,  r9");
    E("    lea  r10, [_cln_disc_addr]");
    E("    mov  [rsp+32], r10");
    E("    lea  r10, [_cln_disc_addrlen]");
    E("    mov  [rsp+40], r10");
    E("    call recvfrom");
    E("    movsxd rax, eax            ; sign-extend 32-bit recvfrom() result");
    E("    cmp  rax, 4");
    E("    jl   .cdp_none");
    E("    cmp  byte [_cln_disc_rbuf+0], 'S'");
    E("    jne  .cdp_none");
    E("    cmp  byte [_cln_disc_rbuf+1], 'D'");
    E("    jne  .cdp_none");
    E("    cmp  byte [_cln_disc_rbuf+2], 'S'");
    E("    jne  .cdp_none");
    E("    cmp  byte [_cln_disc_rbuf+3], 'R'");
    E("    jne  .cdp_none");
    E("    mov  r11d, [_cln_disc_addr+4]   ; discovered server's raw ip");
    E("    xor  rbx, rbx");
    E("    lea  r9, [_disc_ip]");
    E(".cdp_scan:");
    E("    cmp  rbx, %d", DISCOVER_MAX);
    E("    jge  .cdp_search_empty");
    E("    mov  eax, [r9 + rbx*4]");
    E("    cmp  eax, r11d");
    E("    je   .cdp_found");
    E("    inc  rbx");
    E("    jmp  .cdp_scan");
    E(".cdp_search_empty:");
    E("    xor  rbx, rbx");
    E("    lea  r9, [_disc_ip]");
    E(".cdp_scan_empty:");
    E("    cmp  rbx, %d", DISCOVER_MAX);
    E("    jge  .cdp_none             ; table full, drop this reply");
    E("    mov  eax, [r9 + rbx*4]");
    E("    test eax, eax");
    E("    jz   .cdp_found");
    E("    inc  rbx");
    E("    jmp  .cdp_scan_empty");
    E(".cdp_found:");
    E("    lea  r9, [_disc_ip]");
    E("    mov  [r9 + rbx*4], r11d");
    E("    mov  eax, [_cln_disc_rbuf+4]");
    E("    lea  r9, [_disc_port]");
    E("    mov  [r9 + rbx*8], rax");
    E("    mov  eax, [_cln_disc_rbuf+8]");
    E("    lea  r9, [_disc_max]");
    E("    mov  [r9 + rbx*8], rax");
    E("    mov  eax, [_cln_disc_rbuf+12]");
    E("    lea  r9, [_disc_cur]");
    E("    mov  [r9 + rbx*8], rax");
    E("    mov  eax, [_cln_disc_rbuf+16]");
    E("    cmp  eax, %d", SERVER_NAME_MAX);
    E("    jle  .cdp_namelen_ok");
    E("    mov  eax, %d", SERVER_NAME_MAX);
    E(".cdp_namelen_ok:");
    E("    lea  r9, [_disc_namelen]");
    E("    mov  [r9 + rbx*8], rax");
    E("    mov  rcx, rax              ; copy count");
    E("    lea  r10, [_cln_disc_rbuf+20]   ; src");
    E("    mov  r11, rbx");
    E("    imul r11, r11, %d", SERVER_NAME_MAX);
    E("    lea  r9, [_disc_name]");
    E("    add  r9, r11               ; dst");
    E("    xor  r8, r8");
    E(".cdp_namecopy:");
    E("    cmp  r8, rcx");
    E("    jge  .cdp_namecopy_done");
    E("    movzx eax, byte [r10 + r8]");
    E("    mov  [r9 + r8], al");
    E("    inc  r8");
    E("    jmp  .cdp_namecopy");
    E(".cdp_namecopy_done:");
    E("    mov  rax, rbx");
    E("    jmp  .cdp_done");
    E(".cdp_none:");
    E("    mov  rax, -1");
    E(".cdp_done:");
    E("    add  rsp, 56");
    E("    pop  rbx");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_discover_count() -> rax = number of discovered-server slots in use
    E("; --- _slag_discover_count -> rax ---");
    E("_slag_discover_count:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    xor  rax, rax");
    E("    xor  rcx, rcx");
    E("    lea  r9, [_disc_ip]");
    E(".cdc_loop:");
    E("    cmp  rcx, %d", DISCOVER_MAX);
    E("    jge  .cdc_done");
    E("    mov  edx, [r9 + rcx*4]");
    E("    test edx, edx");
    E("    jz   .cdc_next");
    E("    inc  rax");
    E(".cdc_next:");
    E("    inc  rcx");
    E("    jmp  .cdc_loop");
    E(".cdc_done:");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_discover_port(idx=rcx) -> rax
    E("; --- _slag_discover_port (rcx=idx) -> rax ---");
    E("_slag_discover_port:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    cmp  rcx, 0");
    E("    jl   .cdpo_fail");
    E("    cmp  rcx, %d", DISCOVER_MAX);
    E("    jge  .cdpo_fail");
    E("    lea  r9, [_disc_port]");
    E("    mov  rax, [r9 + rcx*8]");
    E("    jmp  .cdpo_done");
    E(".cdpo_fail:");
    E("    mov  rax, -1");
    E(".cdpo_done:");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_discover_max(idx=rcx) -> rax
    E("; --- _slag_discover_max (rcx=idx) -> rax ---");
    E("_slag_discover_max:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    cmp  rcx, 0");
    E("    jl   .cdma_fail");
    E("    cmp  rcx, %d", DISCOVER_MAX);
    E("    jge  .cdma_fail");
    E("    lea  r9, [_disc_max]");
    E("    mov  rax, [r9 + rcx*8]");
    E("    jmp  .cdma_done");
    E(".cdma_fail:");
    E("    mov  rax, -1");
    E(".cdma_done:");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_discover_clients(idx=rcx) -> rax (current connected count as of last reply)
    E("; --- _slag_discover_clients (rcx=idx) -> rax ---");
    E("_slag_discover_clients:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    cmp  rcx, 0");
    E("    jl   .cdcl_fail");
    E("    cmp  rcx, %d", DISCOVER_MAX);
    E("    jge  .cdcl_fail");
    E("    lea  r9, [_disc_cur]");
    E("    mov  rax, [r9 + rcx*8]");
    E("    jmp  .cdcl_done");
    E(".cdcl_fail:");
    E("    mov  rax, -1");
    E(".cdcl_done:");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_discover_name(idx=rcx) -> rax=ptr, rdx=len (direct view, no copy)
    E("; --- _slag_discover_name (rcx=idx) -> rax=ptr, rdx=len ---");
    E("_slag_discover_name:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    cmp  rcx, 0");
    E("    jl   .cdn_fail");
    E("    cmp  rcx, %d", DISCOVER_MAX);
    E("    jge  .cdn_fail");
    E("    lea  rax, [_disc_name]");
    E("    mov  r10, rcx");
    E("    imul r10, r10, %d", SERVER_NAME_MAX);
    E("    add  rax, r10");
    E("    lea  r9, [_disc_namelen]");
    E("    mov  rdx, [r9 + rcx*8]");
    E("    jmp  .cdn_done");
    E(".cdn_fail:");
    E("    xor  rax, rax");
    E("    xor  rdx, rdx");
    E(".cdn_done:");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_discover_ip(idx=rcx) -> rax=ptr, rdx=len ("a.b.c.d" formatted
    // into a shared scratch buffer -- caller should consume it right away,
    // e.g. `local str ip = net.discover_ip(i);` before calling anything
    // else that might reuse the buffer).
    E("; --- _slag_discover_ip (rcx=idx) -> rax=ptr, rdx=len ---");
    E("_slag_discover_ip:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push rbx");
    E("    push rsi");
    E("    sub  rsp, 32               ; 3 pushes -> need subamount = 0 (mod 16)");
    E("    cmp  rcx, 0");
    E("    jl   .cdip_fail");
    E("    cmp  rcx, %d", DISCOVER_MAX);
    E("    jge  .cdip_fail");
    E("    lea  r9, [_disc_ip]");
    E("    mov  eax, [r9 + rcx*4]     ; raw ip bytes, low-to-high = first..last octet");
    E("    mov  ebx, eax");
    E("    lea  rsi, [_disc_ipstrbuf] ; output cursor");
    E("    movzx rcx, bl");
    E("    mov  rdx, rsi");
    E("    call _slag_itoa");
    E("    add  rsi, rax");
    E("    mov  byte [rsi], '.'");
    E("    inc  rsi");
    E("    mov  eax, ebx");
    E("    shr  eax, 8");
    E("    movzx rcx, al");
    E("    mov  rdx, rsi");
    E("    call _slag_itoa");
    E("    add  rsi, rax");
    E("    mov  byte [rsi], '.'");
    E("    inc  rsi");
    E("    mov  eax, ebx");
    E("    shr  eax, 16");
    E("    movzx rcx, al");
    E("    mov  rdx, rsi");
    E("    call _slag_itoa");
    E("    add  rsi, rax");
    E("    mov  byte [rsi], '.'");
    E("    inc  rsi");
    E("    mov  eax, ebx");
    E("    shr  eax, 24");
    E("    movzx rcx, al");
    E("    mov  rdx, rsi");
    E("    call _slag_itoa");
    E("    add  rsi, rax");
    E("    lea  rax, [_disc_ipstrbuf]");
    E("    mov  rdx, rsi");
    E("    sub  rdx, rax             ; total formatted length");
    E("    jmp  .cdip_done");
    E(".cdip_fail:");
    E("    xor  rax, rax");
    E("    xor  rdx, rdx");
    E(".cdip_done:");
    E("    add  rsp, 32");
    E("    pop  rsi");
    E("    pop  rbx");
    E("    mov  rsp, rbp");
    E("    pop  rbp");
    E("    ret");
    E("");
}
