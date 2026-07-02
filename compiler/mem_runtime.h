#ifndef MEM_RUNTIME_H
#define MEM_RUNTIME_H

#include "codegen_internal.h"

// Memory/buffer primitives runtime. Mirrors window_runtime.c / net_runtime.c.
// Provides raw heap buffers addressed by plain-int pointers, with sized
// (byte and qword) unchecked accessors for maximum speed.

void emit_mem_imports(Codegen *cg);   // HeapAlloc/HeapFree/GetProcessHeap externs
void emit_mem_bss(Codegen *cg);       // .bss globals (cached heap handle)
void emit_mem_runtime(Codegen *cg);   // the _slag_mem_* procs (.text)

#endif
