// crypto_runtime.c -- bcrypt.dll (CNG) crypto runtime for Slag.
// Emits NASM asm via cg_emit. ECDH P-256 key exchange + AES-256-CBC.
//
// Linker dependency: -lbcrypt
//
// Pure primitives. No automatic dispatch from net_runtime.c or
// server_runtime.c -- script calls these manually around whichever
// transport (net.* or net.server_*) it is using.
//
// Builtins (wired in codegen.c as crypto.*):
//   crypto.dh_keygen()                        -> (void)
//     Generates an ephemeral ECDH P-256 keypair into the single
//     module-level slot. One keypair in flight at a time (matches the
//     single-key-slot style of other runtimes, e.g. audio's 32 fixed
//     slots) -- call again to rotate to a fresh keypair.
//   crypto.dh_pubkey(out_ptr)                 -> int len (bytes written)
//     Exports the public key (BCRYPT_ECCPUBLIC_BLOB) to out_ptr. Send
//     these raw bytes to the peer over net.send_buf/server_send_buf.
//   crypto.dh_derive(peer_pub_ptr, peer_pub_len) -> (void)
//     Imports the peer's raw public key bytes, runs ECDH secret
//     agreement against our keypair, and derives a 32-byte AES-256 key
//     via BCryptDeriveKey (HASH, SHA-256) into the single key slot.
//   crypto.aes_encrypt(in_ptr, in_len, out_ptr) -> int len (bytes written)
//     AES-256-CBC with PKCS7 padding. A fresh random 16-byte IV is
//     generated per call and prepended to out_ptr (out_len = 16 +
//     padded ciphertext length). out_ptr must have room for
//     16 + in_len rounded up to the next 16-byte block.
//   crypto.aes_decrypt(in_ptr, in_len, out_ptr) -> int len (bytes written)
//     Expects the 16-byte IV prepended (as produced by aes_encrypt).
//     Returns -1 on padding/decrypt failure.
//
// Typical handshake (either side of net.* or net.server_*):
//   crypto.dh_keygen()
//   crypto.dh_pubkey(buf)              -> send buf over the wire
//   crypto.dh_derive(peer_buf, peer_len)  ; from the wire
//   ; both sides now hold the same AES-256 key in the module slot
//   crypto.aes_encrypt(msg, msg_len, out)  -> send out over the wire
//   crypto.aes_decrypt(in, in_len, out)    ; from the wire

#include "codegen_internal.h"
#include "crypto_runtime.h"

#define E(...) cg_emit(cg, __VA_ARGS__)

void emit_crypto_imports(Codegen *cg) {
    E("extern BCryptOpenAlgorithmProvider");
    E("extern BCryptGenerateKeyPair");
    E("extern BCryptFinalizeKeyPair");
    E("extern BCryptExportKey");
    E("extern BCryptImportKeyPair");
    E("extern BCryptSecretAgreement");
    E("extern BCryptDeriveKey");
    E("extern BCryptDestroySecret");
    E("extern BCryptDestroyKey");
    E("extern BCryptSetProperty");
    E("extern BCryptGenerateSymmetricKey");
    E("extern BCryptEncrypt");
    E("extern BCryptDecrypt");
    E("extern BCryptGenRandom");
}

void emit_crypto_bss(Codegen *cg) {
    E("; --- crypto runtime bss ---");
    E("_crypto_ecdh_provider: resq 1    ; BCRYPT_ALG_HANDLE, ECDH P-256");
    E("_crypto_aes_provider:  resq 1    ; BCRYPT_ALG_HANDLE, AES");
    E("_crypto_keypair:       resq 1    ; BCRYPT_KEY_HANDLE, our ephemeral ECDH keypair");
    E("_crypto_peer_key:      resq 1    ; BCRYPT_KEY_HANDLE, transient imported peer pubkey");
    E("_crypto_aes_key:       resq 1    ; BCRYPT_KEY_HANDLE, derived AES-256 session key");
    E("_crypto_pubkey_blob:   resb 104  ; exported BCRYPT_ECCPUBLIC_BLOB (8 hdr + 32 X + 32 Y, P-256)");
    E("_crypto_peer_blob:     resb 104  ; peer's raw pubkey bytes, imported as-is");
    E("_crypto_secret:        resq 1    ; BCRYPT_SECRET_HANDLE from BCryptSecretAgreement");
    E("_crypto_iv_scratch:    resb 16   ; per-call IV working copy (BCryptEncrypt/Decrypt mutates it)");
    E("_crypto_ulresult:      resd 1    ; scratch ULONG out-param for *Result calls");
    E("_crypto_derived_key_bytes: resb 32  ; raw AES-256 key material from BCryptDeriveKey");
    E("_crypto_kdf_desc:      resb 16   ; BCryptBufferDesc {ulVersion, cBuffers, pBuffers}");
    E("_crypto_kdf_buf:       resb 16   ; BCryptBuffer {cbBuffer, BufferType, pvBuffer}");
}

