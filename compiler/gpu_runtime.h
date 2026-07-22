#ifndef GPU_RUNTIME_H
#define GPU_RUNTIME_H

#include "codegen_internal.h"

// D3D11 GPU runtime emitter (AMD Radeon Vega 8 target).
void emit_gpu_imports(Codegen *cg);   // d3d11 / dxgi extern decls
void emit_gpu_bss(Codegen *cg);       // .bss globals
void emit_gpu_data(Codegen *cg);      // shader DXBC blobs + semantic strings
void emit_gpu_runtime(Codegen *cg);   // the _slag_gpu_* procs (.text)

#endif // GPU_RUNTIME_H
