// audio_runtime.c — winmm waveOut audio runtime for Slag.
// Emits NASM asm via cg_emit. Provides low-level PCM audio output
// with software mixing support.
//
// Linker dependency: -lwinmm

#include "codegen_internal.h"
#include "audio_runtime.h"

#define E(...) cg_emit(cg, __VA_ARGS__)

/*
================================================================================
RUNTIME SCOPE - Planned waveOut Audio System
================================================================================

Target API (Slag built-ins):
    audio.init(sample_rate, channels, bits)  - Initialize audio device
    audio.close()                            - Close audio device
    audio.load(filename)                     - Load WAV file, return handle
    audio.free(handle)                       - Free loaded audio
    audio.play(handle)                       - Play sound (fire and forget)
    audio.loop(handle)                       - Play sound looping
    audio.stop(handle)                       - Stop a playing sound
    audio.volume(handle, vol)                - Set volume (0-255)
    audio.master_volume(vol)                 - Set master volume (0-255)

Internal architecture:
    - Double-buffered waveOut for gapless playback
    - Software mixer: N channels mixed to single output buffer
    - Fixed format: 44100 Hz, 16-bit, stereo (CD quality)
    - Sound slots for concurrent playback (8-16 simultaneous sounds)

winmm functions required:
    waveOutOpen          - Open audio device with WAVEFORMATEX
    waveOutClose         - Close audio device
    waveOutPrepareHeader - Prepare WAVEHDR buffer for playback
    waveOutUnprepareHeader - Release WAVEHDR buffer
    waveOutWrite         - Queue buffer for playback
    waveOutReset         - Stop playback, flush buffers

Data structures (in .bss):
    _audio_device        - HWAVEOUT handle (8 bytes)
    _audio_format        - WAVEFORMATEX struct (18 bytes, padded to 24)
    _audio_headers[2]    - WAVEHDR double buffer (2 x 48 bytes)
    _audio_buffers[2]    - PCM data buffers (2 x buffer_size)
    _audio_sounds[16]    - Sound slot array (ptr, len, pos, vol, loop, active)
    _audio_master_vol    - Master volume (0-255)
    _audio_initialized   - Init flag

Mixing strategy:
    - Background thread or callback fills buffers
    - Mix all active sound slots into output buffer
    - Apply per-sound and master volume
    - Submit filled buffer, swap to other buffer

WAV loading:
    - Parse RIFF/WAVE header manually (no mmio dependency)
    - Extract PCM data pointer and length
    - Store in sound slot

================================================================================
*/

// ----- imports (winmm) -----------------------------------------------------
void emit_audio_imports(Codegen *cg) {
    E("extern waveOutOpen");
    E("extern waveOutClose");
    E("extern waveOutPrepareHeader");
    E("extern waveOutUnprepareHeader");
    E("extern waveOutWrite");
    E("extern waveOutReset");
}

// ----- .bss globals --------------------------------------------------------
void emit_audio_bss(Codegen *cg) {
    E("; --- audio runtime bss ---");
    E("_audio_device:      resq 1    ; HWAVEOUT handle");
    E("_audio_format:      resb 24   ; WAVEFORMATEX (18 bytes, padded)");
    E("_audio_header0:     resb 48   ; WAVEHDR for buffer 0");
    E("_audio_header1:     resb 48   ; WAVEHDR for buffer 1");
    E("_audio_initialized: resq 1    ; 1 if audio system ready");
    E("_audio_master_vol:  resq 1    ; master volume 0-255");
}

// ----- runtime procs -------------------------------------------------------
void emit_audio_runtime(Codegen *cg) {
    E("; ===================== audio runtime (winmm) =====================");
    E("; STUB - implementation pending");
    E("");
}
