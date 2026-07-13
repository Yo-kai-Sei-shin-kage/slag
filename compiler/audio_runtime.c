// audio_runtime.c -- winmm waveOut audio runtime for Slag.
// Emits NASM asm via cg_emit. Software-mixed PCM audio output.
//
// Linker dependency: -lwinmm
//
// Builtins (wired in codegen.c as audio.*):
//   audio.init(rate, channels, bits) -> int bool (1/0)
//     Opens the waveOut device at the given format. All sounds loaded
//     afterward must match this exact format -- no resampling/conversion.
//   audio.close()                    -> (void)
//   audio.load(path)                 -> int handle (-1 on fail)
//     Parses a RIFF/WAVE file, validates its format against whatever
//     audio.init opened, and registers it in one of 32 sound slots.
//     If the file has a "smpl" chunk with at least one loop point, its
//     start/end (converted from sample-frame indices to byte offsets)
//     are stored for audio.loop() to use; otherwise looping wraps the
//     whole file, same as before.
//   audio.free(handle)               -> (void)
//   audio.play(handle)               -> (void)   one-shot, whole file,
//                                                 ignores loop points
//   audio.loop(handle)               -> (void)   plays from byte 0 once,
//                                                 then repeats
//                                                 [loop_start, loop_end)
//                                                 forever (whole file if
//                                                 no smpl chunk was found)
//   audio.stop(handle)               -> (void)
//   audio.pause(handle)              -> (void)   halt playback, keep position
//   audio.resume(handle)             -> (void)   continue a paused sound
//   audio.is_paused(handle)          -> int bool (1/0)
//   audio.volume(handle, vol)        -> (void)   per-sound volume 0-255
//   audio.master_volume(vol)         -> (void)
//   audio.is_playing(handle)         -> int bool (1/0)
//   audio.position(handle)           -> int byte offset into PCM data
//
// Up to 32 sounds may be loaded and mixed simultaneously. A background
// thread (spawned by audio.init, following the same CreateThread-based
// persistent-worker pattern as the rasterizer pool in window_runtime.c)
// continuously sums all active sound slots into a double-buffered
// output, clamped to int16 range, and feeds it to waveOut via polling
// on WHDR_DONE (CALLBACK_NULL, not event/callback-based).

#include "codegen_internal.h"
#include "audio_runtime.h"

#define E(...) cg_emit(cg, __VA_ARGS__)

void emit_audio_imports(Codegen *cg) {
    E("extern waveOutOpen");
    E("extern waveOutClose");
    E("extern waveOutPrepareHeader");
    E("extern waveOutUnprepareHeader");
    E("extern waveOutWrite");
    E("extern waveOutReset");
}

void emit_audio_bss(Codegen *cg) {
    E("; --- audio runtime bss ---");
    E("_audio_device:        resq 1    ; HWAVEOUT handle");
    E("_audio_rate:          resq 1");
    E("_audio_channels:      resq 1");
    E("_audio_bits:          resq 1");
    E("_audio_format:        resb 24   ; WAVEFORMATEX (18 bytes, padded)");
    E("_audio_header0:       resb 48   ; WAVEHDR for buffer 0");
    E("_audio_header1:       resb 48   ; WAVEHDR for buffer 1");
    E("_audio_buf0:          resb 16384 ; AUDIO_BUFFER_BYTES");
    E("_audio_buf1:          resb 16384");
    E("_audio_sounds:        resq 32   ; loaded-sound slot pointers (0=empty)");
    E("_audio_master_vol:    resq 1");
    E("_audio_thread_handle: resq 1");
    E("_audio_running:       resq 1    ; mixer thread loop flag");
    E("_audio_hdr0_submitted: resq 1");
    E("_audio_hdr1_submitted: resq 1");
}