void emit_crypto_runtime(Codegen *cg) {
    E("; ===================== crypto runtime (bcrypt / CNG) =====================");
    E("STATUS_SUCCESS   equ 0");
    E("AES_BLOCK_LEN    equ 16");
    E("BCRYPT_ECCPUBLIC_BLOB_LEN equ 72   ; P-256: 8-byte BCRYPT_ECCKEY_BLOB hdr (magic+cbKey) + X(32) + Y(32)");
    E("");

    // --- helper: ensure algorithm providers are open (lazy, idempotent) ---
    // Frame: 1 push (odd) -> subamount %16==0; BCryptSetProperty's 5th arg
    // at [rsp+32] needs frame >=40 -> 48.
    E("; --- _slag_crypto_init_providers (no args) -- opens ECDH+AES providers once ---");
    E("_slag_crypto_init_providers:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48");
    E("    cmp  qword [_crypto_ecdh_provider], 0");
    E("    jne  .cip_aes");
    E("    lea  rcx, [_crypto_ecdh_provider]");
    E("    lea  rdx, [rel _bcrypt_ecdh_id]");
    E("    xor  r8, r8");
    E("    xor  r9, r9");
    E("    call BCryptOpenAlgorithmProvider");
    E(".cip_aes:");
    E("    cmp  qword [_crypto_aes_provider], 0");
    E("    jne  .cip_done");
    E("    lea  rcx, [_crypto_aes_provider]");
    E("    lea  rdx, [rel _bcrypt_aes_id]");
    E("    xor  r8, r8");
    E("    xor  r9, r9");
    E("    call BCryptOpenAlgorithmProvider");
    E("    ; set chaining mode to CBC");
    E("    ; BCryptSetProperty(hObject, pszProperty, pbInput, cbInput, dwFlags) -- 5 args, 1 on stack");
    E("    mov  rcx, [_crypto_aes_provider]");
    E("    lea  rdx, [rel _bcrypt_chaining_mode_id]");
    E("    lea  r8, [rel _bcrypt_chain_mode_cbc]");
    E("    mov  r9, _bcrypt_chain_mode_cbc_len");
    E("    mov  qword [rsp+32], 0        ; dwFlags");
    E("    call BCryptSetProperty");
    E(".cip_done:");
    E("    leave");
    E("    ret");
    E("");

    // --- crypto.dh_keygen() ---
    // 1 push (odd) -> subamount %16==0; BCryptExportKey's stack args reach
    // [rsp+48] -> frame >=56 -> 64.
    E("; --- _slag_crypto_dh_keygen (no args) ---");
    E("_slag_crypto_dh_keygen:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 64");
    E("    call _slag_crypto_init_providers");
    E("");
    E("    ; drop any prior ephemeral keypair before generating a new one");
    E("    cmp  qword [_crypto_keypair], 0");
    E("    je   .dhk_gen");
    E("    mov  rcx, [_crypto_keypair]");
    E("    call BCryptDestroyKey");
    E("    mov  qword [_crypto_keypair], 0");
    E(".dhk_gen:");
    E("    ; BCryptGenerateKeyPair(hAlgorithm, *phKey, dwLength, dwFlags)");
    E("    mov  rcx, [_crypto_ecdh_provider]");
    E("    lea  rdx, [_crypto_keypair]");
    E("    mov  r8, 256             ; key length bits (P-256)");
    E("    xor  r9, r9");
    E("    call BCryptGenerateKeyPair");
    E("");
    E("    ; BCryptFinalizeKeyPair(hKey, dwFlags)");
    E("    mov  rcx, [_crypto_keypair]");
    E("    xor  rdx, rdx");
    E("    call BCryptFinalizeKeyPair");
    E("");
    E("    ; export the public key blob into _crypto_pubkey_blob now so");
    E("    ; dh_pubkey() is just a memcpy of a known length");
    E("    ; BCryptExportKey(hKey, hExportKey, pszBlobType, pOutput, cbOutput, *pcbResult, dwFlags)");
    E("    ; args 1-4 in rcx/rdx/r8/r9, args 5-7 on stack at [rsp+32/40/48]");
    E("    mov  rcx, [_crypto_keypair]");
    E("    xor  rdx, rdx            ; hExportKey = NULL");
    E("    lea  r8, [rel _bcrypt_eccpublic_blob_id]");
    E("    lea  r9, [_crypto_pubkey_blob]");
    E("    mov  qword [rsp+32], BCRYPT_ECCPUBLIC_BLOB_LEN  ; cbOutput");
    E("    lea  rax, [_crypto_ulresult]");
    E("    mov  [rsp+40], rax       ; pcbResult");
    E("    mov  qword [rsp+48], 0   ; dwFlags");
    E("    call BCryptExportKey");
    E("    leave");
    E("    ret");
    E("");

    // --- crypto.dh_pubkey(out_ptr) -> rax = len ---
    E("; --- _slag_crypto_dh_pubkey (rcx=out_ptr) -> rax = bytes written ---");
    E("_slag_crypto_dh_pubkey:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    mov  r10, rcx            ; out_ptr");
    E("    lea  r11, [_crypto_pubkey_blob]   ; src (r11 volatile -- no save needed)");
    E("    xor  rax, rax");
    E(".dhp_copy:");
    E("    cmp  rax, BCRYPT_ECCPUBLIC_BLOB_LEN");
    E("    jge  .dhp_done");
    E("    mov  cl, [r11+rax]");
    E("    mov  [r10+rax], cl");
    E("    inc  rax");
    E("    jmp  .dhp_copy");
    E(".dhp_done:");
    E("    mov  rax, BCRYPT_ECCPUBLIC_BLOB_LEN");
    E("    leave");
    E("    ret");
    E("");

    // --- crypto.dh_derive(peer_pub_ptr, peer_pub_len) ---
    // 3 pushes (odd) -> subamount %16==0; deepest calls' stack args reach
    // [rsp+48] -> frame >=56 -> 64.
    E("; --- _slag_crypto_dh_derive (rcx=peer_pub_ptr, rdx=peer_pub_len) ---");
    E("_slag_crypto_dh_derive:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push r12");
    E("    push r13");
    E("    sub  rsp, 64               ; 3 pushes -> need subamount = 0 (mod 16)");
    E("    mov  r12, rcx            ; peer_pub_ptr");
    E("    mov  r13, rdx            ; peer_pub_len");
    E("");
    E("    ; copy peer's raw blob into our import buffer verbatim (producer");
    E("    ; side already wrote a correctly-shaped BCRYPT_ECCPUBLIC_BLOB)");
    E("    xor  rax, rax");
    E(".dhd_copy:");
    E("    cmp  rax, r13");
    E("    jge  .dhd_import");
    E("    cmp  rax, BCRYPT_ECCPUBLIC_BLOB_LEN");
    E("    jge  .dhd_import");
    E("    mov  cl, [r12+rax]");
    E("    lea  rdx, [_crypto_peer_blob]");
    E("    mov  [rdx+rax], cl");
    E("    inc  rax");
    E("    jmp  .dhd_copy");
    E(".dhd_import:");
    E("    ; BCryptImportKeyPair(hAlgorithm, hImportKey, pszBlobType, *phKey, pInput, cbInput, dwFlags)");
    E("    mov  rcx, [_crypto_ecdh_provider]");
    E("    xor  rdx, rdx            ; hImportKey = NULL");
    E("    lea  r8, [rel _bcrypt_eccpublic_blob_id]");
    E("    lea  r9, [_crypto_peer_key]");
    E("    lea  rax, [_crypto_peer_blob]");
    E("    mov  [rsp+32], rax       ; pInput");
    E("    mov  qword [rsp+40], BCRYPT_ECCPUBLIC_BLOB_LEN  ; cbInput");
    E("    mov  qword [rsp+48], 0   ; dwFlags");
    E("    call BCryptImportKeyPair");
    E("");
    E("    ; secret agreement: our keypair (priv) x peer pubkey");
    E("    ; BCryptSecretAgreement(hPrivKey, hPubKey, *phSecret, dwFlags)");
    E("    mov  rcx, [_crypto_keypair]");
    E("    mov  rdx, [_crypto_peer_key]");
    E("    lea  r8, [_crypto_secret]");
    E("    xor  r9, r9");
    E("    call BCryptSecretAgreement");
    E("");
    E("    ; destroy the transient peer key handle now that agreement is done");
    E("    mov  rcx, [_crypto_peer_key]");
    E("    call BCryptDestroyKey");
    E("    mov  qword [_crypto_peer_key], 0");
    E("");
    E("    ; derive a 32-byte AES-256 key from the shared secret via SHA-256 KDF");
    E("    cmp  qword [_crypto_aes_key], 0");
    E("    je   .dhd_derive");
    E("    mov  rcx, [_crypto_aes_key]");
    E("    call BCryptDestroyKey");
    E("    mov  qword [_crypto_aes_key], 0");
    E(".dhd_derive:");
    E("    ; Build the KDF param list so BCryptDeriveKey uses the HASH KDF with");
    E("    ; SHA-256 (NOT the default SHA-1). pwszKDF must be \"HASH\"; the hash");
    E("    ; algorithm is supplied via a BCryptBufferDesc -> BCryptBuffer of type");
    E("    ; KDF_HASH_ALGORITHM(0) pointing at the \"SHA256\" wide-string id.");
    E("    ; (validated against the in-process ECDH round-trip smoke test)");
    E("    lea  r10, [_crypto_kdf_buf]");
    E("    mov  dword [r10+0], 14   ; cbBuffer: 'SHA256'+NUL = 7 wchar * 2 bytes");
    E("    mov  dword [r10+4], 0    ; BufferType = KDF_HASH_ALGORITHM");
    E("    lea  rax, [rel _bcrypt_sha256_id]");
    E("    mov  [r10+8], rax        ; pvBuffer -> SHA256 id");
    E("    lea  r11, [_crypto_kdf_desc]");
    E("    mov  dword [r11+0], 0    ; ulVersion = BCRYPTBUFFER_VERSION");
    E("    mov  dword [r11+4], 1    ; cBuffers");
    E("    mov  [r11+8], r10        ; pBuffers -> kdf_buf");
    E("    ; BCryptDeriveKey(hSharedSecret, pwszKDF, pParameterList, pbDerivedKey, cbDerivedKey, *pcbResult, dwFlags)");
    E("    ; captures 32 raw AES-256 key bytes; those are then wrapped into a key");
    E("    ; handle below via BCryptGenerateSymmetricKey for Encrypt/Decrypt.");
    E("    mov  rcx, [_crypto_secret]");
    E("    lea  rdx, [rel _bcrypt_hash_kdf_id]   ; pwszKDF = \"HASH\"");
    E("    lea  r8, [_crypto_kdf_desc]           ; pParameterList");
    E("    lea  r9, [_crypto_derived_key_bytes]");
    E("    mov  qword [rsp+32], 32  ; cbDerivedKey (AES-256)");
    E("    lea  rax, [_crypto_ulresult]");
    E("    mov  [rsp+40], rax       ; pcbResult");
    E("    mov  qword [rsp+48], 0   ; dwFlags");
    E("    call BCryptDeriveKey");
    E("");
    E("    mov  rcx, [_crypto_secret]");
    E("    call BCryptDestroySecret");
    E("    mov  qword [_crypto_secret], 0");
    E("");
    E("    ; wrap the raw 32-byte key into an AES key handle for Encrypt/Decrypt");
    E("    ; BCryptGenerateSymmetricKey(hAlgorithm, *phKey, pbKeyObject, cbKeyObject, pbSecret, cbSecret, dwFlags)");
    E("    mov  rcx, [_crypto_aes_provider]");
    E("    lea  rdx, [_crypto_aes_key]");
    E("    xor  r8, r8              ; pbKeyObject = NULL -> CNG allocates");
    E("    xor  r9, r9              ; cbKeyObject = 0");
    E("    lea  rax, [_crypto_derived_key_bytes]");
    E("    mov  [rsp+32], rax       ; pbSecret");
    E("    mov  qword [rsp+40], 32  ; cbSecret");
    E("    mov  qword [rsp+48], 0   ; dwFlags");
    E("    call BCryptGenerateSymmetricKey");
    E("");
    E("    add  rsp, 64");
    E("    pop  r13");
    E("    pop  r12");
    E("    leave");
    E("    ret");
    E("");

    // --- crypto.aes_encrypt(in_ptr, in_len, out_ptr) -> rax = out_len ---
    // 4 pushes (even) -> subamount %16==8; BCryptEncrypt's stack args reach
    // [rsp+72] -> frame >=80 -> 88.
    E("; --- _slag_crypto_aes_encrypt (rcx=in_ptr, rdx=in_len, r8=out_ptr) -> rax ---");
    E("_slag_crypto_aes_encrypt:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    sub  rsp, 88               ; 4 pushes -> need subamount = 8 (mod 16)");
    E("    mov  r12, rcx            ; in_ptr");
    E("    mov  r13, rdx            ; in_len");
    E("    mov  r14, r8             ; out_ptr");
    E("");
    E("    ; fresh random 16-byte IV, written straight to out_ptr[0:16]");
    E("    ; BCryptGenRandom(hAlgorithm, pbBuffer, cbBuffer, dwFlags)");
    E("    xor  rcx, rcx            ; hAlgorithm = NULL (use system RNG)");
    E("    mov  rdx, r14");
    E("    mov  r8, AES_BLOCK_LEN");
    E("    mov  r9, 2               ; BCRYPT_USE_SYSTEM_PREFERRED_RNG");
    E("    call BCryptGenRandom");
    E("    ; working IV copy (BCryptEncrypt mutates its IV arg in place)");
    E("    lea  r11, [_crypto_iv_scratch]   ; dst (r11 volatile -- no save needed)");
    E("    xor  rax, rax");
    E(".aese_ivcopy:");
    E("    cmp  rax, AES_BLOCK_LEN");
    E("    jge  .aese_go");
    E("    mov  cl, [r14+rax]");
    E("    mov  [r11+rax], cl");
    E("    inc  rax");
    E("    jmp  .aese_ivcopy");
    E(".aese_go:");
    E("    ; BCryptEncrypt(hKey, pbInput, cbInput, pPaddingInfo, pbIV, cbIV, pbOutput, cbOutput, *pcbResult, dwFlags)");
    E("    ; args 1-4 in rcx/rdx/r8/r9, args 5-10 on stack at [rsp+32..+72]");
    E("    mov  rcx, [_crypto_aes_key]");
    E("    mov  rdx, r12            ; pbInput (plaintext)");
    E("    mov  r8, r13             ; cbInput");
    E("    xor  r9, r9              ; pPaddingInfo = NULL (unused for AES-CBC)");
    E("    lea  rax, [_crypto_iv_scratch]");
    E("    mov  [rsp+32], rax       ; pbIV");
    E("    mov  qword [rsp+40], AES_BLOCK_LEN     ; cbIV");
    E("    lea  rax, [r14+AES_BLOCK_LEN]");
    E("    mov  [rsp+48], rax       ; pbOutput (ciphertext, after the IV prefix)");
    E("    mov  rax, r13");
    E("    add  rax, AES_BLOCK_LEN");
    E("    mov  [rsp+56], rax       ; cbOutput (caller must size out_ptr >= in_len rounded up a block + IV)");
    E("    lea  rax, [_crypto_ulresult]");
    E("    mov  [rsp+64], rax       ; pcbResult");
    E("    mov  qword [rsp+72], 1   ; dwFlags = BCRYPT_BLOCK_PADDING");
    E("    call BCryptEncrypt");
    E("");
    E("    mov  eax, [_crypto_ulresult]");
    E("    add  rax, AES_BLOCK_LEN  ; total out_len = IV prefix + ciphertext");
    E("    add  rsp, 88");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    leave");
    E("    ret");
    E("");

    // --- crypto.aes_decrypt(in_ptr, in_len, out_ptr) -> rax = out_len (-1 on failure) ---
    // Same frame shape as aes_encrypt: 4 pushes -> subamount = 8 (mod 16).
    E("; --- _slag_crypto_aes_decrypt (rcx=in_ptr, rdx=in_len, r8=out_ptr) -> rax ---");
    E("_slag_crypto_aes_decrypt:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    sub  rsp, 88               ; 4 pushes -> need subamount = 8 (mod 16)");
    E("    mov  r12, rcx            ; in_ptr (IV prefix + ciphertext)");
    E("    mov  r13, rdx            ; in_len");
    E("    mov  r14, r8             ; out_ptr");
    E("");
    E("    lea  r11, [_crypto_iv_scratch]   ; dst (r11 volatile -- no save needed)");
    E("    xor  rax, rax");
    E(".aesd_ivcopy:");
    E("    cmp  rax, AES_BLOCK_LEN");
    E("    jge  .aesd_go");
    E("    mov  cl, [r12+rax]");
    E("    mov  [r11+rax], cl");
    E("    inc  rax");
    E("    jmp  .aesd_ivcopy");
    E(".aesd_go:");
    E("    ; BCryptDecrypt(hKey, pbInput, cbInput, pPaddingInfo, pbIV, cbIV, pbOutput, cbOutput, *pcbResult, dwFlags)");
    E("    mov  rcx, [_crypto_aes_key]");
    E("    lea  rdx, [r12+AES_BLOCK_LEN]   ; pbInput (ciphertext, after IV prefix)");
    E("    mov  r8, r13");
    E("    sub  r8, AES_BLOCK_LEN          ; cbInput (ciphertext len)");
    E("    xor  r9, r9              ; pPaddingInfo = NULL");
    E("    lea  rax, [_crypto_iv_scratch]");
    E("    mov  [rsp+32], rax       ; pbIV");
    E("    mov  qword [rsp+40], AES_BLOCK_LEN     ; cbIV");
    E("    mov  [rsp+48], r14       ; pbOutput");
    E("    mov  [rsp+56], r13       ; cbOutput (caller-sized capacity)");
    E("    lea  rax, [_crypto_ulresult]");
    E("    mov  [rsp+64], rax       ; pcbResult");
    E("    mov  qword [rsp+72], 1   ; dwFlags = BCRYPT_BLOCK_PADDING");
    E("    call BCryptDecrypt");
    E("");
    E("    cmp  eax, STATUS_SUCCESS");
    E("    je   .aesd_ok");
    E("    mov  rax, -1");
    E("    jmp  .aesd_ret");
    E(".aesd_ok:");
    E("    mov  eax, [_crypto_ulresult]");
    E(".aesd_ret:");
    E("    add  rsp, 88");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    leave");
    E("    ret");
    E("");

    // --- wide-string algorithm IDs bcrypt expects (UTF-16LE, NUL-terminated) ---
    // Hand-encoded as dw byte lists -- no wstring literal macro exists in this
    // codebase's NASM usage, so each ASCII char is spelled out as its UTF-16LE
    // code unit (identical numeric value for ASCII range).
    E("section .data");
    E("_bcrypt_ecdh_id:           dw 'E','C','D','H','_','P','2','5','6',0");
    E("_bcrypt_aes_id:            dw 'A','E','S',0");
    E("_bcrypt_hash_kdf_id:       dw 'H','A','S','H',0   ; BCRYPT_KDF_HASH -- the KDF name");
    E("_bcrypt_sha256_id:         dw 'S','H','A','2','5','6',0  ; hash alg fed to the KDF via param list");
    E("_bcrypt_eccpublic_blob_id: dw 'E','C','C','P','U','B','L','I','C','B','L','O','B',0");
    E("_bcrypt_chaining_mode_id:  dw 'C','h','a','i','n','i','n','g','M','o','d','e',0");
    E("_bcrypt_chain_mode_cbc:    dw 'C','h','a','i','n','i','n','g','M','o','d','e','C','B','C',0");
    E("_bcrypt_chain_mode_cbc_len: equ 32   ; 16 wchar_t incl. NUL, in bytes");
    E("section .text");
    E("");
}
