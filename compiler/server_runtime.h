#ifndef SERVER_RUNTIME_H
#define SERVER_RUNTIME_H

#include "codegen_internal.h"

// Persistent multi-client TCP server runtime (net.server_* builtins).
// Standalone from net_runtime.c: owns its own imports, .bss state, and
// connection-slot table -- no shared globals or dependencies between them.

void emit_server_imports(Codegen *cg);
void emit_server_bss(Codegen *cg);
void emit_server_runtime(Codegen *cg);

#endif