void emit_audio_runtime(Codegen *cg) {
    E("; ===================== audio runtime (winmm mixer) =====================");
    E("SOUND_PCM_PTR    equ 0");
    E("SOUND_PCM_LEN    equ 8");
    E("SOUND_POS        equ 16");
    E("SOUND_VOLUME     equ 24");
    E("SOUND_LOOPING    equ 32");
    E("SOUND_ACTIVE     equ 40");
    E("SOUND_FILEBUF    equ 48");
    E("SOUND_LOOP_START equ 56");
    E("SOUND_LOOP_END   equ 64");
    E("SOUND_PAN        equ 72   ; 0=hard left .. 128=center .. 255=hard right");
    E("SOUND_PAUSED     equ 80   ; 1=paused (ACTIVE=0 but POS retained), 0=not");
    E("SOUND_SIZE       equ 88");
    E("AUDIO_BUFFER_FRAMES equ 4096");
    E("AUDIO_BUFFER_BYTES  equ 16384");
    E("");

    // _slag_audio_init(rcx=rate, rdx=channels, r8=bits) -> rax = 1/0
    E("; --- _slag_audio_init (rcx=rate, rdx=channels, r8=bits) -> rax ---");
    E("_slag_audio_init:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    push r12");
    E("    push r13");
    E("    push r14");
    E("    sub  rsp, 8             ; 3 pushes (odd) -> pad to realign");
    E("    mov  r12, rcx           ; rate");
    E("    mov  r13, rdx           ; channels");
    E("    mov  r14, r8            ; bits");
    E("");
    E("    mov  [_audio_rate], r12");
    E("    mov  [_audio_channels], r13");
    E("    mov  [_audio_bits], r14");
    E("");
    E("    ; Build WAVEFORMATEX at _audio_format");
    E("    mov  word [_audio_format+0], 1      ; wFormatTag = WAVE_FORMAT_PCM");
    E("    mov  rax, r13");
    E("    mov  word [_audio_format+2], ax     ; nChannels");
    E("    mov  rax, r12");
    E("    mov  dword [_audio_format+4], eax   ; nSamplesPerSec");
    E("");
    E("    mov  rax, r14           ; bits");
    E("    mov  rcx, 8");
    E("    xor  rdx, rdx");
    E("    idiv rcx                ; rax = bytes per sample");
    E("    mov  r10, rax");
    E("    mov  rax, r13");
    E("    imul rax, r10           ; rax = nBlockAlign");
    E("    mov  r11, rax");
    E("    mov  word [_audio_format+12], ax    ; nBlockAlign");
    E("");
    E("    mov  rax, r12");
    E("    imul rax, r11           ; rax = nAvgBytesPerSec");
    E("    mov  dword [_audio_format+8], eax");
    E("");
    E("    mov  rax, r14");
    E("    mov  word [_audio_format+14], ax    ; wBitsPerSample");
    E("    mov  word [_audio_format+16], 0     ; cbSize");
    E("");
    E("    ; waveOutOpen(&_audio_device, WAVE_MAPPER, &_audio_format, 0, 0, CALLBACK_NULL)");
    E("    lea  rcx, [_audio_device]");
    E("    mov  edx, 0xFFFFFFFF   ; WAVE_MAPPER, zero-extended into rdx");
    E("    lea  r8,  [_audio_format]");
    E("    xor  r9,  r9");
    E("    sub  rsp, 48");
    E("    mov  qword [rsp+32], 0");
    E("    mov  qword [rsp+40], 0");
    E("    call waveOutOpen");
    E("    add  rsp, 48");
    E("    test rax, rax");
    E("    jnz  .ai_fail");
    E("");
    E("    ; Build WAVEHDR for both buffers");
    E("    lea  rax, [_audio_buf0]");
    E("    mov  [_audio_header0+0], rax");
    E("    mov  dword [_audio_header0+8], AUDIO_BUFFER_BYTES");
    E("    mov  dword [_audio_header0+12], 0");
    E("    mov  qword [_audio_header0+16], 0");
    E("    mov  dword [_audio_header0+24], 0");
    E("    mov  dword [_audio_header0+28], 0");
    E("    mov  qword [_audio_header0+32], 0");
    E("    mov  qword [_audio_header0+40], 0");
    E("");
    E("    lea  rax, [_audio_buf1]");
    E("    mov  [_audio_header1+0], rax");
    E("    mov  dword [_audio_header1+8], AUDIO_BUFFER_BYTES");
    E("    mov  dword [_audio_header1+12], 0");
    E("    mov  qword [_audio_header1+16], 0");
    E("    mov  dword [_audio_header1+24], 0");
    E("    mov  dword [_audio_header1+28], 0");
    E("    mov  qword [_audio_header1+32], 0");
    E("    mov  qword [_audio_header1+40], 0");
    E("");
    E("    mov  rcx, [_audio_device]");
    E("    lea  rdx, [_audio_header0]");
    E("    mov  r8,  48");
    E("    sub  rsp, 32");
    E("    call waveOutPrepareHeader");
    E("    add  rsp, 32");
    E("    test rax, rax");
    E("    jnz  .ai_fail");
    E("");
    E("    mov  rcx, [_audio_device]");
    E("    lea  rdx, [_audio_header1]");
    E("    mov  r8,  48");
    E("    sub  rsp, 32");
    E("    call waveOutPrepareHeader");
    E("    add  rsp, 32");
    E("    test rax, rax");
    E("    jnz  .ai_fail");
    E("");
    E("    mov  qword [_audio_running], 1");
    E("    mov  qword [_audio_master_vol], 255  ; default full master volume (BSS is 0 -> silence otherwise)");
    E("    mov  qword [_audio_hdr0_submitted], 0");
    E("    mov  qword [_audio_hdr1_submitted], 0");
    E("");
    E("    xor  rcx, rcx");
    E("    xor  rdx, rdx");
    E("    lea  r8,  [_slag_audio_mixer_thread]");
    E("    xor  r9,  r9");
    E("    sub  rsp, 48");
    E("    mov  qword [rsp+32], 0");
    E("    mov  qword [rsp+40], 0");
    E("    call CreateThread");
    E("    add  rsp, 48");
    E("    test rax, rax");
    E("    jz   .ai_thread_fail");
    E("    mov  [_audio_thread_handle], rax");
    E("    mov  rax, 1");
    E("    jmp  .ai_done");
    E(".ai_thread_fail:");
    E("    mov  qword [_audio_running], 0");
    E("    xor  rax, rax");
    E("    jmp  .ai_done");
    E(".ai_fail:");
    E("    xor  rax, rax");
    E(".ai_done:");
    E("    add  rsp, 8");
    E("    pop  r14");
    E("    pop  r13");
    E("    pop  r12");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_audio_close()
    E("; --- _slag_audio_close () ---");
    E("_slag_audio_close:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("");
    E("    mov  qword [_audio_running], 0");
    E("");
    E("    mov  rcx, [_audio_thread_handle]");
    E("    test rcx, rcx");
    E("    jz   .ac_no_thread");
    E("    mov  rdx, 0xFFFFFFFF    ; INFINITE");
    E("    call WaitForSingleObject");
    E(".ac_no_thread:");
    E("");
    E("    mov  rcx, [_audio_device]");
    E("    test rcx, rcx");
    E("    jz   .ac_done");
    E("    call waveOutReset");
    E("");
    E("    mov  rcx, [_audio_device]");
    E("    lea  rdx, [_audio_header0]");
    E("    mov  r8,  48");
    E("    call waveOutUnprepareHeader");
    E("");
    E("    mov  rcx, [_audio_device]");
    E("    lea  rdx, [_audio_header1]");
    E("    mov  r8,  48");
    E("    call waveOutUnprepareHeader");
    E("");
    E("    mov  rcx, [_audio_device]");
    E("    call waveOutClose");
    E("    mov  qword [_audio_device], 0");
    E(".ac_done:");
    E("    add  rsp, 32");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_audio_load(rcx=path) -> rax = handle (-1 on fail)
    // Stack layout (all cross-call values live here, never trusted in a
    // register across a call):
    //   [rbp-8]  file_buf_ptr      [rbp-88]  data_len
    //   [rbp-16] file_len          [rbp-96]  loop_start_byte
    //   [rbp-24] parse_pos         [rbp-104] loop_end_byte
    //   [rbp-32] found_fmt         [rbp-112] block_align
    //   [rbp-40] found_data        [rbp-120] found_smpl
    //   [rbp-48] fmt_channels      [rbp-128] smpl_loop_start_frame
    //   [rbp-56] fmt_rate          [rbp-136] smpl_loop_end_frame
    //   [rbp-64] fmt_bits          [rbp-144] sound_struct_ptr
    //   [rbp-72] fmt_tag
    //   [rbp-80] data_ptr
    E("; --- _slag_audio_load (rcx=path) -> rax ---");
    E("_slag_audio_load:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 208");
    E("");
    E("    call _slag_readfile");
    E("    mov  [rbp-8], rax");
    E("    mov  [rbp-16], rdx");
    E("    test rax, rax");
    E("    jz   .al_fail_noalloc");
    E("");
    E("    mov  rax, [rbp-8]");
    E("    cmp  dword [rax+0], 0x46464952   ; \"RIFF\"");
    E("    jne  .al_fail_free");
    E("    cmp  dword [rax+8], 0x45564157   ; \"WAVE\"");
    E("    jne  .al_fail_free");
    E("");
    E("    mov  qword [rbp-32], 0   ; found_fmt");
    E("    mov  qword [rbp-40], 0   ; found_data");
    E("    mov  qword [rbp-120], 0  ; found_smpl");
    E("    mov  qword [rbp-24], 12  ; parse_pos");
    E("");
    E(".al_chunk_loop:");
    E("    mov  rax, [rbp-24]");
    E("    mov  rcx, [rbp-16]");
    E("    cmp  rax, rcx");
    E("    jge  .al_chunk_done");
    E("    mov  rdx, rax");
    E("    add  rdx, 8");
    E("    cmp  rdx, rcx");
    E("    jg   .al_chunk_done");
    E("");
    E("    mov  r8,  [rbp-8]");
    E("    add  r8,  rax           ; r8 = &file_buf[parse_pos]");
    E("    mov  r9d, [r8+0]        ; chunk_id");
    E("    mov  r10d, [r8+4]       ; chunk_size");
    E("    mov  r11, rax");
    E("    add  r11, 8             ; r11 = chunk_data_offset");
    E("");
    E("    cmp  r9d, 0x20746D66    ; \"fmt \"");
    E("    jne  .al_check_data");
    E("    mov  rax, [rbp-8]");
    E("    add  rax, r11");
    E("    movzx rcx, word [rax+0]");
    E("    mov  [rbp-72], rcx      ; fmt_tag");
    E("    movzx rcx, word [rax+2]");
    E("    mov  [rbp-48], rcx      ; fmt_channels");
    E("    mov  ecx, [rax+4]");
    E("    mov  [rbp-56], rcx      ; fmt_rate");
    E("    movzx rcx, word [rax+14]");
    E("    mov  [rbp-64], rcx      ; fmt_bits");
    E("    mov  qword [rbp-32], 1  ; found_fmt");
    E("    jmp  .al_chunk_advance");
    E("");
    E(".al_check_data:");
    E("    cmp  r9d, 0x61746164    ; \"data\"");
    E("    jne  .al_check_smpl");
    E("    mov  rax, [rbp-8]");
    E("    add  rax, r11");
    E("    mov  [rbp-80], rax      ; data_ptr");
    E("    mov  [rbp-88], r10      ; data_len");
    E("    mov  qword [rbp-40], 1  ; found_data");
    E("    jmp  .al_chunk_advance");
    E("");
    E(".al_check_smpl:");
    E("    cmp  r9d, 0x6C706D73    ; \"smpl\"");
    E("    jne  .al_chunk_advance");
    E("    mov  rax, [rbp-8]");
    E("    add  rax, r11");
    E("    mov  ecx, [rax+28]      ; cSampleLoops");
    E("    test ecx, ecx");
    E("    jz   .al_chunk_advance");
    E("    mov  edx, [rax+36+8]    ; first loop dwStart (sample frame)");
    E("    mov  [rbp-128], rdx");
    E("    mov  edx, [rax+36+12]   ; first loop dwEnd (sample frame)");
    E("    mov  [rbp-136], rdx");
    E("    mov  qword [rbp-120], 1 ; found_smpl");
    E("");
    E(".al_chunk_advance:");
    E("    mov  rax, r11");
    E("    add  rax, r10           ; + chunk_size");
    E("    mov  rdx, r10");
    E("    and  rdx, 1             ; +1 pad byte if chunk_size is odd");
    E("    add  rax, rdx");
    E("    mov  [rbp-24], rax      ; parse_pos");
    E("    jmp  .al_chunk_loop");
    E("");
    E(".al_chunk_done:");
    E("    cmp  qword [rbp-32], 0");
    E("    je   .al_fail_free");
    E("    cmp  qword [rbp-40], 0");
    E("    je   .al_fail_free");
    E("    cmp  qword [rbp-72], 1  ; fmt_tag must be PCM");
    E("    jne  .al_fail_free");
    E("    mov  rax, [rbp-48]");
    E("    cmp  rax, [_audio_channels]");
    E("    jne  .al_fail_free");
    E("    mov  rax, [rbp-56]");
    E("    cmp  rax, [_audio_rate]");
    E("    jne  .al_fail_free");
    E("    mov  rax, [rbp-64]");
    E("    cmp  rax, [_audio_bits]");
    E("    jne  .al_fail_free");
    E("");
    E("    mov  rax, [rbp-64]");
    E("    mov  rcx, 8");
    E("    xor  rdx, rdx");
    E("    idiv rcx                ; rax = bytes per sample");
    E("    mov  rcx, rax");
    E("    mov  rax, [rbp-48]");
    E("    imul rax, rcx           ; block_align");
    E("    mov  [rbp-112], rax");
    E("");
    E("    mov  rax, [rbp-88]");
    E("    mov  [rbp-104], rax     ; loop_end_byte = data_len (default)");
    E("    mov  qword [rbp-96], 0  ; loop_start_byte = 0 (default)");
    E("");
    E("    cmp  qword [rbp-120], 0");
    E("    je   .al_no_smpl");
    E("    mov  rax, [rbp-128]");
    E("    imul rax, [rbp-112]");
    E("    mov  [rbp-96], rax      ; loop_start_byte");
    E("    mov  rax, [rbp-136]");
    E("    imul rax, [rbp-112]");
    E("    mov  [rbp-104], rax     ; loop_end_byte");
    E(".al_no_smpl:");
    E("");
    E("    sub  rsp, 32");
    E("    call GetProcessHeap");
    E("    add  rsp, 32");
    E("    mov  rcx, rax");
    E("    xor  rdx, rdx");
    E("    mov  r8,  SOUND_SIZE");
    E("    sub  rsp, 32");
    E("    call HeapAlloc");
    E("    add  rsp, 32");
    E("    test rax, rax");
    E("    jz   .al_fail_free");
    E("    mov  [rbp-144], rax     ; sound_struct_ptr");
    E("");
    E("    mov  rcx, [rbp-144]");
    E("    mov  rax, [rbp-80]");
    E("    mov  [rcx+SOUND_PCM_PTR], rax");
    E("    mov  rax, [rbp-88]");
    E("    mov  [rcx+SOUND_PCM_LEN], rax");
    E("    mov  qword [rcx+SOUND_POS], 0");
    E("    mov  qword [rcx+SOUND_VOLUME], 255");
    E("    mov  qword [rcx+SOUND_LOOPING], 0");
    E("    mov  qword [rcx+SOUND_ACTIVE], 0");
    E("    mov  qword [rcx+SOUND_PAUSED], 0");
    E("    mov  qword [rcx+SOUND_PAN], 128   ; center pan (no-op) by default");
    E("    mov  rax, [rbp-8]");
    E("    mov  [rcx+SOUND_FILEBUF], rax");
    E("    mov  rax, [rbp-96]");
    E("    mov  [rcx+SOUND_LOOP_START], rax");
    E("    mov  rax, [rbp-104]");
    E("    mov  [rcx+SOUND_LOOP_END], rax");
    E("");
    E("    lea  r10, [_audio_sounds]");
    E("    xor  r11, r11");
    E(".al_slot_loop:");
    E("    cmp  r11, 32");
    E("    jge  .al_no_slot");
    E("    cmp  qword [r10 + r11*8], 0");
    E("    je   .al_slot_found");
    E("    inc  r11");
    E("    jmp  .al_slot_loop");
    E(".al_slot_found:");
    E("    mov  rax, [rbp-144]");
    E("    mov  [r10 + r11*8], rax");
    E("    jmp  .al_done");
    E("");
    E(".al_no_slot:");
    E("    sub  rsp, 32");
    E("    call GetProcessHeap");
    E("    add  rsp, 32");
    E("    mov  rcx, rax");
    E("    xor  rdx, rdx");
    E("    mov  r8,  [rbp-144]");
    E("    sub  rsp, 32");
    E("    call HeapFree");
    E("    add  rsp, 32");
    E("    sub  rsp, 32");
    E("    call GetProcessHeap");
    E("    add  rsp, 32");
    E("    mov  rcx, rax");
    E("    xor  rdx, rdx");
    E("    mov  r8,  [rbp-8]");
    E("    sub  rsp, 32");
    E("    call HeapFree");
    E("    add  rsp, 32");
    E("    jmp  .al_fail_noalloc");
    E("");
    E(".al_fail_free:");
    E("    sub  rsp, 32");
    E("    call GetProcessHeap");
    E("    add  rsp, 32");
    E("    mov  rcx, rax");
    E("    xor  rdx, rdx");
    E("    mov  r8,  [rbp-8]");
    E("    sub  rsp, 32");
    E("    call HeapFree");
    E("    add  rsp, 32");
    E(".al_fail_noalloc:");
    E("    mov  rax, -1");
    E("    jmp  .al_ret");
    E(".al_done:");
    E("    mov  rax, [rbp-144]");
    E(".al_ret:");
    E("    add  rsp, 208");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_audio_free(rcx=handle)
    E("; --- _slag_audio_free (rcx=handle) ---");
    E("_slag_audio_free:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 48   ; 32 shadow + 16 locals; keeps [rbp-8] out of callee shadow");
    E("    mov  [rbp-8], rcx");
    E("    cmp  rcx, 0");
    E("    jle  .af_done");
    E("");
    E("    mov  qword [rcx+SOUND_ACTIVE], 0");
    E("    mov  qword [rcx+SOUND_PAUSED], 0");
    E("");
    E("    lea  r10, [_audio_sounds]");
    E("    xor  r11, r11");
    E(".af_slot_loop:");
    E("    cmp  r11, 32");
    E("    jge  .af_slot_done");
    E("    mov  rax, [rbp-8]");
    E("    cmp  [r10 + r11*8], rax");
    E("    jne  .af_slot_next");
    E("    mov  qword [r10 + r11*8], 0");
    E("    jmp  .af_slot_done");
    E(".af_slot_next:");
    E("    inc  r11");
    E("    jmp  .af_slot_loop");
    E(".af_slot_done:");
    E("");
    E("    ; free the file buffer only if it is non-null, then null it out so a");
    E("    ; second audio.free (or a free after audio.close) can't double-free.");
    E("    mov  rax, [rbp-8]");
    E("    mov  r8,  [rax+SOUND_FILEBUF]");
    E("    test r8,  r8");
    E("    jz   .af_free_struct");
    E("    call GetProcessHeap");
    E("    mov  rcx, rax");
    E("    xor  rdx, rdx");
    E("    mov  rax, [rbp-8]");
    E("    mov  r8,  [rax+SOUND_FILEBUF]");
    E("    call HeapFree");
    E("    mov  rax, [rbp-8]");
    E("    mov  qword [rax+SOUND_FILEBUF], 0");
    E("");
    E(".af_free_struct:");
    E("    call GetProcessHeap");
    E("    mov  rcx, rax");
    E("    xor  rdx, rdx");
    E("    mov  r8,  [rbp-8]");
    E("    call HeapFree");
    E("");
    E(".af_done:");
    E("    add  rsp, 48");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_audio_play(rcx=handle)
    E("; --- _slag_audio_play (rcx=handle) ---");
    E("_slag_audio_play:");
    E("    cmp  rcx, 0");
    E("    jle  .ap_done");
    E("    mov  qword [rcx+SOUND_POS], 0");
    E("    mov  qword [rcx+SOUND_LOOPING], 0");
    E("    mov  qword [rcx+SOUND_ACTIVE], 1");
    E("    mov  qword [rcx+SOUND_PAUSED], 0");
    E(".ap_done:");
    E("    ret");
    E("");

    // _slag_audio_loop(rcx=handle)
    E("; --- _slag_audio_loop (rcx=handle) ---");
    E("_slag_audio_loop:");
    E("    cmp  rcx, 0");
    E("    jle  .alo_done");
    E("    mov  qword [rcx+SOUND_POS], 0");
    E("    mov  qword [rcx+SOUND_LOOPING], 1");
    E("    mov  qword [rcx+SOUND_ACTIVE], 1");
    E("    mov  qword [rcx+SOUND_PAUSED], 0");
    E(".alo_done:");
    E("    ret");
    E("");

    // _slag_audio_stop(rcx=handle)
    E("; --- _slag_audio_stop (rcx=handle) ---");
    E("_slag_audio_stop:");
    E("    cmp  rcx, 0");
    E("    jle  .as_done");
    E("    mov  qword [rcx+SOUND_ACTIVE], 0");
    E("    mov  qword [rcx+SOUND_PAUSED], 0");
    E("    mov  qword [rcx+SOUND_POS], 0");
    E(".as_done:");
    E("    ret");
    E("");
    // _slag_audio_pause(rcx=handle): stop advancing but retain POS.
    // Only pauses a currently-active sound; no-op on stopped/paused.
    E("; --- _slag_audio_pause (rcx=handle) ---");
    E("_slag_audio_pause:");
    E("    cmp  rcx, 0");
    E("    jle  .apause_done");
    E("    cmp  qword [rcx+SOUND_ACTIVE], 0");
    E("    je   .apause_done   ; not playing -> nothing to pause");
    E("    mov  qword [rcx+SOUND_ACTIVE], 0");
    E("    mov  qword [rcx+SOUND_PAUSED], 1");
    E(".apause_done:");
    E("    ret");
    E("");

    // _slag_audio_resume(rcx=handle): re-activate only a paused sound,
    // continuing from its retained POS. No-op on stopped/playing sounds.
    E("; --- _slag_audio_resume (rcx=handle) ---");
    E("_slag_audio_resume:");
    E("    cmp  rcx, 0");
    E("    jle  .aresume_done");
    E("    cmp  qword [rcx+SOUND_PAUSED], 0");
    E("    je   .aresume_done  ; not paused -> nothing to resume");
    E("    mov  qword [rcx+SOUND_PAUSED], 0");
    E("    mov  qword [rcx+SOUND_ACTIVE], 1");
    E(".aresume_done:");
    E("    ret");
    E("");

    // _slag_audio_is_paused(rcx=handle) -> rax = 1/0
    E("; --- _slag_audio_is_paused (rcx=handle) -> rax ---");
    E("_slag_audio_is_paused:");
    E("    cmp  rcx, 0");
    E("    jle  .aispaused_false");
    E("    mov  rax, [rcx+SOUND_PAUSED]");
    E("    ret");
    E(".aispaused_false:");
    E("    xor  rax, rax");
    E("    ret");
    E("");


    // _slag_audio_volume(rcx=handle, rdx=vol)
    E("; --- _slag_audio_volume (rcx=handle, rdx=vol) ---");
    E("_slag_audio_volume:");
    E("    cmp  rcx, 0");
    E("    jle  .av_done");
    E("    cmp  rdx, 0");
    E("    jge  .av_low_ok");
    E("    xor  rdx, rdx");
    E(".av_low_ok:");
    E("    cmp  rdx, 255");
    E("    jle  .av_high_ok");
    E("    mov  rdx, 255");
    E(".av_high_ok:");
    E("    mov  [rcx+SOUND_VOLUME], rdx");
    E(".av_done:");
    E("    ret");
    E("");

    // _slag_audio_pan(rcx=handle, rdx=pan): 0=hard left, 128=center,
    // 255=hard right. Clamped to 0-255, stored per-sound.
    E("; --- _slag_audio_pan (rcx=handle, rdx=pan) ---");
    E("_slag_audio_pan:");
    E("    cmp  rcx, 0");
    E("    jle  .apan_done");
    E("    cmp  rdx, 0");
    E("    jge  .apan_low_ok");
    E("    xor  rdx, rdx");
    E(".apan_low_ok:");
    E("    cmp  rdx, 255");
    E("    jle  .apan_high_ok");
    E("    mov  rdx, 255");
    E(".apan_high_ok:");
    E("    mov  [rcx+SOUND_PAN], rdx");
    E(".apan_done:");
    E("    ret");
    E("");

    // _slag_audio_master_volume(rcx=vol)
    E("; --- _slag_audio_master_volume (rcx=vol) ---");
    E("_slag_audio_master_volume:");
    E("    cmp  rcx, 0");
    E("    jge  .amv_low_ok");
    E("    xor  rcx, rcx");
    E(".amv_low_ok:");
    E("    cmp  rcx, 255");
    E("    jle  .amv_high_ok");
    E("    mov  rcx, 255");
    E(".amv_high_ok:");
    E("    mov  [_audio_master_vol], rcx");
    E("    ret");
    E("");

    // _slag_audio_is_playing(rcx=handle) -> rax = 1/0
    E("; --- _slag_audio_is_playing (rcx=handle) -> rax ---");
    E("_slag_audio_is_playing:");
    E("    cmp  rcx, 0");
    E("    jle  .aip_false");
    E("    mov  rax, [rcx+SOUND_ACTIVE]");
    E("    ret");
    E(".aip_false:");
    E("    xor  rax, rax");
    E("    ret");
    E("");

    // _slag_audio_position(rcx=handle) -> rax = byte offset into PCM data
    E("; --- _slag_audio_position (rcx=handle) -> rax ---");
    E("_slag_audio_position:");
    E("    cmp  rcx, 0");
    E("    jle  .apos_zero");
    E("    mov  rax, [rcx+SOUND_POS]");
    E("    ret");
    E(".apos_zero:");
    E("    xor  rax, rax");
    E("    ret");
    E("");

    // _slag_audio_mix_buffer(rcx=buf_ptr): fills AUDIO_BUFFER_FRAMES stereo
    // int16 frames by summing every active sound slot (per-sound + master
    // volume + pan, all 0-255), clamped to int16 range. No calls are made
    // inside this proc, so alignment isn't a concern here.
    // Stack: [rbp-8]=buf_ptr  [rbp-16]=frame_i [rbp-24]=acc_l
    //        [rbp-32]=acc_r   [rbp-40]=slot_i  [rbp-48]=pan_gain_l
    //        [rbp-56]=pan_gain_r
    E("; --- _slag_audio_mix_buffer (rcx=buf_ptr) ---");
    E("_slag_audio_mix_buffer:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 64");
    E("    mov  [rbp-8], rcx");
    E("    mov  qword [rbp-16], 0   ; frame_i");
    E("");
    E(".mb_frame_loop:");
    E("    mov  rax, [rbp-16]");
    E("    cmp  rax, AUDIO_BUFFER_FRAMES");
    E("    jge  .mb_frame_done");
    E("");
    E("    mov  qword [rbp-24], 0   ; acc_l");
    E("    mov  qword [rbp-32], 0   ; acc_r");
    E("    mov  qword [rbp-40], 0   ; slot_i");
    E("");
    E(".mb_slot_loop:");
    E("    mov  rax, [rbp-40]");
    E("    cmp  rax, 32");
    E("    jge  .mb_slot_done");
    E("");
    E("    lea  r10, [_audio_sounds]");
    E("    mov  rax, [rbp-40]");
    E("    mov  r11, [r10 + rax*8]");
    E("    test r11, r11");
    E("    jz   .mb_slot_next");
    E("    mov  rax, [r11+SOUND_ACTIVE]");
    E("    test rax, rax");
    E("    jz   .mb_slot_next");
    E("");
    E("    mov  rax, [r11+SOUND_POS]");
    E("    mov  rcx, [r11+SOUND_LOOPING]");
    E("    test rcx, rcx");
    E("    jz   .mb_check_end_oneshot");
    E("    ; A frame read touches bytes [pos .. pos+3]; wrap when pos+4 would");
    E("    ; exceed LOOP_END so we never read past the PCM buffer into heap.");
    E("    mov  rcx, rax");
    E("    add  rcx, 4");
    E("    cmp  rcx, [r11+SOUND_LOOP_END]");
    E("    jle  .mb_read_sample");
    E("    mov  rax, [r11+SOUND_LOOP_START]");
    E("    mov  [r11+SOUND_POS], rax");
    E("    jmp  .mb_read_sample");
    E(".mb_check_end_oneshot:");
    E("    ; same guard: need a full 4-byte frame before PCM_LEN.");
    E("    mov  rcx, rax");
    E("    add  rcx, 4");
    E("    cmp  rcx, [r11+SOUND_PCM_LEN]");
    E("    jle  .mb_read_sample");
    E("    mov  qword [r11+SOUND_ACTIVE], 0");
    E("    mov  qword [r11+SOUND_PAUSED], 0");
    E("    jmp  .mb_slot_next");
    E("");
    E(".mb_read_sample:");
    E("    mov  rax, [r11+SOUND_POS]");
    E("    mov  r10, [r11+SOUND_PCM_PTR]");
    E("    add  r10, rax           ; r10 = &pcm[pos]");
    E("    mov  r8,  [r11+SOUND_VOLUME]");
    E("    mov  r9,  [_audio_master_vol]");
    E("");
    E("    ; pan gains: left = min(128,255-pan), right = min(128,pan).");
    E("    ; Peak gain is 128, matching the 128 term in the /8323200 divide");
    E("    ; so center (128) and full-side both render at unity, not ~2x.");
    E("    mov  rcx, [r11+SOUND_PAN]");
    E("    mov  rax, 255");
    E("    sub  rax, rcx           ; 255 - pan");
    E("    cmp  rax, 128");
    E("    jle  .mb_panl_ok");
    E("    mov  rax, 128");
    E(".mb_panl_ok:");
    E("    mov  [rbp-48], rax      ; pan_gain_l");
    E("    mov  rax, rcx");
    E("    cmp  rax, 128");
    E("    jle  .mb_panr_ok");
    E("    mov  rax, 128");
    E(".mb_panr_ok:");
    E("    mov  [rbp-56], rax      ; pan_gain_r");
    E("");
    E("    movsx rax, word [r10+0] ; sample_l");
    E("    imul rax, r8");
    E("    imul rax, r9");
    E("    imul rax, [rbp-48]      ; * pan_gain_l");
    E("    mov  rcx, 8323200       ; 255*255*128");
    E("    cqo");
    E("    idiv rcx                ; rax = scaled_l");
    E("    add  [rbp-24], rax      ; acc_l += scaled_l");
    E("");
    E("    movsx rax, word [r10+2] ; sample_r");
    E("    imul rax, r8");
    E("    imul rax, r9");
    E("    imul rax, [rbp-56]      ; * pan_gain_r");
    E("    mov  rcx, 8323200");
    E("    cqo");
    E("    idiv rcx                ; rax = scaled_r");
    E("    add  [rbp-32], rax      ; acc_r += scaled_r");
    E("");
    E("    mov  rax, [r11+SOUND_POS]");
    E("    add  rax, 4");
    E("    mov  [r11+SOUND_POS], rax");
    E("");
    E(".mb_slot_next:");
    E("    mov  rax, [rbp-40]");
    E("    inc  rax");
    E("    mov  [rbp-40], rax");
    E("    jmp  .mb_slot_loop");
    E("");
    E(".mb_slot_done:");
    E("    mov  rax, [rbp-24]      ; acc_l");
    E("    cmp  rax, 32767");
    E("    jle  .mb_l_max_ok");
    E("    mov  rax, 32767");
    E(".mb_l_max_ok:");
    E("    cmp  rax, -32768");
    E("    jge  .mb_l_min_ok");
    E("    mov  rax, -32768");
    E(".mb_l_min_ok:");
    E("    mov  [rbp-24], rax      ; clamped acc_l");
    E("");
    E("    mov  rax, [rbp-32]      ; acc_r");
    E("    cmp  rax, 32767");
    E("    jle  .mb_r_max_ok");
    E("    mov  rax, 32767");
    E(".mb_r_max_ok:");
    E("    cmp  rax, -32768");
    E("    jge  .mb_r_min_ok");
    E("    mov  rax, -32768");
    E(".mb_r_min_ok:");
    E("    mov  [rbp-32], rax      ; clamped acc_r");
    E("");
    E("    mov  rax, [rbp-8]       ; buf_ptr");
    E("    mov  rcx, [rbp-16]      ; frame_i");
    E("    imul rcx, 4");
    E("    add  rax, rcx           ; rax = &buf[frame_i*4]");
    E("    mov  rdx, [rbp-24]");
    E("    mov  word [rax+0], dx   ; clamped_l");
    E("    mov  rdx, [rbp-32]");
    E("    mov  word [rax+2], dx   ; clamped_r");
    E("");
    E("    mov  rax, [rbp-16]");
    E("    inc  rax");
    E("    mov  [rbp-16], rax");
    E("    jmp  .mb_frame_loop");
    E("");
    E(".mb_frame_done:");
    E("    add  rsp, 64");
    E("    pop  rbp");
    E("    ret");
    E("");

    // _slag_audio_mixer_thread(rcx=lpParameter) -> rax (DWORD exit code)
    // Persistent background thread spawned once by audio.init. Polls
    // WHDR_DONE on each of the two buffers (CALLBACK_NULL, no event/
    // callback wiring), refills and resubmits whichever is free, sleeps
    // briefly between passes.
    E("; --- _slag_audio_mixer_thread (rcx=lpParameter) -> rax ---");
    E("_slag_audio_mixer_thread:");
    E("    push rbp");
    E("    mov  rbp, rsp");
    E("    sub  rsp, 32");
    E("");
    E(".mt_loop:");
    E("    mov  rax, [_audio_running]");
    E("    test rax, rax");
    E("    jz   .mt_exit");
    E("");
    E("    mov  rax, [_audio_hdr0_submitted]");
    E("    test rax, rax");
    E("    jz   .mt_fill0");
    E("    mov  eax, [_audio_header0+24]   ; dwFlags");
    E("    and  eax, 1                     ; WHDR_DONE");
    E("    jz   .mt_check1");
    E(".mt_fill0:");
    E("    and  dword [_audio_header0+24], 0xFFFFFFFE  ; clear WHDR_DONE only, keep WHDR_PREPARED");
    E("    lea  rcx, [_audio_buf0]");
    E("    call _slag_audio_mix_buffer");
    E("    mov  rcx, [_audio_device]");
    E("    lea  rdx, [_audio_header0]");
    E("    mov  r8,  48");
    E("    call waveOutWrite");
    E("    test rax, rax");
    E("    jnz  .mt_write0_failed");
    E("    mov  qword [_audio_hdr0_submitted], 1");
    E(".mt_write0_failed:");
    E("");
    E(".mt_check1:");
    E("    mov  rax, [_audio_hdr1_submitted]");
    E("    test rax, rax");
    E("    jz   .mt_fill1");
    E("    mov  eax, [_audio_header1+24]");
    E("    and  eax, 1");
    E("    jz   .mt_sleep");
    E(".mt_fill1:");
    E("    and  dword [_audio_header1+24], 0xFFFFFFFE  ; clear WHDR_DONE only, keep WHDR_PREPARED");
    E("    lea  rcx, [_audio_buf1]");
    E("    call _slag_audio_mix_buffer");
    E("    mov  rcx, [_audio_device]");
    E("    lea  rdx, [_audio_header1]");
    E("    mov  r8,  48");
    E("    call waveOutWrite");
    E("    test rax, rax");
    E("    jnz  .mt_write1_failed");
    E("    mov  qword [_audio_hdr1_submitted], 1");
    E(".mt_write1_failed:");
    E("");
    E(".mt_sleep:");
    E("    mov  rcx, 5");
    E("    call Sleep");
    E("    jmp  .mt_loop");
    E("");
    E(".mt_exit:");
    E("    xor  rax, rax");
    E("    add  rsp, 32");
    E("    pop  rbp");
    E("    ret");
    E("");
}
