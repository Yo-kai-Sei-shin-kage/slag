#ifndef AUDIO_RUNTIME_H
#define AUDIO_RUNTIME_H

#include "codegen_internal.h"

// Audio runtime (winmm / waveOut). Low-level PCM audio output.
// Call these from codegen_program alongside other runtime emitters.

void emit_audio_imports(Codegen *cg);   // winmm extern decls
void emit_audio_bss(Codegen *cg);       // .bss globals
void emit_audio_runtime(Codegen *cg);   // the _slag_audio_* procs (.text)

#endif
