#ifndef NET_RUNTIME_H
#define NET_RUNTIME_H

#include "codegen_internal.h"

// Networking runtime (Winsock / ws2_32). Mirrors window_runtime.c.
// Call these from codegen_program alongside the window_* equivalents.

void emit_net_imports(Codegen *cg);   // ws2_32 extern decls
void emit_net_bss(Codegen *cg);       // .bss globals
void emit_net_runtime(Codegen *cg);   // the _slag_net_* procs (.text)

#endif
