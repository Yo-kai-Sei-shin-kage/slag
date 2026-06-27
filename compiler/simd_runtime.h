#ifndef SIMD_RUNTIME_H
#define SIMD_RUNTIME_H

#include "codegen_internal.h"

// SSE2 SIMD runtime for Slag
// Provides 128-bit vector operations for graphics and bulk data processing

void emit_simd_imports(Codegen *cg);   // No external imports needed for SSE2
void emit_simd_bss(Codegen *cg);       // .bss globals (temp vectors if needed)
void emit_simd_data(Codegen *cg);      // .data constants (RGB565 masks)
void emit_simd_runtime(Codegen *cg);   // the _slag_simd_* procs (.text)

#endif
